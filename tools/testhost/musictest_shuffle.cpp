// How shuffle orders a round (src\music\shuffle.cpp): every song once, songs not heard lately
// first, the same artist never back-to-back where possible. Pure functions, fixed seeds.
#include "musictest.h"

#include "music/shuffle.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace music;

namespace {

constexpr int64_t kNow = 1800000000;   // a fixed "now" in Unix seconds
constexpr int64_t kDay = 86400;

bool IsPermutation(const std::vector<int>& order, size_t n) {
    if (order.size() != n) return false;
    std::vector<char> seen(n, 0);
    for (const int i : order) {
        if (i < 0 || static_cast<size_t>(i) >= n || seen[static_cast<size_t>(i)]) return false;
        seen[static_cast<size_t>(i)] = 1;
    }
    return true;
}

std::vector<ShuffleItem> Artists(std::initializer_list<std::pair<const wchar_t*, int>> groups) {
    std::vector<ShuffleItem> items;
    for (const auto& group : groups) {
        for (int i = 0; i < group.second; ++i) items.push_back({group.first, 0});
    }
    return items;
}

}  // namespace

MT_TEST(230, ShuffleOrder, "shuffle: every song once, not heard lately first, artists spread") {
    {
        std::vector<ShuffleItem> items(50);
        uint64_t rng = 1;
        bool ok = true;
        for (int round = 0; round < 50 && ok; ++round) {
            const std::vector<int> order = MakeShuffleOrder(items, round % 50, kNow, &rng);
            ok = IsPermutation(order, items.size()) && order[0] == round % 50;
        }
        mt::Check(ok, "every round plays every song exactly once and starts with the song asked for");
        mt::Check(MakeShuffleOrder({}, 0, kNow, &rng).empty() && IsPermutation(MakeShuffleOrder(items, 99, kNow, &rng), 50),
                  "an empty list gives an empty round; a first song that is not in the list is ignored");
    }
    {
        // 60 songs: 20 by A, 15 by B, 10 by C, 10 by D, 5 by E. Any order can keep them apart.
        const std::vector<ShuffleItem> items = Artists({{L"a", 20}, {L"b", 15}, {L"c", 10}, {L"d", 10}, {L"e", 5}});
        uint64_t rng = 7;
        int together = 0;
        for (int round = 0; round < 300; ++round) {
            const std::vector<int> order = MakeShuffleOrder(items, -1, kNow, &rng);
            for (size_t i = 1; i < order.size(); ++i) {
                if (items[static_cast<size_t>(order[i])].artist == items[static_cast<size_t>(order[i - 1])].artist) ++together;
            }
        }
        mt::Check(together == 0, "the same artist is never played twice in a row when the list allows it (300 rounds of 60 songs)",
                  "%d times back-to-back", together);
    }
    {
        // One artist for everything: nothing to spread; still a full round, no endless search.
        const std::vector<ShuffleItem> items = Artists({{L"only", 12}});
        uint64_t rng = 3;
        mt::Check(IsPermutation(MakeShuffleOrder(items, 4, kNow, &rng), 12), "a list by one artist still gives a full round");
    }
    {
        // 50 songs heard an hour ago, 50 never heard: the never-heard ones come first.
        std::vector<ShuffleItem> items(100);
        for (size_t i = 0; i < 50; ++i) items[i].lastPlayed = kNow - 3600;
        uint64_t rng = 11;
        int early = 0;
        for (int round = 0; round < 100; ++round) {
            const std::vector<int> order = MakeShuffleOrder(items, -1, kNow, &rng);
            for (size_t i = 0; i < 50; ++i) early += order[i] >= 50 ? 1 : 0;
        }
        mt::Check(early >= 100 * 50 * 95 / 100, "songs never heard come before songs heard an hour ago (at least 95 % of the first half)",
                  "%.1f %% of the first half", early / 50.0);
    }
    {
        // 50 songs heard yesterday, 50 a month ago: mostly the month-old ones first, but not strictly.
        std::vector<ShuffleItem> items(100);
        for (size_t i = 0; i < 50; ++i) items[i].lastPlayed = kNow - kDay;
        for (size_t i = 50; i < 100; ++i) items[i].lastPlayed = kNow - 30 * kDay;
        uint64_t rng = 13;
        int early = 0;
        int roundsWithMix = 0;
        for (int round = 0; round < 100; ++round) {
            const std::vector<int> order = MakeShuffleOrder(items, -1, kNow, &rng);
            int here = 0;
            for (size_t i = 0; i < 50; ++i) here += order[i] >= 50 ? 1 : 0;
            early += here;
            if (here < 50) ++roundsWithMix;
        }
        mt::Check(early >= 100 * 50 * 80 / 100 && roundsWithMix > 0,
                  "songs not heard for a month mostly come before yesterday's, without a fixed order",
                  "%.1f %% of the first half, %d of 100 rounds mixed", early / 50.0, roundsWithMix);
    }
    {
        // Equal history: every song is about as likely to come first.
        std::vector<ShuffleItem> items(10);
        uint64_t rng = 17;
        std::vector<int> firsts(10, 0);
        for (int round = 0; round < 5000; ++round) ++firsts[static_cast<size_t>(MakeShuffleOrder(items, -1, kNow, &rng)[0])];
        const auto [low, high] = std::minmax_element(firsts.begin(), firsts.end());
        mt::Check(*low >= 400 && *high <= 600, "with equal history every song is about as likely to come first (5000 rounds of 10)",
                  "between %d and %d times, 500 expected", *low, *high);
    }
    {
        // The owner's list is about 2000 songs; a round is made on the player thread.
        std::vector<ShuffleItem> items(2000);
        for (size_t i = 0; i < items.size(); ++i) {
            items[i].artist = L"artist " + std::to_wstring(i % 300);
            items[i].lastPlayed = i % 3 == 0 ? 0 : kNow - static_cast<int64_t>(i) * 3600;
        }
        uint64_t rng = 23;
        const ULONGLONG start = GetTickCount64();
        const std::vector<int> order = MakeShuffleOrder(items, 5, kNow, &rng);
        const ULONGLONG took = GetTickCount64() - start;
        mt::Check(IsPermutation(order, items.size()) && took < 100, "a round of 2000 songs by 300 artists is made in under 0.1 s",
                  "%llu ms", took);
    }
    {
        // Songs added mid-round are mixed into what is still to come; what was played stays.
        std::vector<ShuffleItem> items(20);
        uint64_t rng = 19;
        std::vector<int> order = MakeShuffleOrder(items, 0, kNow, &rng);
        const std::vector<int> before(order.begin(), order.begin() + 8);
        RemixRest(order, 8, items, kNow, &rng);
        mt::Check(IsPermutation(order, 20) && std::equal(before.begin(), before.end(), order.begin()),
                  "mixing in the rest of a round leaves the songs already played where they were");
    }
}
