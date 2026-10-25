# DSfix 3.0 — Dark Souls: Prepare to Die Edition

This is the modernized overhaul for Dark Souls: Prepare to Die Edition, engineered to deliver high-resolution, smooth gameplay on modern Windows 10 and 11 systems.

---

## System Requirements

* **Steam Version Only:** This mod is strictly compatible ONLY with the LATEST STEAM VERSION of the game. Retail or older GFWL builds will fail to hook properly.
* **DirectX End-User Runtimes (June 2010):** This is absolutely mandatory. While Windows 11 natively supports DirectX 9, Microsoft stopped bundling the specific legacy D3D helper libraries (like `d3dx9_43.dll` and `d3dcompiler_43.dll`) with the OS. DSfix 3.0 relies heavily on these exact libraries to compile and render its advanced shaders (SMAA, FXAA, SSAO, and Depth of Field). Without this package, the game will instantly crash with a missing file error.
* **Visual C++ Redistributable (x86):** You must install the 32-bit (x86) versions of the Visual C++ redistributables. The original Dark Souls PTDE executable relies on the older Visual C++ 2010 Redistributable, while modern community tools (and heavily modernized DLLs) often expect the Visual C++ 2015–2022 Redistributable. Ensure both are installed to prevent `MSVCR100.dll / VCRUNTIME140.dll was not found` application launch errors.
* **Disabled In-Game Rendering Effects:** Before launching with DSfix 3.0, you must ensure that the game's native Anti-Aliasing and Motion Blur settings are turned completely OFF in the game menu or `DarkSouls.ini` configuration file. Leaving the vanilla AA enabled can cause a rendering conflict with DSfix.
* **Fullscreen:** In the game menu, set Fullscreen to OFF. Then, open DSfix.ini or DSfix GUI and set 'borderlessFullscreen' to 1
---

## Base Installation

Extract the contents of the DSfix 3.0 archive into your Dark Souls executable directory, right next to `DARKSOULS.exe`.

---
## Best Practices & Recommendations

To ensure maximum stability on modern machines, configure your environment using these proven guidelines:

* **Native Resolution Mapping:** Ensure your in-game resolution is set to match your desktop monitor and the rendering resolution in dsfix so the image is sharp and on all screen size. You can try to set your resolution in game menu or in: `%LOCALAPPDATA%\NBGI\DarkSouls\DarkSouls.ini`.
* **Unlock FPS Enabled:** Always keep the `unlockFPS` enabled in `DSFix.ini` even when `FPSlimit` is 30FPS. With `unlockFPS` disabled, DSfix skips its frame-timing code entirely and the game uses its original, unpatched engine logic.
* **Texture Prefetching:** If you don't use textures that are more than 2GB in the dsfix\tex_override folder you can set the option `enableTexturePrefetch` to `1` in `DSfix.ini` for a faster texture load at startup. Dark Souls PTDE is a 32-bit executable, meaning the entire game will hard-crash if it tries to use more than 4GB of total RAM. If you use a massive 4K texture pack (like Mulderland's Enhancement Pack) and enable prefetching, DSfix will attempt to load gigabytes of `.png` files into RAM all at once.
* **Bypass the 60Hz Crash:** In the game menu, set Fullscreen to OFF. Then, open `DSfix.ini` and set `borderlessFullscreen 1`. This solves the engine limitation where the game hard-crashes when selecting "New Game" or "PC Settings" if your monitor's refresh rate is above 60Hz.

---

## Modernized Features and Fixes of DSfix 3.0

