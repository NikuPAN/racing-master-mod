#pragma once

// How shuffle orders a round (0.3.3). Pure: no I/O, no clock, no global state; the caller passes
// the time and the random state, so tests are exact.
//
// The owner (2026-11-15) chose "fair shuffle, not heard lately first":
//   - every song once per round (the order is a permutation; playlist_logic.h walks it);
//   - songs not heard for a long time, and songs never heard, tend to come first;
//   - the same artist is never back-to-back where the list allows it.
// Spotify's Smart Shuffle also adds songs the listener does not own; that needs Spotify's
// catalogue and the internet, and this mod never goes online.

#include <cstdint>
#include <string>
#include <vector>

namespace music {

struct ShuffleItem {
    std::wstring artist;      // compared as given; the caller folds case and fills a stand-in
    int64_t lastPlayed = 0;   // Unix seconds when it last counted as heard; 0 = never
};

// A play order for a round: a permutation of 0..items.size()-1, starting with `first` when it is
// a valid index. `now` is Unix seconds; `rng` is a random state the call moves on.
std::vector<int> MakeShuffleOrder(const std::vector<ShuffleItem>& items, int first, int64_t now, uint64_t* rng);

// Mixes order[from..] again the same way (songs added mid-round), leaving order[0..from) alone.
void RemixRest(std::vector<int>& order, size_t from, const std::vector<ShuffleItem>& items, int64_t now, uint64_t* rng);

}  // namespace music
