// Pure playlist logic: no I/O, no threads, no audio. Shuffle is always off.
// Invariants: count == 0  <=>  current == -1;  count > 0  =>  0 <= current < count;  count == 0  =>  status == Stopped.
//
// This is the validated reference logic from the engine design brief (sweep of 273 states x 15
// events x 7 arguments plus named cases). The transition table it encodes is in
// docs/agent-notes/music-engine-playlist-table.md; every row is a case in
// tools/testhost/musictest_logic.cpp. Do not add I/O here.
#pragma once

#include "player.h"

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
        if (current > 0) return StartAt(current - 1);
        return StartAt(loop == LoopMode::Off ? 0 : count - 1);
    }
    Step PlayIndex(int index) {
        if (index < 0 || index >= count) return {};
        failures = 0;
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
        if (count == 0) current = 0;
        else if (at <= current) current += howMany;
        count += howMany;
        return {};
    }
    Step Removed(int index) {
        if (index < 0 || index >= count) return {};
        --count;
        failures = 0;  // the list changed, so "every track failed" has to be counted afresh
        if (index < current) {
            --current;
            return {};
        }
        if (index > current) return {};
        // The current track itself was removed.
        if (count == 0) {
            current = -1;
            const bool wasStopped = status == State::Stopped;
            status = State::Stopped;
            return wasStopped ? Step{} : Step{Action::Stop, -1};
        }
        const bool wrapped = index >= count;  // it was the last entry
        current = wrapped ? 0 : index;        // the track that followed it now sits at the same index
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
        status = State::Stopped;
        return wasStopped ? Step{} : Step{Action::Stop, -1};
    }

    // Which track follows the current one when it ends by itself; -1 = playback stops. Used to open the next
    // track early so the change-over has no gap.
    int PeekNextOnEnd() const {
        if (status != State::Playing || count == 0) return -1;
        if (loop == LoopMode::One) return current;
        if (current + 1 < count) return current + 1;
        return loop == LoopMode::All ? 0 : -1;
    }

private:
    Step StartAt(int index) {
        current = index;
        status = State::Playing;
        return {Action::Start, index};
    }
    Step Forward() {
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