* **Backup Saves Fix:** Now, the default path can be set in `DSFix.ini` file and it supports Windows environment variables. Example: `%USERPROFILE%\Documents\NBGI\DarkSouls`
* **Smarter Save Backups:** The backup system now verifies if your game save has actually changed before creating a new backup. This prevents your limited backup slots from being overwritten by identical copies if you frequently launch and exit the game without making any new progress.
* **Quick Save and Quick Load:** Press F5 to quick save and press F8 twice to quick load; turn it on in DSfix.ini (`quickSave`, `quickSaveKey`, `quickLoadKey`). Only your current character is affected, and your save is backed up before every quick load (in the `QuickSave` folder). Not available in menus, cutscenes, loading screens, when dead, or online with other players.
* **HUD Scaling and Opacity Customization:** Resize Individualy the size of the health and stamina bars, the item inventory HUD, the souls counter, and the boss health bar. Set Individual Opacity Levels for the each of the three hud elements. Fixed the issue with boss health bars,switching indicator (for weapons/spells/items), subtitles, damage numbers.
* **Menu and subtitles scaling Individualy:** Shrinks the game’s menus and windows so they cover less of the screen.
* **Disable Item Auto-Equip**: Consumables you pick up are no longer put into your quick item slots automatically. The Estus Flask works as before. This is an optional setting.
* **Multiple Item Use:** Using a soul item or a Humanity in the pause Inventory menu asks how many to use, then uses them all with a single animation — the convenience Dark Souls Remastered added, brought to the Prepare to Die Edition. Two groups, each with its own switch: the ten ordinary soul items (Soul of a Lost Undead up to Soul of a Great Hero), and Humanity with Twin Humanities. Everything else keeps its normal behaviour, so Boss Souls and Fire Keeper Souls still ask their own Yes/No question, and the quick item button during play always uses a single item with no question. The number offered is never more than you own and never so many that the reward would be lost at the game’s limits (999,999,999 souls, 99 humanity), and owning only one uses it straight away. The picker reads correctly in all ten of the game’s languages and follows your menu scaling. This is an optional setting.
* **Multiple Covenant Offerings:** `covenantOfferings` (off by default) turns the "Offer ...?" question at a covenant altar or leader into the game's own number picker, so many Sunlight Medals, Dragon Scales, Eyes of Death, Souvenirs of Reprisal, or many Humanity, can be handed over in one step instead of answering Yes once per item. Six covenants take offerings and all six are covered: Warriors of Sunlight, Darkwraith, Path of the Dragon, Gravelord Servant, Blade of the Darkmoon and Chaos Servant. Forest Hunter ranks up by killing invaders rather than by offerings, and Way of White and Princess's Guard have no ranks. The picker never offers enough to reach the next covenant rank, so the offering that ranks you up is always the game's own unchanged single question and its reward arrives exactly as it always did; below rank 1 the picker stops at 9, between the first two ranks at 29, between the next two at 79, and after the last rank it goes up to 100. The number offered is never more than you hold, the game's own question is kept word for word so it reads correctly in all ten languages, and the window follows the `menuScale` setting. The items taken and the devotion gained are written in the same single step, so the two can never disagree, and if anything does not add up the offering is simply left as a normal single one. Before anything is patched, both places in the game's code are compared against the bytes this build should have, and if either differs nothing is changed and the feature stays off. The addresses and the technique come from the Dark Souls Overhaul team's research (metal-crow and Sean Pesce, "Multi-Tribute"); the implementation here is DSfix 3.0's own code.
* **Durability Bars for weapons: **Shows a thin Dark Souls 3 style bar under each weapon slot. The dark red part shrinks as the weapon loses durability, and the dark background stays. An empty hand has no bar. While two-handing, the bar fades together with the other hand's weapon icon. If a bow or crossbow is equipped, the arrow/bolt box and its count move down so they stay below the bar. The bars are part of the bottom-left HUD, so every scaling option apply to them.
* **Sun/Moon & Water Issue Fixed:** Now they render perfectly at their intended screen-space locations. For resolution 1440p or 4K, DoF Override Resolution must not be 0 otherwise the water stays at vanilla quality.
* **60 FPS Bonfire Fix and Smart Protection:** Automatically detects and neutralizes the infamous bonfire animation softlock while dynamically disabling SSAO while resting to prevent dark halo artifacts around your character.
* **High-DPI & Display Scaling Fix:** Stripped out the 150% DPI stretching. On Windows 10/11 with 1440p or 4K monitors set to desktop scaling, the game misinterprets dimensions causing blurry bilinear upscaling and mouse-capture offset bugs. With DSfix 3.0, it will never artificially scale the game window.
* **High-Precision Frame Pacing:** Replaced the broken framerate pacing with a modern solution.
* **Alt+Tab Memory Leak Plugged:** In vanilla DSfix 2.4, the mod forgot to delete custom FXAA, SSAO, and DoF buffers during a Device Lost event, silently eating VRAM until the game crashed. This is now fixed, the engine is vastly more resilient to buffer collisions.
* **Modern CPU Power Management:** Dynamically disables Windows 11 EcoQoS (Power Throttling) to ensure the game engine runs at maximum turbo clocks. By removing artificial priority masks, it allows the native OS Thread Director to smoothly balance the game's rendering, audio, and networking threads across modern hybrid CPUs without causing driver starvation or micro-stutters.
* **Controller button icons.** button prompts and quick menu, built into DSfix. It detects the controller connected at game start (PlayStation, Switch Pro or Xbox). Can be set to OFF to use the game's default icons, or files in tex_override. 
* **VRAM Optimization:** Post-processing effects (AA, SSAO, DoF) now utilize a shared buffer pool, reducing VRAM usage and improving frame pacing.
* **Text:** Includes high visual quality of the in-game fonts
* **Controller button prompt swap: **Swaps the controller button icons the game displays, for players who have swapped the buttons themselves in a controller mapper (for example Japanese-style controls, where circle confirms)
* **Sharpening: **brings back the fine texture detail that anti-aliasing softens. It uses contrast-adaptive sharpening (AMD FidelityFX CAS), which sharpens flat detail more than edges that are already sharp, so it doesn’t create bright halos.It only affects the 3D image: the HUD, menus and text are never sharpened.
* **High-FPS Fixes:**
  - **Core Physics & Movement**
    - **Slope Gravity & Friction:** Sliding down slopes and embankments no longer happens at light-speed.
    - **Stairs and Slopes (Foot Placement):** The legs and hips no longer bounce on stairs and slopes above 30 FPS. The game's foot placement corrected the body a fixed amount every frame, so at 144 FPS it reacted almost five times faster than at 30 FPS and the camera shook while running on stairs. Its three settings now follow the real frame time, so they behave as at 30 FPS. The small tremble of the feet when walking down stairs is the game's own and happens at 30 FPS too.
	- **Airborne Damping:** Normalizes jump distance and walk-off momentum to match the native 30 FPS console release.
    - **Sliding Down Steep Slopes:** While steering, the game restarts the steering animation every frame, and the engine cut that animation's movement to about a tenth at 144 FPS. The engine's limit now follows the frame time, so the hero steers at the same speed as at 30 FPS. Walking, rolling, sprinting and combat are unchanged.At the top of a steep slide, the hero sometimes did a "walking in the air" stumble before sliding above 30 FPS, also when attacking at the edge. The game checks every frame whether the hero is touching the slide surface, and at high frame rates that check could miss for a single frame. The game then stopped the slide. A touch now counts for one 30 FPS frame (1/30 s), which is what the game sees at 30 FPS, so the slide starts straight away.
	- **Ladder & Ledge Snap:** Prevents the player from falling through the floor at the bottom of ladders, and stops the engine from aggressively gluing the character to curved lips or stairs.
    - **Sprint Graze Check:** Fixes the engine bug where running near walls or complex geometry artificially slows the player down at high framerates.
  - **Combat & Camera**
    - **Accurate Stamina Drain:** Keeps the leftover time of each sprint tick instead of discarding it, guaranteeing a strict 10-point-per-second drain at any frame rate, so stamina no longer lasts artificially long at frame rates that don’t divide evenly into the tick.
    - **Lock-On Body Tracking:** Normalizes the bone rotation speeds for the torso, head, and hips. Enemies will no longer instantly snap their upper bodies to track you, and your own aiming remains smooth.
    - **Camera Smoothing & Panning:** Makes all 16 camera blend weights framerate-independent. Switching lock-on targets smoothly pans the camera instead of instantly snapping, and the mouse/stick "look-at boost" fades out correctly in real-time.
    - **Smooth Cutscenes at High-FPS:** Tested at max 144fps. The cutscene camera no longer jerks.
  - **Multiplayer & Phantoms**
    - **Flawless Ghost Replays:** Implements a fixed-point translation scaler so bloodstains, wandering ghosts, and network phantoms move at the exact correct speed. Perfectly syncs their walking/running animations to the game's internal 10-frame update clock, completely eliminating phantom skipping, skating, and fast-forwarding.
  - **Visuals, Audio & UI**
    - **Particle Effect Spawning:** Clamps engine emitter intervals to 1/30s as they load into memory, preventing sparks, bonfire smoke, and magic effects from becoming blindingly dense and opaque at 120 FPS.
    - **Ragdolls & 3D Audio:** Normalizes frame-velocity multipliers so ragdolls maintain proper physical weight upon death, and moving sound sources (FMOD Doppler effect) do not drop in pitch.
    - **Timers & Fades:** Corrects the duration of weapon buff glows, dropped item pulses, and ragdoll blend-in animations.
    - **Menu Input Repeat:** Locks the D-Pad and menu navigation delay to the engine's default timing so players do not accidentally skip past items in their inventory.
    - **HUD Animations:** Health, stamina, and boss bars smoothly fill and deplete rather than instantly snapping to new values. The loading screen bonfire swirl rotates at the correct cinematic speed.
	- **Soul Counter:** The souls counter no longer rolls up too fast at high FPS.
  - **Engine Stability**
    - **VRR / G-Sync Stutter Fix:** Bypasses the engine's hardcoded "Present" catch-up loop, eliminating erratic refresh-rate bouncing and micro-stutters on variable-refresh-rate monitors.

