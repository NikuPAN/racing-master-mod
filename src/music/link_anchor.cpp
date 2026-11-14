// Keeps the music engine inside version.dll until the panel calls it.
//
// Nothing in the loader calls the engine yet (integration comes with the panel). Without a
// reference the linker's /OPT:REF would drop every music function, and the import gate in
// build.ps1 would pass without ever seeing the engine's DLL imports (Media Foundation, COM, the
// shell). build.ps1 links with /INCLUDE:MusicEngineLinkAnchor, which keeps this function and so
// everything it names. The function is never called. Delete this file, its line in build.ps1 and
// the /INCLUDE option once the panel calls the engine from the worker.
#include "player.h"

extern "C" void MusicEngineLinkAnchor() {
    music::Player& player = music::Player::Get();
    player.Start();
    player.Play();
    player.Pause();
    player.TogglePlayPause();
    player.Stop();
    player.Next();
    player.Previous();
    player.PlayIndex(0);
    player.Seek(0);
    player.SetVolume(0);
    player.SetLoop(music::LoopMode::Off);
    player.SetPauseWhenGameUnfocused(false);
    player.OnGameFocusChanged(true);
    player.AddFiles({});
    player.AddFolder(L"");
    player.Remove(0);
    player.Clear();
    player.RescanMusicFolder();
    music::Snapshot snapshot = player.GetSnapshot();
    player.GetSnapshotInto(snapshot);
    (void)player.MusicFolder();
    player.Shutdown();
}
