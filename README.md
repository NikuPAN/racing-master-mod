# Racing Master Mod

[English](#english) · [繁體中文](#繁體中文)

---

## English

An unofficial, free, open-source mod for the PC version of **Racing Master** (巔峰極速, NetEase). It is a fan project and has no connection to NetEase, Codemasters or NVIDIA.

### Status: loader test only

Nothing in this repository adds music or DLSS yet. The only thing that exists today is the **loader**: a small DLL the game picks up at start, which proves it is running by drawing a white square in the top-left corner for 20 seconds and writing a log file.

Planned, in this order:

1. **Loader** (this release): confirm the game starts and stays online with the DLL present.
2. **In-game music panel**: play your own MP3, FLAC, WAV and OGG files over the game, or control Spotify and other players, with play, pause, stop, skip, loop one and loop all.
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

If it works, a white square with a dark centre appears in the top-left corner of the game window for 20 seconds.

### Remove

Close the game and delete `version.dll` from that folder. Nothing else was changed.

### Log file

The mod writes what it did to

```
%LOCALAPPDATA%\RacingMasterMod\mod.log
```

Paste that path into the File Explorer address bar to open it. Earlier runs are kept next to it as `mod.<date>-<time>.log` (the newest five). If two copies of the game run at once, the second writes `mod.<process id>.log`. Please attach the current log and the one before it when you report a problem.

### Settings

Optional. Create a text file named `settings.ini` in `%LOCALAPPDATA%\RacingMasterMod\` (the folder that holds the log; no administrator permission needed):

```ini
[mod]
; 0 = switched off, 1 = load and write the log only, 2 = also draw the square (default)
stage=2
; how long the square stays on screen, in seconds (0 to 600)
marker_seconds=20
```

A file named `racing-master-mod.ini` next to `version.dll` is read as well and overrides it. Save either file as UTF-8 (with or without a byte order mark) or as the UTF-16 that Notepad calls "Unicode". If a value cannot be read, the mod switches itself off and the log says which line and what to write.

### How it works

- Windows looks for `version.dll` in the game's own folder before the system folder. The mod's file takes that name, passes every one of the 17 real `version.dll` functions straight through to Windows, and additionally starts the mod.
- The mod only wakes up inside `g112-Win64-Shipping.exe`. In any other program that happens to load it, it does nothing but forward.
- To draw, it attaches to the function Direct3D uses to show a finished frame. Steam, Discord and OBS overlays attach in the same place.
- No game file is edited and no game memory is patched.

### Build from source

Needs Visual Studio 2022 or newer with the C++ compiler. The Windows SDK is downloaded automatically into `.deps\`.

```powershell
powershell -ExecutionPolicy Bypass -File build\build.ps1      # builds out\version.dll
powershell -ExecutionPolicy Bypass -File tools\run-host-test.ps1   # checks it without the game
```

The second command runs the DLL inside a small test program with a window that is never shown, so nothing appears on screen.

### Licence

MIT. See [LICENSE](LICENSE).

---

## 繁體中文

《**巔峰極速**》（Racing Master，網易）PC 版的非官方免費開源模組。這是玩家自己做的專案，與網易、Codemasters、NVIDIA 都沒有關係。

### 目前進度：只有載入器

這個專案現在還**沒有**音樂功能，也還沒有 DLSS。目前做好的只有**載入器**：一個遊戲啟動時會一起讀進去的小 DLL。它會在畫面左上角顯示一個白色方塊 20 秒，並寫下一份紀錄檔，用來確認模組真的有跑起來。

接下來的順序：

1. **載入器**（這一版）：確認放了這個 DLL 之後，遊戲能正常開、能正常連線。
2. **遊戲內音樂面板**：在遊戲裡播自己的 MP3、FLAC、WAV、OGG，或是遙控 Spotify 等播放器；有播放、暫停、停止、下一首、單曲循環、全部循環。
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

成功的話，遊戲視窗左上角會出現一個中間是深色的白色方塊，20 秒後消失。

### 移除

關掉遊戲，把那個資料夾裡的 `version.dll` 刪掉就好，其他東西都沒動過。

### 紀錄檔

模組做了什麼都會寫在這裡：

```
%LOCALAPPDATA%\RacingMasterMod\mod.log
```

把這行貼到檔案總管的網址列就能打開。之前幾次執行的紀錄會留在旁邊，檔名是 `mod.<日期>-<時間>.log`，最多留五份。如果同時開了兩個遊戲，第二個會寫到 `mod.<處理程序編號>.log`。回報問題時，請把目前這份和上一份紀錄檔一起附上。

### 設定

可有可無。在 `%LOCALAPPDATA%\RacingMasterMod\`（就是放紀錄檔的那個資料夾）新增一個文字檔，檔名 `settings.ini`，不需要系統管理員權限：

```ini
[mod]
; 0 = 關閉，1 = 只載入並寫紀錄檔，2 = 另外顯示方塊（預設）
stage=2
; 方塊顯示幾秒（0 到 600）
marker_seconds=20
```

放在 `version.dll` 旁邊、檔名 `racing-master-mod.ini` 的檔案也會讀，而且以它的設定為準。兩個檔案都可以存成 UTF-8（有沒有 BOM 都行）或記事本裡叫「Unicode」的 UTF-16。設定值如果看不懂，模組會自己關掉，紀錄檔會寫出是哪一行、該怎麼改。

### 運作原理

- Windows 找 `version.dll` 時，會先找遊戲自己的資料夾，再找系統資料夾。模組的檔案就用這個名字，把原本 `version.dll` 的 17 個函式原封不動轉交給 Windows，同時把模組啟動起來。
- 模組只會在 `g112-Win64-Shipping.exe` 裡面啟動。萬一被別的程式讀到，它只做轉交，其他什麼都不做。
- 要在畫面上畫東西，它會掛在 Direct3D 把畫好的一幀送上螢幕的那個函式上。Steam、Discord、OBS 的遊戲內介面也是掛在同一個地方。
- 不修改任何遊戲檔案，也不改寫遊戲的記憶體。

### 從原始碼編譯

需要 Visual Studio 2022 以上，並安裝 C++ 編譯器。Windows SDK 會自動下載到 `.deps\`。

```powershell
powershell -ExecutionPolicy Bypass -File build\build.ps1      # 編出 out\version.dll
powershell -ExecutionPolicy Bypass -File tools\run-host-test.ps1   # 不開遊戲也能檢查
```

第二行會把 DLL 放進一個小測試程式裡跑，那個程式的視窗不會顯示，螢幕上不會跳出任何東西。

### 授權

MIT，見 [LICENSE](LICENSE)。
