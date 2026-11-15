# Racing Master Mod

[繁體中文](#繁體中文) · [English](#english)

---

## 繁體中文

<!-- DRAFT 0.3.2：以下到「移除」為止整段改寫，待作者審閱 -->
《**巔峰極速**》（Racing Master，網易）PC 版的非官方免費模組，在遊戲裡加一個**音樂面板**：比賽時可以放自己的歌，也可以控制 Spotify、瀏覽器或其他播放器，不用切出遊戲。這是玩家自己做的專案，跟網易、Codemasters、NVIDIA 都沒有關係。

### 這個模組會對遊戲做什麼，又絕對不會做什麼
<!-- DRAFT 0.3.2：整段中文待作者審閱 -->

整個模組都是開源的，下面每一條都能在程式碼裡查證。

**會做的**

- 在遊戲資料夾放一個檔案 `version.dll`，移除就是把它刪掉。
- 把音樂面板畫在遊戲畫面上，用的是 Steam、Discord、OBS 這類遊戲內介面同一個繪製位置。
- 在可點擊的模式下，它會擋在遊戲視窗訊息的前面，才能看到你在面板上的點擊和按鍵，其他訊息一律原樣交還遊戲。（較輕量的「精簡」模式不這麼做，改用全域快速鍵。）
- 用自己的執行緒，透過 Windows 的一般音訊輸出播你的音樂。
- 只讀寫自己的檔案，位置在 `%LOCALAPPDATA%\RacingMasterMod`（你的歌單、設定和紀錄檔）。

**絕對不會做的**

- 更動、搬移或刪除任何遊戲檔案。
- 讀取或寫入遊戲的記憶體。
- 透過網路傳送任何東西，不會回傳、也不會自動更新。
- 改變車子的操控、你的單圈成績，或任何影響比賽的東西。
- 躲避反作弊，或干擾反作弊。

這些都不代表遊戲官方允許這個模組（請看下一段），但這就是模組會碰到的全部範圍。

### 開始之前，請先看完這段

- **遊戲並不允許這樣做，帳號有可能被封。**《巔峰極速》是有反作弊的連線遊戲，這個模組會在遊戲資料夾放一個遊戲會讀進去的檔案。它不改玩法、不改車、不碰連線，也不改任何遊戲檔案，但反作弊分辨不出這些，網易也可以把任何多出來的檔案都當成違反使用條款。
- **每個新版本都先用分身帳號試，**不要拿自己在乎的帳號第一個試。封號可能在用過之後幾天、幾週才來，今天沒事不代表安全。
- **使用風險自負。**這個模組不提供任何保證（見 MIT 授權）。遊戲、電腦或帳號出了問題，作者不負責。
- **遊戲更新後可能就不能用了。**遇到這種情況，請先照下面的方法移除，等新版本出來再裝。
- 這個模組永遠不會想辦法躲過反作弊。遊戲要是擋下它，就到此為止。

### 需要什麼

- Windows 10 或 11（64 位元）
- 《巔峰極速》PC 版（在港澳台版上測試過）

### 安裝步驟

1. **下載。**在這個專案的 GitHub 頁面，點右邊的 **Releases**，選最新的 **Music panel** 版本，在 **Assets** 底下下載 `.zip` 檔。
2. **解壓縮。**在「下載」資料夾對 `.zip` 檔按右鍵，選「**全部解壓縮…**」，再按「**解壓縮**」。打開的資料夾裡會有 `version.dll`。
3. **把遊戲和啟動器完全關掉。**
4. **找到遊戲資料夾。**對桌面上的《巔峰極速》捷徑按右鍵，選「**開啟檔案位置**」，再依序打開 `Binaries`、`Win64`。看到一個叫 `g112-Win64-Shipping` 的檔案，就是這裡沒錯。照預設安裝的話，路徑是 `C:\Program Files (x86)\g112\Binaries\Win64`。
5. **把檔案放進去。**把解壓縮出來的 `version.dll` 拖進 `Win64` 資料夾。因為這是程式資料夾，Windows 會問你要不要給權限，按「**繼續**」。如果 Windows 說已經有同名檔案（舊版的這個模組），選「**取代目的地中的檔案**」。
6. **照平常一樣開遊戲。**過幾秒，畫面下方會出現一行小提示，說音樂面板準備好了。按 **Insert** 就能打開。

只想聽自己的歌的話，在遊戲自己的聲音設定裡把音樂關小或關掉。

### 移除

關掉遊戲，打開同一個 `Win64` 資料夾（第 4 步），把 `version.dll` 刪掉，這樣就好了，遊戲資料夾裡其他東西都沒動過。歌單和設定存在 `%LOCALAPPDATA%\RacingMasterMod\`（把這串貼到檔案總管的網址列就能打開），不想留的話，這個資料夾也一起刪掉。

### 好像怪怪的？

- **沒有出現提示，按 Insert 也沒反應：**確認檔名剛好是 `version.dll`（不是 `version (1).dll`），而且跟 `g112-Win64-Shipping` 放在同一個資料夾。Windows 可能把 `.dll` 副檔名藏起來，只要名字是 `version` 就沒問題。
- **面板一直閃：**表示這個遊戲開了 NVIDIA Smooth Motion。到 NVIDIA App 裡把《巔峰極速》的 Smooth Motion 關掉。
- **其他問題：**打開 `%LOCALAPPDATA%\RacingMasterMod\`，回報時附上 `mod.log`（和前一份 `mod.<日期>-<時間>.log`）。紀錄檔裡會有你自己歌曲的檔名和資料夾路徑（路徑裡有你的 Windows 使用者名稱），但不會記下其他程式在放什麼。

### 目前進度

0.3.2 版。音樂面板已經用測試帳號在遊戲裡跑過（0.3.0 版）；0.3.1 和 0.3.2 版修正並加上了那幾次試玩發現需要的東西：關遊戲時記住正在播的歌和播到哪裡、面板可以放在畫面邊緣的六個位置、只有一列的精簡面板、時間不會被截掉、其他程式只顯示簡單的名稱、「清空清單」按鈕，以及控制遊戲本身音樂的按鈕。每個版本發行前，都會先用分身帳號在遊戲裡試過。

另外正在研究 DLSS 外掛，之後會是另一個獨立下載，音樂面板不需要它。

### 音樂面板

按 **Insert** 打開或收起面板。面板是畫面下方中間的一條小工具列，上面有音樂來源、正在播的歌、各個按鈕、音量和進度條；按「顯示清單」，歌單會在工具列上方展開。在「設定」裡可以把面板移到六個位置：四個角落、上方中間或下方中間。不管放在哪裡，面板都會貼齊畫面邊緣，盡量少擋到遊戲。<!-- DRAFT 0.3.2：這兩句待作者審閱 -->點在面板上的滑鼠只會操作面板，點在其他地方就照常交給遊戲，所以面板開著也能繼續開車。

**精簡面板**：連工具列都嫌太大的話，按「設定」旁邊那個箭頭朝內的按鈕，面板就會縮成一列，只留上一首、播放或暫停、下一首、歌名、時間和一小段進度條。按這一列最右邊、箭頭朝外的按鈕就能回到完整面板，「設定」裡的「精簡面板」也一樣。面板會記住你上次用的是哪一種。音量、重複方式、歌單和設定只有完整面板才有。<!-- DRAFT 0.3.2：整段待作者審閱 -->

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

工具列上的「設定」會打開一列選項：語言（跟隨 Windows、English 或繁體中文）、文字大小、面板位置（畫面上緣或下緣的六個位置）<!-- DRAFT 0.3.2 -->，以及遊戲不在最前面時，自己的歌要不要暫停。

按「加入檔案」或「加入資料夾」時，遊戲上面會開一個一般的 Windows 檔案視窗，關掉它之前，遊戲不會回應鍵盤和滑鼠。遊戲在獨佔全螢幕模式時，這兩個按鈕會變灰，因為檔案視窗會讓遊戲縮到最小；這時請改用音樂資料夾，或把遊戲切成視窗或無邊框模式。


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
; 左上角顯示白色測試方塊幾秒（0 到 600；預設 0，不顯示）
marker_seconds=0

[panel]
; on = 完整面板（預設）；keys = 畫出面板，只用 F5-F9 控制，不能點（最小干擾）；
; display = 只顯示，不能點也不能用按鍵；off = 沒有面板，也沒有音樂
mode=on
; auto（跟隨 Windows）、en 或 zh-TW
language=auto
; small、medium 或 large
text_size=medium
; 1 = 遊戲不在最前面時，自己的歌會暫停
pause_when_unfocused=0
; my_files（我的檔案）或 other_app（其他程式）
source=my_files
; top-left、top-centre（上方中間）、top-right、bottom-left、bottom-centre（下方中間，預設）或 bottom-right
corner=bottom-centre
; 1 = 精簡面板（一列）；0 = 完整面板
compact=0

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

`mode=display` 適合第一次小心試用：面板會畫出來，但不接受滑鼠和按鍵，也完全不碰遊戲視窗。面板會把你在面板裡改的設定（語言、文字大小、暫停、音樂來源、面板位置、精簡或完整面板）寫回這個檔案，音樂播放器也會在裡面保留自己的 `[music]` 段落；這兩部分請不要手動改。

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

---

## English

An unofficial, free mod for the PC version of **Racing Master** (巔峰極速, NetEase) that adds an in-game **music panel**: play your own songs while you race, or control Spotify, a browser or another music app, without leaving the game. It is a fan project and has no connection to NetEase, Codemasters or NVIDIA.

### What this mod does to your game — and what it never does

The whole mod is open source, so every line below can be checked in the code.

**What it does**

- Adds one file, `version.dll`, to the game folder. Removing it is just deleting that file.
- Draws the music panel on top of the game, at the same point in Windows that the Steam, Discord and OBS overlays use.
- In the clickable mode, it sits in front of the game window's messages so it can see your clicks and key presses on the panel, and passes every other message straight to the game untouched. (The lighter "minimal" mode skips this and uses global hotkeys instead.)
- Plays your music from its own threads through Windows' normal sound output.
- Reads and writes only its own files, in `%LOCALAPPDATA%\RacingMasterMod` (your song list, settings and log).

**What it never does**

- Change, move or delete any game file.
- Read or write the game's memory.
- Send anything over the internet — it never phones home and never auto-updates.
- Change how your car drives, your lap times, or anything that affects a race.
- Hide itself from, or interfere with, the anti-cheat.

None of this makes the mod officially allowed — please read the next part — but it is the whole of what the mod touches.

### Before you start: please read

- **This is not allowed by the game, and your account could be banned.** Racing Master is an online game with anti-cheat. The mod puts an extra file into the game folder that the game loads. The mod does not change how the game plays, your car, the network or any game file, but the anti-cheat cannot tell that, and NetEase can treat any added file as breaking its terms of use.
- **Try every new version on a spare account first,** never on the account you care about. A ban can arrive days or weeks after you used the mod, so "nothing happened" today does not mean it is safe.
- **You use it at your own risk.** The mod comes without any guarantee (see the MIT licence). If something goes wrong with your game, your PC or your account, the authors are not responsible.
- **A game update can stop it working.** If that happens, remove it (below) until a new version is out.
- The mod will never try to hide from the anti-cheat. If the game blocks it, that is the end of it.

### What you need

- Windows 10 or 11 (64-bit)
- Racing Master for PC (tested with the Hong Kong, Macau and Taiwan client)

### Install, step by step

1. **Download.** On this project's GitHub page, open **Releases** (on the right), pick the newest **Music panel** release and download the `.zip` file under **Assets**.
2. **Unzip.** In your Downloads folder, right-click the `.zip` file and choose **Extract All…**, then **Extract**. A folder opens with `version.dll` in it.
3. **Close the game** and its launcher completely.
4. **Find the game folder.** Right-click your Racing Master shortcut, choose **Open file location**, then open `Binaries`, then `Win64`. You are in the right place when you see a file called `g112-Win64-Shipping`. With the normal install this is `C:\Program Files (x86)\g112\Binaries\Win64`.
5. **Copy the file in.** Drag `version.dll` from the unzipped folder into the `Win64` folder. Windows asks for permission because it is a program folder: click **Continue**. If Windows says a file with that name already exists (an older version of this mod), choose **Replace the file**.
6. **Start the game** the way you always do. After a few seconds a short note at the bottom of the screen says the music panel is ready. Press **Insert** to open it.

To hear only your own music, turn down or switch off the music in the game's own sound settings.

### Remove

Close the game, open the same `Win64` folder (step 4) and delete `version.dll`. That is all: nothing else in the game folder was changed. Your song list and settings are kept in `%LOCALAPPDATA%\RacingMasterMod\` (paste that into the File Explorer address bar); delete that folder too if you want them gone.

### Something's not right?

- **No note and Insert does nothing:** check the file is called exactly `version.dll` (not `version (1).dll`) and sits next to `g112-Win64-Shipping`. Windows may hide the `.dll` ending; that is fine as long as the name is `version`.
- **The panel blinks:** NVIDIA Smooth Motion is on for the game. Turn it off for Racing Master in the NVIDIA app.
- **Anything else:** open `%LOCALAPPDATA%\RacingMasterMod\` and send the file `mod.log` (and the one before it, `mod.<date>-<time>.log`) with your report. It lists your own songs' file names and folder paths (which include your Windows user name), and never what other apps are playing.

### Status

Version 0.3.2. The music panel has run in the game on a test account (0.3.0); 0.3.1 and 0.3.2 fix and add what those runs showed: the song and its place are saved when you close the game, six places for the panel along the screen edges, a compact one-row panel, times that are never cut off, other apps shown by plain names, a Clear list button, and a button for the game's own music. Each version is tried in the game on a spare account before it is released.

A separate DLSS plugin is being investigated. It will be its own download; the music panel does not need it.

### The music panel

Press **Insert** to show or hide the panel. It sits at the bottom centre of the screen as a compact bar: where the music comes from and what is playing, the buttons, the volume and the time line. **Show list** opens the song list above the bar. In **Settings** you can move it to any of six places: the four corners, the top centre or the bottom centre. Wherever it sits, it touches the edge of the screen, so it covers as little of the game as it can. Clicks on the panel stay on the panel; clicks anywhere else go to the game as usual, so you can keep driving with the panel open.

**Compact panel.** When even the bar is too big, the button with two arrows pointing inward (next to **Settings**) shrinks the panel to one row: previous, play or pause, next, the song, the time and a short time line. The button at the right end of that row (arrows pointing outward) brings the full panel back, and so does **Compact panel** in **Settings**. The panel remembers which one you used last. Volume, repeat, the song list and the settings are only in the full panel.

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

**Settings** in the bar opens a row with the language (follow Windows, English or Traditional Chinese), the text size, where the panel sits (six places along the top and bottom edges), and whether your own songs pause while the game is not the active window.

When you use **Add files** or **Add folder**, a normal Windows file window opens over the game, and the game does not react to keys or the mouse until you close it. In exclusive full screen these two buttons are greyed out, because the file window would minimise the game; use the music folder instead, or switch the game to windowed or borderless.

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
; a white test square in the top-left corner for this many seconds (0 to 600; 0, the default, shows none)
marker_seconds=0

[panel]
; on = the full panel (default); keys = drawn, controlled by F5-F9 only, no clicks (smallest footprint);
; display = shown but cannot be clicked, no keys; off = no panel and no music
mode=on
; auto (follow Windows), en or zh-TW
language=auto
; small, medium or large
text_size=medium
; 1 = your own songs pause while the game is not the active window
pause_when_unfocused=0
; my_files or other_app
source=my_files
; top-left, top-centre, top-right, bottom-left, bottom-centre (default) or bottom-right
corner=bottom-centre
; 1 = the compact panel (one row); 0 = the full panel
compact=0

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

`mode=display` is meant for a first careful try: the panel is drawn but takes no clicks or keys, and the game window is left alone. The panel writes its own changes (language, text size, pausing, source, position, compact or full) back into this file, and the music player keeps its own `[music]` section there; leave both as they are.

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
