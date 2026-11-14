#pragma once

// Every word the panel shows, in one table per language. UTF-8; the files that include this
// header are compiled with /utf-8.
//
// English: written for someone who has never seen the mod. Sentence case, no internal names,
// no key codes. Where a line takes a value, the placeholder is a printf "%s" or "%d" and the
// comment says what goes in.
//
// Traditional Chinese: *** DRAFT ONLY ***. A first pass so the layout can be checked with
// Chinese text. The owner, a native speaker, rewrites this column before release; do not polish
// it here and do not ship it as final wording.

enum class Language { English = 0, ChineseTraditional = 1 };

struct Strings {
    // Row 1: source and what is playing
    const char* sourceMyFiles;
    const char* sourceMyFilesHint;
    const char* sourceOtherApp;
    const char* sourceOtherAppHint;
    const char* nothingPlaying;          // title line when a list exists but nothing is loaded
    const char* noSongsYet;              // title line when the list is empty
    const char* unknownTitle;            // a song or app without a title
    const char* settings;
    const char* settingsHint;
    const char* hide;
    const char* hideHint;                // %s = the key that shows the panel again

    // Row 2: buttons (icons; these words are their hover text)
    const char* previous;
    const char* play;
    const char* pause;
    const char* stop;
    const char* next;
    const char* loopOff;
    const char* loopOne;
    const char* loopAll;
    const char* loopUnknown;             // another app that does not say
    const char* loopHint;
    const char* loopUnknownHint;
    const char* volumeWord;              // in front of the volume slider (the slider shows the percentage)
    const char* volumeHint;

    // Row 3: time and list
    const char* seekHint;
    const char* showList;
    const char* hideList;
    const char* listHint;
    const char* noTimeline;              // another app that does not report a position

    // List
    const char* addFiles;
    const char* addFilesHint;
    const char* addFolder;
    const char* addFolderHint;
    const char* songCountOne;            // "1 song"
    const char* songCountMany;           // %d = number of songs
    const char* listEmpty;
    const char* playThisHint;
    const char* cannotPlay;              // %s = the reason the player gave
    const char* cannotPlayUnknown;       // when the player gave no reason
    const char* remove;                  // the small button at the end of each row
    const char* removeHint;

    // Why a control is greyed out (shown when the pointer rests on it)
    const char* reasonNoSongs;
    const char* reasonNothingLoaded;
    const char* reasonAppNoControl;      // %s = the app's name
    const char* reasonAppNoControlNoName;
    const char* reasonNoApp;
    const char* reasonAppVolume;
    const char* reasonListOtherApp;
    const char* reasonNoLength;          // a song whose length is not known cannot be moved through
    const char* reasonAlreadyStopped;
    const char* reasonDisplayOnly;       // [panel] mode=display

    // "Other app"
    const char* noAppPlaying;            // row 1 when no app is playing anything
    const char* noAppHelp;               // a second line under the bar

    // Settings row
    const char* language;
    const char* languageFollowWindows;
    const char* textSize;
    const char* textSmall;
    const char* textMedium;
    const char* textLarge;
    const char* pauseWhenUnfocused;
    const char* pauseWhenUnfocusedHint;
    const char* keysLine;                // six %s: panel, stop, previous, play or pause, next, repeat
    const char* keyNone;                 // an action without a key

    // Short notices in the corner
    const char* showPanelNotice;         // %s = the key that shows the panel
    const char* testBuildNotice;         // shown only by a build with the example player
};

inline const Strings kStringsEnglish = {
    "My files",
    "Play songs from your own computer.",
    "Other app",
    "Control what Spotify, a web browser or another music player is playing.",
    "Nothing is playing",
    "No songs yet",
    "Unknown title",
    "Settings",
    "Language, text size and when to pause.",
    "Hide",
    "Hide this panel. Press %s to show it again.",

    "Previous song",
    "Play",
    "Pause",
    "Stop",
    "Next song",
    "Repeat: off",
    "Repeat: this song",
    "Repeat: all",
    "Repeat: unknown",
    "Click to change what repeats: nothing, this song, or the whole list.",
    "This app does not say what repeats. Click to change it.",
    "Volume",
    "Drag to make the music louder or quieter.",

    "Drag to move to another point in the song.",
    "Show list",
    "Hide list",
    "Show or hide your songs.",
    "This app does not show how far into the song it is.",

    "Add files",
    "Choose songs on your computer to add to the list.",
    "Add folder",
    "Add every song in a folder.",
    "1 song",
    "%d songs",
    "No songs yet. Use Add files or Add folder to add some.",
    "Click to play this song.",
    "This file cannot be played: %s",
    "This file cannot be played.",
    "Remove",
    "Take this song off the list. The file stays on your computer.",

    "Add songs first, with Add files or Add folder.",
    "Choose a song in the list first.",
    "%s does not allow this from here.",
    "This app does not allow this from here.",
    "No other app is playing anything right now.",
    "Change this app's volume in the app itself.",
    "The list holds your own songs. Choose My files to see it.",
    "The length of this song is not known, so you cannot move through it.",
    "The music is already stopped.",
    "This is a display test: the panel cannot be used until mode=on in the settings file.",

    "No other app is playing anything",
    "Start music in Spotify, a web browser or another player and it shows here.",

    "Language",
    "Same as Windows",
    "Text size",
    "Small",
    "Medium",
    "Large",
    "Pause music when the game is not in focus",
    "Your own songs pause when you switch to another window and carry on when you come back to the game. "
    "Music in another app is not paused.",
    "Keys while racing: %s shows or hides this panel, %s stops, %s plays the previous song, %s plays or pauses, "
    "%s plays the next song, %s changes what repeats.",
    "no key",

    "Music panel: press %s to show it.",
    "Test build: these songs are examples and no sound is played.",
};

