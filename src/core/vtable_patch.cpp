#include "vtable_patch.h"

#include <windows.h>

bool PatchSlot(void** table, int slot, void* replacement, void* volatile* original) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(&table[slot], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        return false;
    }
    bool patched = false;
    __try {
        for (;;) {
            void* current = table[slot];
            if (current == replacement) {
                break;   // already ours; never chain to ourselves
            }
            // 1. publish the original, 2. only then switch the entry to the hook
            InterlockedExchangePointer(const_cast<void**>(original), current);
            if (InterlockedCompareExchangePointer(&table[slot], replacement, current) == current) {
                patched = true;
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Another program changed the page protection under us. Leave the table alone.
    }
    DWORD ignored = 0;
    VirtualProtect(&table[slot], sizeof(void*), oldProtect, &ignored);
    return patched;
}
