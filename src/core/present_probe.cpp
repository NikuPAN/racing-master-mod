#include "present_probe.h"

#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>

volatile LONG g_probeOn = 0;

namespace {

constexpr int kCallers = 8;
constexpr int kThreads = 8;
constexpr int kChains = 4;
constexpr int kFlagBits = 10;      // DXGI_PRESENT_* bits 0..9
constexpr int kSyncSlots = 6;      // sync interval 0..4, and "other"
constexpr int kOutcomes = static_cast<int>(ProbeOutcome::Count);

// One module that called Present, found by its address range. `base` is claimed first with a
// compare-exchange, then `end` is written; a reader that sees end == 0 takes the slow path.
struct Caller {
    volatile LONG64 base;
    volatile LONG64 end;
    volatile LONG64 calls[2];          // Present, Present1
    volatile LONG64 outcomes[kOutcomes];
};
struct ThreadSlot {
    volatile LONG id;
    volatile LONG64 calls;
    volatile LONG64 outcomes[kOutcomes];   // which thread the mod draws on, and which it passes on
};
struct ChainSlot {
    volatile LONG64 chain;
    volatile LONG64 table;
    volatile LONG64 calls;
};

// The last slot is never claimed: it holds every call from code in no module (a trampoline or
// other generated code, a likely way for a driver or an overlay to call Present).
constexpr int kNoModuleSlot = kCallers;
Caller g_callers[kCallers + 1];
// Pages (address >> 12, plus 1) recently found in no module, so such a caller is not looked up
// again on every present.
constexpr int kUnresolvedPages = 4;
volatile LONG64 g_unresolvedPages[kUnresolvedPages];
volatile LONG g_unresolvedNext;
ThreadSlot g_threads[kThreads];
ChainSlot g_chains[kChains];
volatile LONG64 g_entryCalls[2];
volatile LONG64 g_moreCallers, g_moreThreads, g_moreChains;
volatile LONG64 g_flagBits[kFlagBits];
volatile LONG64 g_noFlags;
volatile LONG64 g_sync[kSyncSlots];
volatile LONG g_inFlight;
volatile LONG g_inFlightMax;
volatile LONG g_presentCount;          // last GetLastPresentCount of the game's swap chain
volatile LONG g_presentCountKnown;
volatile LONG64 g_recorded;

// Worker only: the values at the previous report, for "in the last 30 s".
LONG64 g_lastRecorded = 0;
LONG g_lastPresentCount = 0;
bool g_lastPresentCountKnown = false;
unsigned long long g_lastGameFrames = 0;

// The size of the module image at `base`, from its PE header (0 if it cannot be read).
LONG64 ImageSize(HMODULE base) {
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(base) + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        return static_cast<LONG64>(nt->OptionalHeader.SizeOfImage);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int CallerSlot(void* address) {
    const LONG64 at = reinterpret_cast<LONG64>(address);
    for (int i = 0; i < kCallers; ++i) {
        const LONG64 end = g_callers[i].end;
        if (end != 0 && at >= g_callers[i].base && at < end) return i;
    }
    const LONG64 page = (at >> 12) + 1;
    for (int i = 0; i < kUnresolvedPages; ++i) {
        if (g_unresolvedPages[i] == page) return kNoModuleSlot;
    }
    // Not seen yet: which module holds it (GetModuleHandleExW does not change any reference count).
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address), &module) ||
        !module) {
        const LONG next = InterlockedIncrement(&g_unresolvedNext);
        InterlockedExchange64(&g_unresolvedPages[next % kUnresolvedPages], page);
        return kNoModuleSlot;
    }
    const LONG64 base = reinterpret_cast<LONG64>(module);
    for (int i = 0; i < kCallers; ++i) {
        const LONG64 previous = InterlockedCompareExchange64(&g_callers[i].base, base, 0);
        if (previous == 0) {
            const LONG64 size = ImageSize(module);
            InterlockedExchange64(&g_callers[i].end, base + (size > 0 ? size : 1));
            return i;
        }
        if (previous == base) return i;
    }
    InterlockedIncrement64(&g_moreCallers);
    return -1;
}