// *** DRAFT *** See the note at the top of this file. The owner rewrites every line below.
inline const Strings kStringsChineseTraditionalDraft = {
    "我的檔案",
    "播放你電腦裡的歌曲。",
    "其他程式",
    "控制 Spotify、瀏覽器或其他播放器正在播放的音樂。",
    "目前沒有播放",
    "還沒有歌曲",
    "未知標題",
    "設定",
    "語言、文字大小和何時暫停。",
    "隱藏",
    "隱藏這個面板。按 %s 可以再顯示。",

    "上一首",
    "播放",
    "暫停",
    "停止",
    "下一首",
    "重複：關",
    "重複：這一首",
    "重複：全部",
    "重複：不明",
    "按一下切換重複方式：不重複、這一首、整個清單。",
    "這個程式沒有提供重複方式。按一下可以切換。",
    "音量",
    "拖曳可以調整音量。",

    "拖曳可以跳到歌曲的另一個位置。",
    "顯示清單",
    "隱藏清單",
    "顯示或隱藏你的歌曲。",
    "這個程式沒有提供播放進度。",

    "加入檔案",
    "選擇電腦裡的歌曲加入清單。",
    "加入資料夾",
    "加入一個資料夾裡的所有歌曲。",
    "1 首歌",
    "%d 首歌",
    "還沒有歌曲。用「加入檔案」或「加入資料夾」加入。",
    "按一下播放這首歌。",
    "這個檔案無法播放：%s",
    "這個檔案無法播放。",
    "移除",
    "把這首歌從清單移除。檔案仍留在你的電腦上。",

    "請先用「加入檔案」或「加入資料夾」加入歌曲。",
    "請先在清單中選一首歌。",
    "%s 不允許從這裡操作。",
    "這個程式不允許從這裡操作。",
    "目前沒有其他程式在播放。",
    "請在那個程式裡調整音量。",
    "清單只放你自己的歌曲。選「我的檔案」就能看到。",
    "不知道這首歌的長度，所以無法跳轉。",
    "音樂已經停止。",
    "這是顯示測試：在設定檔寫 mode=on 之後才能使用面板。",

    "沒有其他程式在播放",
    "在 Spotify、瀏覽器或其他播放器播放音樂，就會顯示在這裡。",

    "語言",
    "跟隨 Windows",
    "文字大小",
    "小",
    "中",
    "大",
    "遊戲不在前景時暫停音樂",
    "切換到其他視窗時，你自己的歌曲會暫停，回到遊戲就繼續。其他程式的音樂不會暫停。",
    "比賽中的按鍵：%s 顯示或隱藏面板，%s 停止，%s 上一首，%s 播放或暫停，%s 下一首，%s 切換重複方式。",
    "沒有按鍵",

    "音樂面板：按 %s 顯示。",
    "測試版：這些歌曲只是範例，不會發出聲音。",
};

inline const Strings& StringsFor(Language language) {
    return language == Language::ChineseTraditional ? kStringsChineseTraditionalDraft : kStringsEnglish;
}

// Language names are shown in their own language, whatever the panel language is.
inline const char* LanguageName(Language language) {
    return language == Language::ChineseTraditional ? "繁體中文" : "English";
}

// The media remote's status lines (remote::Message in src/remote/remote.h), by its numeric
// value, so the panel can show them in its own language. {app} is replaced by the app's name.
// English is the remote's own wording. The Chinese column is a *** DRAFT *** like the one above.
// The order must match remote::Message exactly; panel.cpp checks the count.
constexpr int kRemoteMessageCount = 37;

