# DSfix 3.0 — Dark Souls: Prepare to Die Edition

This is the modernized overhaul for Dark Souls: Prepare to Die Edition, engineered to deliver high-resolution, smooth gameplay on modern Windows 10 and 11 systems.

---

## System Requirements

* **Steam Version Only:** This mod is strictly compatible ONLY with the LATEST STEAM VERSION of the game. Retail or older GFWL builds will fail to hook properly.
* **DirectX End-User Runtimes (June 2010):** This is absolutely mandatory. While Windows 11 natively supports DirectX 9, Microsoft stopped bundling the specific legacy D3D helper libraries (like `d3dx9_43.dll` and `d3dcompiler_43.dll`) with the OS. DSfix 3.0 relies heavily on these exact libraries to compile and render its advanced shaders (SMAA, FXAA, SSAO, and Depth of Field). Without this package, the game will instantly crash with a missing file error.
* **Visual C++ Redistributable (x86):** You must install the 32-bit (x86) versions of the Visual C++ redistributables. The original Dark Souls PTDE executable relies on the older Visual C++ 2010 Redistributable, while modern community tools (and heavily modernized DLLs) often expect the Visual C++ 2015–2022 Redistributable. Ensure both are installed to prevent `0xc0150002` application launch errors.
* **Disabled In-Game Rendering Effects:** Before launching with DSfix 3.0, you must ensure that the game's native Anti-Aliasing and Motion Blur settings are turned completely OFF in the game menu or `DarkSouls.ini` configuration file. Leaving the vanilla AA enabled causes a severe rendering conflict with DSfix's pipeline that will result in a black screen.

---

## Base Installation

Extract the contents of the DSfix 3.0 archive into your Dark Souls executable directory, right next to `DARKSOULS.exe`.

---

## Modernized Features and Fixes of DSfix 3.0

* **High-DPI & Display Scaling Fix:** Stripped out the 150% DPI stretching. On Windows 10/11 with 1440p or 4K monitors set to desktop scaling, the game misinterprets dimensions causing blurry bilinear upscaling and mouse-capture offset bugs. With DSfix 3.0, it will never artificially scale the game window.
* **High-Precision Frame Pacing:** Replaced the broken framerate pacing with a modern solution.
* **The 60 FPS Physics Traps (Ladder Glitch & Short Rolls):** PTDE’s physics engine was hardcoded for 30 ticks per second. At 60 FPS, sliding down ladders causes collision clipping, and jump distances are shortened.
Replaced the original DSfix 100% CPU spinlock with a modern, low-overhead hybrid waitable timer. It guarantees sub-millisecond frame delivery without oversleeping. Furthermore, it feeds exact, unquantized delta-time directly to the Havok physics engine, ensuring pixel-perfect camera panning and smooth transitions when toggling the 30 FPS lock.
* **Alt+Tab Memory Leak Plugged:** In vanilla DSfix 2.4, the mod forgot to delete custom FXAA, SSAO, and DoF buffers during a Device Lost event, silently eating VRAM until the game crashed. This is now fixed, the engine is vastly more resilient to buffer collisions.
* **Sun/Moon & Water Issue Fixed:** Now they render perfectly at their intended screen-space locations, whether you are playing at 1080p, 1440p, or 4K.
* **Modern CPU Power Management:** Dynamically disables Windows 11 EcoQoS (Power Throttling) to ensure the game engine runs at maximum turbo clocks. By removing artificial priority masks, it allows the native OS Thread Director to smoothly balance the game's rendering, audio, and networking threads across modern hybrid CPUs without causing driver starvation or micro-stutters.
* **60 FPS Bonfire Fix and Smart Protection:** Automatically detects and neutralizes the infamous bonfire animation softlock while dynamically disabling SSAO while resting to prevent dark halo artifacts around your character.
* **Dynamic SSAO Reloading:** SSAO type switches (VSSAO/HBAO/SCAO) can also be dynamically reloaded in real time using dev hotkeys (`reloadVSSAOEffect`, etc.).
* **Backup Saves Fix:** Now, the default path can be set in `DSFix.ini` file and it supports Windows environment variables. Example: `%USERPROFILE%\Documents\NBGI\DarkSouls`
* **VRAM Optimization:** Post-processing effects (AA, SSAO, DoF) now utilize a shared buffer pool, reducing VRAM usage and improving frame pacing.