* **Native 16:10 & Steam Deck Aspect Ratio Support:** Dark Souls PTDE natively forces 16:9 letterboxing (black bars) on taller displays, such as 16:10 monitors or the Steam Deck's 1280x800 screen. This fix automatically detects displays taller than 16:9 and removes the black bars by seamlessly expanding the vertical camera field-of-view (Vert+). The game now perfectly fills the entire screen without any image stretching or horizontal squashing. Standard 16:9 displays are left completely untouched.
---

## Bugs and Issues Fixed

### Frame Pacing Conflict between Limiter and Present
* **Issue:** In `RSManager::frameTimeManagement()`, the timer waits for the target frame time before calling `d3ddev->Present()`. If GPU VSync or the driver frame queue blocks inside `Present()`, the next frame measures this block as elapsed render time, resulting in double-throttling and persistent frame-time jitter.
* **Applied Solution:** Prevents Limiter Desync, Absorbs Driver Delays and Reduces Input Latency.

### Memory Deallocation Mismatch (delete vs delete[])
* **Issue:** In `RSManager::prefetchTextures()`, texture buffer data is allocated as an array using `new char[data.size]`. However, `RSManager::~RSManager()` deletes it using `SAFEDELETE` (which executes scalar delete) instead of `delete[]`, causing undefined behavior and heap corruption upon shutdown.
* **Applied Solution:** Proper Heap Bookkeeping, Preventing Useless Copies.

