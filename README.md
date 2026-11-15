# Racing Master Mod

[English](#english) · [繁體中文](#繁體中文)

---

## English

An unofficial, free, open-source mod for the PC version of **Racing Master** (巔峰極速, NetEase). It is a fan project and has no connection to NetEase, Codemasters or NVIDIA.

### Status: version 0.3.0, the music panel, not yet tried in the game

Version 0.3.0 adds an in-game **music panel**: play your own songs over the game, or control the music another app is playing, without leaving the race. It has been checked only in a test program on the developer's PC, never in the real game. The loader underneath it has run in the game twice (0.1.0 and 0.2.0) without a problem.

Planned, in this order:

1. **Loader**: confirm the game starts and stays online with the DLL present.
2. **In-game music panel** (this release): your own songs, or remote control of Spotify, a browser or another player.
3. **DLSS 4.5**: replace the game's anti-aliasing with NVIDIA DLSS through NVIDIA's official SDK.
4. **DLSS 5**: once NVIDIA releases it for more graphics cards.

### Read this before you use it

- **You can lose your account.** Racing Master is an online game with anti-cheat. This mod loads a DLL into the game. It does not change gameplay, physics, network traffic or any game file, but the anti-cheat cannot know that, and NetEase may treat any third-party DLL as a violation of its terms.
- **A clean test does not prove it is safe.** Bans in online games are often issued days or weeks later.
- **Test on an account you are willing to lose.** Never try a new version on your main account first.
- This project will never include or support a way to hide from the anti-cheat. If the game blocks the mod, the mod stops there.

### Requirements

- Windows 10 or 11, 64-bit
- Racing Master PC (HMT client tested; the game executable is `g112-Win64-Shipping.exe`)
- For the future DLSS feature: an NVIDIA GeForce RTX graphics card

### Install

1. Close the game and its launcher.
2. Copy `version.dll` into the folder that contains `g112-Win64-Shipping.exe`. With the default install location that is
   `C:\Program Files (x86)\g112\Binaries\Win64\`.
   Windows asks for administrator permission because the folder is under Program Files.
3. Start the game the way you always do.

If it works, a white square with a dark centre appears in the top-left corner of the game window for 20 seconds, and a short note at the bottom centre says which key opens the music panel.

### Remove

Close the game and delete `version.dll` from that folder. Nothing else in the game folder was changed. Your song list and settings stay in `%LOCALAPPDATA%\RacingMasterMod\`; delete that folder too if you want them gone.

### The music panel

Press **Insert** to show or hide the panel. It sits at the bottom centre of the screen as a compact bar: where the music comes from and what is playing, the buttons, the volume and the time line. **Show list** opens the song list above the bar. The panel can sit in any of the four corners instead (in **Settings**). Clicks on the panel stay on the panel; clicks anywhere else go to the game as usual, so you can keep driving with the panel open.

Keys that work while racing, with the panel open or closed:

| Key | Does |
|---|---|
| Insert | show or hide the panel |
| F5 | stop |
| F6 | previous song (restarts the song when more than 3 seconds have played) |
| F7 | play or pause |
| F8 | next song |
| F9 | what repeats: off, this song, the whole list |

The keyboard's own media keys also work for your own songs. Every key can be changed in the settings file (see below).

Two sources, chosen with the two buttons at the left of the bar:

- **My files**: your own songs, played inside the game. Use **Add files** or **Add folder** in the list, or copy songs into the music folder (below). Click a song to play it; the cross at the end of a row takes it off the list (the file stays on your computer). The list, the volume and what repeats are remembered for next time. Nothing starts playing by itself when the game starts.
- **Other app**: controls whatever Windows reports as playing music, for example Spotify or a browser. The panel shows what that app allows; buttons it does not allow are greyed out, and resting the pointer on one says why. **Not yet tried in the game.** The game runs with administrator rights, and whether Windows lets it see and control other apps from there is still unknown.

**Settings** in the bar opens a row with the language (follow Windows, English or Traditional Chinese), the text size, where the panel sits (bottom centre or one of the four corners), and whether your own songs pause while the game is not the active window.

When you use **Add files** or **Add folder**, a normal Windows file window opens over the game, and the game does not react to keys or the mouse until you close it. In exclusive full screen these two buttons are greyed out, because the file window would minimise the game; use the music folder instead, or switch the game to windowed or borderless.

To hear only your music, turn down the game's own music with the music volume slider in the game's settings.

### The music folder

```
%LOCALAPPDATA%\RacingMasterMod\music
```

Paste that path into the File Explorer address bar. Songs you copy into this folder (sub-folders included) are added to the list every time the game starts. The full path is also written in the log file.

### Supported formats

MP3, FLAC, WAV, OGG Vorbis (`.ogg`, `.oga`), Opus (`.opus`) and M4A (`.m4a` with AAC or Apple Lossless, and `.aac`). Songs play one after another without a gap. A file that cannot be played stays in the list, greyed out, with the reason.

- M4A uses Windows' own Media Foundation. Windows "N" editions do not include it until the Media Feature Pack is installed; the other formats work without it.
- `.webm` files (often what video sites give you) are not supported. Convert them to `.opus` first; the sound stays the same.

### Log file

The mod writes what it did to

```
%LOCALAPPDATA%\RacingMasterMod\mod.log
```

Paste that path into the File Explorer address bar to open it. Earlier runs are kept next to it as `mod.<date>-<time>.log` (the newest five). If two copies of the game run at once, the second writes `mod.<process id>.log`. Please attach the current log and the one before it when you report a problem. The log never contains song titles or artists from other apps.

### Settings

Optional. Create a text file named `settings.ini` in `%LOCALAPPDATA%\RacingMasterMod\` (the folder that holds the log; no administrator permission needed). Do not put it in the game folder: a `settings.ini` next to `version.dll` is ignored, and the log says so.

```ini
[mod]
; 0 = switched off, 1 = load and write the log only, 2 = full mod (default)
stage=2
; how long the white square stays on screen, in seconds (0 to 600)
marker_seconds=20

[panel]
; on = the full panel (default); display = shown but cannot be clicked, no keys; off = no panel and no music
mode=on
; auto (follow Windows), en or zh-TW
language=auto
; small, medium or large
text_size=medium
; 1 = your own songs pause while the game is not the active window
pause_when_unfocused=0
; my_files or other_app
source=my_files
; bottom-centre (default), top-left, top-right, bottom-left or bottom-right
corner=bottom-centre

[keys]
; a key name such as Insert, F7 or Home, with Ctrl+, Shift+ or Alt+ in front if you like; none = no key
panel=Insert
stop=F5
previous=F6
play_pause=F7
next=F8
loop=F9
; 1 = the keyboard's media keys control your own songs
media_keys=1
```

`mode=display` is meant for a first careful try: the panel is drawn but takes no clicks or keys, and the game window is left alone. The panel writes its own changes (language, text size, pausing, source, position) back into this file, and the music player keeps its own `[music]` section there; leave both as they are.

A file named `racing-master-mod.ini` next to `version.dll` is read as well and overrides it. Save either file as UTF-8 (with or without a byte order mark) or as the UTF-16 that Notepad calls "Unicode". If a `[mod]` value cannot be read, the mod switches itself off and the log says which line and what to write; a `[panel]` or `[keys]` value that cannot be read keeps its default, and the log says why.

### How it works

- Windows looks for `version.dll` in the game's own folder before the system folder. The mod's file takes that name, passes every one of the 17 real `version.dll` functions straight through to Windows, and additionally starts the mod.
- The mod only wakes up inside `g112-Win64-Shipping.exe`. In any other program that happens to load it, it does nothing but forward.
- To draw, it attaches to the function Direct3D uses to show a finished frame. Steam, Discord and OBS overlays attach in the same place. To see the panel's clicks and keys, it watches the game window's messages and passes everything not meant for the panel on to the game.
- The music plays from the mod's own threads through Windows' default sound output, and follows it when you switch the default device.
- No game file is edited and no game memory is patched.

### Build from source

Needs Visual Studio 2022 or newer with the C++ compiler. The Windows SDK and the libraries the mod uses are downloaded automatically into `.deps\`, each checked against a fixed checksum.

```powershell
powershell -ExecutionPolicy Bypass -File build\build.ps1      # builds out\version.dll
powershell -ExecutionPolicy Bypass -File tools\run-host-test.ps1   # checks it without the game
```

The second command runs the DLL inside small test programs with windows that are never shown and a sound output that plays nothing, so nothing appears on screen and nothing is heard.

### Licence

MIT. See [LICENSE](LICENSE).

The panel is drawn with Dear ImGui, and the music player uses miniaudio, stb_vorbis, libogg, opus and opusfile. Their licences, and the notices that ship with every release, are in [THIRD_PARTY.md](THIRD_PARTY.md).

---

## 繁體中文

《**巔峰極速**》（Racing Master，網易）PC 版的非官方免費開源模組。這是玩家自己做的專案，與網易、Codemasters、NVIDIA 都沒有關係。

### 目前進度：0.3.0 版加入音樂面板，還沒在遊戲裡實測

0.3.0 版多了一個**遊戲內音樂面板**：比賽中不用切出遊戲，就能播自己的歌，或是控制其他程式正在放的音樂。目前只在開發者電腦上的測試程式裡檢查過，還沒有在真正的遊戲裡跑過。底下的載入器則已經在遊戲裡順利跑過兩次（0.1.0 和 0.2.0 版）。

接下來的順序：

1. **載入器**：確認放了這個 DLL 之後，遊戲能正常開、能正常連線。
2. **遊戲內音樂面板**（這一版）：播自己的歌，或遙控 Spotify、瀏覽器等播放器。
3. **DLSS 4.5**：用 NVIDIA 官方 SDK，把遊戲原本的反鋸齒換成 DLSS。
4. **DLSS 5**：等 NVIDIA 開放給更多顯示卡之後再做。

### 使用前請先看

- **帳號有可能被封。**《巔峰極速》是有反作弊的連線遊戲，而這個模組會把一個 DLL 載入遊戲。它不改玩法、不改物理、不碰網路封包，也不改任何遊戲檔案，但反作弊系統分辨不出來，網易也可能把任何第三方 DLL 都當成違反使用條款。
- **測試沒事不代表安全。** 連線遊戲的封號常常是過了幾天、幾週才下來。
- **請用被封也不心痛的帳號測試。** 新版本不要第一次就用主帳號試。
- 這個專案不會加入、也不會支援任何躲避反作弊的手段。遊戲如果擋下模組，那就到此為止。

### 需求

- Windows 10 或 11，64 位元
- 《巔峰極速》PC 版（已測試港澳台版，遊戲主程式是 `g112-Win64-Shipping.exe`）
- 之後的 DLSS 功能需要 NVIDIA GeForce RTX 顯示卡

### 安裝

1. 關掉遊戲和啟動器。
2. 把 `version.dll` 複製到 `g112-Win64-Shipping.exe` 所在的資料夾。預設安裝位置是
   `C:\Program Files (x86)\g112\Binaries\Win64\`。
   因為資料夾在 Program Files 底下，Windows 會跳出系統管理員權限的確認。
3. 照平常的方式開遊戲。

成功的話，遊戲視窗左上角會出現一個中間是深色的白色方塊，20 秒後消失；畫面下方中間也會跳出一行小提示，告訴你按哪個鍵打開音樂面板。

### 移除

關掉遊戲，把那個資料夾裡的 `version.dll` 刪掉就好，遊戲資料夾裡其他東西都沒動過。歌單和設定存在 `%LOCALAPPDATA%\RacingMasterMod\`，不想留的話，把這個資料夾一起刪掉。

### 音樂面板

按 **Insert** 打開或收起面板。面板是畫面下方中間的一條小工具列，上面有音樂來源、正在播的歌、各個按鈕、音量和進度條；按「顯示清單」，歌單會在工具列上方展開。想把面板放到四個角落也可以，在「設定」裡改。點在面板上的滑鼠只會操作面板，點在其他地方就照常交給遊戲，所以面板開著也能繼續開車。

比賽中可以用的按鍵，面板開著或收起來都有效：

| 按鍵 | 功能 |
|---|---|
| Insert | 打開或收起面板 |
| F5 | 停止 |
| F6 | 上一首（已經播超過 3 秒時，會從頭再播一次） |
| F7 | 播放或暫停 |
| F8 | 下一首 |
| F9 | 切換重複方式：不重複、重複這一首、重複整個清單 |

鍵盤上的多媒體鍵也可以控制自己的歌。每個按鍵都能在設定檔裡改（見下面的「設定」）。

音樂來源有兩種，用工具列最左邊的兩個按鈕切換：

- **我的檔案**：在遊戲裡播你自己的歌。在歌單裡按「加入檔案」或「加入資料夾」，或是把歌直接複製到下面說的音樂資料夾。點一首歌就開始播；每一列最後面的叉叉會把那首歌從清單拿掉，檔案本身還留在電腦裡。歌單、音量和重複方式下次開遊戲都還在。遊戲開起來時不會自己開始放歌。
- **其他程式**：控制 Windows 認為正在放音樂的程式，例如 Spotify 或瀏覽器。面板只會開放那個程式允許的操作，不允許的按鈕會變灰，滑鼠停在上面就會說明原因。**這部分還沒在遊戲裡試過。** 遊戲是用系統管理員權限執行的，Windows 會不會讓它看到、控制其他程式，目前還不知道。

工具列上的「設定」會打開一列選項：語言（跟隨 Windows、English 或繁體中文）、文字大小、面板位置（下方中間或四個角落），以及遊戲不在最前面時，自己的歌要不要暫停。

按「加入檔案」或「加入資料夾」時，遊戲上面會開一個一般的 Windows 檔案視窗，關掉它之前，遊戲不會回應鍵盤和滑鼠。遊戲在獨佔全螢幕模式時，這兩個按鈕會變灰，因為檔案視窗會讓遊戲縮到最小；這時請改用音樂資料夾，或把遊戲切成視窗或無邊框模式。

只想聽自己的音樂的話，到遊戲設定裡把遊戲本身的音樂音量調低。

### 音樂資料夾

```
%LOCALAPPDATA%\RacingMasterMod\music
```

把這行貼到檔案總管的網址列就能打開。複製到這個資料夾（包括裡面的子資料夾）的歌，每次開遊戲都會自動加進歌單。完整路徑也會寫在紀錄檔裡。

### 支援的格式

MP3、FLAC、WAV、OGG Vorbis（`.ogg`、`.oga`）、Opus（`.opus`），以及 M4A（AAC 或 Apple Lossless 的 `.m4a`，還有 `.aac`）。歌與歌之間會無縫接著播。播不了的檔案會留在清單裡，以灰色顯示，並寫出原因。

- M4A 用的是 Windows 內建的 Media Foundation。Windows「N」版本要先裝「媒體功能套件」（Media Feature Pack）才有；其他格式不受影響。
- 不支援 `.webm`（影音網站下載下來常是這種檔）。請先轉成 `.opus`，音質不會變。

### 紀錄檔

模組做了什麼都會寫在這裡：

```
%LOCALAPPDATA%\RacingMasterMod\mod.log
```

把這行貼到檔案總管的網址列就能打開。之前幾次執行的紀錄會留在旁邊，檔名是 `mod.<日期>-<時間>.log`，最多留五份。如果同時開了兩個遊戲，第二個會寫到 `mod.<處理程序編號>.log`。回報問題時，請把目前這份和上一份紀錄檔一起附上。紀錄檔不會記下其他程式正在放的歌名和歌手。

### 設定

可有可無。在 `%LOCALAPPDATA%\RacingMasterMod\`（就是放紀錄檔的那個資料夾）新增一個文字檔，檔名 `settings.ini`，不需要系統管理員權限。不要放進遊戲資料夾：放在 `version.dll` 旁邊的 `settings.ini` 不會被讀取，紀錄檔裡會寫明這件事。

```ini
[mod]
; 0 = 關閉，1 = 只載入並寫紀錄檔，2 = 完整模組（預設）
stage=2
; 白色方塊顯示幾秒（0 到 600）
marker_seconds=20

[panel]
; on = 完整面板（預設）；display = 只顯示，不能點也不能用按鍵；off = 沒有面板，也沒有音樂
mode=on
; auto（跟隨 Windows）、en 或 zh-TW
language=auto
; small、medium 或 large
text_size=medium
; 1 = 遊戲不在最前面時，自己的歌會暫停
pause_when_unfocused=0
; my_files（我的檔案）或 other_app（其他程式）
source=my_files
; bottom-centre（下方中間，預設）、top-left、top-right、bottom-left 或 bottom-right
corner=bottom-centre

[keys]
; 按鍵名稱，例如 Insert、F7、Home，前面可以加 Ctrl+、Shift+ 或 Alt+；none = 不設按鍵
panel=Insert
stop=F5
previous=F6
play_pause=F7
next=F8
loop=F9
; 1 = 鍵盤的多媒體鍵控制自己的歌
media_keys=1
```

`mode=display` 適合第一次小心試用：面板會畫出來，但不接受滑鼠和按鍵，也完全不碰遊戲視窗。面板會把你在面板裡改的設定（語言、文字大小、暫停、音樂來源、面板位置）寫回這個檔案，音樂播放器也會在裡面保留自己的 `[music]` 段落；這兩部分請不要手動改。

放在 `version.dll` 旁邊、檔名 `racing-master-mod.ini` 的檔案也會讀，而且以它的設定為準。兩個檔案都可以存成 UTF-8（有沒有 BOM 都行）或記事本裡叫「Unicode」的 UTF-16。`[mod]` 裡的設定值如果看不懂，模組會自己關掉，紀錄檔會寫出是哪一行、該怎麼改；`[panel]` 或 `[keys]` 裡看不懂的值則會用預設值，紀錄檔同樣會說明原因。

### 運作原理

- Windows 找 `version.dll` 時，會先找遊戲自己的資料夾，再找系統資料夾。模組的檔案就用這個名字，把原本 `version.dll` 的 17 個函式原封不動轉交給 Windows，同時把模組啟動起來。
- 模組只會在 `g112-Win64-Shipping.exe` 裡面啟動。萬一被別的程式讀到，它只做轉交，其他什麼都不做。
- 要在畫面上畫東西，它會掛在 Direct3D 把畫好的一幀送上螢幕的那個函式上。Steam、Discord、OBS 的遊戲內介面也是掛在同一個地方。為了接收面板的滑鼠和按鍵，它會看著遊戲視窗收到的訊息，不是給面板的就原樣交回遊戲。
- 音樂由模組自己的執行緒播放，走 Windows 的預設音訊輸出；你在 Windows 換了預設裝置，它也會跟著換。
- 不修改任何遊戲檔案，也不改寫遊戲的記憶體。

### 從原始碼編譯

需要 Visual Studio 2022 以上，並安裝 C++ 編譯器。Windows SDK 和模組用到的函式庫會自動下載到 `.deps\`，每個檔案都會比對固定的檢查碼。

```powershell
powershell -ExecutionPolicy Bypass -File build\build.ps1      # 編出 out\version.dll
powershell -ExecutionPolicy Bypass -File tools\run-host-test.ps1   # 不開遊戲也能檢查
```

第二行會把 DLL 放進幾個小測試程式裡跑，視窗不會顯示，聲音輸出也不會發出任何聲音，所以螢幕上不會跳出東西，喇叭也不會響。

### 授權

MIT，見 [LICENSE](LICENSE)。

面板用 Dear ImGui 繪製，音樂播放器用到 miniaudio、stb_vorbis、libogg、opus 和 opusfile。這些函式庫的授權，以及每次發行都要附上的聲明，都在 [THIRD_PARTY.md](THIRD_PARTY.md)。