int CountThread() {
    const LONG id = static_cast<LONG>(GetCurrentThreadId());
    for (int i = 0; i < kThreads; ++i) {
        const LONG previous = InterlockedCompareExchange(&g_threads[i].id, id, 0);
        if (previous == 0 || previous == id) {
            InterlockedIncrement64(&g_threads[i].calls);
            return i;
        }
    }
    InterlockedIncrement64(&g_moreThreads);
    return -1;
}

void CountChain(IDXGISwapChain* swapChain) {
    const LONG64 chain = reinterpret_cast<LONG64>(swapChain);
    for (int i = 0; i < kChains; ++i) {
        const LONG64 previous = InterlockedCompareExchange64(&g_chains[i].chain, chain, 0);
        if (previous == 0 || previous == chain) {
            if (previous == 0) {
                // The object's function table pointer: its first field. Read once, when first seen.
                LONG64 table = 0;
                __try {
                    table = *reinterpret_cast<LONG64*>(swapChain);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
                InterlockedExchange64(&g_chains[i].table, table);
            }
            InterlockedIncrement64(&g_chains[i].calls);
            return;
        }
    }
    InterlockedIncrement64(&g_moreChains);
}

// The base name of the module holding `address` ("dxgi.dll"), or a description.
void ModuleName(const void* address, char* out, size_t size) {
    HMODULE module = nullptr;
    if (!address) {
        snprintf(out, size, "nothing");
        return;
    }
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address), &module) ||
        !module) {
        snprintf(out, size, "no module (generated code)");
        return;
    }
    wchar_t path[MAX_PATH] = L"";
    if (!GetModuleFileNameW(module, path, MAX_PATH)) {
        snprintf(out, size, "an unnamed module");
        return;
    }
    const wchar_t* name = wcsrchr(path, L'\\');
    LogUtf8(name ? name + 1 : path, out, static_cast<int>(size));
}

