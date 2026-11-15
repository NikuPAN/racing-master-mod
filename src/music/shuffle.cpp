#include "shuffle.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace music {

namespace {

// splitmix64: small, fast, good enough to order songs.
uint64_t NextRandom(uint64_t* state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

double Uniform(uint64_t* state) {   // in (0, 1]
    return (static_cast<double>(NextRandom(state) >> 11) + 1.0) / 9007199254740992.0;
}

// How strongly a song is pulled to the front: the days since it was last heard (at least an hour,
// at most 30 days); a song never heard counts as 60 days.
double Weight(const ShuffleItem& item, int64_t now) {
    if (item.lastPlayed <= 0) return 60.0;
    const double days = static_cast<double>(now - item.lastPlayed) / 86400.0;
    return std::min(30.0, std::max(1.0 / 24.0, days));
}

}  // namespace

void RemixRest(std::vector<int>& order, size_t from, const std::vector<ShuffleItem>& items, int64_t now, uint64_t* rng) {
    if (from >= order.size()) return;
    // Weighted random order (Efraimidis-Spirakis): each song draws log(u) / weight and the largest
    // go first, so a song unheard for a month is far more likely to come early than one heard an
    // hour ago, yet every order stays possible.
    std::vector<std::pair<double, int>> keyed;
    keyed.reserve(order.size() - from);
    for (size_t i = from; i < order.size(); ++i) {
        const int song = order[i];
        keyed.emplace_back(std::log(Uniform(rng)) / Weight(items[static_cast<size_t>(song)], now), song);
    }
    std::sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

    // Spread artists. The songs are taken in that weighted order, each time the earliest one
    // whose artist differs from the song before it, except when the artist with the most songs
    // left has more than half of what remains: then that artist must go now, or its songs would
    // be forced together at the end. Where one artist fills more than half the round they cannot
    // all be kept apart. A song without an artist never clashes.
    std::vector<int> rest;
    rest.reserve(keyed.size());
    for (const auto& k : keyed) rest.push_back(k.second);
    std::map<std::wstring, int> left;
    for (const int song : rest) {
        const std::wstring& a = items[static_cast<size_t>(song)].artist;
        if (!a.empty()) ++left[a];
    }
    const std::wstring* previous = from > 0 ? &items[static_cast<size_t>(order[from - 1])].artist : nullptr;
    size_t out = from;
    while (!rest.empty()) {
        const std::wstring* most = nullptr;
        int mostLeft = 0;
        for (const auto& [a, n] : left) {
            if (n > mostLeft) {
                mostLeft = n;
                most = &a;
            }
        }
        const int remaining = static_cast<int>(rest.size());
        auto clashes = [&](int song) {
            const std::wstring& a = items[static_cast<size_t>(song)].artist;
            return previous && !a.empty() && a == *previous;
        };
        size_t pick = rest.size();
        if (most && 2 * mostLeft > remaining && !(previous && *most == *previous)) {
            for (size_t i = 0; i < rest.size(); ++i) {
                if (items[static_cast<size_t>(rest[i])].artist == *most) {
                    pick = i;
                    break;
                }
            }
        }
        if (pick == rest.size()) {
            for (size_t i = 0; i < rest.size(); ++i) {
                if (!clashes(rest[i])) {
                    pick = i;
                    break;
                }
            }
        }
        if (pick == rest.size()) pick = 0;   // only the previous song's artist is left
        const int song = rest[pick];
        rest.erase(rest.begin() + static_cast<std::ptrdiff_t>(pick));
        const std::wstring& a = items[static_cast<size_t>(song)].artist;
        if (!a.empty()) {
            auto it = left.find(a);
            if (--it->second == 0) left.erase(it);
        }
        order[out++] = song;
        previous = &items[static_cast<size_t>(song)].artist;
    }
}

std::vector<int> MakeShuffleOrder(const std::vector<ShuffleItem>& items, int first, int64_t now, uint64_t* rng) {
    std::vector<int> order(items.size());
    for (size_t i = 0; i < items.size(); ++i) order[i] = static_cast<int>(i);
    size_t from = 0;
    if (first >= 0 && first < static_cast<int>(items.size())) {
        std::swap(order[0], order[static_cast<size_t>(first)]);
        from = 1;
    }
    RemixRest(order, from, items, now, rng);
    return order;
}

}  // namespace music
