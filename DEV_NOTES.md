# RetroESP32 Development Notes

> Last updated: March 23, 2026
> These notes document all custom modifications made to the RetroESP32 firmware for Odroid Go.

---

## Build Environment

| Component | Path |
|-----------|------|
| ESP-IDF v3.3.1 | `C:\Users\97254\esp\v3.3.1\esp-idf` |
| Toolchain | `C:\Users\97254\esp\toolchains\xtensa-esp32-elf\bin` |
| MSYS2 | `C:\msys64` (MINGW32 subsystem) |
| Project root | `C:\ESPIDFprojects\RetroESP32-master` |

### Build Commands

**Launcher only:**
```bash
export IDF_PATH=/c/Users/97254/esp/v3.3.1/esp-idf
export PATH=/c/Users/97254/esp/toolchains/xtensa-esp32-elf/bin:/usr/bin:$PATH
cd /c/ESPIDFprojects/RetroESP32-master/Launchers/retro-esp32
make -j4
```

**Copy binary + package firmware:**
```bash
cp /c/ESPIDFprojects/RetroESP32-master/Launchers/retro-esp32/build/retro-esp32.bin \
   /c/ESPIDFprojects/RetroESP32-master/Firmware/Bins/retro-esp32.bin
cd /c/temp_build
bash build_full_fw.sh
```

**From Windows PowerShell (one-liner):**
```powershell
C:\msys64\usr\bin\env.exe MSYSTEM=MINGW32 C:\msys64\usr\bin\bash.exe -l -c "export IDF_PATH=/c/Users/97254/esp/v3.3.1/esp-idf && export PATH=/c/Users/97254/esp/toolchains/xtensa-esp32-elf/bin:/usr/bin:$PATH && cd /c/ESPIDFprojects/RetroESP32-master/Launchers/retro-esp32 && make -j4"
```

**Firmware output:** `Firmware/Releases/RetroESP32_full.fw`
**mkfw tool:** `Odroid/odroid-go-firmware/tools/mkfw/mkfw.exe`

---

## Project Structure

```
RetroESP32-master/
├── Launchers/retro-esp32/          # Main launcher (carousel + browser)
│   ├── main/
│   │   ├── main.c                  # ~2994 lines — ALL launcher logic
│   │   └── includes/
│   │       ├── declarations.h      # Forward declarations
│   │       ├── definitions.h       # WIDTH=320, HEIGHT=240, COUNT=16, etc.
│   │       └── structures.h        # SYSTEM, LIST, THEMES[22], ROMS, etc.
│   │   └── sprites/
│   │       └── systems.h           # 32×32 icon bitmaps (RGB565, const, ~105KB in flash)
│   └── components/odroid/
│       ├── odroid_display.c        # LCD driver (SPI @ 40MHz)
│       └── odroid_input.c/h        # Gamepad input
├── Emulators/
│   ├── retro-go/nesemu-go/         # NES (retro-go based)
│   ├── retro-go/gnuboy-go/         # GB/GBC (retro-go based)
│   ├── retro-go/smsplusgx-go/      # SMS/GG (retro-go based)
│   ├── stella-odroid-go/           # Atari 2600
│   ├── prosystem-odroid-go/        # Atari 7800
│   ├── handy-go/                   # Atari Lynx
│   ├── odroid-go-pcengine-huexpress/ # PC Engine
│   └── odroid-go-spectrum/         # ZX Spectrum
├── Firmware/
│   ├── Bins/                       # Compiled .bin files (incl. OpenTyrian.bin)
│   └── Releases/                   # Packaged .fw files
└── Odroid/odroid-go-firmware/tools/ # mkfw packaging tool

OpenTyrian (separate project):
  C:\ESPIDFprojects\OpenTyrian/
  ├── main/app_main.c               # FreeRTOS entry, return-to-launcher
  ├── components/OpenTyrian/         # Game engine (opentyr.c, sprite.c, varz.c, etc.)
  └── components/SDL/                # Custom SDL layer (SDL_video.c, spi_lcd.c, etc.)
```

---

## Critical Constraints

- **Display buffer:** `unsigned short buffer[40000]` — any single draw call MUST NOT exceed 40,000 pixels (80KB)
- **Launcher partition:** Max 524,288 bytes (~395KB currently used)
- **Display:** 320×240 LCD, ILI9341, SPI @ 40MHz
- **Color format:** RGB565 (16-bit)
- **Systems count:** 16 (`COUNT=16` in definitions.h) — 14 original + OpenTyrian + Atari 800

---

## Modifications Made

### 1. Emulator In-Game Menus

Added Continue/Restart/Quit overlay menus to four emulators, triggered by SELECT+START:

| Emulator | File Modified |
|----------|--------------|
| Stella (Atari 2600) | `Emulators/stella-odroid-go/main/main.c` |
| Prosystem (Atari 7800) | `Emulators/prosystem-odroid-go/main/main.c` |
| PC Engine | `Emulators/odroid-go-pcengine-huexpress/pcengine-go/main/main.c` |
| Lynx | `Emulators/handy-go/main/main.c` |

Each menu draws a semi-transparent overlay with UP/DOWN navigation and A to select.

### 2. Launcher Carousel Redesign (RetroPie-style)

**File:** `Launchers/retro-esp32/main/main.c`

Replaced the original launcher with a horizontal carousel:
- **LEFT/RIGHT** scrolls through systems with smooth animation
- **Press A** enters ROM browser or settings
- **Three modes:** Carousel (!BROWSER && !LAUNCHER), Browser (BROWSER), Launch Options (LAUNCHER)

Key functions:
- `animate(direction)` — smooth sliding animation (~line 2118)
- `restore_layout()` — positions icons, draws system name + logo/settings (~line 2158)
- `draw_carousel_screen()` — full carousel redraw (~line 2417)
- `launcher()` — main input loop (~line 2598)

### 3. System Logo Artwork

**File:** `main.c`, function `draw_system_logo()` (~line 878)

- Draws a 3× scaled (96×96) version of the current system's 32×32 icon
- Centered at x=112, y=72
- **Monochrome mode (COLOR=0):** All visible pixels rendered in intense red (0xF800)
- **Colored mode (COLOR=1):** Uses actual RGB565 color values from `_color` sprite data
- Skipped for STEP==0 (settings screen)
- Buffer usage: 9,216 pixels (safe)

### 4. Scaled Text Rendering

**File:** `main.c`, function `draw_text_scaled()` (~line 425)

- 2× scale font rendering (10×14 pixels per character)
- Used for "press a to browse" hint text below the logo
- Takes x, y, string, and color parameters

### 5. Cosmetic Defaults

| Setting | Value | Location |
|---------|-------|----------|
| Default theme | Cloud (USER=21) | `get_theme()` (~line 768) |
| Default icons | Colored (COLOR=1) | NVS fallback (~line 603) |
| Theme colors | `{65535, 33840, 16904, "cloud"}` | `structures.h` THEMES[21] |

**Note:** NVS-stored preferences are preserved. Defaults only apply on first boot or after NVS erase.

### 6. Browser Scroll Behavior

**File:** `main.c`, browser input handlers (~line 2766)

Added `BROWSER_SEL` global variable (line 24) for cursor-within-page tracking:

- **UP/DOWN:** Cursor moves through visible items. When cursor reaches top/bottom edge, the list scrolls by one item while cursor stays pinned at the edge. Wraps at boundaries.
- **LEFT/RIGHT:** Pages by BROWSER_LIMIT (12 items), cursor resets to top.
- **Header counter:** Shows `(ROMS.offset + BROWSER_SEL + 1) / ROMS.total`
- Applied to: ROM browser (`draw_browser_list`), favorites (`draw_favorites`), recents (`draw_recents`)

`BROWSER_SEL` is reset to 0 on: enter_browser, leave_browser, enter folder, exit folder, enter favorites, enter recents, LEFT/RIGHT page.

### 7. Settings Input Fix

**File:** `main.c`, LEFT/RIGHT handlers (~line 2607)

Fixed regression from carousel redesign where LEFT/RIGHT in settings (STEP==0) would navigate the carousel instead of adjusting volume/brightness:

```c
// Before (broken):
if (!SETTINGS) { /* always navigated carousel */ }

// After (fixed):
if (!SETTINGS && (STEP != 0 || SETTING == 0)) { /* carousel nav */ }
else if (STEP == 0 && !SETTINGS) { /* adjust settings values */ }
```

- When on Themes row (SETTING==0): LEFT/RIGHT navigates carousel (exits settings)
- When on Volume/Brightness/Color/Cover rows: LEFT/RIGHT adjusts values
- UP/DOWN cycles through settings items when STEP==0
- B button exits theme picker (SETTINGS=true → false)

---

## Key Global Variables (main.c)

| Variable | Type | Purpose |
|----------|------|---------|
| `STEP` | int8_t | Current system index (0=Settings, 1=Favorites, 2=Recent, 3-13=Emulators, 14=Tyrian, 15=Atari 800) |
| `BROWSER` | bool | In ROM browser mode |
| `LAUNCHER` | bool | In launch options overlay |
| `SETTINGS` | bool | In theme picker sub-screen |
| `SETTING` | int8_t | Current settings row (0=Themes, 1=Color, 2=Volume, 3=Brightness, 4=Cover, 5=Delete) |
| `BROWSER_SEL` | int | Cursor position within visible browser page (0..BROWSER_LIMIT-1) |
| `ROMS.offset` | int16_t | Index of first visible ROM in browser |
| `ROMS.total` | int16_t | Total ROM count |
| `ROMS.limit` | int | Max items per page (BROWSER_LIMIT=12 in browser, 8 in carousel) |
| `COLOR` | int8_t | 0=monochrome, 1=colored icons |
| `USER` | int8_t | Theme index (0-21) |
| `buffer[40000]` | unsigned short | Shared pixel buffer for all draw operations |

---

## Systems Map (STEP index → System)

| STEP | System | Directory |
|------|--------|-----------|
| 0 | Settings | — |
| 1 | Favorites | — |
| 2 | Recent | — |
| 3 | NES | `roms/nes` |
| 4 | Game Boy | `roms/gb` |
| 5 | Game Boy Color | `roms/gbc` |
| 6 | Sega Master System | `roms/sms` |
| 7 | Game Gear | `roms/gg` |
| 8 | ColecoVision | `roms/col` |
| 9 | ZX Spectrum | `roms/zx` |
| 10 | Atari 2600 | `roms/a26` |
| 11 | Atari 7800 | `roms/a78` |
| 12 | Atari Lynx | `roms/lnx` |
| 13 | PC Engine | `roms/pce` |
| 14 | OpenTyrian | `roms/tyrian` |
| 15 | Atari 800 | `roms/a800` |

---

## Draw Pipeline (Carousel Mode)

1. `draw_background()` — fills screen with `GUI.bg`
2. `restore_layout()` — positions system icons, draws:
   - System name at y=16
   - Icon ribbon at y=32 (32×32 icons with GAP=48)
   - If STEP==0: `draw_settings()` (settings list)
   - If STEP!=0: `draw_system_logo()` (96×96 big icon) + `draw_text_scaled()` hint
3. Battery/speaker/contrast indicators overlay

## Draw Pipeline (Browser Mode)

1. `clear_screen()` — 4× strips of 320×60
2. `draw_browser_header()` — system name + (N/Total) counter
3. `draw_browser_list()` — BROWSER_LIMIT rows, 18px each, with scrollbar

---

## Themes (structures.h)

22 themes indexed 0-21. Each: `{bg, fg, hl, name}` in RGB565.
Default: index 21 = `"cloud"` = white bg (65535), gray fg (33840), dark hl (16904).

---

## Return to Browser After Game Exit

When the user exits a game (ESP_RST_SW software restart), the launcher now returns directly to the ROM browser at the exact position where the user left off, instead of going back to the carousel.

### How it works

1. **BROWSER_SEL saved to NVS** — New NVS key `"BSEL"` (int16) stores the cursor position within the visible page. Saved alongside `STEP` and `ROMS.offset` in `set_restore_states()` before launching a game.
2. **BROWSER_SEL restored on boot** — `get_sel_state()` reads `"BSEL"` from NVS during `get_restore_states()`.
3. **Restart enters browser mode** — In `app_main()`, when `RESTART == true` (game exit detected), instead of `restore_layout()` → carousel, the code:
   - Calls `restart()` (shows "restarting" progress bar)
   - Sets `BROWSER = true`, `ROMS.limit = BROWSER_LIMIT`
   - Calls `count_files()` + `seek_files()` with the NVS-restored offset
   - Clamps `ROMS.offset` and `BROWSER_SEL` to valid ranges
   - Draws the browser screen at the saved position
4. **`restore_layout()` removed from `restart()`** — Prevents a brief carousel flash before the browser appears.

### New functions (main.c, States region)

| Function | Purpose |
|---|---|
| `get_sel_state()` | Reads `BROWSER_SEL` from NVS key `"BSEL"` |
| `set_sel_state()` | Writes `BROWSER_SEL` to NVS key `"BSEL"` |

### Forward declarations added (declarations.h)

- `get_sel_state()`, `set_sel_state()` — in States section
- `clear_screen()`, `draw_browser_header()`, `draw_browser_screen()`, `draw_browser_list()`, `browser_seek_and_draw()` — new Browser section

### NVS keys (updated)

| Key | Type | Purpose |
|---|---|---|
| `"STEP"` | int8 | System index (0-14) |
| `"LAST"` | int16 | `ROMS.offset` — page scroll position |
| `"BSEL"` | int16 | `BROWSER_SEL` — cursor within visible page (0..11) |

---

## Sorted File Listing + Last-Played First

ROM file listing is now sorted alphabetically, with the last-played ROM moved to the top of the list.

### How it works

1. **`count_files()` reads ALL filenames** into a dynamically allocated `SORTED_FILES` array (up to `MAX_FILES` entries).
2. **Alphabetical sort** via `qsort()` with case-insensitive `strcasecmp` comparator.
3. **Last-played ROM moved to front** — After sorting, the ROM whose path matches `odroid_settings_RomFilePath_get()` is swapped to index 0.
4. **`seek_files()` copies a page** from `SORTED_FILES[offset..offset+limit]` into `FILES[]` for display — no more `seekdir()`-based random access.
5. **`free_sorted_files()`** frees the array when leaving the browser.