// Entry `slot` of a function table, read safely (0 if it cannot be read).
void* TableEntry(LONG64 table, int slot) {
    if (!table) return nullptr;
    __try {
        return reinterpret_cast<void**>(table)[slot];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

int Append(char* buffer, int used, int size, const char* format, ...) {
    if (used >= size - 1) return used;
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(buffer + used, static_cast<size_t>(size - used), format, args);
    va_end(args);
    return n > 0 ? (used + n < size ? used + n : size - 1) : used;
}

}  // namespace

ProbeSlots ProbeEnter(int entry, void* returnAddress, IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    InterlockedIncrement64(&g_recorded);
    InterlockedIncrement64(&g_entryCalls[entry ? 1 : 0]);
    const LONG inFlight = InterlockedIncrement(&g_inFlight);
    for (LONG seen = g_inFlightMax; inFlight > seen;) {
        const LONG previous = InterlockedCompareExchange(&g_inFlightMax, inFlight, seen);
        if (previous == seen) break;
        seen = previous;
    }
    ProbeSlots slots;
    slots.thread = CountThread();
    CountChain(swapChain);
    if (flags == 0) {
        InterlockedIncrement64(&g_noFlags);
    }
    for (int bit = 0; bit < kFlagBits; ++bit) {
        if (flags & (1u << bit)) InterlockedIncrement64(&g_flagBits[bit]);
    }
    InterlockedIncrement64(&g_sync[syncInterval < kSyncSlots - 1 ? syncInterval : kSyncSlots - 1]);
    slots.caller = CallerSlot(returnAddress);
    if (slots.caller >= 0) InterlockedIncrement64(&g_callers[slots.caller].calls[entry ? 1 : 0]);
    return slots;
}

void ProbeRecordOutcome(ProbeSlots slots, ProbeOutcome outcome) {
    const int o = static_cast<int>(outcome);
    if (o < 0 || o >= kOutcomes) return;
    if (slots.caller >= 0 && slots.caller <= kNoModuleSlot) {
        InterlockedIncrement64(&g_callers[slots.caller].outcomes[o]);
    }
    if (slots.thread >= 0 && slots.thread < kThreads) {
        InterlockedIncrement64(&g_threads[slots.thread].outcomes[o]);
    }
}

void ProbeLeave() {
    InterlockedDecrement(&g_inFlight);
}

void ProbeRecordPresentCount(UINT count) {
    InterlockedExchange(&g_presentCount, static_cast<LONG>(count));
    InterlockedExchange(&g_presentCountKnown, 1);
}

unsigned long long ProbeRecordedCalls() {
    return static_cast<unsigned long long>(g_recorded);
}

void ProbeReport(void* originalPresent, void* originalPresent1, void** table, unsigned long long gameFrames,
                 unsigned seconds) {
    static char line[960];   // a log line holds about 1000 characters
    char name[MAX_PATH * 3];
    const LONG64 recorded = g_recorded;

    // 1. Totals, threads, how many threads were inside the hook at once.
    int used = snprintf(line, sizeof(line),
                        "present probe, last %u s: %lld presents reached the hook (since start: Present %lld, Present1 "
                        "%lld); at most %ld threads inside the hook at once; threads:",
                        seconds, recorded - g_lastRecorded, static_cast<LONG64>(g_entryCalls[0]),
                        static_cast<LONG64>(g_entryCalls[1]), static_cast<long>(g_inFlightMax));
    static const char* const kOutcomeNames[kOutcomes] = {"panel drawn", "game frame without the panel",
                                                         "another swap chain", "passed on while another thread drew",
                                                         "frame work off",
                                                         "game frame from a thread the mod does not draw on"};
    int threads = 0;
    for (const ThreadSlot& t : g_threads) {
        if (t.id == 0) continue;
        ++threads;
        used = Append(line, used, sizeof(line), " %ld x%lld", static_cast<long>(t.id), static_cast<LONG64>(t.calls));
        // What the frame work did with this thread's presents: which thread the mod draws on.
        const char* separator = " (";
        for (int o = 0; o < kOutcomes; ++o) {
            if (!t.outcomes[o]) continue;
            used = Append(line, used, sizeof(line), "%s%s %lld", separator, kOutcomeNames[o], static_cast<LONG64>(t.outcomes[o]));
            separator = ", ";
        }
        if (separator[0] == ',') used = Append(line, used, sizeof(line), ")");
    }
    if (g_moreThreads) used = Append(line, used, sizeof(line), " and %lld from more threads", static_cast<LONG64>(g_moreThreads));
    Append(line, used, sizeof(line), " (%d in all)", threads);
    Log("%s", line);

    // 2. Who called Present, and what the frame work did for each: one line per calling module.
    for (int i = 0; i <= kNoModuleSlot; ++i) {
        const Caller& c = g_callers[i];
        if (i < kNoModuleSlot && c.base == 0) continue;
        if (i == kNoModuleSlot && c.calls[0] == 0 && c.calls[1] == 0) continue;
        if (i == kNoModuleSlot) {
            snprintf(name, sizeof(name), "code in no module");
        } else {
            ModuleName(reinterpret_cast<const void*>(c.base), name, sizeof(name));
        }
        used = snprintf(line, sizeof(line), "present probe: called from %s: Present %lld, Present1 %lld;", name,
                        static_cast<LONG64>(c.calls[0]), static_cast<LONG64>(c.calls[1]));
        for (int o = 0; o < kOutcomes; ++o) {
            if (c.outcomes[o]) used = Append(line, used, sizeof(line), " %s %lld", kOutcomeNames[o], static_cast<LONG64>(c.outcomes[o]));
        }
        Log("%s", line);
    }
    if (g_moreCallers) {
        Log("present probe: called from modules past the first %d %lld times", kCallers,
            static_cast<LONG64>(g_moreCallers));
    }

    // 3. Swap chains and whose Present their function tables point to: one line each.
    for (const ChainSlot& s : g_chains) {
        if (s.chain == 0) continue;
        char tableOwner[MAX_PATH * 3], presentOwner[MAX_PATH * 3];
        ModuleName(reinterpret_cast<const void*>(s.table), tableOwner, sizeof(tableOwner));
        ModuleName(TableEntry(s.table, 8), presentOwner, sizeof(presentOwner));
        Log("present probe: swap chain %llx, %lld presents: its function table is in %s (%s), and its Present entry "
            "points into %s",
            static_cast<unsigned long long>(s.chain), static_cast<LONG64>(s.calls), tableOwner,
            s.table == reinterpret_cast<LONG64>(table) ? "the table the mod patched" : "not the table the mod patched",
            presentOwner);
    }
    if (g_moreChains) {
        Log("present probe: %lld presents came from swap chains past the first %d", static_cast<LONG64>(g_moreChains), kChains);
    }

    // 4. Flags and sync intervals.
    static const char* const kFlagNames[kFlagBits] = {"TEST", "DO_NOT_SEQUENCE", "RESTART", "DO_NOT_WAIT",
                                                      "STEREO_PREFER_RIGHT", "STEREO_TEMPORARY_MONO", "RESTRICT_TO_OUTPUT",
                                                      "bit7", "USE_DURATION", "ALLOW_TEARING"};
    used = snprintf(line, sizeof(line), "present probe: flags: none %lld", static_cast<LONG64>(g_noFlags));
    for (int bit = 0; bit < kFlagBits; ++bit) {
        if (g_flagBits[bit]) used = Append(line, used, sizeof(line), ", %s %lld", kFlagNames[bit], static_cast<LONG64>(g_flagBits[bit]));
    }
    used = Append(line, used, sizeof(line), "; sync interval:");
    for (int i = 0; i < kSyncSlots; ++i) {
        if (!g_sync[i]) continue;
        if (i < kSyncSlots - 1) {
            used = Append(line, used, sizeof(line), " %d x%lld", i, static_cast<LONG64>(g_sync[i]));
        } else {
            used = Append(line, used, sizeof(line), " higher x%lld", static_cast<LONG64>(g_sync[i]));
        }
    }
    Log("%s", line);

    // 5. Hook order: whose Present the hook calls on, and what the patched table entry points to now.
    char original[MAX_PATH * 3], original1[MAX_PATH * 3], now[MAX_PATH * 3];
    ModuleName(originalPresent, original, sizeof(original));
    ModuleName(originalPresent1, original1, sizeof(original1));
    ModuleName(TableEntry(reinterpret_cast<LONG64>(table), 8), now, sizeof(now));
    used = snprintf(line, sizeof(line),
                    "present probe: the Present the mod passes frames on to is in %s (Present1 in %s); the patched table's "
                    "Present entry now points into %s; game frames %llu in the last %u s",
                    original, original1, now, gameFrames - g_lastGameFrames, seconds);
    const bool known = g_presentCountKnown != 0;
    const LONG count = g_presentCount;
    if (known && g_lastPresentCountKnown) {
        Append(line, used, sizeof(line), ", Windows counted %ld presents of the game's swap chain", static_cast<long>(count - g_lastPresentCount));
    } else if (known) {
        Append(line, used, sizeof(line), ", Windows' present count of the game's swap chain is %ld", static_cast<long>(count));
    }
    Log("%s", line);

    g_lastRecorded = recorded;
    g_lastGameFrames = gameFrames;
    g_lastPresentCount = count;
    g_lastPresentCountKnown = known;
}
