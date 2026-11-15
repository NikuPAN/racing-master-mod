// Pure playlist logic: no I/O, no threads, no audio.
// Invariants: count == 0  <=>  current == -1;  count > 0  =>  0 <= current < count;  count == 0  =>  status == Stopped.
// With shuffle on (0.3.3): `order` is a permutation of 0..count-1 (the order this round plays in)
// and, when count > 0, order[pos] == current.
//
// This is the validated reference logic from the engine design brief (sweep of 273 states x 15
// events x 7 arguments plus named cases). The transition table it encodes is in
// docs/agent-notes/music-engine-playlist-table.md; every row is a case in
// tools/testhost/musictest_logic.cpp. Do not add I/O here.
//
// Shuffle (the owner, 2026-11-15): every song once per round, in an order the engine makes
// (shuffle.h: songs not heard lately first, the same artist never back-to-back). This logic only
// walks that order; it never invents one beyond a plain rotation as a stand-in. When a round
// wraps or ends it sets `roundEnded`, and the engine hands it a new order that starts with the
// current song (SetShuffleOrder).
#pragma once

#include "player.h"

#include <algorithm>
#include <vector>

namespace music {

// What the audio engine must do after a transition.
enum class Action {
    None,
    Start,          // open track `index` at 0:00 and play it
    Resume,         // continue the paused track
    Pause,          // fade out and hold position
    Stop,           // fade out, close the track, position 0:00
    StopAllFailed,  // like Stop, and tell the user that no track in the list could be played
    Seek,           // move within the current track
};

struct Step {
    Action action = Action::None;
    int index = -1;  // valid for Start
};

struct PlaylistState {
    int count = 0;
    int current = -1;
    State status = State::Stopped;
    LoopMode loop = LoopMode::All;
    int failures = 0;  // tracks that failed in a row since the last one that played
    bool shuffle = false;
    std::vector<int> order;   // shuffle on: the play order of this round (indices into the list)
    int pos = -1;             // shuffle on: where `current` is in `order` (-1 when the list is empty)
    bool roundEnded = false;  // shuffle on: the round wrapped or ended; the engine makes a new order

    // ---- shuffle ----
    // Switching on keeps the current song and plays the others after it (a rotation until the
    // engine sets a real order); switching off continues the list in order from the current song.
    Step SetShuffle(bool on) {
        shuffle = on;
        order.clear();
        pos = -1;
        roundEnded = false;
        if (!on || count == 0) return {};
        for (int i = 0; i < count; ++i) order.push_back((current + i) % count);
        pos = 0;
        return {};
    }
    // A new play order (a permutation of 0..count-1). The current song keeps playing; the round
    // goes on from where it sits in the new order. Ignored when it is not a permutation.
    void SetShuffleOrder(const std::vector<int>& newOrder) {
        if (!shuffle || static_cast<int>(newOrder.size()) != count) return;
        std::vector<char> seen(static_cast<size_t>(count), 0);
        for (const int i : newOrder) {
            if (i < 0 || i >= count || seen[static_cast<size_t>(i)]) return;
            seen[static_cast<size_t>(i)] = 1;
        }
        order = newOrder;
        pos = count == 0 ? -1 : static_cast<int>(std::find(order.begin(), order.end(), current) - order.begin());
        roundEnded = false;
    }
    // The engine moved on by itself to `index` (a gapless change-over decided on a copy of this
    // state); keeps `pos` in step.
    void Arrived(int index) {
        if (index < 0 || index >= count) return;
        failures = 0;
        if (index == current) return;   // the same song again (repeat one)
        current = index;
        if (!shuffle) return;
        if (pos + 1 < count && order[static_cast<size_t>(pos + 1)] == index) {
            ++pos;
            return;
        }
        const int at = static_cast<int>(std::find(order.begin(), order.end(), index) - order.begin());
        if (at <= pos) roundEnded = true;   // it went round
        pos = at;
    }

    // ---- transport ----
    Step Play() {
        if (count == 0 || status == State::Playing) return {};
        if (status == State::Paused) {
            status = State::Playing;
            return {Action::Resume, -1};
        }
        failures = 0;
        return StartAt(current);
    }
    Step Pause() {
        if (status != State::Playing) return {};
        status = State::Paused;
        return {Action::Pause, -1};
    }
    Step Stop() {
        if (status == State::Stopped) return {};
        status = State::Stopped;
        return {Action::Stop, -1};
    }
    Step Next() {
        if (count == 0) return {};
        failures = 0;
        return Forward();
    }
    // `pastRestartPoint`: the current track has played for more than 3 seconds.
    Step Previous(bool pastRestartPoint) {
        if (count == 0) return {};
        failures = 0;
        if (status != State::Stopped && pastRestartPoint) return StartAt(current);
        if (shuffle) {
            // Back through this round's order, the songs as they were heard.
            if (pos > 0) return StartAtPos(pos - 1);
            return StartAtPos(loop == LoopMode::Off ? 0 : count - 1);
        }
        if (current > 0) return StartAt(current - 1);
        return StartAt(loop == LoopMode::Off ? 0 : count - 1);
    }
    Step PlayIndex(int index) {
        if (index < 0 || index >= count) return {};
        failures = 0;
        if (shuffle && index != current) {
            // The chosen song plays next in the round; the songs still to come keep their order.
            const int at = static_cast<int>(std::find(order.begin(), order.end(), index) - order.begin());
            order.erase(order.begin() + at);
            if (at < pos) --pos;
            order.insert(order.begin() + pos + 1, index);
            return StartAtPos(pos + 1);
        }
        return StartAt(index);
    }
    Step Seek() { return status == State::Stopped ? Step{} : Step{Action::Seek, -1}; }
    Step SetLoop(LoopMode mode) {
        loop = mode;
        return {};
    }