### Thread-Unsafe Static Buffer in GetDirectoryFile()
* **Issue:** `GetDirectoryFile()` uses a single local static `char path[320]`. Because `SaveManager`, `BonfireGlitchDetectionThread`, and the rendering thread all execute asynchronously, concurrent calls to `GetDirectoryFile()` overwrite the buffer simultaneously, leading to file-path corruption and crashes.
* **Applied Solution:** Thread Isolation, Zero Caller Refactoring.

### Floating-Point Truncation in updateFramerate()
* **Issue:** The smoothing check `if (abs(deltaTime - targetTime) < 2.0)` uses integer `abs()` instead of floating-point `fabs()` or `std::abs()`. Passing `double` values implicitly casts the delta to an `int`, stripping sub-millisecond precision and compromising frame-time smoothing.
* **Applied Solution:** Restored Precision: `std::abs()` correctly overloads for `double` types; Performance Improvement: Using `std::abs()` natively on a `double` compiles down to a single, instantaneous bitwise operation.

### Missing HWND Verification in KeyActions::processIO()
* **Issue:** Hotkey checks evaluate `GetForegroundWindow() != NULL`. This condition evaluates to true even when typing in other applications (such as Discord, a web browser, or an overlay), causing DSfix actions and the `g_Force30FPS` toggle to trigger outside the game.
* **Applied Solution:** Process Isolation, Overlay & Alt-Tab Immunity.

### Excessive Work Under Loader Lock in DllMain
* **Issue:** `DllMain` directly creates background threads, loads dependent libraries (`dinput8.dll`), and engages Detours transactions during `DLL_PROCESS_ATTACH`. Doing heavy I/O and synchronization under loader lock risks deadlocks during startup on Windows 11.
* **Applied Solution:** Prevents Windows 11 Deadlocks, Safe and Seamless Hooking.

