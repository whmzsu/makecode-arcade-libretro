# MakeCode Arcade Core · Player Setup & Play Guide (Beginner Edition)

This guide is for people who just want to **play MakeCode Arcade games with RetroArch** — no code, no compiling. Just follow along.

---

## 0. First things first: what this is, and what it is not

**What it is**: a RetroArch "core" (think of it as the emulator engine). Once installed, RetroArch can load and run MakeCode Arcade games as if they were "cartridges."

**What it plays**: only **`.pxt64`** game files (called "cartridge images").

**Two easiest traps to fall into (read this first)**:

1. **64-bit only.** If your device/OS is 32-bit (very old hardware), this core will install but show a black screen and won't run. The vast majority of handhelds, phones, and PCs today are 64-bit, so you're usually fine — but don't try to force it onto a 32-bit system.
2. **It does NOT read `.png` cartridges.** MakeCode Arcade `.png` images you find online contain **source code only, not a directly playable file**. You must first "export" them into `.pxt64` to play them (see Section 4). The core's file browser also lists only `.pxt64`.

---

## 1. Download the core

Go to the releases page and grab the build for your device:

> https://github.com/whmzsu/makecode-arcade-libretro/releases

Find the newest release (currently **core-v0.3.9**). Below it is a row of archives — pick by platform:

| Your device | Download this |
|---|---|
| Windows PC (64-bit) | `makecode_arcade_windows-x64.zip` |
| Linux PC / handheld (x64) | `makecode_arcade_linux-x64.zip` |
| Android phone / handheld (64-bit) | `makecode_arcade_android-arm64.zip` |
| Lakka / LibreELEC systems (musl) | `makecode_arcade_lakka-arm64.zip` |

> Not sure which one? Easiest is to try the Windows or Linux build on a PC first. Handheld owners: check your system docs; when in doubt, pick the one with `arm64`.

**Unzip after downloading.** Inside you'll find two key files:

