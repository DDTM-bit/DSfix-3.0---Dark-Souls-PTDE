# DSfix 3.0 — Dark Souls: Prepare to Die Edition

This is the modernized overhaul for Dark Souls: Prepare to Die Edition, engineered to deliver high-resolution, smooth gameplay on modern Windows 10 and 11 systems.

---

## System Requirements

* **Steam Version Only:** This mod is strictly compatible ONLY with the LATEST STEAM VERSION of the game. Retail or older GFWL builds will fail to hook properly.
* **DirectX End-User Runtimes (June 2010):** This is absolutely mandatory. While Windows 11 natively supports DirectX 9, Microsoft stopped bundling the specific legacy D3D helper libraries (like `d3dx9_43.dll` and `d3dcompiler_43.dll`) with the OS. DSfix 3.0 relies heavily on these exact libraries to compile and render its advanced shaders (SMAA, FXAA, SSAO, and Depth of Field). Without this package, the game will instantly crash with a missing file error.
* **Visual C++ Redistributable (x86):** You must install the 32-bit (x86) versions of the Visual C++ redistributables. The original Dark Souls PTDE executable relies on the older Visual C++ 2010 Redistributable, while modern community tools (and heavily modernized DLLs) often expect the Visual C++ 2015–2022 Redistributable. Ensure both are installed to prevent `MSVCR100.dll / VCRUNTIME140.dll was not found` application launch errors.
* **Disabled In-Game Rendering Effects:** Before launching with DSfix 3.0, you must ensure that the game's native Anti-Aliasing and Motion Blur settings are turned completely OFF in the game menu or `DarkSouls.ini` configuration file. Leaving the vanilla AA enabled can cause a rendering conflict with DSfix.
* **Fullscreen:** In the game menu, set Fullscreen to ON. Then, open DSfix GUI and set borderlessFullscreen 0, Vsync ON and set you monitor Refresh Rate Hz.
---

## Base Installation

Extract the contents of the DSfix 3.0 archive into your Dark Souls executable directory, right next to `DARKSOULS.exe`.

---
## Best Practices & Recommendations

To ensure maximum stability on modern machines, configure your environment using these proven guidelines:

* **Native Resolution Mapping:** Ensure your in-game resolution is set to match your desktop monitor and the rendering resolution in dsfix so the image is sharp and on all screen size. You can try to set your resolution in game menu or in: `%LOCALAPPDATA%\NBGI\DarkSouls\DarkSouls.ini`.
* **Frame rate (`unlockFPS`):** choose one of two ways.
  * `unlockFPS 1`: DSfix unlocks the frame rate up to `FPSlimit` and, above 30 FPS, applies its own high-FPS fixes (see below). Other high-FPS bugs of the game are not fixed by DSfix. `FPSlimit 30` gives the original game speed and behaviour.
  * `unlockFPS 0`: DSfix does not change the frame rate or timing at all. Use this when another mod unlocks the frame rate and fixes the high-FPS bugs. All other DSfix 3.0 features keep working.
* **Texture Prefetching:** If you don't use textures that are more than 2GB in the dsfix\tex_override folder you can set the option `enableTexturePrefetch` to `1` in `DSfix.ini` for a faster texture load at startup. Dark Souls PTDE is a 32-bit executable, meaning the entire game will hard-crash if it tries to use more than 4GB of total RAM. If you use a massive 4K texture pack (like Mulderland's Enhancement Pack) and enable prefetching, DSfix will attempt to load gigabytes of `.png` files into RAM all at once.
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
* **Durability Bars for weapons:** Shows a thin Dark Souls 3 style bar under each weapon slot. The dark red part shrinks as the weapon loses durability, and the dark background stays. An empty hand has no bar. While two-handing, the bar fades together with the other hand's weapon icon. If a bow or crossbow is equipped, the arrow/bolt box and its count move down so they stay below the bar. The bars are part of the bottom-left HUD, so every scaling option apply to them.
### In-game map (gameMap)
Open the gesture window (Back) while you play and a map appears next to it, a little see-through
(the game is not paused; the bottom-left HUD fades out meanwhile).