### Uninitialized hudVertices Buffer
* **Issue:** `RSManager` declares `INT16 hudVertices[32]` but leaves it uninitialized in the constructor. Calling `reloadHUDVertices` when `hudvertices.txt` is missing leaves uninitialized garbage in memory.
* **Applied Solution:** Safe Fallback - `memset`.

### Out-of-Bounds Memory Read in Config Parser
* **Issue:** KeyActions: The out-of-bounds memory read occurs in `KeyActions::load()` because the macro attempts to read the character immediately following a key name before verifying if the key name actually exists in the parsed line.
* **Applied Solution:** Move the read inside the check.

### Infinite Freeze on Zero FPS Limit
* **Applied Solution:** 60 FPS for user input of 0 fps limit.

### Override Language not keeping the change, sometimes the user needed to play with compatibility mode.
* **Applied Solution:** Language override no longer modifies the Windows registry or requires compatibility mode. It is applied inside the game process only

### Script Memory Fix
* **Applied Solution:** Keeps the enemy AI scripts cleaning up their memory as often as they do at 30 FPS. At high frame rates, unused script data used to build up much faster, and in crowded areas the game could run out of memory and close itself without warning.

### Online Player Movement
* **Applied Solution:** Other players' characters (summons and invaders) now walk, run and sprint according to their real speed. Above 30 FPS, a running player used to be shown walking.

### 16:10 and Taller Screens
* **Applied Solution:** On 16:10 (and 4:3) resolutions, the game's camera now uses the real shape of your screen, so you see more of the world above and below, with the same width as on 16:9. The world, effects, lock-on marker and enemy health bars all line up, with nothing cut off at the edges. 16:9 screens are not affected.

### Skip Intro
* **Applied Solution:** `skipIntro` now takes you straight to the title screen, skipping the start-up autosave notice and the Bandai Namco, FromSoftware and Havok logos. It no longer speeds up the game's clock to do this, so it has no effect on timing or performance during play. The flaming autosave icon still appears in game as usual.

### Bonfire Softlock Fix (improved)
* **Applied Solution:** The bonfire softlock check now verifies the game's memory before reading it. It works exactly as before, but no longer triggers hidden errors during loading screens or when joining another player's world.
---

## Credits
* **[Durante/PeterTh](https://github.com/PeterTh/dsfix):** Huge thanks for the foundational work on original DSFix 2.4
* **[zofrea](https://github.com/zofrea1/DS1-FPS-Unlock-PTDE-Remastered):** Special thanks for reverse-engineering discoveries and high-fps fixes.
* **[Hal47](https://github.com/Hal47/dsfix):** Credit for the Sun and Moon rendering fixes.
* **Bonfire softlock fix:** The Idea is from [Sean Pesce](https://github.com/SeanPesce/FPSFix-Plus).
* **Dark Souls Overhaul team (metal-crow & Sean Pesce)** – memory research (equipment and parameter table layouts, durability addresses, and the item-use and covenant-offering addresses) used for the item auto-equip, weapon durability bar, multiple item use and multiple covenant offerings features. All of it is DSfix 3.0's own code. https://github.com/metal-crow/Dark-Souls-1-Overhaul
* **AMD FidelityFX CAS:** the sharpening uses AMD's Contrast Adaptive Sharpening (MIT license, see Third-Party Licenses). https://github.com/GPUOpen-Effects/FidelityFX-CAS

## Third-Party Licenses

DSfix 3.0 contains code adapted from the following projects, used under its license:

### DS1 FPS Unlock (PTDE & Remastered) by zofrea
Source: https://github.com/zofrea1/DS1-FPS-Unlock-PTDE-Remastered
Parts of the FPS fixes are based on the DS1-FPS-Unlock-PTDE-Remastered project.

MIT License

Copyright (c) 2026 zofrea1

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

### AMD FidelityFX CAS (Contrast Adaptive Sharpening)
Source: https://github.com/GPUOpen-Effects/FidelityFX-CAS
Used for the sharpening (`sharpenStrength`).

MIT License

Copyright (c) 2020 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.