---

## Best Practices & Recommendations

To ensure maximum stability on modern machines, configure your environment using these proven guidelines:

* **Native Resolution Mapping:** Ensure your in-game resolution is set to match your desktop monitor exactly so the image is sharp and on all screen sizes. You can try to set your resolution in game menu or in: `%LOCALAPPDATA%\NBGI\DarkSouls\DarkSouls.ini`.
* **Unlock FPS Enabled:** Always keep the `unlockFPS` enabled in `DSFix.ini` even when `FPSlimit` is 30FPS. With `unlockFPS` disabled, DSfix skips its frame-timing code entirely and the game uses its original, unpatched engine logic.
* **Texture Prefetching:** If you don't use textures that are more than 2GB you can set the option `enableTexturePrefetch` to `1` in `DSfix.ini` for a faster texture load. Dark Souls PTDE is a 32-bit executable, meaning the entire game will hard-crash if it tries to use more than 4GB of total RAM. If you use a massive 4K texture pack (like Mulderland's Enhancement Pack) and enable prefetching, DSfix will attempt to load gigabytes of `.png` files into RAM all at once.
* **Disable Fullscreen Optimizations:** Find your compiled `DARKSOULS.exe`, right-click > Properties > Compatibility, and check "Disable fullscreen optimizations". This allows the modernized `WaitableTimer` from DSFix 3.0 and its native borderless window code to control the pacing perfectly without Windows 11 interfering.
* **Bypass the 60Hz Crash:** In the game menu, set Fullscreen to OFF. Then, open `DSfix.ini` and set `borderlessFullscreen 1`. This solves the engine limitation where the game hard-crashes when selecting "New Game" or "PC Settings" if your monitor's refresh rate is above 60Hz.

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

---

## Issues Requiring External Workarounds

Certain engine limitations are hardcoded too deeply into Havok or DirectX 9 to be intercepted by DSfix 3.0. For these specific issues, you must rely on external tools.

1. **Disabling the Steam Overlay**
   * **DSfix 3.0 Status:** CANNOT FIX.
   * **Details:** The Steam overlay forcibly injects its own DirectX hooks. Because old and new DSfix is also hooking the exact same `Present` and `Reset` functions, leaving the Steam overlay on creates a race condition that freezes the game. Keep the overlay disabled.
2. **Flawless Widescreen (21:9 Aspect Ratios)**
   * **DSfix 3.0 Status:** CANNOT FIX.
   * **Details:** DSfix forces the game to render at ultrawide resolutions, but the UI coordinates in `Hud.cpp` are mapped using fixed 16:9 float mathematics. If you have an ultrawide monitor, you still need Flawless Widescreen to intercept the UI memory and un-stretch the health bars.
3. **Tomb of the Giants Stuttering & Throttlestop**
   * **DSfix 3.0 Status:** CANNOT FIX.
   * **Details:** The massive frame drops in Tomb of the Giants and Blighttown are caused by engine culling failures (the game is trying to render the entire level geometry at once). DSfix cannot fix bad level design.
4. **DXVK (Vulkan Wrapper)**
   * **DSfix 3.0 Status:** COMPATIBLE.
   * **Details:** DirectX 9 is inherently single-threaded. DSfix cannot change this. Dropping the DXVK `d3d9.dll` into your folder forces DirectX 9 to translate into Vulkan, which utilizes modern multi-core CPUs much better.
   * **Attention:** If you want to use PVP Watchdog with Vulkan... you can't. PVP Watchdog needs `d3d9.dll`, the same one for Vulkan, and if you try with `DSPWSteam.ini` to change `d3d9dllWrapper` it will make the game have serious performance issues.

---

## Obsolete Mods (No Longer Required)

Thanks to the native engine-level integrations in DSfix 3.0, you no longer need to bloat your game folder with the following external fixes:

* **Standalone FPSFix / Bonfire Softlock Fix:** New solution completely integrated natively.
* **Custom Resolution Utilities (CRU):** Handled seamlessly by setting the native Borderless Fullscreen toggle in `DSfix.ini`.
* **High DPI Override Executables:** Windows 11 scaling is bypassed automatically on startup.
* **Task Manager CPU Affinity Scripts:** Thread priority is now locked at the engine level.
