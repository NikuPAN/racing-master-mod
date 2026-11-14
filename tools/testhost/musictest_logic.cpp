// The playlist state machine (src\music\playlist_logic.h): every row of the transition table in
// docs\agent-notes\music-engine-playlist-table.md, the invariant sweep from the design brief (273
// states x 15 events x 7 arguments) and its named cases.
#include "musictest.h"

#include "music/playlist_logic.h"

#include <cstdio>

using namespace music;

namespace {

PlaylistState Make(int count, int current, State status, LoopMode loop, int failures = 0) {
    PlaylistState s;
    s.count = count;
    s.current = current;
    s.status = status;
    s.loop = loop;
    s.failures = failures;
    return s;
}

struct Expect {
    int current;
    State status;
    Action action;
    int index = -1;    // for Start
    int failures = -1; // -1 = not checked
};

constexpr State St = State::Stopped;
constexpr State Pl = State::Playing;
constexpr State Pa = State::Paused;
constexpr LoopMode Off = LoopMode::Off;
constexpr LoopMode All = LoopMode::All;
constexpr LoopMode One = LoopMode::One;

int g_rowFailures = 0;
int g_rowChecks = 0;

void Row(const char* label, PlaylistState s, Step (*event)(PlaylistState&), Expect e) {
    ++g_rowChecks;
    const Step step = event(s);
    const bool ok = s.current == e.current && s.status == e.status && step.action == e.action &&
                    (e.action != Action::Start || step.index == e.index) && (e.failures < 0 || s.failures == e.failures);
    if (!ok) {
        ++g_rowFailures;
        mt::Fail(label, "got current %d, status %d, action %d, index %d, failures %d", s.current, static_cast<int>(s.status),
                 static_cast<int>(step.action), step.index, s.failures);
    }
}

}  // namespace