### Key globals

| Variable | Type | Purpose |
|---|---|---|
| `SORTED_FILES` | `char**` | Heap-allocated array of sorted filenames |
| `SORTED_COUNT` | `int` | Number of entries in `SORTED_FILES` |

---

## Fix: In-Game Menu Hangs (Stella, Prosystem & PC Engine)

### Problem
Pressing MENU during gameplay in Stella, Prosystem, or PC Engine would sometimes cause the device to appear frozen/hung.

### Root Causes

**Stella (main.cpp) & Prosystem (main.c) — identical architecture:**
1. **SPI bus race condition** — `videoTask` (Core 1) writes frames via `ili9341_write_frame_atari2600()` / `ili9341_write_frame_atari7800()` while `show_game_menu()` (Core 0) writes the menu via `ili9341_write_frame_rectangleLE()`. No display mutex protects the SPI bus, so simultaneous access can deadlock the SPI peripheral.
2. **Zero-tick delay** — `vTaskDelay(10 / portTICK_PERIOD_MS)` evaluates to 0 when `portTICK_PERIOD_MS` > 10, causing a busy-spin that starves other tasks.
3. **No timeout on key-release wait** — The `do { read(); } while (buttons_held)` loop at the end of the menu had no timeout, so a stuck/bouncing button would hang forever.

**PC Engine (odroid_ui.c):**
1. **Busy-spin in `odroid_ui_wait_for_key()`** — The function had a `while(true) { read(); if (match) break; }` loop with NO delay, starving the FreeRTOS scheduler and other tasks (watchdog, WiFi, etc.).

### Fixes Applied

**Stella — `Emulators/stella-odroid-go/main/main.cpp`:**
- Before entering `show_game_menu()`, drain `vidQueue` with `xQueueReceive(..., 0)` so `videoTask` blocks on `xQueuePeek` and stops writing to SPI. Added 50 ms delay after drain to let any in-progress SPI transfer complete.
- Changed menu loop delay to `vTaskDelay(pdMS_TO_TICKS(20))` for guaranteed non-zero tick count.
- Added 100-iteration timeout (~2 seconds) to the key-release wait loop.

**Prosystem — `Emulators/prosystem-odroid-go/main/main.c`:**
- Same three fixes as Stella (identical `videoTask` / `show_game_menu()` architecture):
  - Drain `vidQueue` + 50 ms delay before entering menu.
  - `vTaskDelay(pdMS_TO_TICKS(20))` in menu input loop.
  - 100-iteration (~2 second) timeout on key-release wait.

**PC Engine — `Emulators/odroid-go-pcengine-huexpress/pcengine-go/components/odroid/odroid_ui.c`:**
- Added `usleep(20 * 1000UL)` (20 ms) per iteration in `odroid_ui_wait_for_key()` to yield CPU time.
- Added 150-iteration timeout (~3 seconds) so the function always returns.

### Key Insight
Stella and Prosystem lack a display mutex entirely — the only way to prevent SPI conflicts is to ensure `videoTask` is blocked (waiting on an empty queue) before the menu writes to the display. PC Engine properly uses `odroid_display_lock()` / `odroid_display_unlock()` around both video frames and menu drawing.

---

## Troubleshooting

- **Buffer overflow crash:** Any draw exceeding 40,000 pixels will corrupt memory. Use `clear_screen()` (4 strips) or row-by-row clearing.
- **Build warnings:** GNUMAKEFLAGS and const qualifier warnings are normal, not errors.
- **NVS persistence:** Changing defaults in code only affects first boot. To force new defaults, erase NVS partition or use `nvs_erase_all()`.
- **Partition size:** If launcher binary exceeds 524,288 bytes, it won't fit. Current size ~398KB.
- **Menu hang after fix:** If MENU still occasionally feels slow, the 50 ms drain delay in Stella or the 20 ms poll in PCE can be tuned. The timeouts (2 s / 3 s) ensure the menu always returns even if a button read is glitchy.
- **OpenTyrian PSRAM:** OpenTyrian requires PSRAM for all dynamic allocations. If it crashes with `StoreProhibited` at address 0x00000000, verify `CONFIG_SPIRAM_USE_MALLOC=y` in its sdkconfig.
- **OpenTyrian no audio:** If `SDL_AudioInit: not enough DMA RAM` appears in the log, the cache workaround has consumed too much internal DMA RAM. Audio is silently disabled — this is by design. The game plays without sound.
- **OpenTyrian game speed:** If the game runs too fast or too slow, check `CONFIG_FREERTOS_HZ` — must be 1000 for correct timing.
- **OpenTyrian display tearing:** If graphics show tearing/corruption, verify `dispDoneSem` synchronization in `spi_lcd.c` is active (not commented out).
- **OpenTyrian enemy bugs:** Do NOT modify `tyrian2.c` enemy logic. Type 13/14 event handling must stay exactly as upstream. Apparent enemy issues were caused by SPIRAM data corruption (stale cache), not event logic.
- **SPIRAM_CACHE_WORKAROUND:** Must be `=y` in OpenTyrian's sdkconfig. Without it, PSRAM reads return stale data causing subtle game corruption. The binary will be ~50 KB larger.

---

## Fix: Favorites & Recents Browser Offset (v2.8)

### Problem
Opening the Favorites or Recently Played browser could crash the device or render items off-screen.

### Root Cause
`draw_favorites()` and `draw_recents()` used carousel-style layout coordinates (y starting at `POS.y + 48 = 80`, with 20 px row spacing) instead of the browser-style layout (y starting at 20, with 18 px rows). With `BROWSER_LIMIT = 12`, items would render past y = 300, overflowing the 240 px screen and trashing memory.

### Fix
Both functions were rewritten to use the same layout as `draw_browser_list()`:
- Start at `y = 20`, row height = 18 px
- Clear rows with per-slot `draw_mask()` calls
- Scrollbar drawn within the same coordinate space

---

## ROM Count Per Emulator in Carousel (v2.9)

### Overview
Each system now shows its ROM file count in parentheses — e.g. `(42)` — centered below the small 32×32 icon in the carousel ribbon.

### Implementation

**New global:** `int ROM_COUNTS[COUNT]` — cached count per system, populated once at startup.

**New function:** `count_all_roms()` (`main.c`, ROM Counts region)
- **Index 0 (Settings):** Skipped (count = 0).
- **Index 1 (Favorites):** Counts non-empty lines in `/sd/odroid/data/RetroESP32/favorites.txt`.
- **Index 2 (Recents):** Counts non-empty lines in `/sd/odroid/data/RetroESP32/recent.txt`.
- **Index 3–13 (emulators):** Opens `/sd/roms/{dir}/`, counts files matching the system extension via `opendir` / `readdir`. No malloc, no sorting — lightweight.

**Startup call:** `count_all_roms()` is called in `app_main()` immediately after `create_settings()`, before theme/drawing code.

**Drawing:** `draw_systems()` was modified — after drawing each 32×32 icon, if `e > 0` (not settings), it renders the count text `(N)` centered below the icon at `y + 34` using `draw_text()` (5×7 font).

**Forward declaration:** `void count_all_roms();` added to `declarations.h`.

### Smearing Fix
The animation loop in `animate()` cleared the icon row with `draw_mask(0, 32, 320, 32)` — only covering y = 32–63. The count text at y = 66 (7 px tall → y = 73) was not erased between frames, causing vertical smearing during scroll. Fixed by extending the mask height from 32 to 42: `draw_mask(0, 32, 320, 42)`.

---

## Fix: Last-Played ROM Duplicate (v2.8 → v2.9)

### Original Behavior (v2.7)
The last-played ROM was **moved** from its alphabetical position to index 0 — it disappeared from its original spot.

### v2.8 Attempt
Changed to insert a **duplicate** at index 0 while keeping the original. However, the shift loop `for (j = i; j > 0; j--)` only shifted elements 0..i−1 down by one, which overwrote the matched entry at position `i` with `SORTED_FILES[i-1]`.

### v2.9 Fix
Changed the shift to `for (j = SORTED_COUNT; j > 0; j--)` which shifts **all** elements right by one (including the original at position `i`, now at `i+1`), then places the duplicate at `[0]`. The original remains in its alphabetical position (shifted by +1 due to the insertion).

---

## Safe Mode (Button A during boot)

Holding the **A button** during boot triggers safe mode. A 500 ms delay after `odroid_input_gamepad_init()` lets GPIO pull-ups settle, then `odroid_input_read_raw()` samples the A button.

**What it does:**
1. Resets the OTA boot partition back to the factory (launcher) partition — breaks out of boot loops caused by a crashing emulator.
2. Sets `STEP = 0`, `RESTART = false`, `SAFE_MODE = true` to force a clean menu start.

NVS state is **not** cleared — the launcher simply ignores any stored restart/resume state when `SAFE_MODE` is set.

**Two-tier recovery strategy:**
- **MENU button at emulator boot** → Each emulator (NES/GB/SMS/Stella/etc.) checks MENU on startup and returns to the launcher if held. This handles a single bad ROM.
- **A button at launcher boot** → Resets OTA to factory, breaking boot loops where the launcher itself would re-launch the crashing emulator.

**Location:** `Launchers/retro-esp32/main/main.c`, `app_main()`, early in initialization after `odroid_input_gamepad_init()`. The launcher is flashed to both factory (`0x10000`) and ota_0 (`0x200000`) so the factory fallback still runs the launcher.

---

## Fix: Clear Recents Not Updating ROM Count (v2.9)

### Problem
After clearing the recent list via Settings → CLEAR RECENTS, the carousel still showed the old item count `(N)` under the Recents icon.

### Fix
Added `ROM_COUNTS[2] = 0;` in `delete_recents()` immediately after truncating the file, so the cached count is zeroed and `draw_systems()` renders `(0)`.

---

## Fix: Return to Browser After Game Exit from Favorites/Recents (v2.9)

### Problem
Exiting a game that was launched from Favorites (STEP=1) or Recently Played (STEP=2) showed "no roms found" instead of the favorites/recents list.

### Root Cause
The restart handler in `app_main()` always called `count_files()`, which scans `/sd/roms/{DIRECTORIES[STEP]}`. For STEP=1 and STEP=2, `DIRECTORIES[]` is an empty string, so it opened `/sd/roms/` and found nothing matching the (also empty) extension.