- `makecode_arcade_libretro.so` (Linux/Android; may be `.dll` or `.dylib` depending on platform)
- `makecode_arcade_libretro.info` (the core's info file — recommended to install alongside it)

---

## 2. Install the core into RetroArch

### Method A: Place the files manually (most reliable, recommended)

1. Open RetroArch.
2. Go to **Settings → Directories** and note the real paths of two things:
   - **Cores** (Core directory)
   - **Core Info** (Core Info directory)
   - Paths differ per device, so look them up here — **don't copy paths you found on the web**.
3. Put the extracted core file (the `.so` / `.dll`) into the **Cores** directory.
4. Put the `.info` file into the **Core Info** directory. **The core still loads and runs `.pxt64` fine without the `.info`, but RetroArch's content browser relies on the extension listed in the `.info` to recognize files — without it, when you pick the core first and then "Load Content," the browser won't show / won't auto-associate `.pxt64`, and the core name shows up as the bare filename; you can still run it via "Load Content → pick the file first → then pick the core manually," or from the command line. Installing the `.info` is just easier — recommended.**

### Method B: Use "Install or Restore Core"

RetroArch main menu → **Online Updater / Install or Restore Core** → in the file browser, select the extracted core file.

> Note: this browser **only lists the current platform's core extension** (`.dll` on Windows, `.so` on Linux). **`.zip` will NOT appear in the list**, so you must unzip first. Also, it does **not** bring the `.info` along — place the `.info` separately per Method A.

Once installed, you should see **MakeCode Arcade (unofficial)** in the core list.

---

## 3. Android-only steps (phone / Android handheld)

If you're playing on an **Android phone or Android handheld** (e.g. G Cloud, Retroid, etc.), installation has a few extra gotchas. Spelled out separately:

**1. Unzip first, then use "Install or Restore Core"**

On Android RetroArch, the "Install or Restore Core" file browser **only lists `.so` files** — **`.zip` will NOT appear**, so you must first unzip the downloaded archive to get `makecode_arcade_libretro.so` out of it.

Main menu → **Online Updater / Install or Restore Core** → select the extracted `.so`; it gets copied into RetroArch's core directory.

**2. The `.info` file goes in separately**

That browser does **not** carry the `.info` along. You must manually copy `makecode_arcade_libretro.info` into RetroArch's **Core Info** directory:

- Open RetroArch → **Settings → Directories**, find "Core Info" (also called `libretro_info_path`).
- Use your phone's file manager to copy the `.info` over. (The core still runs `.pxt64` without the `.info`, but the content browser won't show / won't auto-associate `.pxt64`, and the core name shows only as a filename — recommended to install it.)

> **⚠️ Note: the default info directory is inside the app's internal data dir, which normal users can't reach**
> RetroArch's "Core Info" directory **defaults to the app's internal data directory** (something like `Android/data/com.retroarch/...`). Starting with Android 11, scoped storage is enforced, and **normal file managers can't get into that directory** — you can't see it or copy into it, so seeing a path in "Settings" doesn't mean you can find it in a file manager.
> **Fix**: In RetroArch, go to **Settings → Directories → Core Info** and **change that directory to a location you can access** (e.g. create a `RetroArch/info` folder at the root of internal storage and point to it), then copy `makecode_arcade_libretro.info` there.
>
> **Don't manually change the core directory path**: let RetroArch install the core (`.so`) into its private directory via "Install or Restore Core" above — that directory has execute permission and is the safest. **Don't** move the core directory to the internal-storage root "for convenience," since that can land it somewhere without execute permission and fail to load. In one line: **let the app install the core, you place the info.**

**3. The core directory must stay in the app's private space (the easiest place to trip up)**

Since Android 10, `/storage/emulated/0` (the phone's "internal storage" root) is **noexec** — a core placed there is **recognized but will fail to load**.

So: let RetroArch install the core into its **own private directory** (using "Install or Restore Core" above does exactly that) — **don't** manually drop the `.so` into the internal-storage root and point to it. To find the real path, check **Settings → Directories → Cores**.

**4. Load a game**

Once the core is installed, it's the same as on PC: **Load Content → pick a `.pxt64` file** (`.png` still isn't accepted — convert it per the next section first).

> Verified: the Android arm64 build runs fine on multiple real devices (tested on Android 11 and Android 16). If you get a black screen after loading, first check point 3 above — is the core sitting in the noexec internal-storage root?

---

## 4. Get playable games (`.pxt64` cartridges)

### Method 1: Export directly from the official editor (simplest, recommended)

MakeCode Arcade has an online editor with a built-in export — no installs needed:

1. Open your game project (the makecode.com web editor).
2. Go to **About → Experiments** and enable **Experimental Hardware**.
3. Switch the hardware target to **VM**.
4. The same **Download** button now produces a `.pxt64` file directly.

> Detailed illustrated tutorial (bilingual, with FAQ):
> - Chinese: https://github.com/whmzsu/makecode-arcade-cartconv/blob/main/MakeCode%20Arcade%20%E5%A6%82%E4%BD%95%E5%AE%98%E6%96%B9%E5%AF%BC%E5%87%BApxt64/%E5%AE%98%E6%96%B9%E5%AF%BC%E5%87%BApxt64%E6%8C%87%E5%AF%BC.md
> - English: the `.en.md` in the same link above

### Method 2: Batch-convert existing `.png` cartridges

Got a pile of `.png` game images you want to convert to `.pxt64` in one go? Use this standalone tool (ships with a portable no-install package):

> https://github.com/whmzsu/makecode-arcade-cartconv

On Windows, download the portable package and unzip; drag a `.png` onto `cartconv.exe`. On non-Windows, use `node cartconv.js game.png`.

---

## 5. Start playing

1. RetroArch main menu → **Load Content**.
2. Find your `.pxt64` file and select it.
3. The first time it asks "which core" — pick **MakeCode Arcade (unofficial)** (you can tick "Remember").
4. The game starts running.

**Display spec**: 160×120 resolution, 60 fps (auto-scaled up on a handheld's small screen — normal). Audio 44.1kHz.

### Default controls

| Your gamepad / keyboard | In-game function |
|---|---|
| A button | A (jump / confirm) |
| B button | B (back / cancel) |
| D-pad ↑↓←→ | Direction |
| START | In-game menu (used by some games) |
| SELECT | Reset current game (restart) |

> **Want to remap?** While the game is running, press RetroArch's hotkey to open the **Quick Menu → Controls** to rebind — e.g. swap A/B, or make another key the menu. This core has **no "Options" page** (the Options entry in the Quick Menu is empty); all key adjustments happen in this one place.

> Tip: RetroArch itself also has a "open Quick Menu" hotkey (which may also be bound to Start/Select by default). If pressing Start also pops up the RetroArch menu, go to **Settings → Input → Hotkeys** and rebind or set Menu Toggle to none.

---

## 6. Common operations

| I want to… | How |
|---|---|
| Fast-forward / slow-motion | Just press RetroArch's fast-forward key — the core auto-speeds-up / slows-down, **no setup needed**. |
| Restart current game | Press **SELECT**, or choose "Restart Game" in the Quick Menu. Video and audio both reset. |
| Swap to another cartridge | Main menu "Load Content" → pick a new `.pxt64`; the old game exits cleanly. |
| Pause | RetroArch's built-in pause. |

> **Savestate / Rewind are NOT supported.** This is intentional — the core only does the "basic features" equivalent to the official simulator, not snapshot saves. RetroArch automatically disables those buttons; you're not doing anything wrong.

---

## 7. Something went wrong? Look here

| Symptom | Cause / fix |
|---|---|
| **All black, nothing at all** | Probably a **32-bit system/frontend** — this core doesn't support it. Switch to a 64-bit environment. |
| **My game doesn't show in the file browser** | Your file is `.png`, not `.pxt64`. Export/convert to `.pxt64` per Section 4. |
| **`VM error 1014` after loading** | A single data section in this cartridge exceeds 32000 bytes; the core **explicitly rejects** it instead of black-screening. This isn't a core bug — the cartridge is just too big. Get a VM-compatible export, or have the author split the large arrays in the game source. (Such cartridges used to silently black-screen; now at least it tells you why.) |
| **Red-on-white `VM PANIC xxxx` banner at the top** | The game crashed on its own (usually a game-code bug, e.g. using undefined as an array). Press SELECT to restart — usually recovers; the core won't take RetroArch down with it. |
| **Pressing Start also pops up the RetroArch menu** | Hotkey conflict — see the "Controls" tip above; rebind in Hotkeys settings. |
| **Can't see MakeCode Arcade in the core list** | The `.info` isn't in the right directory, or the core file didn't land in the Cores directory. Go back to Section 2. |

---

## 8. One-line summary

Download the zip for your platform → unzip → put the core and `.info` into RetroArch's matching directories → prepare a `.pxt64` cartridge → Load Content and play. Remember three things: **64-bit, `.pxt64`, remap keys in the Quick Menu's Controls**.

Enjoy!