MT_TEST(200, LogicTable, "playlist state machine: the 50 rows of the transition table") {
    using A = Action;
    // Transport.
    Row("row 1: Play, empty list", Make(0, -1, St, All), [](PlaylistState& s) { return s.Play(); }, {-1, St, A::None});
    Row("row 2: Play, stopped", Make(3, 1, St, All), [](PlaylistState& s) { return s.Play(); }, {1, Pl, A::Start, 1, 0});
    Row("row 3: Play, paused", Make(3, 1, Pa, All), [](PlaylistState& s) { return s.Play(); }, {1, Pl, A::Resume});
    Row("row 4: Play, playing", Make(3, 1, Pl, All), [](PlaylistState& s) { return s.Play(); }, {1, Pl, A::None});
    Row("row 5: Pause, playing", Make(3, 1, Pl, All), [](PlaylistState& s) { return s.Pause(); }, {1, Pa, A::Pause});
    Row("row 6: Pause, stopped", Make(3, 1, St, All), [](PlaylistState& s) { return s.Pause(); }, {1, St, A::None});
    Row("row 6: Pause, paused", Make(3, 1, Pa, All), [](PlaylistState& s) { return s.Pause(); }, {1, Pa, A::None});
    Row("row 7: Stop, playing", Make(3, 1, Pl, All), [](PlaylistState& s) { return s.Stop(); }, {1, St, A::Stop});
    Row("row 7: Stop, paused", Make(3, 1, Pa, All), [](PlaylistState& s) { return s.Stop(); }, {1, St, A::Stop});
    Row("row 8: Stop, stopped", Make(3, 1, St, All), [](PlaylistState& s) { return s.Stop(); }, {1, St, A::None});
    Row("row 9: Next, empty list", Make(0, -1, St, All), [](PlaylistState& s) { return s.Next(); }, {-1, St, A::None});
    Row("row 10: Next, not the last track", Make(3, 1, Pl, Off), [](PlaylistState& s) { return s.Next(); }, {2, Pl, A::Start, 2, 0});
    Row("row 10: Next from stopped plays", Make(3, 1, St, Off), [](PlaylistState& s) { return s.Next(); }, {2, Pl, A::Start, 2});
    Row("row 11: Next on the last track, loop all", Make(3, 2, Pl, All), [](PlaylistState& s) { return s.Next(); }, {0, Pl, A::Start, 0});
    Row("row 11: Next on the last track, loop one", Make(3, 2, Pl, One), [](PlaylistState& s) { return s.Next(); }, {0, Pl, A::Start, 0});
    Row("row 12: Next on the last track, loop off, playing", Make(3, 2, Pl, Off), [](PlaylistState& s) { return s.Next(); }, {0, St, A::Stop});
    Row("row 12: Next on the last track, loop off, paused", Make(3, 2, Pa, Off), [](PlaylistState& s) { return s.Next(); }, {0, St, A::Stop});
    Row("row 13: Next on the last track, loop off, stopped", Make(3, 2, St, Off), [](PlaylistState& s) { return s.Next(); }, {0, St, A::None});
    Row("row 14: Previous, empty list", Make(0, -1, St, All), [](PlaylistState& s) { return s.Previous(true); }, {-1, St, A::None});
    Row("row 15: Previous after 3 s restarts the track", Make(3, 1, Pl, All), [](PlaylistState& s) { return s.Previous(true); }, {1, Pl, A::Start, 1});
    Row("row 15: Previous after 3 s while paused", Make(3, 1, Pa, All), [](PlaylistState& s) { return s.Previous(true); }, {1, Pl, A::Start, 1});
    Row("row 16: Previous goes back one", Make(3, 1, Pl, All), [](PlaylistState& s) { return s.Previous(false); }, {0, Pl, A::Start, 0});
    Row("row 16: Previous while stopped goes back one", Make(3, 1, St, All), [](PlaylistState& s) { return s.Previous(true); }, {0, Pl, A::Start, 0});
    Row("row 17: Previous on the first track, loop off", Make(3, 0, Pl, Off), [](PlaylistState& s) { return s.Previous(false); }, {0, Pl, A::Start, 0});
    Row("row 18: Previous on the first track, loop all", Make(3, 0, Pl, All), [](PlaylistState& s) { return s.Previous(false); }, {2, Pl, A::Start, 2});
    Row("row 18: Previous on the first track, loop one", Make(3, 0, St, One), [](PlaylistState& s) { return s.Previous(false); }, {2, Pl, A::Start, 2});
    Row("row 19: play a chosen track", Make(3, 0, St, All, 2), [](PlaylistState& s) { return s.PlayIndex(2); }, {2, Pl, A::Start, 2, 0});
    Row("row 19: choosing the playing track restarts it", Make(3, 2, Pl, All), [](PlaylistState& s) { return s.PlayIndex(2); }, {2, Pl, A::Start, 2});
    Row("row 20: play a track outside the list", Make(3, 1, Pa, All), [](PlaylistState& s) { return s.PlayIndex(3); }, {1, Pa, A::None});
    Row("row 20: play track -1", Make(3, 1, St, All), [](PlaylistState& s) { return s.PlayIndex(-1); }, {1, St, A::None});
    Row("row 21: Seek, playing", Make(3, 1, Pl, All), [](PlaylistState& s) { return s.Seek(); }, {1, Pl, A::Seek});
    Row("row 21: Seek, paused", Make(3, 1, Pa, All), [](PlaylistState& s) { return s.Seek(); }, {1, Pa, A::Seek});
    Row("row 22: Seek, stopped", Make(3, 1, St, All), [](PlaylistState& s) { return s.Seek(); }, {1, St, A::None});
    Row("row 23: Set loop", Make(3, 1, Pl, All), [](PlaylistState& s) { return s.SetLoop(LoopMode::Off); }, {1, Pl, A::None});
    // Reports from the engine.
    Row("row 24: track ended while paused (stale)", Make(3, 1, Pa, All), [](PlaylistState& s) { return s.TrackEnded(); }, {1, Pa, A::None});
    Row("row 24: track ended while stopped (stale)", Make(3, 1, St, All), [](PlaylistState& s) { return s.TrackEnded(); }, {1, St, A::None});
    Row("row 25: track ended, loop one", Make(3, 1, Pl, One), [](PlaylistState& s) { return s.TrackEnded(); }, {1, Pl, A::Start, 1});
    Row("row 26: track ended, more follow", Make(3, 1, Pl, Off), [](PlaylistState& s) { return s.TrackEnded(); }, {2, Pl, A::Start, 2});
    Row("row 27: last track ended, loop all", Make(3, 2, Pl, All), [](PlaylistState& s) { return s.TrackEnded(); }, {0, Pl, A::Start, 0});
    Row("row 27: only track ended, loop all, repeats", Make(1, 0, Pl, All), [](PlaylistState& s) { return s.TrackEnded(); }, {0, Pl, A::Start, 0});
    Row("row 28: last track ended, loop off", Make(3, 2, Pl, Off), [](PlaylistState& s) { return s.TrackEnded(); }, {0, St, A::Stop});
    Row("row 29: track started clears failures", Make(3, 1, Pl, All, 2), [](PlaylistState& s) { return s.TrackStarted(); }, {1, Pl, A::None, -1, 0});
    Row("row 30: track failed while stopped (stale)", Make(3, 1, St, All), [](PlaylistState& s) { return s.TrackFailed(); }, {1, St, A::None});
    Row("row 31: every track failed in a row", Make(3, 1, Pl, All, 2), [](PlaylistState& s) { return s.TrackFailed(); }, {1, St, A::StopAllFailed, -1, 0});
    Row("row 32: failed, move to the next", Make(3, 1, Pl, All), [](PlaylistState& s) { return s.TrackFailed(); }, {2, Pl, A::Start, 2, 1});
    Row("row 32: failed while paused, move to the next", Make(3, 1, Pa, Off), [](PlaylistState& s) { return s.TrackFailed(); }, {2, Pl, A::Start, 2, 1});
    Row("row 33: last failed, loop all, wrap", Make(3, 2, Pl, All), [](PlaylistState& s) { return s.TrackFailed(); }, {0, Pl, A::Start, 0, 1});
    Row("row 33: last failed, loop one, wrap (no retry)", Make(3, 2, Pl, One), [](PlaylistState& s) { return s.TrackFailed(); }, {0, Pl, A::Start, 0, 1});
    Row("row 34: last failed, loop off, stop at the top", Make(3, 2, Pl, Off), [](PlaylistState& s) { return s.TrackFailed(); }, {0, St, A::Stop});
    // List edits.
    Row("row 35: tracks added to an empty list are cued", Make(0, -1, St, All), [](PlaylistState& s) { return s.Inserted(0, 2); }, {0, St, A::None});
    Row("row 36: inserted above the current track", Make(3, 1, Pl, All), [](PlaylistState& s) { return s.Inserted(1, 2); }, {3, Pl, A::None});
    Row("row 37: inserted below the current track", Make(3, 1, Pl, All), [](PlaylistState& s) { return s.Inserted(2, 2); }, {1, Pl, A::None});
    Row("row 38: removed above the current track", Make(3, 2, Pl, All, 1), [](PlaylistState& s) { return s.Removed(0); }, {1, Pl, A::None, -1, 0});
    Row("row 39: removed below the current track", Make(3, 0, Pl, All, 1), [](PlaylistState& s) { return s.Removed(2); }, {0, Pl, A::None, -1, 0});
    Row("row 40: removed the only track while playing", Make(1, 0, Pl, All), [](PlaylistState& s) { return s.Removed(0); }, {-1, St, A::Stop});
    Row("row 40: removed the only track while stopped", Make(1, 0, St, All), [](PlaylistState& s) { return s.Removed(0); }, {-1, St, A::None});
    Row("row 41: removed the cued track while stopped", Make(3, 1, St, All), [](PlaylistState& s) { return s.Removed(1); }, {1, St, A::None});
    Row("row 41: removed the cued last track while stopped", Make(3, 2, St, All), [](PlaylistState& s) { return s.Removed(2); }, {0, St, A::None});
    Row("row 42: removed the paused track", Make(3, 1, Pa, All), [](PlaylistState& s) { return s.Removed(1); }, {1, St, A::Stop});
    Row("row 43: removed the playing track, the next starts", Make(3, 1, Pl, All), [](PlaylistState& s) { return s.Removed(1); }, {1, Pl, A::Start, 1});
    Row("row 44: removed the playing last track, loop all", Make(3, 2, Pl, All), [](PlaylistState& s) { return s.Removed(2); }, {0, Pl, A::Start, 0});
    Row("row 44: removed the playing last track, loop one", Make(3, 2, Pl, One), [](PlaylistState& s) { return s.Removed(2); }, {0, Pl, A::Start, 0});
    Row("row 45: removed the playing last track, loop off", Make(3, 2, Pl, Off), [](PlaylistState& s) { return s.Removed(2); }, {0, St, A::Stop});
    Row("row 46: moved the current track", Make(4, 1, Pl, All), [](PlaylistState& s) { return s.Moved(1, 3); }, {3, Pl, A::None});
    Row("row 47: moved from above to below", Make(4, 2, Pl, All), [](PlaylistState& s) { return s.Moved(0, 2); }, {1, Pl, A::None});
    Row("row 48: moved from below to above", Make(4, 1, Pl, All), [](PlaylistState& s) { return s.Moved(3, 0); }, {2, Pl, A::None});
    Row("row 49: moved elsewhere", Make(4, 1, Pl, All), [](PlaylistState& s) { return s.Moved(2, 3); }, {1, Pl, A::None});
    Row("row 50: cleared while playing", Make(3, 1, Pl, All), [](PlaylistState& s) { return s.Cleared(); }, {-1, St, A::Stop});
    Row("row 50: cleared while stopped", Make(3, 1, St, All), [](PlaylistState& s) { return s.Cleared(); }, {-1, St, A::None});
    char name[96];
    snprintf(name, sizeof(name), "state machine table: all %d row cases agree with the logic", g_rowChecks);
    mt::Check(g_rowFailures == 0, name, "%d of them disagreed (listed above)", g_rowFailures);
}