inline const char* const kRemoteMessagesEnglish[kRemoteMessageCount] = {
    "",                                                                                       // None
    "Looking for music apps...",                                                              // LookingForApps
    "Controlling other apps needs Windows 10 (version 1809) or newer. Local files still work.",  // Unsupported
    "Windows is not answering about music apps right now. The panel keeps trying. Local files still work.",  // NotAnswering
    "Controlling other apps stopped working in this session. Local files still work.",       // Faulted
    "No music app found. Start playing something in Spotify, a browser or another player and it will show up here.",  // NoApp
    "Waiting for {app}. Start it and it will show up here.",                                  // WaitingForApp
    "{app} is not running. Start it, or pick another app below.",                             // PickedNotRunning
    "{app} has closed. Start it again, or pick another app below.",                           // PickedClosed
    "{app} has closed.",                                                                      // AppClosed
    "{app} is playing.",                                                                      // IsPlaying
    "{app} is paused.",                                                                       // IsPaused
    "{app} is stopped.",                                                                      // IsStopped
    "{app} is already playing.",                                                              // AlreadyPlaying
    "{app} is already paused.",                                                               // AlreadyPaused
    "Sending to {app}...",                                                                    // Sending
    "{app} is playing.",                                                                      // DonePlaying
    "{app} is paused.",                                                                       // DonePaused
    "{app} has stopped.",                                                                     // DoneStopped
    "{app} accepted play/pause.",                                                             // DonePlayPause
    "Asked {app} for the next track.",                                                        // DoneNext
    "Asked {app} for the previous track.",                                                    // DonePrevious
    "Looping is off in {app}.",                                                               // DoneLoopOff
    "Loop one is on in {app}.",                                                               // DoneLoopOne
    "Loop all is on in {app}.",                                                               // DoneLoopAll
    "{app} does not offer \"play\" right now.",                                               // NoPlay
    "{app} does not offer \"pause\" right now.",                                              // NoPause
    "{app} does not offer \"play/pause\" right now.",                                         // NoPlayPause
    "{app} has no stop. Use pause.",                                                          // NoStop
    "{app} does not offer \"next track\" right now.",                                         // NoNext
    "{app} does not offer \"previous track\" right now.",                                     // NoPrevious
    "{app} does not let other programs change looping. Change it in {app} itself.",           // LoopNotAllowed
    "{app} did not change looping. Change it in {app} itself.",                               // LoopIgnored
    "{app} did not accept that. Try again, or use {app} itself.",                             // Refused
    "{app} did not answer. Try again, or use {app} itself.",                                  // NoReply
    "{app} did not respond. Try again, or use {app} itself.",                                 // NoEffect
    "The test host may not send commands to {app}.",                                          // TestGuard
};

// *** DRAFT *** The owner rewrites every line below.
inline const char* const kRemoteMessagesChineseTraditionalDraft[kRemoteMessageCount] = {
    "",
    "正在尋找音樂程式……",
    "控制其他程式需要 Windows 10（1809 版）或更新版本。本機檔案仍可使用。",
    "Windows 目前沒有回應音樂程式的查詢。面板會繼續嘗試。本機檔案仍可使用。",
    "這次控制其他程式的功能已停止運作。本機檔案仍可使用。",
    "找不到音樂程式。在 Spotify、瀏覽器或其他播放器開始播放，就會顯示在這裡。",
    "正在等待 {app}。開啟它就會顯示在這裡。",
    "{app} 沒有在執行。請開啟它，或在下面選擇其他程式。",
    "{app} 已經關閉。請重新開啟，或在下面選擇其他程式。",
    "{app} 已經關閉。",
    "{app} 正在播放。",
    "{app} 已暫停。",
    "{app} 已停止。",
    "{app} 已經在播放。",
    "{app} 已經暫停。",
    "正在傳送到 {app}……",
    "{app} 正在播放。",
    "{app} 已暫停。",
    "{app} 已停止。",
    "{app} 已接受播放／暫停。",
    "已要求 {app} 播放下一首。",
    "已要求 {app} 播放上一首。",
    "{app} 的重複播放已關閉。",
    "{app} 已開啟單曲重複。",
    "{app} 已開啟全部重複。",
    "{app} 目前不提供「播放」。",
    "{app} 目前不提供「暫停」。",
    "{app} 目前不提供「播放／暫停」。",
    "{app} 沒有停止功能。請用暫停。",
    "{app} 目前不提供「下一首」。",
    "{app} 目前不提供「上一首」。",
    "{app} 不允許其他程式更改重複方式。請在 {app} 裡更改。",
    "{app} 沒有更改重複方式。請在 {app} 裡更改。",
    "{app} 沒有接受。請再試一次，或直接使用 {app}。",
    "{app} 沒有回應。請再試一次，或直接使用 {app}。",
    "{app} 沒有反應。請再試一次，或直接使用 {app}。",
    "測試主程式不可以傳送指令給 {app}。",
};

inline const char* RemoteMessageText(Language language, int id) {
    if (id < 0 || id >= kRemoteMessageCount) {
        return "";
    }
    return language == Language::ChineseTraditional ? kRemoteMessagesChineseTraditionalDraft[id]
                                                    : kRemoteMessagesEnglish[id];
}