    // ---- reports from the audio engine ----
    // The current track reached its natural end.
    Step TrackEnded() {
        if (status != State::Playing) return {};
        if (loop == LoopMode::One) return StartAt(current);
        return Forward();
    }
    // The current track produced sound.
    Step TrackStarted() {
        failures = 0;
        return {};
    }
    // The current track could not be opened or broke while decoding.
    Step TrackFailed() {
        if (status == State::Stopped) return {};
        if (++failures >= count) {
            failures = 0;
            status = State::Stopped;
            return {Action::StopAllFailed, -1};
        }
        return Forward();  // loop one must not retry the broken track
    }

    // ---- list edits ----
    Step Inserted(int at, int howMany) {
        if (howMany <= 0 || at < 0 || at > count) return {};
        if (shuffle) {
            // Indices move up behind the insert; the new songs join the songs still to come (at the
            // end of the round; the engine mixes them in).
            for (int& i : order) {
                if (i >= at) i += howMany;
            }
            for (int i = 0; i < howMany; ++i) order.push_back(at + i);
            if (count == 0) pos = 0;
        }
        if (count == 0) current = 0;
        else if (at <= current) current += howMany;
        count += howMany;
        if (shuffle && count > 0 && pos >= 0 && order[static_cast<size_t>(pos)] != current) {
            pos = static_cast<int>(std::find(order.begin(), order.end(), current) - order.begin());
        }
        return {};
    }
    Step Removed(int index) {
        if (index < 0 || index >= count) return {};
        int removedAt = -1;
        if (shuffle) {
            removedAt = static_cast<int>(std::find(order.begin(), order.end(), index) - order.begin());
            order.erase(order.begin() + removedAt);
            for (int& i : order) {
                if (i > index) --i;
            }
        }
        --count;
        failures = 0;  // the list changed, so "every track failed" has to be counted afresh
        if (index != current) {
            if (index < current) --current;
            if (shuffle && removedAt < pos) --pos;
            return {};
        }
        // The current track itself was removed.
        if (count == 0) {
            current = -1;
            pos = -1;
            const bool wasStopped = status == State::Stopped;
            status = State::Stopped;
            return wasStopped ? Step{} : Step{Action::Stop, -1};
        }
        bool wrapped = false;
        if (shuffle) {
            // The song that came next in the round now sits at the same place in the order.
            wrapped = pos >= count;
            if (wrapped) {
                pos = 0;
                roundEnded = true;
            }
            current = order[static_cast<size_t>(pos)];
        } else {
            wrapped = index >= count;       // it was the last entry
            current = wrapped ? 0 : index;  // the track that followed it now sits at the same index
        }
        if (status == State::Stopped) return {};
        if (status == State::Paused || (wrapped && loop == LoopMode::Off)) {
            status = State::Stopped;
            return {Action::Stop, -1};
        }
        return StartAt(current);
    }
    // The entry at `from` now sits at `to`.
    Step Moved(int from, int to) {
        if (from < 0 || from >= count || to < 0 || to >= count || from == to) return {};
        if (shuffle) {
            for (int& i : order) {
                if (i == from) i = to;
                else if (from < to && i > from && i <= to) --i;
                else if (from > to && i >= to && i < from) ++i;
            }
        }
        if (from == current) current = to;
        else if (from < current && to >= current) --current;
        else if (from > current && to <= current) ++current;
        return {};
    }
    Step Cleared() {
        const bool wasStopped = status == State::Stopped;
        count = 0;
        current = -1;
        failures = 0;
        order.clear();
        pos = -1;
        roundEnded = false;
        status = State::Stopped;
        return wasStopped ? Step{} : Step{Action::Stop, -1};
    }

    // Which track follows the current one when it ends by itself; -1 = playback stops. Used to open the next
    // track early so the change-over has no gap.
    int PeekNextOnEnd() const {
        if (status != State::Playing || count == 0) return -1;
        if (loop == LoopMode::One) return current;
        if (shuffle) {
            if (pos + 1 < count) return order[static_cast<size_t>(pos + 1)];
            return loop == LoopMode::All ? order[0] : -1;
        }
        if (current + 1 < count) return current + 1;
        return loop == LoopMode::All ? 0 : -1;
    }

private:
    Step StartAt(int index) {
        current = index;
        status = State::Playing;
        return {Action::Start, index};
    }
    Step StartAtPos(int p) {
        pos = p;
        return StartAt(order[static_cast<size_t>(p)]);
    }
    Step Forward() {
        if (shuffle) {
            if (pos + 1 < count) return StartAtPos(pos + 1);
            roundEnded = true;   // every song of this round has had its turn
            if (loop != LoopMode::Off) return StartAtPos(0);
            pos = 0;
            current = order[0];
            if (status == State::Stopped) return {};
            status = State::Stopped;
            return {Action::Stop, -1};
        }
        if (current + 1 < count) return StartAt(current + 1);
        if (loop != LoopMode::Off) return StartAt(0);
        // End of the list with looping off: rewind to the top and stop.
        current = 0;
        if (status == State::Stopped) return {};
        status = State::Stopped;
        return {Action::Stop, -1};
    }
};

}  // namespace music