namespace {

bool Invariant(const PlaylistState& s) {
    if (s.count < 0) return false;
    if (s.count == 0) return s.current == -1 && s.status == State::Stopped;
    return s.current >= 0 && s.current < s.count && s.failures >= 0 && s.failures < s.count;
}

Step ApplyEvent(PlaylistState& s, int e, int a) {
    switch (e) {
        case 0: return s.Play();
        case 1: return s.Pause();
        case 2: return s.Stop();
        case 3: return s.Next();
        case 4: return s.Previous(a % 2 == 0);
        case 5: return s.PlayIndex(a - 1);  // includes -1 and count (out of range)
        case 6: return s.Seek();
        case 7: return s.SetLoop(static_cast<LoopMode>(a % 3));
        case 8: return s.TrackEnded();
        case 9: return s.TrackStarted();
        case 10: return s.TrackFailed();
        case 11: return s.Inserted(a, 1 + a % 2);
        case 12: return s.Removed(a - 1);
        case 13: return s.Moved(a, (a * 2 + 1) % 5);
        default: return s.Cleared();
    }
}

}  // namespace

MT_TEST(210, LogicSweep, "playlist state machine: invariant sweep") {
    const State statuses[] = {St, Pl, Pa};
    const LoopMode loops[] = {Off, All, One};
    int checks = 0;
    int failures = 0;
    char first[200] = "";
    auto check = [&](bool ok, const char* what, int e, int a, int count, int current, State status, LoopMode loop) {
        ++checks;
        if (ok) return;
        if (failures++ == 0) {
            snprintf(first, sizeof(first), "%s (event %d, argument %d, from %d tracks, current %d, status %d, loop %d)", what, e, a,
                     count, current, static_cast<int>(status), static_cast<int>(loop));
        }
    };
    int states = 0;
    for (int count = 0; count <= 4; ++count) {
        for (int current = (count == 0 ? -1 : 0); current < (count == 0 ? 0 : count); ++current) {
            for (const State status : statuses) {
                if (count == 0 && status != St) continue;
                for (const LoopMode loop : loops) {
                    for (int failed = 0; failed < (count == 0 ? 1 : count); ++failed) {
                        ++states;
                        for (int e = 0; e < 15; ++e) {
                            for (int a = 0; a <= 6; ++a) {
                                PlaylistState s = Make(count, current, status, loop, failed);
                                const Step step = ApplyEvent(s, e, a);
                                check(Invariant(s), "invariant broken", e, a, count, current, status, loop);
                                if (step.action == Action::Start) {
                                    check(step.index == s.current && s.status == Pl, "Start must point at the current track and play", e, a,
                                          count, current, status, loop);
                                }
                                if (step.action == Action::Stop || step.action == Action::StopAllFailed) {
                                    check(s.status == St, "Stop must leave the status Stopped", e, a, count, current, status, loop);
                                }
                                if (status != St && s.status == St) {
                                    check(step.action == Action::Stop || step.action == Action::StopAllFailed,
                                          "leaving Playing or Paused for Stopped must tell the engine to stop", e, a, count, current,
                                          status, loop);
                                }
                                if (status == St && s.status == Pl) {
                                    check(step.action == Action::Start, "entering Playing from Stopped must start a track", e, a, count,
                                          current, status, loop);
                                }
                                const int peek = s.PeekNextOnEnd();
                                check(peek >= -1 && peek < s.count, "PeekNextOnEnd out of range", e, a, count, current, status, loop);
                                if (s.status == Pl) {
                                    PlaylistState t = s;
                                    const Step ended = t.TrackEnded();
                                    check((peek == -1 && ended.action == Action::Stop) ||
                                              (peek >= 0 && ended.action == Action::Start && ended.index == peek),
                                          "PeekNextOnEnd must agree with TrackEnded", e, a, count, current, status, loop);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    char name[128];
    snprintf(name, sizeof(name), "sweep: %d states x 15 events x 7 arguments, %d checks hold", states, checks);
    mt::Check(failures == 0, name, "%d failed; first: %s", failures, first);
}

MT_TEST(220, LogicNamed, "playlist state machine: named cases from the brief") {
    {
        PlaylistState s = Make(3, 0, St, All);
        s.Play();
        mt::Check(s.TrackFailed().index == 1 && s.TrackFailed().index == 2 && s.TrackFailed().action == Action::StopAllFailed &&
                      s.status == St,
                  "a list of broken files stops after one pass instead of spinning");
    }
    {
        PlaylistState s = Make(3, 0, Pl, All);
        s.TrackFailed();   // 0 broken -> 1
        s.TrackStarted();  // 1 plays
        s.TrackEnded();    // -> 2
        s.TrackEnded();    // -> 0 (broken again)
        mt::Check(s.TrackFailed().index == 1, "one broken track in a looping list is skipped on every lap");
    }
    {
        PlaylistState s = Make(3, 1, Pl, All);
        s.Inserted(0, 2);
        s.Inserted(5, 1);
        s.Moved(3, 0);
        s.Moved(4, 0);
        s.Moved(0, 5);
        mt::Check(s.current == 0 && s.count == 6, "inserts and moves keep pointing at the same track");
    }
    {
        PlaylistState s = Make(3, 1, St, All);
        const bool ok = s.Play().index == 1 && s.Play().action == Action::None && s.Pause().action == Action::Pause &&
                        s.Play().action == Action::Resume && s.Stop().action == Action::Stop && s.current == 1;
        mt::Check(ok, "play, pause, resume and stop keep the track cued");
    }
    {
        // Removing a non-current track used to leave failures == count (the brief's sweep finding).
        PlaylistState s = Make(3, 0, Pl, All, 2);
        s.Removed(2);
        mt::Check(s.failures < s.count, "removing a track resets the failure count (the bug the brief's sweep found)");
    }
}
