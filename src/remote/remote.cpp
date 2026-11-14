// Remote control of other music apps through Windows media sessions.
//
// This first version fixes the public interface (remote.h) so the music panel can be written
// against it. Nothing here reads or controls a session yet: the snapshot always says the media
// side is off, and every command is ignored.

#include "remote.h"

namespace remote {

Remote& Remote::Get() {
    static Remote instance;
    return instance;
}

void Remote::Start() {}
void Remote::Shutdown() {}
void Remote::Play() {}
void Remote::Pause() {}
void Remote::TogglePlayPause() {}
void Remote::Stop() {}
void Remote::Next() {}
void Remote::Previous() {}
void Remote::SetRepeat(Repeat) {}
void Remote::SelectApp(const std::wstring&) {}
void Remote::FollowWindows() {}

Snapshot Remote::GetSnapshot() const {
    return Snapshot();
}

void StartTestScriptIfAsked() {}

}  // namespace remote