### Fix
The restart handler now branches by STEP:
- **STEP == 1:** Calls `get_favorites()` → `read_favorites()` + `process_favorites()` + `draw_favorites()`
- **STEP == 2:** Calls `get_recents()` → `read_recents()` + `process_recents()` + `draw_recents()`
- **STEP >= 3:** Original `count_files()` + `seek_files()` path (emulator ROM directories)
- **STEP == 0:** Falls back to carousel (settings — shouldn't normally happen on game exit)

---

## Icon Smoothing (attempted & reverted)

A bilinear edge-blending pass was added to `draw_system_logo()` to anti-alias the 3× scaled 96×96 icons. The two-pass algorithm (horizontal then vertical) blended adjacent pixels at color boundaries using `blend565()`. The result looked too blurry on the 320×240 display, so it was reverted back to clean nearest-neighbor scaling.

---

## OpenTyrian Integration (v3.0)

### Overview

OpenTyrian — an open-source port of the DOS vertical scrolling shooter Tyrian — was integrated as the 15th carousel entry in the RetroESP32 launcher. The ESP32 port (originally by Gadget Workbench / jkirsons) uses a custom SDL layer over SPI LCD and runs game data from the SD card at `/sd/tyrian/data/`.

**Source project:** `C:\ESPIDFprojects\OpenTyrian` (separate ESP-IDF project, built independently)

### Build Setup

OpenTyrian is built with ESP-IDF v3.3.1 (same toolchain as all other emulators):

```bash
export IDF_PATH=/c/Users/97254/esp/v3.3.1/esp-idf
export PATH=/c/Users/97254/esp/toolchains/xtensa-esp32-elf/bin:/usr/bin:$PATH
cd /c/ESPIDFprojects/OpenTyrian
yes "" | make app -j4
```

The `yes ""` pipe auto-accepts any new Kconfig defaults during config generation.

**Build script:** `/c/temp_build/build_opentyrian.sh`
**Binary output:** `build/OpenTyrian.bin` (~443 KB)
**DRAM usage:** ~13 KB (7.2% of 176 KB) — most BSS is in PSRAM

### sdkconfig Key Settings

| Setting | Value | Purpose |
|---------|-------|---------|
| `CONFIG_SPIRAM_SUPPORT` | `y` | Enable 4 MB external PSRAM |
| `CONFIG_SPIRAM_USE_MALLOC` | `y` | Standard `malloc()` falls back to PSRAM when DRAM is low |
| `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` | `4096` | Allocations ≤ 4 KB stay in fast internal DRAM |
| `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` | `32768` | 32 KB always reserved for DMA/stacks |
| `CONFIG_SPIRAM_SPEED_80M` | `y` | 80 MHz PSRAM clock |
| `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` | `y` | Allows linker to place BSS in PSRAM |
| `CONFIG_SPIRAM_BANKSWITCH_ENABLE` | `y` | Bank switching for >4 MB PSRAM |

### PSRAM / Memory Architecture

OpenTyrian's game engine uses dozens of `malloc()` calls for sprites, sound, shapes, palettes, and config buffers. Total dynamic allocation exceeds internal DRAM capacity.

**Solution chain (3 fixes applied):**

1. **Linker script** (`esp-idf/components/esp32/ld/esp32.extram.bss.ld`): Moves `.bss` and `COMMON` symbols from `libOpenTyrian.a`, `libSDL.a`, `libmbedtls.a`, `libwpa_supplicant.a`, `libmdns.a`, `libmesh.a` to PSRAM via `extern_ram_seg` placement. This resolved a 460 KB DRAM overflow at link time.

2. **sdkconfig — SPIRAM_USE_MALLOC**: Initially set to `SPIRAM_USE_MEMMAP` (only maps PSRAM for static BSS, does NOT add to heap). Changed to `SPIRAM_USE_CAPS_ALLOC` (heap-caps only), then finally to `SPIRAM_USE_MALLOC` which makes standard `malloc()` transparently use PSRAM. This was necessary because the game code has dozens of `malloc()` calls that cannot all be individually changed to `heap_caps_malloc()`.

3. **SDL_video.c NULL check**: `SDL_CreateRGBSurface()` allocates pixel buffers via `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`. Added a NULL check with fallback to `MALLOC_CAP_8BIT` (internal RAM) and fixed a `memset` size bug that only cleared 1/4 of the buffer (was dividing by `sizeof(void*)` instead of using `width * height`).

### Return to Launcher

Two mechanisms ensure the user can always return to the carousel:

1. **MENU button watchdog** (`app_main.c`): A background task on Core 1 polls GPIO 13 (MENU button, active low) every 100 ms. If held for 3 seconds continuously, it sets the OTA boot partition to slot 0 (launcher) and calls `esp_restart()`.

2. **Game exit handler** (`varz.c`): `JE_tyrianHalt()` — the game's normal shutdown path — originally called `exit()`, which triggers `abort()` on ESP-IDF (no OS to exit to). Replaced with `return_to_launcher()` which saves config, cleans up, then OTA-switches to the launcher and restarts.

### Launcher Integration

**Files modified in RetroESP32-master:**

| File | Changes |
|------|---------|
| `definitions.h` | `COUNT` 14 → 15 |
| `structures.h` | Added `SYSTEMS[14]` entry: `{&tyrian, &tyrian_color, "tyrian"}` |
| `systems.h` | Added `const uint16_t tyrian[32][32]` and `tyrian_color[32][32]` icon arrays (spaceship pixel art) |
| `main.c` | Added `EMULATORS[14]`, `DIRECTORIES[14]="tyrian"`, `EXTENSIONS[14]=".dat"`, `PROGRAMS[14]=9` (→ OTA slot 9), restart handler for STEP 14, A-button handler, hint text |
| `partitions.csv` | Added `tyrian, 0, ota_9, 0x9F0000, 0x80000` (512 KB at offset ~10 MB) |
| `package_fw.sh` | Added mkfw entry: `0 25 524288 tyrian $BINS/OpenTyrian.bin` |

### Launcher DRAM Fix (const icons)

All 30 icon arrays in `systems.h` (15 systems × 2 arrays = 30 arrays of `uint16_t[32][32]`) were originally non-`const`, placing ~53 KB in `.dram0.data`. Adding OpenTyrian's two arrays pushed total DRAM past 176 KB, crashing the FreeRTOS scheduler at boot (`cpu_start.c:395: "not enough free heap"`).

**Fix:** Added `const` qualifier to all icon array declarations. This moves them from DRAM to `.flash.rodata` (SPI flash), freeing 53 KB of DRAM. Updated `SYSTEM` struct pointer types in `structures.h` to `const uint16_t (*)[32][32]`.

### Partition Layout

| Name | SubType | Offset | Size | Binary |
|------|---------|--------|------|--------|
| launcher | ota_0 (16) | 0x200000 | 512 KB | retro-esp32.bin |
| nes | ota_1 (17) | 0x280000 | 768 KB | nesemu-go.bin |
| gb | ota_2 (18) | 0x340000 | 704 KB | gnuboy-go.bin |
| sms | ota_3 (19) | 0x3F0000 | 1408 KB | smsplusgx-go.bin |
| spectrum | ota_4 (20) | 0x550000 | 576 KB | spectrum.bin |
| a26 | ota_5 (21) | 0x5E0000 | 1664 KB | stella-go.bin |
| a78 | ota_6 (22) | 0x780000 | 768 KB | prosystem-go.bin |
| lnx | ota_7 (23) | 0x840000 | 960 KB | handy-go.bin |
| pce | ota_8 (24) | 0x930000 | 768 KB | pcengine-go.bin |
| tyrian | ota_9 (25) | 0x9F0000 | 512 KB | OpenTyrian.bin |
| a800 | ota_10 (26) | 0xA70000 | 768 KB | atari800-go.bin |
| data_0 | 0x40 | 0xB30000 | ~5 MB | (spiffs/storage) |

### SD Card Structure

```
/sd/tyrian/
├── data/
│   ├── palette.dat
│   ├── tyrian.shp
│   ├── tyrian.hdt
│   └── ... (all Tyrian data files)
├── tyrian.sav        (game saves, auto-created)
├── tyrian.cfg        (game config, auto-created)
└── opentyrian.cfg    (OpenTyrian config, auto-created)
```

### Bugs Fixed During Integration

| Bug | Root Cause | Fix |
|-----|-----------|-----|
| DRAM overflow at link (460 KB) | OpenTyrian BSS too large for 176 KB DRAM | Modified `esp32.extram.bss.ld` to place BSS/COMMON in PSRAM |
| Launcher crash on boot (FreeRTOS scheduler) | Non-const icon arrays consumed 53 KB DRAM | Added `const` to all icon arrays in `systems.h` |
| `StoreProhibited` at `SDL_CreateRGBSurface` | `heap_caps_malloc(MALLOC_CAP_SPIRAM)` returned NULL (PSRAM not in heap) | Changed sdkconfig from `SPIRAM_USE_MEMMAP` to `SPIRAM_USE_CAPS_ALLOC` + added NULL check |
| `StoreProhibited` in `load_sprites` / `fread` | `malloc()` returned NULL for sprite data (DRAM exhausted) | Changed sdkconfig to `SPIRAM_USE_MALLOC` so `malloc()` falls back to PSRAM |
| `abort()` when exiting game | `JE_tyrianHalt()` called `exit()` → `_exit()` → `abort()` on ESP-IDF | Replaced `exit(code)` with `return_to_launcher()` (OTA switch + restart) |
| `memset` only clearing 1/4 of surface | `width*height/sizeof(surface->pixels)` divides by `sizeof(void*)=4` | Changed to `width * height` |

---

## Fix: Boot Logo Partial Display (v3.0)

### Problem
The 96×96 system logo (drawn by `draw_system_logo()` in `main.c`) appeared partially filled or had missing pixels depending on the current theme.

### Root Cause
The logo drawing loop tested `color != 0` to decide whether a pixel was "visible." However, in theme-relative color mapping, the background color is not always 0x0000 — it varies by theme. Pixels that happened to match the theme background were incorrectly skipped.

### Fix
Changed the visibility test to compare against the actual theme background color (`GUI.bg`) instead of literal 0. This ensures all non-background pixels are drawn regardless of theme palette.

Additionally, fixed the `PROGRAMS[]` array in `main.c` which had insufficient padding — the array needed 15 entries (one per system) but was missing trailing entries, causing undefined OTA slot values for newly added systems.

---

## Fix: Game Speed — OpenTyrian Running Too Fast (v3.1)

### Problem
OpenTyrian gameplay ran at approximately 2× intended speed. Enemy patterns, player movement, and all timed events were noticeably faster than original.

### Root Cause
`CONFIG_FREERTOS_HZ=100` in the OpenTyrian sdkconfig — this means each FreeRTOS tick is 10 ms. The game uses `SDL_Delay()` which maps to `vTaskDelay()`, but many game delays request small values (1–5 ms). With 10 ms tick granularity, a `vTaskDelay(1)` delay of 1 tick = 10 ms instead of 1 ms. However, the net effect was that all the internal timing loops resolved faster than intended because `SDL_GetTicks()` (based on `xTaskGetTickCount()`) reported time in 10 ms increments, causing the game loop to think less time had passed and run extra iterations.

### Fix
**File:** `C:\ESPIDFprojects\OpenTyrian\sdkconfig`

Changed `CONFIG_FREERTOS_HZ` from 100 to 1000:
```
CONFIG_FREERTOS_HZ=1000
```

This gives 1 ms tick granularity, matching the timing assumptions of the game engine. Gameplay speed is now correct.

---

## Fix: Graphics Tearing / Framebuffer Race (v3.1)

### Problem
The display showed visible tearing, flickering, and occasional corruption during gameplay — especially during fast-scrolling scenes with many sprites.

### Root Cause
**File:** `C:\ESPIDFprojects\OpenTyrian\components\SDL\spi_lcd.c`

The SPI LCD driver uses DMA transfers to send framebuffer data to the ILI9341 display. The original code had a `dispDoneSem` semaphore that was supposed to synchronize CPU pixel writes with DMA transfers, but it was commented out in several critical locations:

1. `xSemaphoreGive(dispDoneSem)` in the SPI post-transfer callback was commented out
2. `xSemaphoreTake(dispDoneSem, ...)` in the frame-send functions was commented out

Without synchronization, the CPU could overwrite framebuffer data while DMA was still transmitting the previous frame to the LCD, causing visual corruption.

### Fix
Restored and corrected the semaphore synchronization:

1. **SPI callback** (line ~430): Uncommented `xSemaphoreGive(dispDoneSem)` in the final transfer callback — signals that DMA is done with the buffer.
2. **Frame send functions** (lines ~448, ~460, ~471): Uncommented `xSemaphoreTake(dispDoneSem, portMAX_DELAY)` — waits for DMA to finish before writing new data.
3. **Initialization** (line ~480–481): `dispDoneSem = xSemaphoreCreateBinary()` followed by `xSemaphoreGive(dispDoneSem)` — starts in "idle" state so the first frame doesn't deadlock.

Two intermediate `xSemaphoreGive` calls (lines ~378, ~423) remain commented out — they were in non-final transfer callbacks and would have released the semaphore too early.

---

## Enemy Spawning Investigation (v3.1 → v3.2)

### Problem
Certain levels appeared to have too few enemies — the screen would go empty for extended periods, making gameplay feel broken.

### Investigation
Added diagnostic logging (`EVT SUMMARY`) counting event types in each level's event table:
- **Type 13 events** (`enemiesActive = false`): Disable background enemy spawning
- **Type 14 events** (`enemiesActive = true`): Re-enable background enemy spawning

Found that some levels have type 13 events but NO type 14 events, meaning once background spawning is disabled it never gets re-enabled for the rest of the level.

### Attempted Fixes (all reverted)
1. **`level_has_type14` flag**: Only honored type 13 in levels that also had type 14. Result: too many enemies flooding the screen in other levels.
2. **1-in-10 spawn throttle gate**: Reduced random spawns to 10% when `enemiesActive` was technically false. Result: inconsistent gameplay feel.

### Final Resolution
All custom enemy logic was **reverted to 100% upstream OpenTyrian** code. The enemy count behavior is identical to the original game — it turns out the apparently "empty" periods were caused by SPIRAM data corruption (see next section), not by the event logic itself. Once `SPIRAM_CACHE_WORKAROUND` was restored, the enemy data was read correctly and gameplay matched the original.

**Files reverted:** `tyrian2.c`, `varz.c`, `varz.h` — all custom additions (`level_has_type14`, diagnostic counters, throttle gate) removed.

---

## SPIRAM Cache Workaround Restoration (v3.2)

### Problem
Subtle data corruption in SPIRAM-stored structures (enemy tables, sprite data, level data) caused unpredictable gameplay bugs — enemies disappearing, incorrect behavior, occasional crashes.

### Root Cause
During earlier sdkconfig changes, `CONFIG_SPIRAM_CACHE_WORKAROUND` had been inadvertently disabled. Comparing the current sdkconfig against the original working version (`sdkconfig.old2`, June 2022) revealed four critical differences:

| Setting | Was (broken) | Restored to (original) | Purpose |
|---------|-------------|----------------------|---------|
| `CONFIG_SPIRAM_CACHE_WORKAROUND` | *(empty/disabled)* | `y` | Prevents stale CPU cache reads of SPIRAM data modified by DMA/peripherals |
| `CONFIG_SPIRAM_MEMTEST` | *(empty/disabled)* | `y` | Tests all 4 MB of PSRAM at boot — catches bad chips |
| `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` | `4096` | `6500` | Allocations ≤ 6500 bytes stay in fast internal DRAM |
| `CONFIG_WIFI_LWIP_ALLOCATION_FROM_SPIRAM_FIRST` | *(empty/disabled)* | `y` | WiFi/LWIP buffers prefer SPIRAM (saves internal DRAM) |

### What SPIRAM_CACHE_WORKAROUND Does
On ESP32 revision 0/1, the CPU data cache can return stale data when reading SPIRAM addresses that were recently modified by DMA or another core. The workaround inserts cache-flush instructions around critical SPIRAM accesses, ensuring the CPU always reads current data. Without it, `malloc()`'d buffers in PSRAM can silently contain corrupted/stale bytes.

### Impact
- Binary size increased from ~443 KB to ~494 KB (cache workaround adds flush instructions throughout the code)
- This consumed additional internal DMA-capable RAM, which triggered the audio DMA crash (see next section)
- All SPIRAM data corruption symptoms were resolved

---

## Fix: Audio DMA Crash — I2S / vQueueDelete(NULL) (v3.2)

### Problem
After restoring `SPIRAM_CACHE_WORKAROUND=y`, the game crashed at boot with:

```
CORRUPT HEAP: multi_heap.c:172 detected at 0x3ffe3b84
assert failed: vQueueDelete queue.c:potions
```

### Root Cause (two layers)

**Layer 1 — DMA RAM exhaustion:**
The cache workaround code consumed additional internal DRAM, leaving insufficient DMA-capable RAM for the I2S audio driver's DMA buffers. `i2s_driver_install()` internally calls `i2s_create_dma_queue()` which `malloc()`s DMA buffers — when this fails, it calls `i2s_destroy_dma_queue()` to clean up.

**Layer 2 — ESP-IDF v3.3.1 bug:**
`i2s_destroy_dma_queue()` calls `vQueueDelete(dma->queue)` on the DMA queue handle. However, when `i2s_create_dma_queue()` fails early, `dma->queue` was zero-initialized by `memset()` (i.e., NULL). `vQueueDelete(NULL)` triggers an assertion failure in FreeRTOS, crashing the system BEFORE `i2s_driver_install()` can return an error code to the caller.

This means **no amount of error checking after `i2s_driver_install()` could prevent the crash** — the abort happens inside the ESP-IDF function itself.

### Fix
**File:** `C:\ESPIDFprojects\OpenTyrian\components\SDL\SDL_audio.c`

Complete rewrite of `SDL_AudioInit()` with a defensive pre-check strategy:

1. **Pre-check DMA RAM** before calling `i2s_driver_install()`:
   ```c
   size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
   if (free_dma < 4096) {
       sdl_buffer = NULL;  // disable audio
       return;
   }
   ```
   This avoids calling `i2s_driver_install()` entirely when there isn't enough DMA RAM, sidestepping the ESP-IDF bug.

2. **Reduced DMA footprint**: Changed from 6 buffers × 1024 samples to **2 buffers × 256 samples** — reduces DMA RAM needed from ~24 KB to ~2 KB.

3. **Audio buffer uses general RAM**: `sdl_buffer` allocated with `MALLOC_CAP_8BIT` (any internal/PSRAM) instead of `MALLOC_CAP_DMA`, since only the I2S driver's internal DMA descriptors need DMA-capable RAM.

4. **Graceful error handling**: If `i2s_driver_install()` fails (returns non-ESP_OK), frees the buffer and sets `sdl_buffer = NULL` instead of crashing.

5. **NULL guards throughout**:
   - `updateTask()`: checks `sdl_buffer != NULL` before writing audio
   - `SDL_CloseAudio()`: checks `sdl_buffer != NULL` before calling `i2s_driver_uninstall()`

### Result
When DMA RAM is insufficient, audio is silently disabled and the game runs without sound. When DMA RAM is available, audio works normally. No crash in either case.

---

## Current OpenTyrian sdkconfig Summary (v3.2)

Complete list of non-default settings relevant to OpenTyrian's operation:

| Setting | Value | Notes |
|---------|-------|-------|
| `CONFIG_SPIRAM_SUPPORT` | `y` | Enable 4 MB external PSRAM |
| `CONFIG_SPIRAM_USE_MALLOC` | `y` | `malloc()` falls back to PSRAM |
| `CONFIG_SPIRAM_CACHE_WORKAROUND` | `y` | **Critical:** Prevents stale cache reads from PSRAM |
| `CONFIG_SPIRAM_MEMTEST` | `y` | Full PSRAM test at boot |
| `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` | `6500` | Small allocs stay in fast DRAM |
| `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` | `32768` | 32 KB reserved for DMA/stacks |
| `CONFIG_SPIRAM_SPEED_80M` | `y` | 80 MHz PSRAM clock |
| `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` | `y` | Linker can place BSS in PSRAM |
| `CONFIG_SPIRAM_BANKSWITCH_ENABLE` | `y` | Bank switching for >4 MB PSRAM |
| `CONFIG_WIFI_LWIP_ALLOCATION_FROM_SPIRAM_FIRST` | `y` | WiFi buffers prefer PSRAM |
| `CONFIG_FREERTOS_HZ` | `1000` | 1 ms tick — correct game timing |
| `CONFIG_TASK_WDT_CHECK_IDLE_TASK_CPU0` | *(disabled)* | Prevents watchdog spam from audio task |

---

## Paddle Potentiometer Support (v3.3 → v3.11)

### Overview

Physical paddle controller support using an external potentiometer on **GPIO36** (ADC1_CHANNEL_0). Works on both Atari 2600 (Stella) and Atari 800 emulators. Provides true analog positioning for paddle games like Breakout, Kaboom!, Warlords, Super Breakout, etc.

### Hardware Wiring

```
Potentiometer (10K recommended):
  Pin 1 (outer) ───── GND
  Pin 2 (wiper) ───── GPIO36 (SVP / ADC1_CH0)
  Pin 3 (outer) ───── 3.3V
```

**Note:** GPIO36 is input-only with no internal pull-up/pull-down. When no pot is connected, the pin floats and produces erratic readings — the auto-detection handles this.

### Why GPIO36 (not GPIO4)

Originally used GPIO4 (ADC2_CHANNEL_0), but ESP32 ADC2 is shared with WiFi RF calibration and I2S built-in DAC, causing random corruption. Migrated to ADC1 in v3.11.

### Cross-Core Thread Safety

ESP32's ADC1 SAR hardware is **not thread-safe across cores**. The gamepad task (`odroid_input_task`) runs on core 1 reading ADC1_CH6 (joyX) and ADC1_CH7 (joyY), while the emulator runs on core 0. Concurrent `adc1_get_raw()` calls from different cores corrupt readings.

**Solution:** The paddle ADC1_CH0 read is done inside `odroid_input_read_raw()` (in `odroid_input.c`) right after the joystick reads — atomically in the same execution context on core 1. The result is stored in `volatile int odroid_paddle_adc_raw`, which the emulator reads from core 0.

### Signal Processing

1. **EMA smoothing** — Exponential moving average with alpha≈0.2 (51/256 fixed-point) reduces ADC noise while staying responsive
2. **Dead zone** — Pot value only updates when it moves by ≥4 units (Atari 800) or >200 axis units (Stella), preventing stationary jitter
3. **Auto-detection at boot** — Takes 4 samples 10ms apart; if spread < 300, pot is present. Works at any pot position including 0.

### Battery Monitor Conflict

The Odroid Go battery monitor reads ADC1_CH0 every 500ms in a background task — the same channel as the paddle. Fixed by calling `odroid_input_battery_monitor_enabled_set(0)` after init to disable the background reads.

### Implementation Details

**Stella (Atari 2600):** `Emulators/stella-odroid-go/main/main.cpp`
- ADC value mapped to Stelladaptor axis range −32767..+32767
- Injected as `Event::SALeftAxis0Value` — same path as real Stelladaptor hardware
- Dead zone prevents noise from crossing the `Paddles::update()` threshold of 10
- Controller auto-detection by ROM MD5 determines if paddle input is active

**Atari 800:** `Emulators/atari800-odroid-go/main/main.cpp`
- ADC value mapped to POKEY POT range 1..228
- Written directly to `AtariPot` variable used by the emulator core
- Software paddle (d-pad with acceleration) active when no pot detected

**Shared component:** `components/odroid/odroid_input.c` (both emulators)
- `odroid_paddle_adc_raw` global updated atomically in `odroid_input_read_raw()`
- ADC1_CH0 configured with 11dB attenuation in `odroid_input_gamepad_init()`

### Key Constants

| Constant | Stella | Atari 800 | Purpose |
|----------|--------|-----------|----------|
| `PADDLE_ADC_CHANNEL` | `ADC1_CHANNEL_0` | `ADC1_CHANNEL_0` | GPIO36 |
| `PADDLE_DEAD_ZONE` | 4 (pot units) | 4 (pot units) | Suppress stationary jitter |
| `PADDLE_DETECT_SPREAD` | 300 | 300 | Max ADC spread for detection |
| EMA alpha | 0.2 (51/256) | 0.2 (51/256) | Smoothing factor |

### Files Modified (v3.11 migration)

| File | Changes |
|------|---------|
| `stella-odroid-go/components/odroid/odroid_input.c` | Added `odroid_paddle_adc_raw` global, ADC1_CH0 read in `odroid_input_read_raw()`, CH0 config in `gamepad_init()` |
| `stella-odroid-go/components/odroid/odroid_input.h` | Declared `extern volatile int odroid_paddle_adc_raw` |
| `stella-odroid-go/main/main.cpp` | ADC2→ADC1, `adc2_get_raw`→`odroid_paddle_adc_raw`, EMA+dead zone, 4-sample spread detection, battery monitor disabled |
| `atari800-odroid-go/components/odroid/odroid_input.c` | Same as Stella |
| `atari800-odroid-go/components/odroid/odroid_input.h` | Same as Stella |
| `atari800-odroid-go/main/main.cpp` | Same architecture (EMA + dead zone + atomic read) |

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| v2.8 | March 1, 2026 | Favorites/Recents browser offset fix, in-game menu hang fixes (Stella/Prosystem/PCE), last-played ROM duplicate (first attempt) |
| v2.9 | March 2, 2026 | ROM count per emulator in carousel, carousel smearing fix, last-played ROM duplicate fix (correct shift), clear recents resets ROM count, return-to-browser fix for favorites/recents (was showing "list empty"), icon smoothing attempted & reverted (too blurry) |
| v3.0 | March 2, 2026 | OpenTyrian (Tyrian) integrated as 15th carousel entry — full build, launcher integration, PSRAM fixes, return-to-launcher, safe mode, const icon arrays to free DRAM |
| v3.1 | March 4, 2026 | Game speed fix (FREERTOS_HZ=1000), graphics tearing fix (dispDoneSem synchronization in spi_lcd.c), boot logo partial display fix, PROGRAMS[] array padding fix |
| v3.2 | March 4, 2026 | Enemy spawning investigation (reverted all custom logic to upstream), SPIRAM_CACHE_WORKAROUND restored from original sdkconfig, audio DMA crash fix (pre-check + graceful disable), reduced I2S DMA to 2×256, sdkconfig aligned with original working values |
| v3.3 | March 4, 2026 | Atari 2600 paddle potentiometer on GPIO4 — ADC2 analog input mapped to Stelladaptor axis, dead zone filtering to preserve d-pad, Int16 overflow clamp fix |
| v3.4 | March 4, 2026 | New boot logo ("RETROESP32 v2.9" with 3D shadow, "Done by Claude Opus 4.6"), ROM browser partial-update optimization (only redraws changed rows instead of full list), usleep delay kept at 150ms |
| v3.5 | March 4, 2026 | Mario-style bold boot logo (emboldened font, outline ring, drop shadow, star decorations), SD card BMP logo loading (`/sd/boot_logo.bmp`), two-phase splash screen (5 sec skippable with A button → 2 sec credit screen) |
| v3.6 | March 5, 2026 | Atari 800 emulator port (libatari800 on ESP-IDF), carousel integration as STEP 15/ota_10, .xex and .atr file extensions, in-game menu with bitmap font rendering, all 11 emulators flashed |
| v3.7 | March 5, 2026 | Favorites/Recents xex/atr icon mapping fix, Fav/Recent partial-update optimization (draw_favrecent_row), Atari 800 in-game menu redesign (border fix, scale 2 text, proper layout) |
| v3.8 | March 5, 2026 | Atari 800 display artifact fix — double buffering, Screen_atari moved to internal DMA memory (matching reference impl), always-render ANTIC frames, NO_SIMPLE_PAL_BLENDING/NO_YPOS_BREAK_FLICKER dead code elimination |
| v3.9 | March 5, 2026 | Atari 2600 phosphor blending — eliminates sprite-multiplexing flicker in Asteroids and other games, always-render TIA frames, dual SPIRAM phosphor buffers |
| v3.10 | March 5, 2026 | Atari 800 save states + launcher Resume/Restart/Delete Save/Favorite integration, safe mode boot, DataSlot protocol |
| v3.11 | March 6, 2026 | Paddle ADC migrated from GPIO4/ADC2 to GPIO36/ADC1 (both Stella & Atari 800) — cross-core thread safety fix, EMA smoothing, dead zone, 4-sample spread detection, battery monitor conflict resolved |
| v3.12 | March 9, 2026 | HUD in-game menu integrated into NES/GB/SMS emulators — odroid_hud.c/h copied, Kconfig.projbuild added, CONFIG_IN_GAME_MENU_YES enabled, source patched with #ifdef blocks, launcher A-button safe mode updated (NVS clear removed, 500ms GPIO settle, raw read) |
| v3.13 | March 9, 2026 | Save hang fix — NES inverted wait condition `while(exitVideoTaskFlag)` → `while(!exitVideoTaskFlag)`, NES/SMS display mutex deadlock on save (unlock before sentinel, matching GB pattern) |
| v3.14 | March 22–23, 2026 | Safe mode A-button bail-out added to all 9 emulators, volume clamp fix (abort→clamp) in all 9 emulators, Stella -O3 optimization, frodo-go/C64 removed from docs, full FW v3.14 generated |
| v3.14.1 | March 23, 2026 | GitHub repository created (github.com/giltal/RetroESP32), project pushed with curated .gitignore, SD Card contents and firmware binaries added for flashing |

---

## GitHub Repository Setup (v3.14.1)

### Repository

**URL:** `https://github.com/giltal/RetroESP32`
**Branch:** `main`
**Pushed:** March 23, 2026

### What's Included

- All source code (Launchers, Emulators, Components, Configs, Scripts)
- Documentation (README, LICENSE, DEV_NOTES, USAGE, DIY, CODE_OF_CONDUCT)
- Asset images, fonts, sprites, icons
- Build configuration files (Makefiles, sdkconfigs, partition tables)
- Python tools (`mkfw.py`, `flash_carousel.py`)
- Arduino source files (excluding compiled `.fw`)
- **Firmware/Bins/** — all compiled `.bin` and `.fw` files for flashing
- **Firmware/Releases/** — `RetroESP32.fw`, `boot_logo.bmp`
- **SD Card/** — `SDCARD.zip`, C64 ROMs (`1541.rom`, `Basic.rom`, `Char.rom`, `Kernal.rom`)

### What's Excluded (.gitignore)

| Pattern | Purpose |
|---------|---------|
| `**/build/` | ESP-IDF and CMake build output |
| `build/` | VS / CMake build output |
| `*.o`, `*.obj`, `*.elf`, `*.exe`, `*.dll` | Compiled object files and executables |
| `*.lib`, `*.a`, `*.so`, `*.dylib` | Library files |
| `*.pdb`, `*.dSYM/`, `*.idb` | Debug symbols |
| `__pycache__/`, `*.pyc` | Python bytecode |
| `Lib/site-packages/` | Python virtual environment packages |
| `.vscode/` | IDE settings |
| `paddle_debug*.txt` | Debug log files |
| `Arduino/firmware/*.fw` | Compiled Arduino firmware |
| `Odroid/odroid-go-firmware/tools/esp32img/*.bin` | ESP32 tool binaries |
| `Assets/2.4/` | Deprecated asset version |

### Git Setup Notes

- Git installed via `winget install Git.Git` (v2.53.0)
- Repository initialized from existing project with one initial commit
- Remote: `origin` → `https://github.com/giltal/RetroESP32.git`
- Binary files (firmware `.bin`/`.fw`, SD Card `.zip`/`.rom`) are committed directly (no Git LFS)

---

## v3.4 — Boot Logo & ROM Browser Optimization

### Boot Logo

**File:** `Launchers/retro-esp32/main/sprites/logo3d.h`

Completely regenerated the boot splash logo using a Python script (`C:\temp_build\gen_logo.py`).

- **Content:** "RETROESP32" (top) + "v2.9" (below), rendered with a 5×7 bitmap font at 2× scale
- **3D shadow effect:** Three layers — dark shadow (RGB565 `0x4208`) at offset (3,3), mid shadow (`0x8410`) at (1,1), main white (`0xFFFF`) at (0,0)
- **Format:** `const uint16_t logo3d[38][155]`, RGB565 pixel data
- **Display:** Centered at (82, 80) in the `splash()` function
- **Credit line:** "Done by Claude Opus 4.6" drawn at y=130 using `draw_text()`

### ROM Browser Partial-Update Optimization

**File:** `Launchers/retro-esp32/main/main.c`

**Problem:** The ROM browser (visible when selecting an emulator and browsing ROMs) redrew the entire screen on every cursor movement — clearing and re-rendering all 12 visible rows plus the header, icons, and scrollbar. Each character was a separate SPI transfer. Combined with the 150ms input delay, this created a visibly choppy scrolling experience.

**Root Cause:** The browser code in `draw_browser_list()` performs:
1. `draw_mask()` (SPI fill) for each of 12 rows to clear them
2. `draw_media()` or `draw_folder()` (SPI icon blit) for each row
3. `draw_text()` with per-character SPI transfers for each filename
4. Scrollbar rendering

This totals ~150+ SPI transactions per cursor move.

**Fix — Partial Row Redraw:**

Added three new functions:

- **`draw_browser_row(int n)`** — Redraws a single row: clears with `draw_mask()`, draws the icon (`draw_media`/`draw_folder`), renders the filename with `draw_text()`, and updates `ROM` info if selected. Only ~15 SPI transactions per row.

- **`draw_browser_scrollbar()`** — Redraws just the scrollbar area (extracted from `draw_browser_list()` for independent use).

- **`browser_partial_update(int oldSel, int newSel)`** — Redraws only the two changed rows (old highlight removed, new highlight applied) plus the header counter. Total: ~35 SPI transactions instead of ~150+.

**Modified UP/DOWN handlers** in the browser input loop:
- When cursor moves within the visible page (no scrolling), calls `browser_partial_update(oldSel, newSel)` instead of `draw_browser_header() + draw_browser_list()`
- When scrolling occurs (offset changes), still uses full `browser_seek_and_draw()` since all filenames change

**Result:** ~4× fewer SPI transactions for within-page cursor movement, producing noticeably smoother navigation.

---

## v3.5 — Mario-Style Logo, SD Card BMP Loading & Two-Phase Splash

### Mario-Style Boot Logo

**Files:**
- `Launchers/retro-esp32/main/sprites/logo3d.h` — pixel data
- `C:\temp_build\gen_logo_mario.py` — generator script

Completely replaced the v3.4 logo with a Super Mario-inspired bold logo:

- **Font:** 5×7 bitmap font emboldened to 6×7 by OR-ing each row with itself shifted right by 1 pixel
- **Scale:** 4× upscaling (each pixel becomes a 4×4 block)
- **Outline ring:** 1-pixel dark outline around all text using 8-neighbor expansion (MID color `0x8410` / RGB565)
- **Drop shadow:** 2-pixel offset shadow layer (DARK color `0x4208` / RGB565)
- **Star decorations:** Diamond-shaped star patterns flanking each text row
- **Content:** "RETRO" (line 1) / "ESP32" (line 2) / "v2.9" (line 3)
- **Format:** `const uint16_t logo3d[100][280]`, RGB565 pixel data
- **Compositing order:** Background (0) → shadow → outline → main text (white `0xFFFF`)

### SD Card BMP Logo Loading

**File:** `Launchers/retro-esp32/main/main.c` — `load_bmp_logo()` function

Added the ability to load a custom boot logo from the SD card at `/sd/boot_logo.bmp`. If present, it replaces the built-in Mario-style logo on the splash screen.

**`static int load_bmp_logo(int *out_w, int *out_h, int *out_x, int *out_y)`:**

1. Opens `/sd/boot_logo.bmp` — returns 0 (failure) if not found
2. Reads the 54-byte BMP header and validates:
   - Magic bytes `'B'`, `'M'`
   - Bits per pixel = 24 (uncompressed RGB)
   - Compression = 0
   - Width ≤ 320, height ≤ 240
   - Total pixels (w × h) ≤ 40000 (buffer constraint)
3. Handles both bottom-up (positive height, standard) and top-down (negative height) BMPs
4. Accounts for BMP row padding: `row_pad = (4 - (row_bytes % 4)) % 4`
5. Converts BGR888 → RGB565: `((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)`
6. Auto-centers on the 320×240 display: `x = (320 - w) / 2`, `y = (240 - h) / 2`
7. Writes directly into the global `buffer[]` array
8. Returns 1 on success with dimensions and position via out parameters

**Supported format:** 24-bit uncompressed BMP, max 320×240, max 40000 total pixels.

### Two-Phase Splash Screen

**File:** `Launchers/retro-esp32/main/main.c` — `splash()` function

Refactored the splash screen into a two-phase sequence with interactive skip:

**Helper functions extracted:**

- **`static void draw_builtin_logo(void)`** — Renders `logo3d[100][280]` into `buffer[]` with 4-color mapping:
  - `0` → `GUI.bg` (background)
  - `65535` → `GUI.hl` (highlight / white)
  - `33808` → mid-color (computed by averaging RGB565 components of `GUI.hl` and `GUI.fg`)
  - Other values → `GUI.fg` (foreground)
  - Blits at position (20, 28) — centered horizontally for the 280-pixel wide logo

- **`static void wait_or_button(int ms)`** — Waits up to `ms` milliseconds, polling `odroid_input_gamepad_read()` every 50ms. Returns immediately if `ODROID_INPUT_A` is pressed.

**Phase 1 — Main Logo (5 seconds, A to skip):**
1. `draw_background()` clears the screen
2. Attempts `load_bmp_logo()` from SD card
3. If BMP found: blits `buffer[]` to LCD at the auto-centered position
4. If no BMP: calls `draw_builtin_logo()` and blits at (20, 28)
5. Draws the BUILD version string at y=225
6. Calls `wait_or_button(5000)` — user can press A to skip immediately

**Phase 2 — Credit Screen (2 seconds, fixed):**
1. `draw_background()` clears the screen
2. `draw_builtin_logo()` always shows the built-in Mario-style logo
3. Draws "Done by Claude Opus 4.6" centered at y=140
4. `sleep(2)` — fixed 2-second display (not skippable)
5. `draw_background()` clears before returning to the carousel

### Atari 800/XL/XE/5200 Emulator (NEW — March 5, 2026)

**Location:** `Emulators/atari800-odroid-go/`

Ported Peter Barrett's Atari800 emulator (from esp_8_bit Arduino/ESP32-S3 project) to ESP-IDF for Odroid Go. Uses libatari800 core v4.2.0 with Altirra OS/BASIC ROMs (no external BIOS needed).

**Source origin:** `C:\Users\97254\My Drive\ArduinoProjects\ESP32_S3\ESP32_S3_8P_GameConsole\ESP32_S3_8P_GC_MultiEmu\ESP32_S3_8P_GC_MultiEmu`

**Project structure:**
```
Emulators/atari800-odroid-go/
├── Makefile                        # PROJECT_NAME := atari800-go
├── partitions.csv                  # factory @ 0x10000, 2MB
├── sdkconfig                       # Copied from prosystem-odroid-go
├── main/
│   ├── main.cpp                    # ~710 lines — ESP-IDF glue layer
│   ├── component.mk
│   └── emu_atari800_ref.cpp        # Reference only (excluded from build)
└── components/
    ├── atari800/                    # 86 files — libatari800 core
    │   ├── config.h                # Build config (R_SERIAL disabled)
    │   └── component.mk           # rdevice.o excluded
    └── odroid/                     # 17 files — HAL from prosystem
```

**Binary size:** 781,920 bytes (~764 KB) — fits in 2MB partition
**ELF sections:** text=300KB, data=476KB (palettes+ROM), BSS=15KB (PAGED_ATTRIB saves 64KB)

**Build command:**
```powershell
$env:IDF_PATH = "C:\Users\97254\esp\v3.3.1\esp-idf"
$env:PATH = "C:\Users\97254\esp\toolchains\xtensa-esp32-elf\bin;C:\msys64\usr\bin;" + $env:PATH
cd C:\ESPIDFprojects\RetroESP32-master\Emulators\atari800-odroid-go
make -j4
```

**Key architecture decisions:**
- **main.cpp** (not main.c) — C++ linkage required because all libatari800 core files are .cpp
- **PAGED_ATTRIB** enabled in config.h — uses function pointer maps (2KB) instead of 64KB MEMORY_attrib array
- **Pre-allocation** — Screen_atari (92KB) in internal DMA memory (MALLOC_CAP_DMA with SPIRAM fallback), MEMORY_mem (64KB+4) in SPIRAM, under_atarixl_os (16KB), under_cart809F/A0BF (8KB each) in SPIRAM, two framebuffers (77KB each) in SPIRAM for double buffering — all allocated before libatari800_init()
- **MEMORY_mem** in SPIRAM — 64KB+4 via heap_caps_malloc(MALLOC_CAP_SPIRAM). Originally used MALLOC_CAP_32BIT which returned IRAM (0x400xxxxx) — IRAM only supports 32-bit aligned access, but MEMORY_dPutByte does byte writes → LoadStoreError crash
- **Altirra OS** — EMUOS_ALTIRRA=1 in config.h, no external BIOS ROMs needed
- **R_SERIAL disabled** — rdevice.cpp excluded (requires serial/network, neither available)

**Platform interface (main.cpp provides):**
- `PLATFORM_Keyboard()` → returns INPUT_key_code
- `PLATFORM_PORT(num)` → reads _joy[] arrays (set in emu_step from gamepad)
- `PLATFORM_TRIG(num)` → reads _trig[] arrays
- `LIBATARI800_Mouse()` → stub (no mouse on Odroid Go)
- `LIBATARI800_Input_Initialise()` → returns TRUE
- `Sound_desired` global — 15720 Hz, 8-bit, mono
- `AtariPot` global — paddle potentiometer value (228 = centered)

**Video pipeline:**
Screen_atari[384×240] indexed → crop SRC_X_START=32 to 320×240 → memcpy to framebuffer → vidQueue → ili9341_write_frame_atari7800(fb, rgb565_palette)

**Audio pipeline:**
Sound_Callback(8bit unsigned) → convert to 16-bit signed stereo → odroid_audio_submit via I2S

**Input mapping (emu_step):**
- D-pad → _joy[0] (forward/back/left/right)
- A button → _trig[0] (fire)
- B button → toggle OPTION via INPUT_key_consol
- START → toggle START via INPUT_key_consol
- SELECT → triggers game menu
- MENU → show_game_menu overlay (Continue/Restart/Quit)

**ROM loading:**
- `map_file()` / `unmap_file()` implemented in cartridge.cpp — heap-based SPIRAM allocation
- Supports .xex, .atr, .rom, .car, .bin, .a52 formats
- Auto-detects machine type from file extension (XL for .xex/.atr, 5200 for .a52)

**Modifications to source files (from original Arduino code):**
1. `cartridge.cpp` — Removed `#include "..\emu.h"`, added `#include "esp_heap_caps.h"`, added map_file/unmap_file implementations
2. `memory.cpp` — Removed `#include <Arduino.h>`, removed `#include "ESP32_S3_8P_GameConsole.h"`, replaced two digitalRead() calls with standard PIA/constant
3. `util.cpp` — Removed `#include "esp32-hal.h"`, replaced `delayMicroseconds()` with `usleep()`
4. `afile.cpp` — Changed `#include "..\config.h"` to `#include "config.h"`
5. `sio.cpp` — Changed `#include "..\config.h"` to `#include "config.h"`
6. `config.h` — Disabled `R_SERIAL`
7. `pokeysnd.cpp` / `pokeysnd.h` — Restored `POKEYSND_sampbuf_val` and `POKEYSND_sampbuf_cnt` from bare pointers (NULL, never allocated) back to static arrays `[POKEYSND_SAMPBUF_MAX]` (1024 entries each). The Arduino port converted them to pointers to save BSS, but never allocated them, causing StoreProhibited crash at EXCVADDR=0x00000000 during first emulation frame.

**Runtime crashes fixed:**
1. **LoadStoreError in MEMORY_dPutByte** — `MEMORY_mem` allocated with `MALLOC_CAP_32BIT` returned IRAM address. IRAM only supports 32-bit aligned access; byte writes via `MEMORY_mem[x] = y` crash. Fixed by changing to `MALLOC_CAP_SPIRAM`.
2. **StoreProhibited in Update_serio_sound_rf** — `POKEYSND_sampbuf_val` and `POKEYSND_sampbuf_cnt` were NULL pointers (declared as `UBYTE *` / `int *` but never malloc'd). Crash backtrace: `Update_serio_sound_rf → POKEY_PutByte → CPU_GO → ANTIC_Frame → LIBATARI800_Frame → libatari800_next_frame → emu_step → app_main`. Fixed by restoring them as static arrays.

**sdkconfig notes:**
- `CONFIG_ESPTOOLPY_PORT="COM20"` — flash port
- `CONFIG_ESP32_PANIC_PRINT_REBOOT=y` — readable crash output (not GDB stub)
- `CONFIG_SPIRAM_SUPPORT=y`, `CONFIG_SPIRAM_USE_CAPS_ALLOC=y` — explicit PSRAM allocation

**Status:** Running on hardware at ~52 FPS sustained. Fully integrated into launcher carousel as STEP 15 / ota_10. Supports .xex and .atr file extensions. In-game menu with bitmap font rendering (Continue/Restart/Quit). Display artifacts fixed in v3.8 (Screen_atari in internal DMA memory + double buffering + always-render ANTIC frames).

---

## v3.6 — Atari 800 Carousel Integration & Full Flash

### Carousel Integration

**File:** `Launchers/retro-esp32/main/main.c`

Atari 800 added as the 16th carousel entry (STEP 15, index 15). Changes:

- **`COUNT`** incremented from 15 → 16 in `definitions.h`
- **`EMULATORS[15]`** = `"Atari 800"`, **`DIRECTORIES[15]`** = `"a800"`, **`EXTENSIONS[15]`** = `"xex"`
- **`PROGRAMS[15]`** = `"atari800-go"` in the launcher program array
- **Carousel sprites:** `consoles/a800.raw` and `logos/a800.raw` added (generated as GIMP .raw exports, 124×84 and 60×18, RGB565 big-endian)
- **`partitions.csv`:** New `ota_10` at offset `0xA70000` (768 KB), `data_0` moved from `0xA70000` to `0xB30000`

### .atr Extension Support

**File:** `Launchers/retro-esp32/main/main.c`

Added Atari 800 disk image (.atr) support alongside .xex:

- **`matches_rom_extension()`** helper function — returns 1 if a filename ends with `.xex` or `.atr` (case-insensitive). Used in ROM browsing to filter files for the Atari 800 system.
- **`get_application()`** — maps both `.xex` and `.atr` extensions to `ota_10` so the launcher boots the correct emulator partition regardless of file type.

### In-Game Menu — Bitmap Font Rendering

**File:** `Emulators/atari800-odroid-go/main/main.cpp`

The Atari 800 emulator doesn't link against the launcher's GPU/text rendering library, so a self-contained in-game menu was implemented:

- **`font5x7[]`** — 95-character 5×7 bitmap font stored as `uint8_t[95][7]`, covering ASCII 32-126
- **`draw_char_scaled()`** — renders a single character to the framebuffer at arbitrary position and scale, with foreground/background RGB565 colors
- **`draw_text_scaled()`** — renders a null-terminated string using `draw_char_scaled()`, character spacing = `6 * scale` pixels
- **`text_width()`** — calculates pixel width of a string at given scale

**Menu overlay (`show_game_menu`):**
- Box at (60,40)→(260,185), dark blue fill (`0x0010`), white border
- Title "ATARI 800" at scale 2, centered horizontally
- Separator line below title
- Options: Continue / Restart / Quit — scale 2, 30px vertical spacing
- Yellow highlight (`0xFFE0`) with black text for selected option, white text for unselected
- Border drawn AFTER fill to prevent title bar overwriting the top border line
- Navigation: D-pad up/down, A to select, B to cancel (returns to game)

### Full Flash — All 10 Emulators

All emulator binaries flashed to the 16MB flash via esptool at 2000000 baud on COM17:

| Partition | Offset | Binary |
|-----------|--------|--------|
| launcher (ota_0) | 0x200000 | retro-esp32.bin |
| nes (ota_1) | 0x280000 | nesemu-go.bin |
| gb (ota_2) | 0x340000 | gnuboy-go.bin |
| sms (ota_3) | 0x3F0000 | smsplusgx-go.bin |
| spectrum (ota_4) | 0x550000 | spectrum.bin |
| a26 (ota_5) | 0x5E0000 | stella-go.bin |
| a78 (ota_6) | 0x780000 | prosystem-go.bin |
| lnx (ota_7) | 0x840000 | handy-go.bin |
| pce (ota_8) | 0x930000 | pcengine-go.bin |
| tyrian (ota_9) | 0x9F0000 | OpenTyrian.bin |
| a800 (ota_10) | 0xA70000 | atari800-go.bin |

---

## v3.7 — Favorites/Recents Fixes & Menu Redesign

### Favorites/Recents Icon Mapping Fix

**File:** `Launchers/retro-esp32/main/main.c`

**Problem:** Favorites and Recents browsers didn't display icons for .xex and .atr files. The extension-to-icon offset mapping in `draw_favorites()`, `draw_recents()`, and `draw_launcher()` only covered the original 14 emulator file types.

**Fix:** Added `xex` and `atr` entries to all three icon lookup maps, both mapping to offset `7*16` (Atari 2600 icon slot, which is the closest visual match for Atari 800 content).

### Favorites/Recents Partial-Update Optimization

**File:** `Launchers/retro-esp32/main/main.c`

**Problem:** The favorites and recents browsers (like the ROM browser before v3.4) performed a full 12-row redraw on every cursor move, causing choppy scrolling. `process_favorites()` / `process_recents()` cleared and re-rendered all visible rows plus header, icons, and scrollbar on each input event.

**Fix — Two new functions:**

- **`draw_favrecent_row(int n, int sel, char list[][256], int count, int offset)`** — Redraws a single favorite/recent row: clears background with `draw_mask()`, draws the file-type icon (with extension-to-offset mapping including xex/atr), renders the truncated filename with `draw_text()`, and highlights the selected row.

- **`favrecent_partial_update(int oldSel, int newSel, char list[][256])`** — Redraws only the old selection row (to remove highlight) and the new selection row (to add highlight), plus updates the header counter. Total: ~30 SPI transactions instead of ~150+.

**Modified UP/DOWN handlers** in the favorites/recents navigation:
- When cursor moves within the visible page (no scrolling or wrapping), calls `favrecent_partial_update()` instead of full `process_favorites()` / `process_recents()`
- When page scrolling or wrap-around occurs, still uses the full redraw path

### Atari 800 In-Game Menu Redesign

**File:** `Emulators/atari800-odroid-go/main/main.cpp`

**Problem (iteration 1):** Initial menu had oversized text, missing upper border frame.

**Problem (iteration 2):** After switching to scale 1, text was too small to read on the 320×240 display.

**Final solution:**
- Scale 2 for both title and menu options (readable on small screen)
- Box expanded to (60,40)→(260,185) to accommodate scale-2 text
- Option spacing increased to 30px for proper vertical separation
- Fill drawn first (by0+1 to by1-1, bx0+1 to bx1-1), then white border lines drawn on top — prevents the fill loop from overwriting the top border pixel row

---

## v3.8 — Atari 800 Display Artifact Fix

### Problem
Caverns of Mars (and potentially other graphically intensive Atari 800 games) showed persistent visual artifacts — corrupted pixels, flickering scanlines, and rendering glitches. An initial double-buffering fix reduced tearing but did not eliminate the core rendering artifacts.

### Root Cause Investigation

Extensive analysis of the ANTIC/GTIA rendering pipeline (antic.cpp, gtia.cpp) and comparison with Peter Barrett's reference implementation (emu_atari800_ref.cpp) revealed three contributing factors:

1. **Screen_atari in SPIRAM** — The framebuffer `Screen_atari` (384×240 = 92,160 bytes) was allocated in external SPIRAM via `heap_caps_malloc(MALLOC_CAP_SPIRAM)`. ANTIC rendering performs thousands of scattered UWORD and ULONG writes per frame through macros like `WRITE_VIDEO` and `WRITE_VIDEO_LONG`. SPIRAM access goes through the ESP32's SPI cache, which on revision 0/1 chips is susceptible to cross-core cache-coherency issues — Core 0 writes pixels while Core 1 reads them for LCD transfer. The reference implementation (`emu_atari800_ref.cpp`) allocates Screen_atari from `MALLOC_CAP_DMA` (internal SRAM), avoiding these issues entirely.

2. **Frame skip state divergence** — When `atari800_draw_frame = 0` (frame skip), `ANTIC_Frame()` takes a different code path: it skips `GTIA_NewPmScanline()`, `draw_antic_ptr()` dispatch, and `scrn_ptr` advancement. While theoretically correct (no pixels written = no pointer needed), this creates state divergence between rendered and skipped frames, which can cause visual discontinuities in games that rely on frame-precise ANTIC/GTIA register changes.

3. **Dead code paths** — PAL blending (`ANTIC_pal_blending`) and YPOS break flicker (`break_ypos`) code was compiled in but inactive. While unlikely to cause bugs, the dead branches add unnecessary code complexity and minor runtime overhead.

### Fixes Applied

**Fix 1 — Double Buffering (prior iteration)**

**File:** `Emulators/atari800-odroid-go/main/main.cpp`

Added two 76,800-byte SPIRAM framebuffers (`framebuffer[0]` and `framebuffer[1]`) with alternating index (`fb_index ^= 1`). Core 0 writes the cropped frame to `framebuffer[fb_index]` while Core 1's `videoTask` reads from the previously completed buffer via `vidQueue`. This eliminates the race condition where both cores accessed the same buffer simultaneously.

```c
static uint8_t* framebuffer[2] = {NULL, NULL};
static int fb_index = 0;
// After crop: push framebuffer[fb_index] to vidQueue, then fb_index ^= 1
```

**Fix 2 — Screen_atari in Internal DMA Memory**

**File:** `Emulators/atari800-odroid-go/main/main.cpp`

Changed Screen_atari allocation from SPIRAM to internal DMA-capable memory, matching the reference implementation:

```c
// Before:
Screen_atari = (UBYTE*)heap_caps_malloc(Screen_SIZE, MALLOC_CAP_SPIRAM);

// After:
Screen_atari = (UBYTE*)heap_caps_malloc(Screen_SIZE, MALLOC_CAP_DMA);
if (!Screen_atari) {
    printf("Screen_atari: DMA alloc failed (%d bytes), falling back to SPIRAM\n", Screen_SIZE);
    Screen_atari = (UBYTE*)heap_caps_malloc(Screen_SIZE, MALLOC_CAP_SPIRAM);
}
```

Internal SRAM is ~3× faster for scattered writes and avoids the SPIRAM cache-coherency issues. A SPIRAM fallback is included in case internal DRAM is exhausted (with a diagnostic log message).

**Fix 3 — Always-Render ANTIC Frames**

**File:** `Emulators/atari800-odroid-go/main/main.cpp`

Changed frame skip logic so ANTIC always renders pixels, but the LCD push is throttled:

```c
// Before:
atari800_draw_frame = draw_this_frame;  // 0 or 1 alternating

// After:
atari800_draw_frame = 1;  // ANTIC always renders
bool push_this_frame = draw_this_frame;  // LCD push every other frame
```

The `push_this_frame` flag controls whether the rendered frame is cropped into the framebuffer and pushed to `vidQueue`. ANTIC's internal state (scrn_ptr, PMG overlays, color registers) stays consistent every frame, eliminating state divergence artifacts.

**Fix 4 — Dead Code Elimination**

**File:** `Emulators/atari800-odroid-go/components/atari800/config.h`

Added two defines to compile out inactive code paths:

```c
#define NO_SIMPLE_PAL_BLENDING 1   // We run NTSC only — no PAL blend post-pass
#define NO_YPOS_BREAK_FLICKER 1    // Debug feature — break_ypos=999, never triggers
```

`NO_SIMPLE_PAL_BLENDING` removes the PAL color-blending loop in `ANTIC_Frame()` that iterates over Screen_atari after each scanline (guarded by `ANTIC_pal_blending`, which is always 0 for NTSC). `NO_YPOS_BREAK_FLICKER` removes the YPOS break flicker feature (used for debugging, `break_ypos` is set to 999 so the check `ypos == break_ypos - 1` never matches).

### Key Insight

The reference implementation (`emu_atari800_ref.cpp` by Peter Barrett / retro-go) places Screen_atari in internal DMA memory — this was the single most impactful difference. The ESP32's SPIRAM cache behavior on revision 0/1 chips causes subtle corruption when one core writes scattered pixels and another core reads them for DMA transfer, even with proper software synchronization. Moving to internal SRAM eliminates this hardware-level issue.

### Result
All visual artifacts in Caverns of Mars (and other tested games) are resolved. Display is smooth and clean.

---

## v3.9 — Atari 2600 Phosphor Blending (Flicker Fix)

### Problem
Atari 2600 games like Asteroids use sprite multiplexing — the TIA can only display a few hardware sprites per scanline, so games alternate which objects are drawn on odd vs. even frames. On a CRT, phosphor persistence naturally blends consecutive frames together, making all objects appear solid. On the Odroid Go's LCD, each frame is displayed discretely, causing severe flickering — asteroids and the player ship visibly blink on and off.

### Root Cause Analysis

Three issues compounded the flicker:

1. **Irregular render pattern** — The original `renderTable` pattern was `TFFTTFFT` (3 renders per 8 frames = 37.5% display rate). This uneven cadence meant the LCD sometimes showed two frames from the same TIA phase, providing no blending benefit.

2. **RenderFlag gates TIA pixel rendering** — `TIA::updateFrame()` (TIA.cpp line 1050) checks `if(clocksToUpdate != 0 && (RenderFlag))`. When `RenderFlag` is false, TIA skips all pixel writes to the framebuffer. The buffer retains stale data from the previous rendered frame. Any attempt to blend by reading the framebuffer on non-render frames just reads the same old data.

3. **No frame accumulation** — Each displayed frame contained only one TIA phase's objects. Without accumulating pixels from consecutive frames, sprite-multiplexed objects were inherently invisible 50% of the time.

### Fixes Applied

**File:** `Emulators/stella-odroid-go/main/main.cpp`

**Fix 1 — Always render TIA frames:**

```c
// Before:
RenderFlag = renderTable[frame & 7];  // false on 5 of 8 frames

// After:
RenderFlag = true;  // TIA always renders fresh pixel data
```

This ensures TIA writes fresh pixel data to the framebuffer every frame, so the phosphor blend always sees current object positions from both odd and even frames.

**Fix 2 — Phosphor blending with dual SPIRAM buffers:**

Two 40,000-byte buffers allocated in SPIRAM (`phosphorBuf[0]` and `phosphorBuf[1]`). Each frame, non-black pixels from TIA's framebuffer are accumulated into the active phosphor buffer:

```c
for (int i = 0; i < visibleSize; i++) {
    if (fb[i]) pbuf[i] = fb[i];  // non-black pixels overwrite
}
```

Every 2nd frame (`pushThisFrame = (frame & 1) == 1`), the accumulated buffer is sent to the LCD, then the other buffer becomes active and is cleared. Since two consecutive TIA frames contribute to each displayed image, objects from both alternating phases appear in every LCD frame.

**Fix 3 — Consistent 50% display rate:**

LCD push every 2nd frame (30fps) instead of the irregular 37.5% pattern. Each displayed frame always accumulates exactly one odd + one even TIA frame.

### TIA Single Buffer Preserved

An intermediate attempt to restore TIA's dual-buffer mode (`myPreviousFrameBuffer` as a separate allocation) caused asteroids to disappear after a few seconds. The ESP32 Stella port's TIA was deliberately designed with `myPreviousFrameBuffer = myCurrentFrameBuffer` (aliased pointers) — `endFrame()` and other TIA methods assume both point to the same memory. Phosphor blending uses its own independent SPIRAM buffers instead, leaving TIA internals untouched.

### Memory Impact

| Buffer | Size | Location | Purpose |
|--------|------|----------|--------|
| `phosphorBuf[0]` | 40,000 bytes | SPIRAM | Accumulation buffer A |
| `phosphorBuf[1]` | 40,000 bytes | SPIRAM | Accumulation buffer B |
| TIA framebuffer | 51,200 bytes | Internal DRAM | Single buffer (unchanged) |

**Net DRAM change:** 0 bytes (phosphor buffers in SPIRAM only)

### Flash Offset Fix

The Atari 2600 partition (`a26`) is at offset `0x5E0000` in the device's partition table (ota_5 / subtype 0x15). An initial flash attempt used `0x510000` (stale offset from DEV_NOTES), causing `esp_image: image at 0x5e0000 has invalid magic byte` and an `abort()` crash. Corrected to `0x5E0000` after reading the boot log partition table.

### Result
Asteroids and other sprite-multiplexing games now display all objects solidly with no visible flicker. Non-multiplexing games are unaffected — the phosphor blend is transparent when consecutive frames draw the same objects (the non-black overwrite produces the same output).

---

## v3.10 — Atari 800 Save States + Launcher Integration

### Features

1. **In-game save/load states** — Full machine state (CPU, ANTIC, GTIA, PIA, POKEY, CARTRIDGE, SIO) serialized to SD card via the existing libatari800 `StateSav` infrastructure.
2. **Launcher ROM-browser integration** — Resume / Restart / Delete Save / Favorite options when a `.sav` file exists (matches NES/GB/SMS pattern).
3. **Safe mode boot escape** — Hold A during emulator boot to bail out to launcher (prevents bad ROM restart loops).

### Save State Implementation

**File:** `Emulators/atari800-odroid-go/main/main.cpp`

The libatari800 core already has complete state serialization (`StateSav_SaveAtariState` / `StateSav_ReadAtariState` in `statesav.cpp`). With `LIBATARI800=1` defined, these use in-memory buffers (`mem_open`/`mem_write`/`mem_close`) that serialize into a `UBYTE*` buffer — but `mem_close` was a no-op (never wrote to disk).

**Approach:** Allocate a 210 KB buffer in SPIRAM, call `LIBATARI800_StateSave()` to serialize, then `fwrite` the buffer to SD. Reverse for load.

```c
// SaveState: serialize → write to SD
LIBATARI800_StateSave(statesav_buffer, &statesav_tags);
ULONG used = StateSav_Tell();
FILE *f = fopen(save_path, "wb");
fwrite(statesav_buffer, 1, used, f);
fclose(f);

// LoadState: read from SD → deserialize
fread(statesav_buffer, 1, st.st_size, f);
LIBATARI800_StateLoad(statesav_buffer);
```

**Save path:** `/sd/odroid/data/a800/{romname}.sav` (created via `ensure_save_dir()` with `mkdir()` calls).

### In-Game Menu

The pause menu (MENU button) now has 5 options:

| # | Option | Action |
|---|--------|--------|
| 0 | Continue | Resume playing |
| 1 | Save & Continue | Save state to SD, resume |
| 2 | Save & Quit | Save state, return to launcher |
| 3 | Restart Game | Cold restart (DataSlot=0) |
| 4 | Quit to Menu | Return to launcher without saving |

Menu box dimensions adjusted from 200×145 to 220×180 to fit 5 options with scale-2 text.

### DataSlot Resume/Restart Protocol

**Problem:** The original `esp_restart()` for "Restart Game" just rebooted the emulator, which auto-loaded the save — so it never actually restarted.

**Solution:** Use `odroid_settings_DataSlot_set()` (NVS key "DataSlot" in "Odroid" namespace) as a resume/restart signal between launcher and emulator:

| DataSlot value | Meaning |
|---|---|
| 0 | Fresh start — skip auto-load |
| 1 | Resume — auto-load saved state |

**Launcher changes** (`Launchers/retro-esp32/main/main.c`):
- `rom_run()` → sets `DataSlot=0` (fresh start)
- `rom_resume()` → sets `DataSlot=1` (resume from save)

**Emulator changes:**
- On boot: reads `DataSlot` — loads save only if `DataSlot==1`
- Clears `DataSlot=0` after reading (prevents crash-resume loops)
- `SaveState()` sets `DataSlot=1` (so next boot auto-loads)
- "Restart Game" sets `DataSlot=0` before `esp_restart()`

### Launcher Save Directory Fix

Added `get_save_subdir()` helper to correctly map ROM extensions to save directories when browsing favorites or recents (STEP==1 or STEP==2):

```c
const char* get_save_subdir() {
    if (STEP != 1 && STEP != 2) return DIRECTORIES[STEP];
    // For favorites/recents, look up directory by extension
    for (int i = 3; i < COUNT; i++) {
        if (strlen(EXTENSIONS[i]) > 0 && ext_eq(ROM.ext, EXTENSIONS[i]))
            return DIRECTORIES[i];
    }
    if (ext_eq(ROM.ext, "atr")) return "a800";
    return ROM.ext;
}
```

This fixes `has_save_file()` and `rom_delete_save()` for `.xex`/`.atr` files accessed from favorites/recents, which previously used the raw extension instead of the `"a800"` directory name.

### Safe Mode Boot

Hold button A during emulator startup to immediately return to the launcher carousel. Checked right after display init, before SD card or ROM loading:

```c
odroid_gamepad_state bail;
odroid_input_gamepad_read(&bail);
if (bail.values[ODROID_INPUT_A]) {
    odroid_system_application_set(0);
    esp_restart();
}
```

### Config Fix

Disabled `HAVE_GETCWD` in `config.h` — the `getcwd()` function doesn't exist in ESP32's newlib, causing a linker error when `statesav.cpp` was pulled in:

```c
// Before:
#define HAVE_GETCWD 1
// After:
/* #define HAVE_GETCWD 1 */  /* ESP32: not available in newlib */
```

### Memory Impact

| Buffer | Size | Location | Purpose |
|--------|------|----------|---------|
| `statesav_buffer` | 210,000 bytes | SPIRAM | State serialization buffer |

**Net DRAM change:** 0 bytes (buffer in SPIRAM only)

### Files Modified

| File | Changes |
|------|---------|
| `Emulators/atari800-odroid-go/main/main.cpp` | SaveState/LoadState, menu update, DataSlot protocol, safe mode boot |
| `Emulators/atari800-odroid-go/components/atari800/config.h` | Disabled `HAVE_GETCWD` |
| `Launchers/retro-esp32/main/main.c` | DataSlot set in rom_run/rom_resume, `get_save_subdir()` helper, save dir fixes |

---

## v3.11 — Stella Save States & UI Fixes

### Stella Save State Support

Added full save/load state support to the Stella Atari 2600 emulator, using Stella's built-in `Serializer` API (file-based mode). Save files are written to the SD card under `/sd/odroid/data/a26/<romname>.sav`.

**Key functions added to `main.cpp`:**

```cpp
static char save_path[256] = "";

static void ensure_save_dir(void);    // Creates /sd/odroid/data/a26/ if missing
static void build_save_path(const char *romfile);  // Extracts ROM name → save path

static bool SaveState(void) {
    string savefile(save_path);
    Serializer ser(savefile);       // File-based, read/write mode
    ser.reset();
    console->save(ser);             // Serializes System + Controllers + Switches
    odroid_settings_DataSlot_set(1); // Mark for auto-load on next boot
}

static bool LoadState(void) {
    string savefile(save_path);
    Serializer ser(savefile, true);  // File-based, readonly mode
    ser.reset();
    console->load(ser);
}
```

**Auto-load on startup:** After `stella_init()`, if `DataSlot==1` (set by a previous save or launcher "Resume"), the saved state is automatically loaded. `DataSlot` is then cleared to `0` to prevent crash–resume loops.

**C++ build fix:** The initial `Serializer ser(string(save_path))` declaration hit the "most vexing parse" — C++ parsed it as a function declaration rather than a variable. Fixed by splitting into two lines:
```cpp
string savefile(save_path);
Serializer ser(savefile);
```

### Stella 5-Option In-Game Menu

Expanded the in-game menu from 3 items (Continue / Restart / Quit) to 5 items:

| # | Option | Action |
|---|--------|--------|
| 0 | Continue | Resume emulation |
| 1 | Save & Continue | `SaveState()`, then resume |
| 2 | Save & Quit | `SaveState()`, restart to launcher |
| 3 | Restart Game | Clear `DataSlot=0`, restart emulator |
| 4 | Quit to Menu | Restart to launcher without saving |

**Menu layout fix:** The original box (y: 40–180) with 28px item spacing couldn't fit 5 items (last item at y=152+28=180, clipped). Enlarged box to y: 30–210 with 26px spacing so all 5 options are comfortably visible.

Menu rendered via uGUI into a SPIRAM-allocated framebuffer, pushed to LCD with `ili9341_write_frame_rectangleLE()`.

### Stella Title Font Fix

Changed ROM selection screen title from `"SELECT A FILE"` with `FONT_10X16` to `"SELECT ROM"` with `FONT_8X12` — smaller, cleaner, and consistent with other emulators.

### Launcher File Counter Overflow Fix

The ROM browser's file counter text (e.g., `(3/128)`) was overflowing past the right edge of the screen. Root cause: width calculation used `strlen(count) * 5`, but the bitmap font actually advances **7px per non-space character** and 3px per space.

**Fixed in three functions:**

| Function | Purpose |
|----------|---------|
| `draw_numbers()` | Draws counter in carousel view |
| `delete_numbers()` | Erases counter in carousel view |
| `draw_browser_header()` | Draws counter in browser/list view |

**Before:**
```c
int w = strlen(count) * 5;
```

**After:**
```c
int w = 0;
for (const char *p = count; *p; p++) w += (*p == ' ') ? 3 : 7;
```

Also adjusted `draw_browser_header()` base x-position from 316 to 311 for better right-margin alignment.

### Flash Address Reference

During testing, launcher was initially flashed to `0x10000` (factory partition) instead of its correct address `0x200000` (ota_0), causing no visible effect. Confirmed correct addresses:

| Binary | Flash Address | Partition |
|--------|--------------|-----------|
| `retro-esp32.bin` (launcher) | `0x200000` | ota_0 |
| `stella-go.bin` (Stella) | `0x5E0000` | ota_5 |
| Factory/recovery | `0x10000` | factory |

### Files Modified

| File | Changes |
|------|---------|
| `Emulators/stella-odroid-go/main/main.cpp` | Save/Load state, 5-option menu, enlarged menu box, title font change |
| `Launchers/retro-esp32/main/main.c` | Fixed width calc in `draw_numbers()`, `delete_numbers()`, `draw_browser_header()` |

---

## v3.12 — HUD In-Game Menu Integration (NES/GB/SMS)

### Overview

Integrated the original HUD in-game menu system (`hud_menu()` from `odroid_hud.c`) into all three go-play emulators (NES, Game Boy, SMS/Game Gear). The HUD was already present in the `Components/` reference directory but was never copied into the `Emulators/go-play/` build trees, and the `CONFIG_IN_GAME_MENU_YES` build flag was not set in any of the three sdkconfig files.

The HUD provides a graphical overlay menu with 22 color themes, embedded 8×8 bitmap font, and options: **Resume / Restart / Reload / Save / Overwrite / Delete Save / Exit**. It communicates the user's choice via the `extern int ACTION` global, which the emulator's MENU button handler reads after `hud_menu()` returns.

### Files Copied

Three files copied from `Components/go-play/*/components/odroid/` into each emulator's `components/odroid/` directory:

| File | Size | Purpose |
|------|------|---------|
| `odroid_hud.c` | 560 lines | Self-contained HUD menu with embedded font, logo, 22 themes |
| `odroid_hud.h` | 51 lines | Public API: `hud_menu()`, `hud_check_saves()`, `hud_logo()`, `extern int ACTION` |
| `Kconfig.projbuild` | ~30 lines | Defines `choice IN_GAME_MENU` (Yes/No) and `choice MENU_HOT_KEYS` (Default/Combo) |

Destinations:
- `Emulators/go-play/nesemu-go/components/odroid/`
- `Emulators/go-play/gnuboy-go/components/odroid/`
- `Emulators/go-play/smsplusgx-go/components/odroid/`

### sdkconfig Additions

Added to all three emulator sdkconfig files (NES, GB, SMS):

```
# Retro ESP32 Configuration
CONFIG_LCD_DRIVER_CHIP_ODROID_GO=y
CONFIG_DEFAULT_MENU_KEY=y
CONFIG_COMBO_MENU_KEY=
CONFIG_IN_GAME_MENU_YES=y
CONFIG_IN_GAME_MENU_NO=
```

`CONFIG_IN_GAME_MENU_YES=y` enables all `#ifdef CONFIG_IN_GAME_MENU_YES` blocks in the emulator source code.
`CONFIG_DEFAULT_MENU_KEY=y` means the MENU button alone triggers the HUD (no combo required).

### Source Patches

**NES — `Emulators/go-play/nesemu-go/components/nofrendo-esp32/video_audio.c`:**
- Added `#ifdef CONFIG_IN_GAME_MENU_YES` include block: `<dirent.h>`, `"../odroid/odroid_hud.h"`, `int ACTION;`
- Replaced simple MENU handler (save + restart) with `#ifdef` block:
  - Locks display with `odroid_display_lock_nes_display()`
  - Calls `hud_menu()` to show the overlay
  - Reads `ACTION` and switches: 1=restart (set DataSlot=0), 3=reload, 4=save+quit, 5=overwrite+quit, 6=delete save, default=resume
  - Unlocks display before sending sentinel to video task queue (critical — see v3.13)
  - Waits for video task exit with `while(!exitVideoTaskFlag)`
  - Saves state, restarts

**NES — `Emulators/go-play/nesemu-go/main/main.c`:**
- Added `#ifdef CONFIG_IN_GAME_MENU_YES` include of `odroid_hud.h`
- Added `hud_check_saves(odroid_util_GetFileName(romPath))` after ROM load — shows Resume/Restart dialog if a save file exists

**GB — `Emulators/go-play/gnuboy-go/main/main.c`:**
- Added `#ifdef CONFIG_IN_GAME_MENU_YES` includes (`<dirent.h>`, `odroid_hud.h`, `int ACTION`)
- Replaced `DoMenuHome()` MENU handler with `#ifdef` block matching the same `hud_menu()` → ACTION pattern
- GB already had the correct mutex unlock order (unlock before sentinel) — this was the working reference pattern

**GB — `Emulators/go-play/gnuboy-go/components/gnuboy/loader.c`:**
- Added `#ifdef CONFIG_IN_GAME_MENU_YES` include block
- Added `hud_check_saves()` call after `fopen` of ROM file

**SMS — `Emulators/go-play/smsplusgx-go/main/main.c`:**
- Added `#ifdef CONFIG_IN_GAME_MENU_YES` includes (`<dirent.h>`, `odroid_hud.h`, `int ACTION`)
- Replaced `DoHome()` MENU handler with `#ifdef` block
- Added `hud_check_saves()` call after ROM path setup
- Unlocks display before audio terminate + sentinel (critical — see v3.13)

### HUD Menu Architecture

```
MENU button pressed
  → odroid_display_lock_*_display()    // Stop video task from writing LCD
  → hud_menu()                         // Draw overlay, wait for user choice
  → ACTION = user's choice (0-6)
  → switch(ACTION):
      0: resume (unlock display, continue)
      1: restart (DataSlot=0, esp_restart)
      3: reload (esp_restart)
      4: save + quit
      5: overwrite + quit
      6: delete save
  → odroid_display_unlock_*_display()  // MUST unlock before sentinel
  → send sentinel to vidQueue          // Signal video task to exit
  → while(!exitVideoTaskFlag) wait     // Wait for video task to stop
  → SaveState() / esp_restart()
```

### Build & Flash

All three emulators clean-built with `make -j4` and flashed:

| Emulator | Flash Address |
|----------|--------------|
| NES (nesemu-go) | `0x280000` |
| GB (gnuboy-go) | `0x340000` |
| SMS (smsplusgx-go) | `0x3F0000` |

`RetroESP32_full.fw` regenerated with all updated binaries.

---

## v3.13 — Fix: Save Hang in NES/SMS/GG

### Problem

After v3.12 HUD integration, choosing **Save & Quit** (or **Overwrite & Quit**) from the in-game menu would hang the device on NES, SMS, and Game Gear. The emulator froze and never wrote the save file to SD.

### Root Cause — Two Bugs

**Bug 1 — NES inverted wait condition:**

In `video_audio.c`, the save path waited for the video task to exit with:
```c
while(exitVideoTaskFlag) { vTaskDelay(10); }
```

`exitVideoTaskFlag` starts at `false` and is set to `true` when the video task exits. The condition `while(exitVideoTaskFlag)` means "wait while the video task HAS exited" — it's backwards. Since the flag is `false` when we enter the loop, the while condition is immediately false, and we skip the wait entirely. However, the video task is still running.

The actual problem is more subtle: after setting `exitVideoTaskFlag = false` and sending the sentinel, the code then enters `while(exitVideoTaskFlag)` which evaluates to `while(false)` — it falls through immediately. But the video task hasn't exited yet. The subsequent `SaveState()` attempts to write while the video task is still running, causing a race condition and hang.

**Fix:** Changed to `while(!exitVideoTaskFlag)` — "wait while the video task has NOT yet exited."

**Bug 2 — Display mutex deadlock (NES & SMS):**

The save path in NES and SMS held the display mutex locked when it sent the sentinel value to `vidQueue`:

```c
// BUG: Display mutex is still locked here
odroid_display_lock_nes_display();   // Acquired earlier for hud_menu()
hud_menu();                          // Menu is now closed
// ... switch on ACTION ...
xQueueSend(vidQueue, &sentinel, portMAX_DELAY);  // Send exit signal
```

The video task receives the sentinel, recognizes it as the exit signal, and before exiting tries to display an hourglass icon by calling `odroid_display_lock_nes_display()`. Since the MENU handler still holds the mutex, the video task blocks on the lock. After 1 second (mutex timeout), it calls `abort()`, crashing the device.

**Fix:** Added `odroid_display_unlock_nes_display()` (NES) and `odroid_display_unlock_sms_display()` (SMS) immediately before sending the sentinel to `vidQueue`. This matches Game Boy's working pattern, where `odroid_display_unlock_gb_display()` was already correctly placed before the sentinel.

### The Pattern (Must Match GB)

Game Boy's `DoMenuHome()` already had the correct order:
```c
odroid_display_lock_gb_display();
hud_menu();
// ... handle ACTION ...
odroid_display_unlock_gb_display();    // ← Unlock BEFORE sentinel
odroid_audio_terminate();
xQueueSend(vidQueue, &msg, portMAX_DELAY);  // Video task can now acquire mutex
while(videoTaskIsRunning) vTaskDelay(10);
```

NES and SMS were missing the unlock step. The fix aligns them with GB's working sequence.

### Fixes Applied

**NES — `video_audio.c` (two changes):**
1. `while(exitVideoTaskFlag)` → `while(!exitVideoTaskFlag)` (inverted wait condition)
2. Added `odroid_display_unlock_nes_display()` before `xQueueSend(vidQueue, &arg, portMAX_DELAY)` (mutex deadlock)

**SMS — `main.c` (one change):**
1. Added `odroid_display_unlock_sms_display()` before `odroid_audio_terminate()` + `xQueueSend()` (mutex deadlock)

### Correct Save Sequence (All Three Emulators)

```
hud_menu() returns with ACTION = 4 (Save & Quit)
  → odroid_display_unlock_*_display()    // Release mutex
  → odroid_audio_terminate()             // Stop audio
  → xQueueSend(vidQueue, &sentinel)      // Signal video task to exit
  → video task receives sentinel
  → video task acquires display mutex for hourglass (no deadlock)
  → video task sets exit flag and exits
  → while(!exitVideoTaskFlag) wait       // Main thread waits for exit
  → SaveState()                          // Safe to write save file
  → esp_restart()                        // Return to launcher
```

### Build & Flash

NES and SMS rebuilt with `make -j4`, flashed to `0x280000` and `0x3F0000` respectively. `Firmware/Bins/` updated. `RetroESP32_full.fw` regenerated (12.1 MB).

### Files Modified

| File | Changes |
|------|---------|
| `Emulators/go-play/nesemu-go/components/nofrendo-esp32/video_audio.c` | Fixed `while(!exitVideoTaskFlag)`, added `odroid_display_unlock_nes_display()` before sentinel |
| `Emulators/go-play/smsplusgx-go/main/main.c` | Added `odroid_display_unlock_sms_display()` before sentinel |

---

## v3.14 — Safe Mode & Volume Clamp (All Emulators)

### Safe Mode: A-Button Bail-Out

Added A-button safe mode to all emulators that were missing it. Holding the **A button** during emulator boot now returns to the launcher carousel, breaking boot loops caused by crashing ROMs.

**Approach by emulator type:**

| Emulator | Method | File |
|----------|--------|------|
| Stella (A26) | New block after `ili9341_clear()`, before SD card open | `stella-odroid-go/main/main.cpp` |
| Prosystem (A78) | New block after `ili9341_clear()`, before SD card open | `prosystem-odroid-go/main/main.c` |
| Handy (Lynx) | New block after `ili9341_init()`, before ROM path read | `handy-go/main/main.c` |
| PC Engine | New block after `odroid_input_gamepad_init()`, before `check_boot_cause()` | `odroid-go-pcengine-huexpress/pcengine-go/main/main.c` |
| ZX Spectrum | New block after `odroid_input_gamepad_init()`, before `check_boot_cause()` | `odroid-go-spectrum-emulator/main/main.c` |
| NES | Added `ODROID_INPUT_A` to existing MENU button check | `go-play/nesemu-go/main/main.c` |
| Game Boy | Added `ODROID_INPUT_A` to existing MENU button check | `go-play/gnuboy-go/main/main.c` |
| SMS/GG | Added `ODROID_INPUT_A` to existing MENU button check | `go-play/smsplusgx-go/main/main.c` |

**Atari 800** already had safe mode from v3.10. **Stella** was added first (during this session), then the remaining 7.

**Implementation pattern (Prosystem/Handy/PC Engine/Spectrum):**
```c
// Safe mode: hold A during boot to bail out to launcher
{
    odroid_gamepad_state bail;
    odroid_input_gamepad_read(&bail);
    if (bail.values[ODROID_INPUT_A]) {
        printf("emulator: Button A held at boot — returning to launcher\n");
        vTaskDelay(200 / portTICK_PERIOD_MS);
        odroid_system_application_set(0);
        esp_restart();
    }
}
```

**Implementation pattern (NES/GB/SMS) — extends existing MENU check:**
```c
if (bootState.values[ODROID_INPUT_MENU] || bootState.values[ODROID_INPUT_A])
```

### Volume Clamp Fix

**Problem:** The launcher stores volume in NVS with a range of 0–8, but each emulator's `odroid_audio_volume_set()` only accepts 0–4 (`ODROID_VOLUME_LEVEL_COUNT = 5`). When the NVS value exceeded 4, the function called `abort()`, crashing the emulator at boot.

**Fix:** Replaced `abort()` with a clamp to `ODROID_VOLUME_LEVEL_COUNT - 1` (max volume) in all 9 emulators' `components/odroid/odroid_audio.c`:

```c
// Before:
if (value >= ODROID_VOLUME_LEVEL_COUNT) {
    printf("odroid_audio_volume_set: value out of range (%d)\n", value);
    abort();
}

// After:
if (value >= ODROID_VOLUME_LEVEL_COUNT) {
    printf("odroid_audio_volume_set: clamping out-of-range value (%d) to %d\n",
           value, ODROID_VOLUME_LEVEL_COUNT - 1);
    value = ODROID_VOLUME_LEVEL_COUNT - 1;
}
```

**Files modified (volume clamp):**

| File |
|------|
| `Emulators/stella-odroid-go/components/odroid/odroid_audio.c` |
| `Emulators/prosystem-odroid-go/components/odroid/odroid_audio.c` |
| `Emulators/handy-go/components/odroid/odroid_audio.c` |
| `Emulators/odroid-go-pcengine-huexpress/pcengine-go/components/odroid/odroid_audio.c` |
| `Emulators/odroid-go-spectrum-emulator/components/odroid/odroid_audio.c` |
| `Emulators/go-play/nesemu-go/components/odroid/odroid_audio.c` |
| `Emulators/go-play/gnuboy-go/components/odroid/odroid_audio.c` |
| `Emulators/go-play/smsplusgx-go/components/odroid/odroid_audio.c` |
| `Emulators/atari800-odroid-go/components/odroid/odroid_audio.c` |

### Docs Cleanup

- Removed stale `frodo-go.bin` entry from v3.6 flash table
- Corrected flash table to match current `partitions.csv` offsets and partition names

### Build & Flash

All 9 emulators rebuilt with `make -j4 app` and flashed in a single esptool batch to COM17 at 2000000 baud:

| Offset | Binary |
|--------|--------|
| 0x280000 | nesemu-go.bin |
| 0x340000 | gnuboy-go.bin |
| 0x3F0000 | smsplusgx-go.bin |
| 0x550000 | spectrum.bin |
| 0x5E0000 | stella-go.bin |
| 0x780000 | prosystem-go.bin |
| 0x840000 | handy-go.bin |
| 0x930000 | pcengine-go.bin |
| 0xA70000 | atari800-go.bin |

### Stella -O3 Optimization

Added `-O3` compiler flags to the Stella emulator for improved performance:

**Files modified:**
- `Emulators/stella-odroid-go/components/stella/component.mk` — added `CFLAGS += -O3` and `CXXFLAGS += -O3`
- `Emulators/stella-odroid-go/main/component.mk` — added `CFLAGS += -O3` and `CXXFLAGS += -O3`

### Phosphor Blending (Attempted & Reverted)

Attempted to implement phosphor blending for Asteroids sprite-multiplexing flicker in Stella (Atari 2600). Multiple approaches were tried:

1. **Dual SPIRAM buffers + accumulation on core 0** — correctly eliminated flicker but caused ~20% speed reduction due to SPIRAM being ~10x slower than internal SRAM
2. **TIA dual framebuffers + merge in LCD function** — restored TIA's `myPreviousFrameBuffer` as separate allocation, merged during `ili9341_write_frame_atari2600()` pixel loop. Speed still degraded because the second TIA buffer landed in PSRAM, and semaphore serialized both cores
3. **Zero-copy with memcpy-in-videoTask** — saved previous frame in internal `framebuffer[]` array during videoTask, merged in LCD function. Good speed but 50% renderTable meant incomplete sprite coverage
4. **100% render + 50% display** — rendered all TIA frames but only displayed every other. Partial asteroid objects still missing

**Conclusion:** All phosphor approaches caused measurable speed degradation on ESP32. The fundamental constraint is that any form of frame accumulation requires either extra memory bandwidth (SPIRAM) or extra CPU (100% render). Reverted all phosphor changes. The Asteroids flicker is a known limitation of the hardware-constrained Stella port.

**What was kept:**
- `-O3` compiler optimization (pure speed gain, no visual changes)
- Safe mode, volume clamp, game menu, paddle support — all unrelated to phosphor

### Firmware Generation

**Tool:** `mkfw.py` (project root)
**Output:** `Firmware/Releases/RetroESP32.fw` (8.2 MB)
**Version string:** "RetroESP32 v3.14"

All updated emulator binaries copied from their build directories to `Firmware/Bins/` before generation. The `.fw` file contains 11 partitions (launcher + 10 emulators) with CRC32 checksum.

| Partition | Binary | Size |
|-----------|--------|------|
| launcher (ota_0) | retro-esp32.bin | 452,400 |
| nes (ota_1) | nesemu-go.bin | 703,824 |
| gb (ota_2) | gnuboy-go.bin | 670,784 |
| sms (ota_3) | smsplusgx-go.bin | 1,384,960 |
| spectrum (ota_4) | spectrum.bin | 436,656 |
| a26 (ota_5) | stella-go.bin | 1,422,000 |
| a78 (ota_6) | prosystem-go.bin | 706,192 |
| lnx (ota_7) | handy-go.bin | 900,464 |
| pce (ota_8) | pcengine-go.bin | 691,456 |
| tyrian (ota_9) | OpenTyrian.bin | 494,112 |
| a800 (ota_10) | atari800-go.bin | 760,736 |

Place on SD card at: `/sd/odroid/firmware/RetroESP32.fw`