- Shows only the ground you have explored; it is revealed as you walk and saved per character
  in `DSfixMap.dat`, next to your save file.
- Shows height by brightness: ground at your level is bright, lower ground is darker, higher ground
  is fainter, so floors stacked above each other stay readable.
- Area pages (Firelink Shrine, Undead Burg, ...) and a world page of all Lordran, with area names.
  Anor Londo is at the top.
- Your lit bonfires (read from the game, so loading an older save shows that save's bonfires),
  with their names; lower or higher bonfires are marked.
- Your soul remains (green spot, or an arrow on the edge when off the map) and your direction.
- No fast travel: the map only shows.
- Controls: pad RS move, LT/RT zoom, LB/RB area, R3 area/world. Mouse drag move, wheel zoom,
  `,` / `.` area, `M` area/world.
- Characters you played before using the map start with an empty map (their lit bonfires show anyway).

`gameMap 1` in DSfix.ini to turn it on (default 0 = off, nothing installed).

* **Camera Distance: Set how far the camera is from your character while you play: closer than vanilla (down to 0.5×) or farther (up to 2×). By default, the lock-on camera keeps the vanilla distance and moves in smoothly when you lock on. Set cameraDistanceLockOn 1 to keep your distance during lock-on too. A key in DSfixKeys.ini (toggleCameraDistance) switches between your distance and vanilla at any time, with a smooth move. Wall collision works as before, and cutscenes and bow aiming are not changed. At cameraDistance 1.0 the feature is off and nothing is patched.
* **Sun/Moon & Water Issue Fixed:** Now they render perfectly at their intended screen-space locations. For resolution 1440p or 4K, DoF Override Resolution must not be 0 otherwise the water stays at vanilla quality.
* **Bonfire Softlock Fix and Smart Protection:** With `unlockFPS 1`, automatically detects and neutralizes the bonfire animation softlock (the character stays seated with no menu after reversing hollowing at a high frame rate). With any setting, SSAO is disabled while resting to prevent dark halo artifacts around your character.
* **High-DPI & Display Scaling Fix:** Stripped out the 150% DPI stretching. On Windows 10/11 with 1440p or 4K monitors set to desktop scaling, the game misinterprets dimensions causing blurry bilinear upscaling and mouse-capture offset bugs. With DSfix 3.0, it will never artificially scale the game window.
* **High-Precision Frame Pacing:** Replaced the broken framerate pacing with a modern solution.
* **Alt+Tab Memory Leak Plugged:** In vanilla DSfix 2.4, the mod forgot to delete custom FXAA, SSAO, and DoF buffers during a Device Lost event, silently eating VRAM until the game crashed. This is now fixed, the engine is vastly more resilient to buffer collisions.
* **Modern CPU Power Management:** Dynamically disables Windows 11 EcoQoS (Power Throttling) to ensure the game engine runs at maximum turbo clocks. By removing artificial priority masks, it allows the native OS Thread Director to smoothly balance the game's rendering, audio, and networking threads across modern hybrid CPUs without causing driver starvation or micro-stutters.
* **Controller button icons.** button prompts and quick menu, built into DSfix. It detects the controller connected at game start (PlayStation, Switch Pro or Xbox). Can be set to OFF to use the game's default icons, or files in tex_override. 
* **VRAM Optimization:** Post-processing effects (AA, SSAO, DoF) now utilize a shared buffer pool, reducing VRAM usage and improving frame pacing.
* **Text:** Includes high visual quality of the in-game fonts
* **Controller button prompt swap:**Swaps the controller button icons the game displays, for players who have swapped the buttons themselves in a controller mapper (for example Japanese-style controls, where circle confirms)
* **Sharpening:**brings back the fine texture detail that anti-aliasing softens. It uses contrast-adaptive sharpening (AMD FidelityFX CAS), which sharpens flat detail more than edges that are already sharp, so it doesn’t create bright halos.It only affects the 3D image: the HUD, menus and text are never sharpened.
* **High-FPS Fixes** (only with `unlockFPS 1` and an FPS limit above 30; with `unlockFPS 0` none of them exists, so another FPS mod can do its own work):
  - **Sliding Down Steep Slopes:** While steering, the game restarts the steering animation every frame, and the engine cut that animation's movement to about a tenth at 144 FPS. The engine's limit now follows the frame time, so the hero steers at the same speed as at 30 FPS. Walking, rolling, sprinting and combat are unchanged. At the top of a steep slide, the hero sometimes did a "walking in the air" stumble before sliding above 30 FPS, also when attacking at the edge. The game checks every frame whether the hero is touching the slide surface, and at high frame rates that check could miss for a single frame. A touch now counts for one 30 FPS frame (1/30 s), which is what the game sees at 30 FPS, so the slide starts straight away.
  - **Smooth Cutscenes:** the in-engine cutscene camera no longer jerks at high FPS (tested up to 144 FPS).
  - **Soul Counter:** the souls counter no longer rolls up too fast at high FPS.
  - **Swamp Poison Build-up (Blighttown):** Above 30 FPS, the poison bar filled up to a third faster while standing in the Blighttown swamp. The swamp adds its poison in steps that the game counts in whole frames, so at high frame rates the steps came more often. DSfix now keeps each step at 0.1 s, as at 30 FPS. The poison damage, the bar going down outside the swamp, and lava are unchanged (they were already correct).

* **Keyboard + Mouse + Controller in the same time** - DDTMControllerFix.dll icon switch: Advanced mapping and support for controllers and Keyboard + Mouse. Play with the controller together with keyboard + mouse. Smooth 1:1 mouse camera, custom cursor, and key bindings (combos of two keys) in the `[Keyboard]` / `[Mouse]` sections of `DDTMControllerMappings.ini`. The game's own keyboard settings are not used.
* **True-Colour Menu Cursor (with DDTM Controller Fix):** The keyboard/mouse menu cursor is now drawn by DSfix inside the game picture, instead of as a Windows hardware cursor. Graphics drivers often show hardware cursors with different colours or brightness; drawn by DSfix, the cursor looks exactly like its picture on every PC, and it shows up in screenshots and recordings. It is made from one 64×64 picture and scaled to your resolution (32 px at 1080p, 43 px at 1440p, 64 px at 4K). Clicks, menus and HUD scaling are not affected. Without DSfix, DDTM Controller Fix shows its own Windows cursor, made from the same picture.
* **Mouse Wheel List Scrolling (with DDTM Controller Fix):** In item lists (Inventory, Bottomless Box, Equipment, shops, repair, upgrade) the mouse wheel now scrolls the list: each notch selects the next hidden item below or above, so the list moves row by row. Every notch counts, also when scrolling fast, and the first notch works right away. Other menus keep the game’s own wheel.
* **Button icons that follow your controls (DDTMControllerMappings)**
Needs DDTMControllerFix.dll. Set `DDTMControllerMappings 1` in DSfix.ini.
- **Keyboard and mouse:** while you play with keyboard and mouse, the in-game button prompts and the quick menu LB / RB icons show your own keys from DDTMControllerMappings.ini. Pick up the controller and the controller icons come back instantly.
- **Controller:** if you remapped buttons in DDTMControllerMappings.ini (face buttons, LB / RB, LT / RT), the prompts show the real button you mapped (for example `A = R1` shows RB where the game shows A). With this on, `swapConfirmIcons` and `swapXYIcons` are not needed.
- Works with every icon set (`controllerIcons`, tex_override or the game's own).
- The key icons come from `KeyboardMouse.png`, built into DSfix. To change their look, replace it with your own 16 × 16 grid picture (for example 1024 × 1024) in the same order.
---
* **Item Sort**
Sort the item lists in the Inventory, the Equipment item list and the Bottomless Box.

- **Controller:** hold **RS**, press **D-pad ←/→** to choose what to sort by and **D-pad ↑/↓** for Desc/Asc, then release **RS** to sort.
- **Keyboard:** hold **Left Alt**, press **←/→** to choose and **↑/↓** for Desc/Asc, then release **Left Alt** to sort.
- **Weapons and Armor:** Game Order, Most Recent, Physical, Magic, Fire, Lightning (attack or defense), Durability, Weight, and Storage (Bottomless Box only).
- **All other lists** (items, materials, keys, spells, arrows, rings): Game Order or Most Recent.
- **Most Recent:** the item you picked up, bought, received, upgraded, equipped or attuned last comes first. Taking an item out of the Bottomless Box does not count. Each copy of a weapon, armor piece or ring counts on its own. Items you had before using this sort are listed after the others.
- When an Equipment list opens, the cursor is on the item you have equipped, also when the list is sorted.
- A prompt in the menu shows the current choice. It works with the HUD on or off.
- Your choices and the Most Recent order (per character) are saved in `%LOCALAPPDATA%\NBGI\DarkSouls\DSfixSort.ini`.
- On by default. Turn it off with `WeaponsArmorSort 0` in DSfix.ini.
---
* **Bottomless Box Markers**
In the Bottomless Box list, a small chest icon in the top-right corner of an item's picture shows that **all of it is in the box** (Held 0).

- Works in all box tabs and in both list views.
- Uses the game's own chest icon, so texture mods change it too.
- Off by default. Turn it on with `BottomlessBoxMarkers 1` in DSfix.ini.
---
* **Quick-Select Estus**
Hold **D-pad Down** (350 ms) during play to jump straight to the Estus Flask in your quick-item slots, with the game's own animation and sound. A tap still switches to the next item as before.
With DDTM Controller Fix, holding its SWITCH_ITEM key works the same.
`QuickSelectEstus 1` in DSfix.ini (default 0 = off, nothing is patched).
---
* **Bonfire Covenant Switching**
Rest at a bonfire and choose **Enter Covenant** (below Attune Magic). You can switch to any covenant you have joined in this playthrough.
- Your current covenant is marked `*`. A broken covenant is marked `x`; picking it tells you why (for example, leader dead or hostile, Kaathe gone, or banished until absolution).
- The leaders' own Yes/No question is shown. On Yes, the oath sound plays and the screen fades.
- The result is the same as vanilla: the covenant you leave loses half of its offerings.
- Oswald is not involved. The list resets in NG+.
- Not available while a phantom is in your world.

`BonfireCovenants 1` in DSfix.ini (default 0 = off, nothing is patched).
## Bugs and Issues Fixed

### High-Precision Frame Limiter
* **Issue:** DSfix 2.4 waited for the next frame in a busy loop, which kept a CPU core busy and was less precise.
* **Applied Solution:** The limiter sleeps on a high-resolution timer and only spins for the last moment before the frame, for even frame times and less CPU use (on older Windows the timer resolution is raised to 1 ms).

### Memory Deallocation Mismatch (delete vs delete[])
* **Issue:** In `RSManager::prefetchTextures()`, texture buffer data is allocated as an array using `new char[data.size]`. However, `RSManager::~RSManager()` deletes it using `SAFEDELETE` (which executes scalar delete) instead of `delete[]`, causing undefined behavior and heap corruption upon shutdown.
* **Applied Solution:** Proper Heap Bookkeeping, Preventing Useless Copies.

### Thread-Unsafe Static Buffer in GetDirectoryFile()
* **Issue:** `GetDirectoryFile()` uses a single local static `char path[320]`. Because `SaveManager`, `BonfireGlitchDetectionThread`, and the rendering thread all execute asynchronously, concurrent calls to `GetDirectoryFile()` overwrite the buffer simultaneously, leading to file-path corruption and crashes.
* **Applied Solution:** Thread Isolation, Zero Caller Refactoring.

### Missing HWND Verification in KeyActions::processIO()
* **Issue:** Hotkey checks evaluate `GetForegroundWindow() != NULL`. This condition evaluates to true even when typing in other applications (such as Discord, a web browser, or an overlay), causing DSfix actions and the `g_Force30FPS` toggle to trigger outside the game.
* **Applied Solution:** Process Isolation, Overlay & Alt-Tab Immunity.

### Less Work Under Loader Lock in DllMain
* **Issue:** DllMain created background threads and loaded the chained dinput8.dll during DLL_PROCESS_ATTACH, which risks deadlocks at start-up.
* **Applied Solution:** Threads and the dinput8.dll loading now start on the game's first DirectInput call, outside the loader lock. Only the hooks that must exist before the game starts are still set in DllMain.

### Uninitialized hudVertices Buffer
* **Issue:** `RSManager` declares `INT16 hudVertices[32]` but leaves it uninitialized in the constructor. Calling `reloadHUDVertices` when `hudvertices.txt` is missing leaves uninitialized garbage in memory.
* **Applied Solution:** Safe Fallback - `memset`.

### Key Parser Read Before Check
* **Issue:** `KeyActions::load()` read the character after a key name before checking that the key name was found in the line.
* **Applied Solution:** The read is now done only after the check.

### Infinite Freeze on Zero FPS Limit
* **Applied Solution:** 60 FPS for user input of 0 fps limit.

### Override Language not keeping the change, sometimes the user needed to play with compatibility mode.
* **Applied Solution:** Language override no longer modifies the Windows registry or requires compatibility mode. It is applied inside the game process only

### Bonfire Softlock Fix (improved)
* **Applied Solution:** The bonfire softlock check now verifies the game's memory before reading it. It no longer triggers hidden errors during loading screens or when joining another player's world. The stand-up fix runs only with `unlockFPS 1`.
---

## Credits

DSfix 3.0 is a modified version of DSfix by Durante (Peter Thoman), released under the GNU GPL v3 (see `LICENSE.txt`).
Source code: <your GitHub link>

* **[Durante / PeterTh](https://github.com/PeterTh/dsfix):** the original DSfix 2.4, the foundation of DSfix 3.0.
* **Nwks (Clement Barnier):** the original DSfix FPS unlock.
* **backfoggen:** [High-Res UI and Subtitle fonts](https://www.nexusmods.com/darksouls/mods/21), built into DSfix 3.0 with credit.
* **[Hal47's DSfix fork](https://github.com/Hal47/dsfix)** (sun and lens flare patch by Seirra): the Sun and Moon rendering fix at high resolutions.
* **Nicklas Lindgren (niligulmohar):** the water surface fix (part of Durante's DSfix since 2016).
* **Bonfire softlock fix:** logic from [Sean Pesce's FPSFix+](https://github.com/SeanPesce/FPSFix-Plus), based on [NullBy7e's FPSFix](https://github.com/NullBy7e/FPSFix).
* **Dark Souls Overhaul team (metal-crow & Sean Pesce):** memory research (equipment and parameter table layouts, durability addresses, and the item-use and covenant-offering addresses) used for the item auto-equip, weapon durability bar, multiple item use and multiple covenant offerings features. All of it is DSfix 3.0's own code. https://github.com/metal-crow/Dark-Souls-1-Overhaul
* **AMD FidelityFX CAS:** the sharpening (MIT license). https://github.com/GPUOpen-Effects/FidelityFX-CAS
* **SMAA:** Uses SMAA. Copyright (C) 2011 by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez.
* **NVIDIA FXAA 3.11** by Timothy Lottes.
* **SSAO effects:** Tomerk (OBGE VSSAO, HBAO), Asmodean (VSSAO2), Durante.
* **SuperFastHash** by Paul Hsieh. **Microsoft Detours** (MIT).
* **[DDTM Controller Fix](https://github.com/DDTM-bit/DDTMControllerFixPDTE)** (`DDTMControllerFix.dll`, included): GNU GPL v3. It uses SDL2, SDL2-CS, MinHook.NET (a port of MinHook), SharpDisasm/udis86 and the .NET runtime.


## Third-Party Licenses

The full license texts are in the archive:
* `LICENSE.txt`: GNU GPL v3 (DSfix 3.0 and DDTM Controller Fix)
* `DSfix_THIRD_PARTY_LICENSES.txt`: AMD FidelityFX CAS, SMAA, NVIDIA FXAA, SuperFastHash, Microsoft Detours, High-Res UI and Subtitle fonts
* `DDTMControllerFix_THIRD_PARTY_LICENSES.txt`: SDL2, SDL2-CS, MinHook.NET, MinHook, SharpDisasm/udis86, .NET runtime

Source code:
* DSfix 3.0: https://github.com/DDTM-bit/DSfix-3.0---Dark-Souls-PTDE
* DDTM Controller Fix: https://github.com/DDTM-bit/DDTMControllerFixPDTE
