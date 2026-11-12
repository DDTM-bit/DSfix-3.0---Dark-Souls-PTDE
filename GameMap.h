// GameMap.h - the in-game map, shown with the gesture window (DSfix.ini: gameMap 1).
// Header-only so the project file does not change: included once, from RenderstateManager.cpp (after swCursor).
// gameMap 0 = nothing at all: no hook, no file, no drawing, no recording (as if the feature did not exist).
//
// WHAT IT DOES
//   * While you play, the ground around the hero is recorded as explored (2 m cells within kRevealRadius of where he
//     walked, at his height, with their area) and kept per character in DSfixMap.dat next to the save (DRAKS0005.sl2).
//   * While the gesture window is open (Back), the map shows in the free space left of the gesture list, a little
//     see-through (the game is not paused). The bottom-left HUD is faded out meanwhile (its own opacity, set to 0).
//   * Area page: one area (Firelink Shrine, Undead Burg, ...), only its explored ground, its lit bonfires and the
//     hero. World page: every area of Lordran together with their names. The Painted World, the DLC areas and the
//     Asylum are far away in world coordinates: they have only their own page.
//   * Explored ground at the hero's height is bright; lower ground darker, higher ground fainter. A lit bonfire below
//     the hero is smaller and darker, one above has a small up mark.
//   * The hero's soul remains (bloodstain): a green spot; when it is outside the map, a green arrow on the edge.
//   * No fast travel: the map only shows.
//   Controls (fixed): pad RS move, LT / RT zoom out / in, LB / RB previous / next area, R3 area / world page (the camera
//   does not move and does not reset meanwhile). Keyboard / mouse: left drag move, wheel zoom, comma / period previous /
//   next area, M area / world page. With DDTMControllerFix the physical buttons are used, whatever its [Bindings] say.
//
// WHERE THE GAME KEEPS IT (DARKSOULS.exe, PE timestamp 0x557F2FD0; all found by DSfix and checked in game)
//   MenuMan [0x013786D0]: +0xEC gesture window (0 closed, 1 / 2 / 3 open, 7 / 9 for one frame while closing),
//     +0xEB4 the area of the area banner (PlaceName id, -1 right after loading), +0x7EC loading (1..4)
//   Hero: ChrIns = [[0x0137D644] + 0x3C]; [[ChrIns + 0x28] + 0x1C]: +0x4 yaw (forward = (-sin, -cos) in x / z),
//     +0x10 x y z (world coordinates, the same for all areas)
//   Map block: the game's 0xD6B790 reads [0x01378520]: +0x08 list (+0x0C count, +0x10 entries of 0x3C, id at +4),
//     +0x14 index; the id is area << 24 | block << 16
//   Bonfires: [[0x0137E204] + 0xB48] + 0x24 = list head (what the bonfire talk scripts read, 0xD75D60 / 0xBD8870):
//     node +0 next, +8 item; item +4 bonfire entity, WORD +8 kindle (0 unlit, 10 lit, 20 / 30 / 40 kindled). A bonfire
//     is in the list once its map was loaded. Positions and entities: the game's MSB files (object o0200).
//   Soul remains: GameDataMan [0x01378700]: [+0x1C] = record, +0 x y z, +0x34 humanity, +0x38 souls (-1 = none; the
//     game's own check 0xD5B3B9 is souls > 0), +0x3C map id (as the map block id). The byte +0x18 alone is not enough:
//     it stays set after the souls were picked up, and is set for a new character too.
//   Character: GameMan [0x013784A0] +0xA70 = save slot in play (0..9); [GameDataMan] + 8 = player data, +0xA0 name
//   Area names: the game's PlaceName text getter 0xD17700 (edi = text id), so they are in the game's language
#pragma once

#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <unordered_map>
#include <shlobj.h>
#include "main.h"
#include "Settings.h"
#include "resource1.h"
#include "Detouring.h"
#include "GameParams.h"

namespace gameMap {

// =====================================================================================================================
// LOOK - everything that decides how the map looks. Sizes are in the game's UI pixels (1280 x 720, scaled to every
// resolution), heights in metres.
// =====================================================================================================================
namespace Look {
    // the window: left of the gesture list, under the gesture bar (the free space of the gesture window)
    const float kLeft = 68.0f, kTop = 216.0f, kRight = 832.0f, kBottom = 668.0f;
    const float kFadeSeconds = 0.12f;                       // fade in / out
    const D3DCOLOR kPanel = D3DCOLOR_ARGB(150, 10, 8, 6);  // the see-through background
    const D3DCOLOR kFrameOuter = D3DCOLOR_ARGB(230, 176, 146, 92);
    const D3DCOLOR kFrameInner = D3DCOLOR_ARGB(120, 176, 146, 92);
    const float kFrameGap = 3.0f;                           // between the outer and the inner frame line
    // explored ground: parchment
    const float kGroundR = 0.87f, kGroundG = 0.79f, kGroundB = 0.62f;
    const float kGroundAlpha = 0.80f;
    const float kRevealRadius = 7.0f;                       // metres around the hero that count as explored
    const float kGroundBlob = 4.0f;                         // each cell adds a soft round spot this many cells wide; where
                                                            // the spots add up to half, the ground's smooth edge is
    const D3DCOLOR kGroundEdge = D3DCOLOR_ARGB(255, 70, 52, 28);   // the outline of the explored ground
    const float kGroundEdgeWidth = 1.5f;
    const float kLevelBand = 4.0f;                          // within this height the ground counts as the hero's level
    const float kFadeHeight = 30.0f;                        // height difference at which the ground reaches its minimum
    const float kBelowMin = 0.28f, kAboveMin = 0.45f;       // brightness of ground far below / far above
    // icons
    const float kBonfireSize = 30.0f;
    const float kBonfireBelowScale = 0.70f, kBonfireBelowDark = 0.55f;
    const float kBonfireLevel = 4.0f;                       // area page: more than this below / above the hero = "below" / "above"
    const float kBonfireLevelWorld = 25.0f;                 // world page: the same, for whole floors / areas under each other
    // bonfire labels: the warp list's name, else the area the game's banner showed right at the bonfire (none if the
    // map has no explored ground there yet). Always on the area page; on the world page from this zoom in.
    const float kBonfireNameSize = 11.0f;
    const D3DCOLOR kBonfireName = D3DCOLOR_ARGB(240, 236, 214, 170);
    const D3DCOLOR kNameBack = D3DCOLOR_ARGB(200, 14, 11, 8), kNameLine = D3DCOLOR_ARGB(210, 176, 146, 92);
    const float kNamePadX = 4.0f, kNamePadY = 1.5f;
    const float kBonfireNameWorldZoom = 1.2f;              // world page: labels when zoomed in to this (metres per UI pixel) or closer
    const D3DCOLOR kUpMark = D3DCOLOR_ARGB(255, 240, 200, 110);
    const float kHeroSize = 12.0f;                          // half the arrow picture
    const D3DCOLOR kHero = D3DCOLOR_ARGB(255, 245, 222, 140), kHeroOutline = D3DCOLOR_ARGB(255, 25, 18, 10);
    const D3DCOLOR kStain = D3DCOLOR_ARGB(255, 90, 235, 120), kStainGlow = D3DCOLOR_ARGB(150, 20, 120, 40);
    const float kStainSize = 11.0f, kStainArrow = 13.0f, kStainEdge = 16.0f;
    // text (heights in UI pixels)
    const wchar_t kFont[] = L"Palatino Linotype";
    const float kTitleSize = 21.0f, kLabelSize = 15.0f, kHelpSize = 13.0f;
    const D3DCOLOR kTitle = D3DCOLOR_ARGB(255, 232, 216, 178), kLabel = D3DCOLOR_ARGB(235, 225, 205, 160);
    const D3DCOLOR kHelp = D3DCOLOR_ARGB(215, 200, 188, 165), kShadow = D3DCOLOR_ARGB(200, 0, 0, 0);
    const float kPad = 10.0f;                               // text distance from the frame
    const float kLegendIcon = 16.0f;                        // icon size in the legend line
    const float kLegendGap = 22.0f;                         // space between two legend entries
    const int kLabelMinCells = 25;                          // the world page names an area once it has this many cells
                                                            // (on its explored ground nearest to the area's middle; labels
                                                            // never overlap: area names first, then bonfire labels)
    // zoom: metres per UI pixel (smaller = closer)
    const float kAreaZoom = 0.35f, kAreaZoomMin = 0.06f, kAreaZoomMax = 3.0f;
    const float kWorldZoom = 2.2f, kWorldZoomMin = 0.4f, kWorldZoomMax = 6.0f;
    const float kTriggerZoomPerSecond = 3.0f;               // full trigger: this factor per second
    const float kWheelZoomStep = 0.80f;                     // one wheel notch up
    const float kStickPanPerSecond = 0.8f;                  // full stick: this part of the window width per second
    const bool kMirrorX = false;                            // true = the map is drawn mirrored left / right
    const float kRotationDeg = 90.0f;                       // the map's turn (counter-clockwise): 90 = Anor Londo on top,
                                                            // Ash Lake at the bottom. The same for every page.
}

// =====================================================================================================================
// Game data
// =====================================================================================================================
    // Areas (PlaceName ids): their map block and whether they are part of the world page. The order is the order of
    // the area pages (LB / RB).
    struct PlaceDef { WORD id; WORD block; bool world; };
    static const PlaceDef kPlaces[] = {
        { 1810, 0x1201, false },   // Northern Undead Asylum
        { 1020, 0x0A02, true },    // Firelink Shrine
        { 1010, 0x0A01, true },    // Undead Burg
        { 1011, 0x0A01, true },    // Undead Parish
        { 1000, 0x0A00, true },    // Depths
        { 1400, 0x0E00, true },    // Blighttown
        { 1401, 0x0E00, true },    // Quelaag's Domain
        { 1200, 0x0C00, true },    // Darkroot Garden
        { 1201, 0x0C00, true },    // Darkroot Basin
        { 1300, 0x0D00, true },    // The Catacombs
        { 1310, 0x0D01, true },    // Tomb of the Giants
        { 1600, 0x1000, true },    // New Londo Ruins
        { 1602, 0x1000, true },    // Valley of Drakes
        { 1601, 0x1000, true },    // The Abyss
        { 1500, 0x0F00, true },    // Sen's Fortress
        { 1510, 0x0F01, true },    // Anor Londo
        { 1100, 0x0B00, false },   // Painted World of Ariamis
        { 1700, 0x1100, true },    // The Duke's Archives
        { 1701, 0x1100, true },    // Crystal Cave
        { 1320, 0x0D02, true },    // The Great Hollow
        { 1321, 0x0D02, true },    // Ash Lake
        { 1410, 0x0E01, true },    // Demon Ruins
        { 1411, 0x0E01, true },    // Lost Izalith
        { 1800, 0x1200, true },    // Kiln of the First Flame
        { 1801, 0x1200, true },    // Firelink Altar
        { 1210, 0x0C01, false },   // Sanctuary Garden (DLC)
        { 1211, 0x0C01, false },   // Oolacile Sanctuary
        { 1212, 0x0C01, false },   // Royal Wood
        { 1213, 0x0C01, false },   // Oolacile Township
        { 1214, 0x0C01, false },   // Chasm of the Abyss
        { 1215, 0x0C01, false },   // Battle of Stoicism
        { 1216, 0x0C01, false },   // Battle of Stoicism Gazebo
    };
    static const int kPlaceCount = sizeof(kPlaces) / sizeof(kPlaces[0]);

    // Bonfires (the game's MSB + EMEVD data; positions are world coordinates). place = the area page it belongs to.
    // name = its PlaceName text id in the game's warp list (table at 0x012DE888: flag, entity, name), -1 = no name.
    // The Firelink Altar (o8310, entity 1801960) is the Lordvessel altar, not a bonfire: left out.
    struct BonfireDef { DWORD entity; WORD block; WORD place; short name; float x, y, z; };
    static const BonfireDef kBonfires[] = {
        { 1001960, 0x0A00, 1000, 2014,  -120.50f,   -74.13f,    12.20f },
        { 1011961, 0x0A01, 1011, 2001,    24.35f,     9.50f,   -22.20f },
        { 1011962, 0x0A01, 1010,   -1,     3.20f,   -10.02f,   -61.20f },
        { 1011964, 0x0A01, 1011, 2015,    87.90f,    15.00f,   107.50f },
        { 1021960, 0x0A02, 1020, 2000,   -51.80f,   -60.60f,    55.70f },
        { 1101960, 0x0B00, 1100, 2016,   -23.00f,    52.32f,   944.00f },
        { 1201961, 0x0C00, 1200,   -1,   257.50f,    -3.30f,   -11.75f },
        { 1211950, 0x0C01, 1214, 2012,   854.00f,  -576.80f,   848.00f },
        { 1211961, 0x0C01, 1211, 2010,   971.70f,  -313.93f,   583.30f },
        { 1211962, 0x0C01, 1213, 2011,  1040.60f,  -332.00f,   875.90f },
        { 1211963, 0x0C01, 1210, 2009,   897.50f,  -329.27f,   451.50f },
        { 1211964, 0x0C01, 1213, 2013,   862.80f,  -447.50f,   911.20f },
        { 1301960, 0x0D00, 1300,   -1,    43.90f,  -119.08f,   202.85f },
        { 1301961, 0x0D00, 1300,   -1,    49.00f,  -111.86f,   301.50f },
        { 1311950, 0x0D01, 1310, 2007,  -160.00f,  -264.90f,   -34.00f },
        { 1311960, 0x0D01, 1310,   -1,    76.30f,  -214.31f,    43.90f },
        { 1311961, 0x0D01, 1310, 2017,    96.00f,  -199.84f,   104.00f },
        { 1321960, 0x0D02, 1321, 2008,  -700.00f,  -413.94f,  -323.50f },
        { 1321961, 0x0D02, 1321,   -1,  -387.95f,  -408.26f,   154.85f },
        { 1321962, 0x0D02, 1320,   -1,  -319.00f,  -236.06f,   122.50f },
        { 1401960, 0x0E00, 1401, 2006,   140.43f,  -252.61f,    95.05f },
        { 1401961, 0x0E00, 1400,   -1,  -197.50f,  -215.04f,   100.50f },
        { 1401962, 0x0E00, 1400,   -1,  -277.50f,  -136.72f,    72.10f },
        { 1411950, 0x0E01, 1411,   -1,   581.08f,  -443.94f,   444.83f },
        { 1411960, 0x0E01, 1411,   -1,   455.60f,  -380.03f,   169.30f },
        { 1411961, 0x0E01, 1410,   -1,   195.00f,  -267.03f,   130.00f },
        { 1411962, 0x0E01, 1410,   -1,   251.90f,  -334.03f,    22.00f },
        { 1411963, 0x0E01, 1410,   -1,   118.80f,  -356.59f,   138.20f },
        { 1411964, 0x0E01, 1410,   -1,   229.50f,  -383.52f,    91.50f },
        { 1501961, 0x0F00, 1500,   -1,    72.50f,    60.00f,   301.30f },
        { 1511950, 0x0F01, 1510, 2003,   593.00f,   161.00f,   255.00f },
        { 1511960, 0x0F01, 1510, 2002,   169.80f,   173.40f,   255.00f },
        { 1511961, 0x0F01, 1510,   -1,   504.00f,   135.00f,   174.60f },
        { 1511962, 0x0F01, 1510, 2004,   392.00f,    69.80f,   255.00f },
        { 1601950, 0x1000, 1601, 2005,    85.30f,  -311.00f,     3.17f },
        { 1601961, 0x1000, 1602,   -1,   165.00f,   -76.68f,   -56.00f },
        { 1701950, 0x1100, 1701, 2019,    95.65f,   134.05f,   864.18f },
        { 1701960, 0x1100, 1700, 2018,   350.08f,   277.59f,   594.38f },
        { 1701961, 0x1100, 1700,   -1,   378.40f,   269.78f,   551.70f },
        { 1701962, 0x1100, 1700,   -1,   230.00f,   199.98f,   482.00f },
        { 1811960, 0x1201, 1810,   -1,     3.10f,   195.91f,     7.44f },
        { 1811961, 0x1201, 1810,   -1,    33.34f,   193.15f,   -26.47f },
    };
    static const int kBonfireCount = sizeof(kBonfires) / sizeof(kBonfires[0]);

    const DWORD kMenuMan      = 0x013786D0;
    const DWORD kWorldChrMan  = 0x0137D644;
    const DWORD kGameMan      = 0x013784A0;
    const DWORD kGameDataMan  = 0x01378700;
    const DWORD kMapList      = 0x01378520;
    const DWORD kBonfireRoot  = 0x0137E204;
    const DWORD kFnPlaceName  = 0x00D17700;      // edi = PlaceName id -> text (MsgMan +0x1E4 patch, then +0x50)

    // ---- safe reads / calls (no C++ objects in these functions: __try) ----
    bool rdD(DWORD a, DWORD& v) {
        __try { v = *(const DWORD*)a; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool rdBlock(DWORD a, void* out, DWORD n) {
        __try { memcpy(out, (const void*)a, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    __declspec(naked) DWORD __stdcall callEdi(DWORD fn, DWORD ediValue) {
        __asm {
            pushad
            mov edi, dword ptr[esp + 0x28]
            call dword ptr[esp + 0x24]
            mov dword ptr[esp + 0x1C], eax
            popad
            ret 8
        }
    }
    const wchar_t* placeNameRaw(DWORD id) {
        __try { return (const wchar_t*)callEdi(kFnPlaceName, id); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    }
    // the text, or NULL when the game has none (or only a blank)
    const wchar_t* placeName(WORD id) {
        const wchar_t* t = placeNameRaw(id);
        if (!t) return NULL;
        __try { if (!t[0] || (t[0] == L' ' && !t[1])) return NULL; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
        return t;
    }

    int placeIndex(DWORD id) {
        for (int i = 0; i < kPlaceCount; ++i) if (kPlaces[i].id == id) return i;
        return -1;
    }
    int blockDefaultPlace(DWORD block) {           // the first area of a map block
        for (int i = 0; i < kPlaceCount; ++i) if (kPlaces[i].block == block) return i;
        return -1;
    }
    DWORD playerChr() {
        DWORD wcm = 0, chr = 0;
        if (!rdD(kWorldChrMan, wcm) || !wcm || !rdD(wcm + 0x3C, chr)) return 0;
        return chr;
    }
    bool heroPos(DWORD chr, float* p4) {          // x y z, yaw
        DWORD ctrl = 0, phys = 0;
        float f[8];
        if (!rdD(chr + 0x28, ctrl) || !ctrl || !rdD(ctrl + 0x1C, phys) || !phys || !rdBlock(phys, f, sizeof(f))) return false;
        if (!(f[4] == f[4]) || !(f[5] == f[5]) || !(f[6] == f[6]) || fabsf(f[4]) > 20000.0f || fabsf(f[6]) > 20000.0f) return false;
        p4[0] = f[4]; p4[1] = f[5]; p4[2] = f[6]; p4[3] = f[1];
        return true;
    }
    bool currentBlock(DWORD& block) {             // area << 8 | block
        DWORD m = 0, arr = 0, idx = 0, cnt = 0, ent = 0, id = 0;
        if (!rdD(kMapList, m) || !m || !rdD(m + 0x08, arr) || !arr || !rdD(m + 0x14, idx)) return false;
        if (!rdD(arr + 0x0C, cnt) || (int)idx < 0 || idx >= cnt || !rdD(arr + 0x10, ent) || !ent) return false;
        if (!rdD(ent + idx * 0x3C + 4, id)) return false;
        block = (id >> 16) & 0xFFFF;
        return true;
    }
    int menuDword(DWORD off) {
        DWORD mm = 0, v = 0;
        if (!rdD(kMenuMan, mm) || !mm || !rdD(mm + off, v)) return 0;
        return (int)v;
    }

// =====================================================================================================================
// Explored ground
// =====================================================================================================================
    // Saved: 2 m cells (x, z), 3 m height steps, and the area. The coarser copies (8 m, 32 m) are only for drawing a
    // zoomed-out map with few blobs.
    struct CellRec { short x, z, y; WORD place; };          // 8 bytes, as in the file (place = PlaceName id)
    const int kLods = 3;
    const float kCellSize[kLods] = { 2.0f, 8.0f, 32.0f };
    const float kCellHeight[kLods] = { 3.0f, 6.0f, 12.0f };
    std::unordered_map<unsigned long long, WORD> g_cellSet[kLods];   // key -> place index
    std::vector<CellRec> g_cells[kLods][kPlaceCount];               // per area (place = place index here)
    struct PlaceStat { double sx, sy, sz; int n; };
    PlaceStat g_stat[kPlaceCount];
    struct Anchor { int n; DWORD tick; float x, z; };   // where the world page writes an area's name (placeAnchor)
    Anchor g_anchor[kPlaceCount];
    DWORD g_cellCount = 0;
    bool g_dirty = false;

    inline unsigned long long cellKey(int x, int z, int y) {
        return ((unsigned long long)(WORD)(short)x << 32) | ((unsigned long long)(WORD)(short)z << 16) | (WORD)(short)y;
    }
    inline int fl(float v, float s) { return (int)floorf(v / s); }

    void clearCells() {
        for (int l = 0; l < kLods; ++l) {
            g_cellSet[l].clear();
            for (int p = 0; p < kPlaceCount; ++p) g_cells[l][p].clear();
        }
        memset(g_stat, 0, sizeof(g_stat));
        memset(g_anchor, 0, sizeof(g_anchor));
        g_cellCount = 0;
    }

    // one 2 m cell; returns true when it was new
    bool addCell(int x0, int z0, int y0, int place) {
        if (place < 0 || place >= kPlaceCount) return false;
        if (x0 < -32000 || x0 > 32000 || z0 < -32000 || z0 > 32000 || y0 < -32000 || y0 > 32000) return false;
        if (!g_cellSet[0].insert(std::make_pair(cellKey(x0, z0, y0), (WORD)place)).second) return false;
        CellRec c = { (short)x0, (short)z0, (short)y0, (WORD)place };
        g_cells[0][place].push_back(c);
        PlaceStat& st = g_stat[place];
        st.sx += (x0 + 0.5) * kCellSize[0]; st.sy += (y0 + 0.5) * kCellHeight[0]; st.sz += (z0 + 0.5) * kCellSize[0];
        ++st.n;
        ++g_cellCount;
        const float x = (x0 + 0.5f) * kCellSize[0], z = (z0 + 0.5f) * kCellSize[0], y = (y0 + 0.5f) * kCellHeight[0];
        for (int l = 1; l < kLods; ++l) {
            const int xl = fl(x, kCellSize[l]), zl = fl(z, kCellSize[l]), yl = fl(y, kCellHeight[l]);
            if (g_cellSet[l].insert(std::make_pair(cellKey(xl, zl, yl), (WORD)place)).second) {
                CellRec cl = { (short)xl, (short)zl, (short)yl, (WORD)place };
                g_cells[l][place].push_back(cl);
            }
        }
        return true;
    }

    // the hero stands here: every cell within kRevealRadius, at his height. Returns true when one was new.
    bool reveal(float x, float y, float z, int place) {
        const int cx = fl(x, kCellSize[0]), cz = fl(z, kCellSize[0]), cy = fl(y, kCellHeight[0]);
        const int r = (int)ceilf(Look::kRevealRadius / kCellSize[0]);
        const float r2 = Look::kRevealRadius * Look::kRevealRadius;
        bool any = false;
        for (int dx = -r; dx <= r; ++dx)
            for (int dz = -r; dz <= r; ++dz) {
                const float ox = (cx + dx + 0.5f) * kCellSize[0] - x, oz = (cz + dz + 0.5f) * kCellSize[0] - z;
                if (ox * ox + oz * oz > r2 && (dx || dz)) continue;
                if (addCell(cx + dx, cz + dz, cy, place)) any = true;
            }
        return any;
    }

    // the area of the explored ground at a point (-1 = none near)
    int placeAt(float x, float y, float z, int maxDy = 2) {
        const int cx = fl(x, kCellSize[0]), cz = fl(z, kCellSize[0]), cy = fl(y, kCellHeight[0]);
        for (int r = 0; r <= 3; ++r)
            for (int dy = 0; dy <= maxDy; ++dy)
                for (int s = 0; s < 2; ++s) {
                    const int yy = cy + (s ? -dy : dy);
                    for (int dx = -r; dx <= r; ++dx)
                        for (int dz = -r; dz <= r; ++dz) {
                            if (abs(dx) != r && abs(dz) != r) continue;
                            std::unordered_map<unsigned long long, WORD>::const_iterator it = g_cellSet[0].find(cellKey(cx + dx, cz + dz, yy));
                            if (it != g_cellSet[0].end()) return it->second;
                        }
                }
        return -1;
    }

// =====================================================================================================================
// The file: DSfixMap.dat next to DRAKS0005.sl2. "DSMP", version 1, 10 slots: used, name (16 wchar), count, cells.
// =====================================================================================================================
    struct SlotData { bool used; wchar_t name[16]; std::vector<CellRec> cells; };
    SlotData g_file[10];
    bool g_fileRead = false;
    std::string g_filePath;                           // empty = no save folder found: kept only while the game runs
    int g_activeSlot = -1;
    wchar_t g_activeName[16] = { 0 };
    volatile LONG g_writing = 0;
    DWORD g_lastSaveTick = 0;
    struct WriteJob { std::string path; std::vector<BYTE> data; };

    // the save folder: the custom one from DSfix.ini, else the newest DRAKS0005.sl2 in Documents\NBGI\DarkSouls (and its
    // sub-folders), as quickSave looks for it
    std::string findSaveFolder() {
        const std::string& custom = Settings::get().getCustomSaveFolder();
        if (custom != "none" && !custom.empty()) return custom;
        char docs[MAX_PATH];
        if (FAILED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, docs))) return std::string();
        const std::string base = std::string(docs) + "\\NBGI\\DarkSouls";
        std::string best;
        FILETIME bestT = { 0, 0 };
        std::vector<std::string> dirs;
        dirs.push_back(base);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((base + "\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, ".."))
                    dirs.push_back(base + "\\" + fd.cFileName);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        for (size_t i = 0; i < dirs.size(); ++i) {
            WIN32_FILE_ATTRIBUTE_DATA a;
            if (!GetFileAttributesExA((dirs[i] + "\\DRAKS0005.sl2").c_str(), GetFileExInfoStandard, &a)) continue;
            if (best.empty() || CompareFileTime(&a.ftLastWriteTime, &bestT) > 0) { best = dirs[i]; bestT = a.ftLastWriteTime; }
        }
        return best;
    }

    void readFile() {
        g_fileRead = true;
        for (int s = 0; s < 10; ++s) { g_file[s].used = false; g_file[s].name[0] = 0; g_file[s].cells.clear(); }
        const std::string folder = findSaveFolder();
        if (folder.empty()) { SDLOG(0, "gameMap: no save folder found, the explored map is kept only while the game runs\n"); return; }
        g_filePath = folder + "\\DSfixMap.dat";
        HANDLE f = CreateFileA(g_filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) { SDLOG(0, "gameMap: %s (new)\n", g_filePath.c_str()); return; }
        LARGE_INTEGER size;
        std::vector<BYTE> d;
        DWORD got = 0;
        if (GetFileSizeEx(f, &size) && size.QuadPart > 12 && size.QuadPart < 256 * 1024 * 1024) {
            d.resize((size_t)size.QuadPart);
            if (!ReadFile(f, &d[0], (DWORD)d.size(), &got, NULL) || got != d.size()) d.clear();
        }
        CloseHandle(f);
        size_t p = 12;
        if (d.size() < 12 || memcmp(&d[0], "DSMP", 4) != 0 || *(const DWORD*)&d[4] != 1 || *(const DWORD*)&d[8] != 10) {
            SDLOG(0, "gameMap: %s is not a map file of this DSfix, it will be written new\n", g_filePath.c_str());
            return;
        }
        for (int s = 0; s < 10; ++s) {
            if (p + 4 + 32 + 4 > d.size()) break;
            const DWORD used = *(const DWORD*)&d[p]; p += 4;
            memcpy(g_file[s].name, &d[p], 32); p += 32;
            g_file[s].name[15] = 0;
            const DWORD n = *(const DWORD*)&d[p]; p += 4;
            if (p + (size_t)n * sizeof(CellRec) > d.size()) break;
            g_file[s].used = used != 0;
            if (n) {
                g_file[s].cells.resize(n);
                memcpy(&g_file[s].cells[0], &d[p], (size_t)n * sizeof(CellRec));
            }
            p += (size_t)n * sizeof(CellRec);
        }
        SDLOG(0, "gameMap: %s read\n", g_filePath.c_str());
    }

    // the character in play into its slot of g_file (place = PlaceName id there)
    void storeActive() {
        if (g_activeSlot < 0 || g_activeSlot > 9) return;
        SlotData& sd = g_file[g_activeSlot];
        sd.used = true;
        memcpy(sd.name, g_activeName, sizeof(sd.name));
        sd.cells.clear();
        sd.cells.reserve(g_cellCount);
        for (int p = 0; p < kPlaceCount; ++p)
            for (size_t i = 0; i < g_cells[0][p].size(); ++i) {
                CellRec c = g_cells[0][p][i];
                c.place = kPlaces[p].id;
                sd.cells.push_back(c);
            }
    }

    DWORD WINAPI writerThread(LPVOID param) {
        WriteJob* j = (WriteJob*)param;
        const std::string tmp = j->path + ".tmp";
        HANDLE f = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        bool ok = false;
        if (f != INVALID_HANDLE_VALUE) {
            DWORD put = 0;
            ok = WriteFile(f, &j->data[0], (DWORD)j->data.size(), &put, NULL) && put == j->data.size();
            CloseHandle(f);
            ok = ok && MoveFileExA(tmp.c_str(), j->path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            if (!ok) DeleteFileA(tmp.c_str());
        }
        delete j;
        InterlockedExchange(&g_writing, ok ? 0 : 2);
        return 0;
    }

    // writes the whole file in the background (the game does not wait). False = not started (still writing, or no
    // save folder): the data stays in memory and is written next time.
    bool saveFile() {
        if (g_filePath.empty() || g_activeSlot < 0) return false;
        if (InterlockedCompareExchange(&g_writing, 1, 0) != 0) return false; // still writing: next time
        storeActive();
        WriteJob* j = new WriteJob();
        j->path = g_filePath;
        size_t total = 12;
        for (int s = 0; s < 10; ++s) total += 40 + g_file[s].cells.size() * sizeof(CellRec);
        j->data.resize(total);
        BYTE* p = &j->data[0];
        memcpy(p, "DSMP", 4); *(DWORD*)(p + 4) = 1; *(DWORD*)(p + 8) = 10; p += 12;
        for (int s = 0; s < 10; ++s) {
            *(DWORD*)p = g_file[s].used ? 1 : 0; p += 4;
            memcpy(p, g_file[s].name, 32); p += 32;
            const DWORD n = (DWORD)g_file[s].cells.size();
            *(DWORD*)p = n; p += 4;
            if (n) { memcpy(p, &g_file[s].cells[0], n * sizeof(CellRec)); p += n * sizeof(CellRec); }
        }
        HANDLE t = CreateThread(NULL, 0, writerThread, j, 0, NULL);
        if (!t) { delete j; InterlockedExchange(&g_writing, 0); return false; }
        CloseHandle(t);
        g_dirty = false;
        return true;
    }

    // the character in play changed (or the first one): its explored ground from the file
    void activate(int slot, const wchar_t* name) {
        if (!g_fileRead) readFile();
        bool unsaved = false;
        if (g_activeSlot >= 0) {
            storeActive();                       // the previous character stays in g_file even if it cannot be written now
            unsaved = g_dirty && !saveFile();
        }
        clearCells();
        g_activeSlot = slot;
        memcpy(g_activeName, name, sizeof(g_activeName));
        g_activeName[15] = 0;
        const SlotData& sd = g_file[slot];
        bool same = sd.used && wcsncmp(sd.name, g_activeName, 16) == 0;
        if (same) {
            for (size_t i = 0; i < sd.cells.size(); ++i) {
                const CellRec& c = sd.cells[i];
                const int p = placeIndex(c.place);
                addCell(c.x, c.z, c.y, p);
            }
        }
        g_dirty = !same || unsaved;              // a new character in this slot: its (empty) map replaces the old one
        char nm[64] = { 0 };
        WideCharToMultiByte(CP_UTF8, 0, g_activeName, -1, nm, sizeof(nm) - 1, NULL, NULL);
        SDLOG(0, "gameMap: slot %d \"%s\": %u explored cells%s\n", slot, nm, g_cellCount, same ? "" : " (new map)");
    }

// =====================================================================================================================
// State
// =====================================================================================================================
    bool g_on = false;
    bool g_inputHooks = false;
    bool g_hadPlayer = false;
    DWORD g_enterTick = 0;                      // when the hero appeared (after a loading screen)
    unsigned long long g_lastKey = 0;
    bool g_haveLastKey = false;
    float g_hero[4] = { 0, 0, 0, 0 };
    int g_heroPlace = -1;
    bool g_shown = false;                       // the gesture window is open
    float g_alpha = 0.0f;                       // 0..1 fade of the map (and 1 - it: the bottom-left HUD)
    ULONGLONG g_lastMs = 0;
    int g_page = 0;                             // 0 area page, 1 world page
    int g_pagePlace = -1;
    float g_zoom[2] = { Look::kAreaZoom, Look::kWorldZoom };
    float g_offX = 0.0f, g_offZ = 0.0f;         // moved away from the page's centre (metres)
    bool g_lit[64];
    int g_bonfirePlace[64];                     // the area page of each bonfire (the table)
    int g_bonfirePlace2[64];                    // and the area of the explored ground around it, if another one
    int g_bonfireGround[64];                    // the game's banner area recorded right at the bonfire (its name), -1 = none
    // soul remains
    bool g_stainHas = false;
    float g_stain[3] = { 0, 0, 0 };
    DWORD g_stainBlock = 0;
    int g_stainPlace = -1;
    DWORD g_stainCheckTick = 0;

    void readStain(bool live) {
        DWORD gdm = 0, rec = 0, has = 0, mapId = 0, hum = 0, souls = 0;
        float p[3];
        if (!rdD(kGameDataMan, gdm) || !gdm || !rdD(gdm + 0x18, has) || !rdD(gdm + 0x1C, rec) || !rec ||
            !rdBlock(rec, p, sizeof(p)) || !rdD(rec + 0x34, hum) || !rdD(rec + 0x38, souls) || !rdD(rec + 0x3C, mapId)) { g_stainHas = false; return; }
        const bool hasNow = (has & 0xFF) != 0 && ((int)souls > 0 || (int)hum > 0);
        const bool moved = hasNow && (!g_stainHas || p[0] != g_stain[0] || p[1] != g_stain[1] || p[2] != g_stain[2]);
        g_stainHas = hasNow;
        if (!moved) return;
        g_stain[0] = p[0]; g_stain[1] = p[1]; g_stain[2] = p[2];
        g_stainBlock = (mapId >> 16) & 0xFFFF;
        // its area: where the hero is when it appears during play (he died there), else the explored ground there,
        // else the first area of its map block
        int pl = live ? g_heroPlace : -1;
        if (pl < 0) pl = placeAt(p[0], p[1], p[2]);
        if (pl < 0) pl = blockDefaultPlace(g_stainBlock);
        g_stainPlace = pl;
    }

    void readLitBonfires() {
        memset(g_lit, 0, sizeof(g_lit));
        DWORD root = 0, db = 0, head = 0, node = 0;
        if (!rdD(kBonfireRoot, root) || !root || !rdD(root + 0xB48, db) || !db || !rdD(db + 0x24, head) || !head) return;
        if (!rdD(head, node)) return;
        for (int n = 0; node && node != head && n < 256; ++n) {
            DWORD item = 0, e = 0, k = 0;
            if (!rdD(node + 8, item)) return;
            if (item && rdD(item + 4, e) && rdD(item + 8, k) && (k & 0xFFFF) != 0)
                for (int i = 0; i < kBonfireCount; ++i) if (kBonfires[i].entity == e) g_lit[i] = true;
            if (!rdD(node, node)) return;
        }
    }

    // the areas with something to show, in page order
    int visiblePlaces(int* out) {
        int n = 0;
        for (int p = 0; p < kPlaceCount; ++p) {
            bool any = g_stat[p].n > 0 || p == g_heroPlace;
            for (int b = 0; b < kBonfireCount && !any; ++b) if (g_lit[b] && (g_bonfirePlace[b] == p || g_bonfirePlace2[b] == p)) any = true;
            if (any) out[n++] = p;
        }
        return n;
    }

    // ---- input (filled by the shared hooks in GameParams.cpp) ----
    struct PadState { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
    struct PhysPad { bool r3, lb, rb; BYTE lt, rt; SHORT rx, ry; };
    PhysPad g_pad[4];
    DWORD g_padTick[4] = { 0, 0, 0, 0 };
    volatile bool g_inputActive = false;        // the map shows: the game does not get the map's buttons
    volatile LONG g_wheelAcc = 0;
    volatile LONG g_rc[4] = { 0, 0, 0, 0 };     // the map in client pixels (for the wheel, window thread)
    HWND g_hwnd = NULL;
    DWORD g_lastPadUse = 0, g_lastKbmUse = 0;

    // DDTMControllerFix's controller mapping: physical button (4 LB, 5 RB, 6 LT, 7 RT) -> the game button carrying it
    // (0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 LT, 7 RT), -1 = it reaches the game as none of them
    int physToGame(int phys) {
        kbIcons::Shared* s = kbIcons::shared();
        if (!s || s->version < 2) return phys;
        for (int g = 0; g < 8; ++g) if (s->pad[g] == phys) return g;
        return s->pad[phys] < 0 ? phys : -1;
    }
    const WORD kGameBit[6] = { 0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200 };
    BYTE gameValue(const PadState* s, int g) {
        if (g < 0) return 0;
        if (g < 6) return (s->buttons & kGameBit[g]) ? 255 : 0;
        return g == 6 ? s->lt : s->rt;
    }
    void gameClear(PadState* s, int g) {
        if (g < 0) return;
        if (g < 6) s->buttons &= (WORD)~kGameBit[g];
        else if (g == 6) s->lt = 0;
        else s->rt = 0;
    }

    // game thread, every pad read (XInput hook 5)
    void __stdcall onPad(unsigned long idx, void* state) {
        if (idx >= 4 || !state) return;
        PadState* s = (PadState*)state;
        const int gLB = physToGame(4), gRB = physToGame(5), gLT = physToGame(6), gRT = physToGame(7);
        PhysPad& o = g_pad[idx];
        const PhysPad old = o;
        o.r3 = (s->buttons & 0x0080) != 0;
        o.lb = gameValue(s, gLB) > 30; o.rb = gameValue(s, gRB) > 30;
        o.lt = gameValue(s, gLT); o.rt = gameValue(s, gRT);
        o.rx = s->rx; o.ry = s->ry;
        g_padTick[idx] = GetTickCount();
        if (o.r3 != old.r3 || o.lb != old.lb || o.rb != old.rb || o.lt > 30 || o.rt > 30 || abs(o.rx) > 9000 || abs(o.ry) > 9000 ||
            s->buttons != 0 || abs(s->lx) > 9000 || abs(s->ly) > 9000) g_lastPadUse = GetTickCount();
        if (g_inputActive) {
            s->rx = 0; s->ry = 0;
            s->buttons &= (WORD)~0x0080;
            gameClear(s, gLB); gameClear(s, gRB); gameClear(s, gLT); gameClear(s, gRT);
        }
    }

    // window thread, every WM_MOUSEWHEEL: kept for the map while the mouse is over it
    bool __stdcall onWheel(int delta) {
        if (!g_inputActive || !g_hwnd) return false;
        POINT pt;
        if (!GetCursorPos(&pt) || !ScreenToClient(g_hwnd, &pt)) return false;
        if (pt.x < g_rc[0] || pt.y < g_rc[1] || pt.x >= g_rc[2] || pt.y >= g_rc[3]) return false;
        InterlockedExchangeAdd(&g_wheelAcc, delta);
        g_lastKbmUse = GetTickCount();
        return true;
    }

// =====================================================================================================================
// Drawing
// =====================================================================================================================
    IDirect3DTexture9* g_rt = NULL;             // the explored ground: the field (the spots added up), window size
    IDirect3DTexture9* g_rt2 = NULL;            // the ground's shape, cut from the field at twice the size
    int g_rtW = 0, g_rtH = 0;
    IDirect3DTexture9* g_blob = NULL;           // soft round spot (icons)
    IDirect3DTexture9* g_bump = NULL;           // the ground spot: alpha = smooth bump, colour white inside the circle
    IDirect3DTexture9* g_bonfireTex = NULL;
    enum { SHAPE_HERO, SHAPE_UP, SHAPE_STAIN_ARROW, SHAPE_COUNT };
    IDirect3DTexture9* g_shape[SHAPE_COUNT] = { NULL, NULL, NULL };
    bool g_bonfireTried = false;
    IDirect3DStateBlock9* g_sb = NULL;
    ID3DXFont* g_font[4] = { NULL, NULL, NULL, NULL };  // title, labels, help, bonfire names
    int g_fontForH = 0;
    struct TV { float x, y, z, rhw; D3DCOLOR c; float u, v; };
    std::vector<TV> g_tv;                       // reused every frame (grows once)

    void release() {
        SAFERELEASE(g_rt);
        SAFERELEASE(g_rt2);
        g_rtW = g_rtH = 0;
        SAFERELEASE(g_blob);
        SAFERELEASE(g_bump);
        SAFERELEASE(g_bonfireTex);
        g_bonfireTried = false;
        for (int i = 0; i < SHAPE_COUNT; ++i) SAFERELEASE(g_shape[i]);
        SAFERELEASE(g_sb);
        for (int i = 0; i < 4; ++i) SAFERELEASE(g_font[i]);
        g_fontForH = 0;
    }

    D3DCOLOR pm(D3DCOLOR c, float f) {            // premultiplied, alpha times f
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        const float a = (float)(c >> 24) / 255.0f * f;
        const DWORD A = (DWORD)(a * 255.0f + 0.5f);
        const DWORD R = (DWORD)(((c >> 16) & 255) * a + 0.5f), G = (DWORD)(((c >> 8) & 255) * a + 0.5f), B = (DWORD)((c & 255) * a + 0.5f);
        return (A << 24) | (R << 16) | (G << 8) | B;
    }
    D3DCOLOR fadeText(D3DCOLOR c, float f) {      // straight alpha (fonts), alpha times f
        const DWORD a = (DWORD)((float)(c >> 24) * (f < 0 ? 0 : (f > 1 ? 1 : f)) + 0.5f);
        return (c & 0x00FFFFFF) | (a << 24);
    }

    bool makeBlob(IDirect3DDevice9* dev) {
        if (g_blob) return true;
        const int S = 64;
        if (FAILED(dev->CreateTexture(S, S, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_blob, NULL)) || !g_blob) { g_blob = NULL; return false; }
        D3DLOCKED_RECT lr;
        if (FAILED(g_blob->LockRect(0, &lr, NULL, 0))) { SAFERELEASE(g_blob); return false; }
        for (int y = 0; y < S; ++y) {
            UINT32* row = (UINT32*)((BYTE*)lr.pBits + (size_t)y * lr.Pitch);
            for (int x = 0; x < S; ++x) {
                const float dx = (x + 0.5f) / S * 2.0f - 1.0f, dy = (y + 0.5f) / S * 2.0f - 1.0f;
                float t = (1.0f - sqrtf(dx * dx + dy * dy)) / 0.50f;   // flat middle, soft edge
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
                t = t * t * (3.0f - 2.0f * t);
                const UINT32 a = (UINT32)(t * 255.0f + 0.5f);
                row[x] = (a << 24) | (a << 16) | (a << 8) | a;            // premultiplied white
            }
        }
        g_blob->UnlockRect(0);
        return true;
    }

    // A filled polygon with an outline, pointing up (toward v = 0), smooth: every pixel is the mean of 4 x 4 samples.
    // Premultiplied alpha. pts: x y pairs in 0..1.
    bool makeShape(IDirect3DDevice9* dev, int which, const float* pts, int n, D3DCOLOR fill, D3DCOLOR line, float lineW) {
        if (g_shape[which]) return true;
        const int S = 64;
        IDirect3DTexture9* t = NULL;
        if (FAILED(dev->CreateTexture(S, S, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, NULL)) || !t) return false;
        D3DLOCKED_RECT lr;
        if (FAILED(t->LockRect(0, &lr, NULL, 0))) { t->Release(); return false; }
        const float fr = ((fill >> 16) & 255) / 255.0f, fg = ((fill >> 8) & 255) / 255.0f, fb = (fill & 255) / 255.0f;
        const float lr_ = ((line >> 16) & 255) / 255.0f, lg = ((line >> 8) & 255) / 255.0f, lb = (line & 255) / 255.0f;
        for (int y = 0; y < S; ++y) {
            UINT32* row = (UINT32*)((BYTE*)lr.pBits + (size_t)y * lr.Pitch);
            for (int x = 0; x < S; ++x) {
                float cf = 0.0f, cl = 0.0f;
                for (int sy = 0; sy < 4; ++sy)
                    for (int sx = 0; sx < 4; ++sx) {
                        const float px = (x + (sx + 0.5f) / 4.0f) / S, py = (y + (sy + 0.5f) / 4.0f) / S;
                        bool in = false;
                        float d2 = 1e9f;
                        for (int i = 0, j = n - 1; i < n; j = i++) {
                            const float xi = pts[2 * i], yi = pts[2 * i + 1], xj = pts[2 * j], yj = pts[2 * j + 1];
                            if (((yi > py) != (yj > py)) && (px < (xj - xi) * (py - yi) / (yj - yi) + xi)) in = !in;
                            const float ex = xj - xi, ey = yj - yi, l2 = ex * ex + ey * ey;
                            float k = l2 > 0 ? ((px - xi) * ex + (py - yi) * ey) / l2 : 0.0f;
                            k = k < 0 ? 0 : (k > 1 ? 1 : k);
                            const float qx = xi + ex * k - px, qy = yi + ey * k - py, q = qx * qx + qy * qy;
                            if (q < d2) d2 = q;
                        }
                        const float d = sqrtf(d2);
                        if (d <= lineW) cl += 1.0f;          // the outline: a band along the edge, inside and outside
                        else if (in) cf += 1.0f;
                    }
                cf /= 16.0f; cl /= 16.0f;
                const float a = cf + cl;
                const UINT32 A = (UINT32)(a * 255.0f + 0.5f);
                const UINT32 R = (UINT32)((fr * cf + lr_ * cl) * 255.0f + 0.5f), G = (UINT32)((fg * cf + lg * cl) * 255.0f + 0.5f),
                    B = (UINT32)((fb * cf + lb * cl) * 255.0f + 0.5f);
                row[x] = (A << 24) | (R << 16) | (G << 8) | B;
            }
        }
        t->UnlockRect(0);
        g_shape[which] = t;
        return true;
    }
    bool makeShapes(IDirect3DDevice9* dev) {
        static const float arrow[8] = { 0.50f, 0.08f, 0.86f, 0.90f, 0.50f, 0.70f, 0.14f, 0.90f };   // tip, right, notch, left
        static const float up[6] = { 0.50f, 0.14f, 0.88f, 0.82f, 0.12f, 0.82f };
        return makeShape(dev, SHAPE_HERO, arrow, 4, Look::kHero, Look::kHeroOutline, 0.045f) &&
            makeShape(dev, SHAPE_UP, up, 3, Look::kUpMark, Look::kHeroOutline, 0.06f) &&
            makeShape(dev, SHAPE_STAIN_ARROW, arrow, 4, Look::kStain, Look::kHeroOutline, 0.05f);
    }
    // a textured square turned so that the texture's top points along (dx, dy) (screen, unit length)
    void quadDir(float cx, float cy, float half, float dx, float dy, D3DCOLOR c) {
        const float rx = -dy, ry = dx;                     // the texture's right
        const float ux[4] = { 0, 1, 0, 1 }, vy[4] = { 0, 0, 1, 1 };
        TV p[4];
        for (int i = 0; i < 4; ++i) {
            const float a = (ux[i] - 0.5f) * 2.0f * half, b = (vy[i] - 0.5f) * 2.0f * half;   // b > 0: backwards
            const TV t = { cx + rx * a - dx * b - 0.5f, cy + ry * a - dy * b - 0.5f, 0, 1, c, ux[i], vy[i] };
            p[i] = t;
        }
        g_tv.push_back(p[0]); g_tv.push_back(p[1]); g_tv.push_back(p[2]);
        g_tv.push_back(p[1]); g_tv.push_back(p[3]); g_tv.push_back(p[2]);
    }

    bool makeBump(IDirect3DDevice9* dev) {
        if (g_bump) return true;
        const int S = 64;
        if (FAILED(dev->CreateTexture(S, S, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_bump, NULL)) || !g_bump) { g_bump = NULL; return false; }
        D3DLOCKED_RECT lr;
        if (FAILED(g_bump->LockRect(0, &lr, NULL, 0))) { SAFERELEASE(g_bump); return false; }
        for (int y = 0; y < S; ++y) {
            UINT32* row = (UINT32*)((BYTE*)lr.pBits + (size_t)y * lr.Pitch);
            for (int x = 0; x < S; ++x) {
                const float dx = (x + 0.5f) / S * 2.0f - 1.0f, dy = (y + 0.5f) / S * 2.0f - 1.0f;
                float t = 1.0f - sqrtf(dx * dx + dy * dy);
                if (t < 0.0f) t = 0.0f;
                t = t * t * (3.0f - 2.0f * t);
                const UINT32 a = (UINT32)(t * 255.0f + 0.5f);
                row[x] = (a << 24) | (a ? 0x00FFFFFF : 0);
            }
        }
        g_bump->UnlockRect(0);
        return true;
    }

    bool loadBonfire(IDirect3DDevice9* dev) {
        if (g_bonfireTried) return g_bonfireTex != NULL;
        g_bonfireTried = true;
        HRSRC r = FindResource(g_hModule, MAKEINTRESOURCE(IDR_TEX_BONFIRE), RT_RCDATA);
        HGLOBAL hg = r ? LoadResource(g_hModule, r) : NULL;
        const void* data = hg ? LockResource(hg) : NULL;
        const DWORD size = r ? SizeofResource(g_hModule, r) : 0;
        if (!data || !size) { SDLOG(0, "gameMap: Bonfire.png is missing from the DLL, bonfires are drawn as spots\n"); return false; }
        if (FAILED(TrueD3DXCreateTextureFromFileInMemoryEx(dev, data, size, D3DX_DEFAULT_NONPOW2, D3DX_DEFAULT_NONPOW2, 1, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, D3DX_FILTER_NONE, D3DX_FILTER_NONE, 0, NULL, NULL, &g_bonfireTex)) || !g_bonfireTex) {
            g_bonfireTex = NULL;
            SDLOG(0, "gameMap: Bonfire.png could not be read, bonfires are drawn as spots\n");
            return false;
        }
        D3DSURFACE_DESC d;
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(g_bonfireTex->GetLevelDesc(0, &d)) && SUCCEEDED(g_bonfireTex->LockRect(0, &lr, NULL, 0))) {
            for (UINT y = 0; y < d.Height; ++y) {
                UINT32* row = (UINT32*)((BYTE*)lr.pBits + (size_t)y * lr.Pitch);
                for (UINT x = 0; x < d.Width; ++x) {
                    const UINT32 p = row[x], a = p >> 24;
                    row[x] = (a << 24) | ((((p >> 16) & 255) * a + 127) / 255 << 16) | ((((p >> 8) & 255) * a + 127) / 255 << 8) | (((p & 255) * a + 127) / 255);
                }
            }
            g_bonfireTex->UnlockRect(0);
        }
        return true;
    }

    bool makeFonts(IDirect3DDevice9* dev, int bbH) {
        if (g_font[0] && g_fontForH == bbH) return true;
        for (int i = 0; i < 4; ++i) SAFERELEASE(g_font[i]);
        const float sizes[4] = { Look::kTitleSize, Look::kLabelSize, Look::kHelpSize, Look::kBonfireNameSize };
        const int weights[4] = { FW_SEMIBOLD, FW_NORMAL, FW_NORMAL, FW_NORMAL };
        for (int i = 0; i < 4; ++i) {
            int h = (int)(sizes[i] * bbH / 720.0f + 0.5f);
            if (h < 9) h = 9;
            if (FAILED(D3DXCreateFontW(dev, h, 0, weights[i], 1, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, ANTIALIASED_QUALITY,
                DEFAULT_PITCH | FF_ROMAN, Look::kFont, &g_font[i]))) { g_font[i] = NULL; return false; }
        }
        g_fontForH = bbH;
        return true;
    }

    void setDrawStates(IDirect3DDevice9* dev, IDirect3DBaseTexture9* tex, bool maxBlend) {
        dev->SetVertexShader(NULL);
        dev->SetPixelShader(NULL);
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_BLENDOP, maxBlend ? D3DBLENDOP_MAX : D3DBLENDOP_ADD);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        dev->SetRenderState(D3DRS_DESTBLEND, maxBlend ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        dev->SetTexture(0, tex);
        if (tex) {
            dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
            dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        }
        else {
            dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        }
        dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    }

    void quad(float x0, float y0, float x1, float y1, D3DCOLOR c, float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1) {
        const TV a = { x0 - 0.5f, y0 - 0.5f, 0, 1, c, u0, v0 }, b = { x1 - 0.5f, y0 - 0.5f, 0, 1, c, u1, v0 };
        const TV d = { x0 - 0.5f, y1 - 0.5f, 0, 1, c, u0, v1 }, e = { x1 - 0.5f, y1 - 0.5f, 0, 1, c, u1, v1 };
        g_tv.push_back(a); g_tv.push_back(b); g_tv.push_back(d);
        g_tv.push_back(b); g_tv.push_back(e); g_tv.push_back(d);
    }
    void flush(IDirect3DDevice9* dev) {
        size_t done = 0;
        while (done < g_tv.size()) {
            const size_t n = (g_tv.size() - done) > 60000 ? 60000 : (g_tv.size() - done);
            dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(n / 3), &g_tv[done], sizeof(TV));
            done += n;
        }
        g_tv.clear();
    }

    // the view of this frame
    struct View {
        float l, t, r, b;          // the window in back buffer pixels
        float cx, cz;              // world point at the window's centre
        float mppBb;               // metres per back buffer pixel
        float s;                   // back buffer pixels per UI pixel
        float refY;                // height drawn brightest
    };
    View g_view;
    // world direction (x, z) <-> screen direction (right, down), with the map's turn and mirror
    const float kRotC = cosf(Look::kRotationDeg * 3.14159265f / 180.0f), kRotS = sinf(Look::kRotationDeg * 3.14159265f / 180.0f);
    inline void worldDirToScreen(float wx, float wz, float& sx, float& sy) {
        const float bx = wx * (Look::kMirrorX ? -1.0f : 1.0f), by = -wz;
        sx = bx * kRotC + by * kRotS;
        sy = -bx * kRotS + by * kRotC;
    }
    inline void screenDirToWorld(float sx, float sy, float& wx, float& wz) {
        const float bx = sx * kRotC - sy * kRotS, by = sx * kRotS + sy * kRotC;
        wx = bx * (Look::kMirrorX ? -1.0f : 1.0f);
        wz = -by;
    }
    void toScreen(float x, float z, float& px, float& py) {
        const View& v = g_view;
        float sx, sy;
        worldDirToScreen(x - v.cx, z - v.cz, sx, sy);
        px = (v.l + v.r) * 0.5f + sx / v.mppBb;
        py = (v.t + v.b) * 0.5f + sy / v.mppBb;
    }
    float brightness(float dy) {
        const float a = fabsf(dy) - Look::kLevelBand;
        if (a <= 0.0f) return 1.0f;
        const float minB = dy < 0 ? Look::kBelowMin : Look::kAboveMin;
        float b = 1.0f - a / Look::kFadeHeight * (1.0f - minB);
        return b < minB ? minB : b;
    }
    bool onPage(int place) {
        if (place < 0) return false;
        return g_page == 1 ? kPlaces[place].world : place == g_pagePlace;
    }
    bool bonfireOnPage(int b) { return onPage(g_bonfirePlace[b]) || (g_page == 0 && onPage(g_bonfirePlace2[b])); }

    // the explored ground into g_rt: every cell adds a soft round spot (alpha adds up, colour keeps the brightest level)
    void drawGround(IDirect3DDevice9* dev) {
        const View& v = g_view;
        const float mppUi = v.mppBb * v.s;
        int lod = kLods - 1;
        for (int l = 0; l < kLods; ++l) if (kCellSize[l] / mppUi >= 2.0f) { lod = l; break; }
        const float cell = kCellSize[lod], hcell = kCellHeight[lod];
        float spot = cell / v.mppBb * Look::kGroundBlob;
        if (spot < 3.0f) spot = 3.0f;
        const float half = spot * 0.5f;
        const float w = v.r - v.l, h = v.b - v.t;
        for (int p = 0; p < kPlaceCount; ++p) {
            if (!onPage(p)) continue;
            const std::vector<CellRec>& cells = g_cells[lod][p];
            for (size_t i = 0; i < cells.size(); ++i) {
                const CellRec& c = cells[i];
                float px, py;
                toScreen((c.x + 0.5f) * cell, (c.z + 0.5f) * cell, px, py);
                px -= v.l; py -= v.t;
                if (px < -half || py < -half || px > w + half || py > h + half) continue;
                const float b = brightness((c.y + 0.5f) * hcell - v.refY);
                // colour: the brightest level wins (MAX); alpha: the spots add up (a quarter each, cut at a quarter
                // in the next step = where the spots reach half of a full one)
                const D3DCOLOR col = D3DCOLOR_ARGB(64, (DWORD)(Look::kGroundR * b * 255.0f),
                    (DWORD)(Look::kGroundG * b * 255.0f), (DWORD)(Look::kGroundB * b * 255.0f));
                quad(px - half, py - half, px + half, py + half, col);
            }
            if (g_tv.size() > 60000) flush(dev);
        }
        flush(dev);
    }

    void text(int font, const wchar_t* s, float x, float y, D3DCOLOR c, DWORD fmt, float fade) {
        if (!g_font[font] || !s) return;
        RECT r = { (LONG)x, (LONG)y, (LONG)x + 2000, (LONG)y + 400 };
        if (fmt & DT_CENTER) { r.left = (LONG)x - 1000; r.right = (LONG)x + 1000; }
        RECT rs = r;
        OffsetRect(&rs, 1, 1);
        g_font[font]->DrawTextW(NULL, s, -1, &rs, fmt | DT_NOCLIP | DT_SINGLELINE, fadeText(Look::kShadow, fade));
        g_font[font]->DrawTextW(NULL, s, -1, &r, fmt | DT_NOCLIP | DT_SINGLELINE, fadeText(c, fade));
    }

    // labels must not cover each other: every placed label / icon is an obstacle for the next ones
    struct LRect { float l, t, r, b; };
    LRect g_obst[256];
    int g_obstN = 0;
    void addObst(const LRect& r) { if (g_obstN < 256) g_obst[g_obstN++] = r; }
    bool freeRect(const LRect& r) {
        for (int i = 0; i < g_obstN; ++i) {
            const LRect& o = g_obst[i];
            if (r.l < o.r && r.r > o.l && r.t < o.b && r.b > o.t) return false;
        }
        return true;
    }
    // where an area's name goes: its explored cell nearest to the middle of its explored ground (kept until it grows)
    void placeAnchor(int p, float& x, float& z) {
        Anchor& a = g_anchor[p];
        const PlaceStat& st = g_stat[p];
        const DWORD now = GetTickCount();
        if (a.n != st.n && (a.n == 0 || now - a.tick > 1000) && st.n > 0) {
            const float mx = (float)(st.sx / st.n), mz = (float)(st.sz / st.n);
            float best = 1e30f, bxw = mx, bzw = mz;
            const std::vector<CellRec>& cells = g_cells[0][p];
            for (size_t i = 0; i < cells.size(); ++i) {
                const float cx = (cells[i].x + 0.5f) * kCellSize[0], cz = (cells[i].z + 0.5f) * kCellSize[0];
                const float d = (cx - mx) * (cx - mx) + (cz - mz) * (cz - mz);
                if (d < best) { best = d; bxw = cx; bzw = cz; }
            }
            a.n = st.n; a.tick = now; a.x = bxw; a.z = bzw;
        }
        x = a.x; z = a.z;
    }

    // the legend line: what the pictures mean
    float textWidth(int font, const wchar_t* t) {
        if (!g_font[font] || !t) return 0.0f;
        RECT r = { 0, 0, 0, 0 };
        g_font[font]->DrawTextW(NULL, t, -1, &r, DT_CALCRECT | DT_SINGLELINE | DT_LEFT | DT_TOP, 0);
        return (float)(r.right - r.left);
    }
    void drawLegend(IDirect3DDevice9* dev, float x, float y, float s, float fade) {
        const float ic = Look::kLegendIcon * s, gap = Look::kLegendGap * s, tg = 5.0f * s;
        const float th = Look::kHelpSize * s * 1.25f, ty = y + (ic - th) * 0.5f;
        const wchar_t* names[5] = { L"Lit bonfire", L"Lower than you", L"Higher than you", L"Soul remains", L"You" };
        float xs[5];
        float cx = x;
        for (int i = 0; i < 5; ++i) { xs[i] = cx; cx += ic + tg + textWidth(2, names[i]) + gap; }
        const DWORD f8 = (DWORD)(fade * 255.0f + 0.5f);
        // 0 bonfire, 1 lower bonfire (smaller, darker)
        setDrawStates(dev, g_bonfireTex ? (IDirect3DBaseTexture9*)g_bonfireTex : (IDirect3DBaseTexture9*)g_blob, false);
        quad(xs[0], y, xs[0] + ic, y + ic, g_bonfireTex ? D3DCOLOR_ARGB(f8, f8, f8, f8) : pm(D3DCOLOR_ARGB(255, 255, 150, 50), fade));
        const float lo = ic * Look::kBonfireBelowScale, lm = (ic - lo) * 0.5f;
        const DWORD d8 = (DWORD)(fade * Look::kBonfireBelowDark * 255.0f + 0.5f);
        quad(xs[1] + lm, y + lm, xs[1] + lm + lo, y + lm + lo,
            g_bonfireTex ? D3DCOLOR_ARGB(f8, d8, d8, d8) : pm(D3DCOLOR_ARGB(255, 255, 150, 50), fade * Look::kBonfireBelowDark));
        flush(dev);
        // 3 soul remains
        setDrawStates(dev, g_blob, false);
        quad(xs[3] - ic * 0.3f, y - ic * 0.3f, xs[3] + ic * 1.3f, y + ic * 1.3f, pm(Look::kStainGlow, fade));
        quad(xs[3] + ic * 0.2f, y + ic * 0.2f, xs[3] + ic * 0.8f, y + ic * 0.8f, pm(Look::kStain, fade));
        flush(dev);
        // 2 up mark, 4 hero arrow (pointing up)
        setDrawStates(dev, g_shape[SHAPE_UP], false);
        quad(xs[2] + ic * 0.1f, y + ic * 0.1f, xs[2] + ic * 0.9f, y + ic * 0.9f, D3DCOLOR_ARGB(f8, f8, f8, f8));
        flush(dev);
        setDrawStates(dev, g_shape[SHAPE_HERO], false);
        quad(xs[4], y, xs[4] + ic, y + ic, D3DCOLOR_ARGB(f8, f8, f8, f8));
        flush(dev);
        for (int i = 0; i < 5; ++i) text(2, names[i], xs[i] + ic + tg, ty, Look::kHelp, DT_LEFT | DT_TOP, fade);
    }

    void draw(IDirect3DDevice9* dev, bool padHelp) {
        IDirect3DSurface9* bb = NULL;
        if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
        D3DSURFACE_DESC bd;
        bb->GetDesc(&bd);
        const float s = bd.Height / 720.0f;
        View& v = g_view;
        v.l = floorf(Look::kLeft * bd.Width / 1280.0f + 0.5f); v.r = floorf(Look::kRight * bd.Width / 1280.0f + 0.5f);
        v.t = floorf(Look::kTop * s + 0.5f); v.b = floorf(Look::kBottom * s + 0.5f);
        v.s = s;
        const int W = (int)(v.r - v.l), H = (int)(v.b - v.t);
        if (W < 16 || H < 16) { bb->Release(); return; }
        if (!makeBlob(dev) || !makeBump(dev) || !makeShapes(dev) || !makeFonts(dev, bd.Height)) { bb->Release(); return; }
        loadBonfire(dev);
        if (g_rt && (g_rtW != W || g_rtH != H)) { SAFERELEASE(g_rt); SAFERELEASE(g_rt2); }
        if (!g_rt || !g_rt2) {
            SAFERELEASE(g_rt); SAFERELEASE(g_rt2);
            if (FAILED(dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_rt, NULL)) || !g_rt ||
                FAILED(dev->CreateTexture(W * 2, H * 2, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_rt2, NULL)) || !g_rt2) {
                SAFERELEASE(g_rt); SAFERELEASE(g_rt2); bb->Release(); return;
            }
            g_rtW = W; g_rtH = H;
        }
        if (!g_sb && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_sb))) g_sb = NULL;
        if (!g_sb) { bb->Release(); return; }

        // the window in client pixels, for the wheel (window thread)
        if (g_hwnd) {
            RECT rc;
            if (GetClientRect(g_hwnd, &rc) && rc.right > 0 && rc.bottom > 0) {
                InterlockedExchange(&g_rc[0], (LONG)(v.l * rc.right / bd.Width)); InterlockedExchange(&g_rc[1], (LONG)(v.t * rc.bottom / bd.Height));
                InterlockedExchange(&g_rc[2], (LONG)(v.r * rc.right / bd.Width)); InterlockedExchange(&g_rc[3], (LONG)(v.b * rc.bottom / bd.Height));
            }
        }

        // what the page is centred on
        const bool heroHere = onPage(g_heroPlace);
        float refX = -51.8f, refZ = 55.7f;                       // Firelink Shrine
        v.refY = g_hero[1];
        if (heroHere) { refX = g_hero[0]; refZ = g_hero[2]; }
        else if (g_page == 0 && g_pagePlace >= 0) {
            const PlaceStat& st = g_stat[g_pagePlace];
            if (st.n > 0) { refX = (float)(st.sx / st.n); refZ = (float)(st.sz / st.n); v.refY = (float)(st.sy / st.n); }
            else {
                int n = 0; float sx = 0, sy = 0, sz = 0;
                for (int b = 0; b < kBonfireCount; ++b)
                    if (g_lit[b] && g_bonfirePlace[b] == g_pagePlace) { sx += kBonfires[b].x; sy += kBonfires[b].y; sz += kBonfires[b].z; ++n; }
                if (n) { refX = sx / n; refZ = sz / n; v.refY = sy / n; }
            }
        }
        v.cx = refX + g_offX; v.cz = refZ + g_offZ;
        v.mppBb = g_zoom[g_page] / s;
        const float fade = g_alpha;

        g_sb->Capture();
        IDirect3DSurface9* oldRT = NULL;
        IDirect3DSurface9* oldDS = NULL;
        dev->GetRenderTarget(0, &oldRT);
        dev->GetDepthStencilSurface(&oldDS);
        dev->SetDepthStencilSurface(NULL);
        if (SUCCEEDED(dev->BeginScene())) {
            // 1. the explored ground of the visible part: the field, into g_rt
            IDirect3DSurface9* rts = NULL;
            if (SUCCEEDED(g_rt->GetSurfaceLevel(0, &rts)) && rts) {
                dev->SetRenderTarget(0, rts);
                D3DVIEWPORT9 vpr = { 0, 0, (DWORD)W, (DWORD)H, 0.0f, 1.0f };
                dev->SetViewport(&vpr);
                dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
                dev->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
                setDrawStates(dev, g_bump, true);
                dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
                dev->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
                dev->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
                dev->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_ONE);
                drawGround(dev);
                rts->Release();
            }
            // 2. the shape: the field at twice the size, cut where it reaches a quarter (alpha test). Stretched back to
            //    the window's size later, every edge pixel is the mean of 2 x 2: smooth edges.
            rts = NULL;
            if (SUCCEEDED(g_rt2->GetSurfaceLevel(0, &rts)) && rts) {
                dev->SetRenderTarget(0, rts);
                D3DVIEWPORT9 vp2 = { 0, 0, (DWORD)W * 2, (DWORD)H * 2, 0.0f, 1.0f };
                dev->SetViewport(&vp2);
                dev->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
                setDrawStates(dev, g_rt, false);
                dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
                dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
                dev->SetRenderState(D3DRS_ALPHAREF, 255);
                dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
                const DWORD ga = (DWORD)(Look::kGroundAlpha * 255.0f + 0.5f);
                dev->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(255, ga, ga, ga));
                dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);       // colour x ground alpha (premultiplied)
                dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
                dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE4X);     // field x 4: full (1.0) from a quarter
                dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
                dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
                quad(0.0f, 0.0f, (float)W * 2, (float)H * 2, 0xFFFFFFFF);
                flush(dev);
                dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
                rts->Release();
            }
            // 2. on the back buffer
            dev->SetRenderTarget(0, bb);
            D3DVIEWPORT9 vp = { 0, 0, bd.Width, bd.Height, 0.0f, 1.0f };
            dev->SetViewport(&vp);
            RECT sc = { (LONG)v.l, (LONG)v.t, (LONG)v.r, (LONG)v.b };
            dev->SetScissorRect(&sc);
            dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
            setDrawStates(dev, NULL, false);
            quad(v.l, v.t, v.r, v.b, pm(Look::kPanel, fade));
            flush(dev);
            setDrawStates(dev, g_rt2, false);
            {
                // the outline: the shape four times, shifted and dark, then the shape itself on top. g_rt2 holds colour x
                // ground alpha and alpha 1 inside the shape.
                const float e = Look::kGroundEdgeWidth * s;
                const float uo = e / W, vo = e / H;
                const DWORD ga = (DWORD)(Look::kGroundAlpha * fade * 255.0f + 0.5f), f8 = (DWORD)(fade * 255.0f + 0.5f);
                const D3DCOLOR ec = D3DCOLOR_ARGB(ga, ((Look::kGroundEdge >> 16) & 255) * f8 / 255, ((Look::kGroundEdge >> 8) & 255) * f8 / 255,
                    (Look::kGroundEdge & 255) * f8 / 255);
                const float du[4] = { -uo, uo, 0, 0 }, dv[4] = { 0, 0, -vo, vo };
                for (int k = 0; k < 4; ++k) quad(v.l, v.t, v.r, v.b, ec, du[k], dv[k], 1.0f + du[k], 1.0f + dv[k]);
                quad(v.l, v.t, v.r, v.b, D3DCOLOR_ARGB(ga, f8, f8, f8));
                flush(dev);
            }

            // ---- layout: icons first, then area names, then bonfire labels; no label covers another or an icon ----
            g_obstN = 0;
            const float bs = Look::kBonfireSize * s;
            const float pad = Look::kPad * s;
            const float zoneTop = v.t + (Look::kPad * 0.6f + Look::kTitleSize * 1.3f) * s;
            const float zoneBottom = v.b - (Look::kPad * 0.6f + Look::kHelpSize * 1.25f + Look::kLegendIcon * 1.35f + 2.0f) * s;
            const wchar_t* title = g_page == 1 ? L"Lordran" : (g_pagePlace >= 0 ? placeName(kPlaces[g_pagePlace].id) : NULL);
            { const LRect t = { v.l, v.t, v.l + pad * 2.0f + textWidth(0, title), zoneTop }; addObst(t); }      // the title
            { const LRect t = { v.l, zoneBottom, v.r, v.b }; addObst(t); }                                       // legend + controls
            int nb = 0;
            int bi[64];
            float bx[64], by[64], bsz[64];
            bool bBelow[64], bAbove[64];
            const float band = g_page == 0 ? Look::kBonfireLevel : Look::kBonfireLevelWorld;
            for (int b = 0; b < kBonfireCount; ++b) {
                if (!g_lit[b] || !bonfireOnPage(b)) continue;
                float px, py;
                toScreen(kBonfires[b].x, kBonfires[b].z, px, py);
                const float dy = kBonfires[b].y - v.refY;
                bi[nb] = b; bx[nb] = px; by[nb] = py;
                bBelow[nb] = dy < -band; bAbove[nb] = dy > band;
                bsz[nb] = bBelow[nb] ? bs * Look::kBonfireBelowScale : bs;
                const LRect r = { px - bsz[nb] * 0.32f, py - bsz[nb] * 0.5f, px + bsz[nb] * 0.32f, py + bsz[nb] * 0.45f };
                addObst(r);
                ++nb;
            }
            if (heroHere) {
                float hx, hy;
                toScreen(g_hero[0], g_hero[2], hx, hy);
                const float h = Look::kHeroSize * s;
                const LRect r = { hx - h, hy - h, hx + h, hy + h };
                addObst(r);
            }
            // area names (world page): on the area's explored ground nearest to its middle
            int na = 0;
            const wchar_t* aText[kPlaceCount];
            float ax[kPlaceCount], ay[kPlaceCount];
            if (g_page == 1) {
                const float lh = Look::kLabelSize * s * 1.2f;
                for (int p = 0; p < kPlaceCount; ++p) {
                    if (!kPlaces[p].world || g_stat[p].n < Look::kLabelMinCells) continue;
                    const wchar_t* name = placeName(kPlaces[p].id);
                    if (!name) continue;
                    float wx, wz, px, py;
                    placeAnchor(p, wx, wz);
                    toScreen(wx, wz, px, py);
                    const float w = textWidth(1, name);
                    const float tries[3] = { -0.5f, 0.6f, -1.6f };          // centred on the point, below, above
                    for (int k = 0; k < 3; ++k) {
                        const LRect r = { px - w * 0.5f, py + lh * tries[k], px + w * 0.5f, py + lh * (tries[k] + 1.0f) };
                        if (r.l < v.l || r.r > v.r || r.t < v.t || r.b > v.b || !freeRect(r)) continue;
                        addObst(r);
                        aText[na] = name; ax[na] = px; ay[na] = r.t; ++na;
                        break;
                    }
                }
            }
            // bonfire labels: under the icon, else above it, else left out
            int nl = 0;
            const wchar_t* lText[64];
            LRect lRect[64];
            if (g_page == 0 || g_zoom[1] <= Look::kBonfireNameWorldZoom) {
                const float lh = Look::kBonfireNameSize * s * 1.2f, px_ = Look::kNamePadX * s, py_ = Look::kNamePadY * s;
                for (int i = 0; i < nb; ++i) {
                    const int b = bi[i];
                    const wchar_t* name = NULL;
                    if (kBonfires[b].name >= 0) name = placeName((WORD)kBonfires[b].name);
                    else if (g_bonfireGround[b] >= 0) name = placeName(kPlaces[g_bonfireGround[b]].id);
                    if (!name) continue;
                    const float w = textWidth(3, name) + px_ * 2.0f, h = lh + py_ * 2.0f;
                    const LRect under = { bx[i] - w * 0.5f, by[i] + bsz[i] * 0.45f + 1.0f, bx[i] + w * 0.5f, by[i] + bsz[i] * 0.45f + 1.0f + h };
                    const LRect over = { bx[i] - w * 0.5f, by[i] - bsz[i] * 0.5f - 1.0f - h, bx[i] + w * 0.5f, by[i] - bsz[i] * 0.5f - 1.0f };
                    const LRect* pick = NULL;
                    if (under.l >= v.l && under.r <= v.r && under.b <= v.b && freeRect(under)) pick = &under;
                    else if (over.l >= v.l && over.r <= v.r && over.t >= v.t && freeRect(over)) pick = &over;
                    if (!pick) continue;
                    addObst(*pick);
                    lText[nl] = name; lRect[nl] = *pick; ++nl;
                }
            }

            // ---- bonfire icons ----
            if (g_bonfireTex) setDrawStates(dev, g_bonfireTex, false);
            else setDrawStates(dev, g_blob, false);
            for (int i = 0; i < nb; ++i) {
                const float k = bBelow[i] ? Look::kBonfireBelowDark : 1.0f, sz = bsz[i];
                const DWORD a = (DWORD)(fade * 255.0f + 0.5f), c = (DWORD)(fade * k * 255.0f + 0.5f);
                const D3DCOLOR col = g_bonfireTex ? D3DCOLOR_ARGB(a, c, c, c) : pm(D3DCOLOR_ARGB(255, 255, 150, 50), fade * k);
                quad(bx[i] - sz * 0.5f, by[i] - sz * 0.5f, bx[i] + sz * 0.5f, by[i] + sz * 0.5f, col);
            }
            flush(dev);
            setDrawStates(dev, g_shape[SHAPE_UP], false);
            for (int i = 0; i < nb; ++i) {                      // the up mark of the bonfires above the hero
                if (!bAbove[i]) continue;
                const float m = 6.0f * s, x0 = bx[i] + bs * 0.34f, y0 = by[i] - bs * 0.40f;
                const DWORD f8 = (DWORD)(fade * 255.0f + 0.5f);
                quad(x0 - m, y0 - m, x0 + m, y0 + m, D3DCOLOR_ARGB(f8, f8, f8, f8));
            }
            flush(dev);

            // ---- labels: the bonfire labels' frames, then all texts ----
            if (nl) {
                setDrawStates(dev, NULL, false);
                const float lw = floorf(s + 0.5f) < 1.0f ? 1.0f : floorf(s + 0.5f);
                for (int i = 0; i < nl; ++i) {
                    const LRect& r = lRect[i];
                    quad(r.l, r.t, r.r, r.b, pm(Look::kNameBack, fade));
                    const D3DCOLOR c = pm(Look::kNameLine, fade);
                    quad(r.l, r.t, r.r, r.t + lw, c); quad(r.l, r.b - lw, r.r, r.b, c);
                    quad(r.l, r.t + lw, r.l + lw, r.b - lw, c); quad(r.r - lw, r.t + lw, r.r, r.b - lw, c);
                }
                flush(dev);
            }
            for (int i = 0; i < na; ++i) text(1, aText[i], ax[i], ay[i], Look::kLabel, DT_CENTER | DT_TOP, fade);
            for (int i = 0; i < nl; ++i)
                text(3, lText[i], (lRect[i].l + lRect[i].r) * 0.5f, lRect[i].t + Look::kNamePadY * s, Look::kBonfireName, DT_CENTER | DT_TOP, fade);

            // soul remains: a green spot, or an arrow on the edge pointing to it
            if (g_stainHas && onPage(g_stainPlace)) {
                float px, py;
                toScreen(g_stain[0], g_stain[2], px, py);
                const float e = Look::kStainEdge * s;
                const float mx = (v.l + v.r) * 0.5f, my = (v.t + v.b) * 0.5f;
                if (px >= v.l + e && px <= v.r - e && py >= v.t + e && py <= v.b - e) {
                    setDrawStates(dev, g_blob, false);
                    const float g = Look::kStainSize * s * 1.8f, c = Look::kStainSize * s;
                    quad(px - g, py - g, px + g, py + g, pm(Look::kStainGlow, fade));
                    quad(px - c * 0.5f, py - c * 0.5f, px + c * 0.5f, py + c * 0.5f, pm(Look::kStain, fade));
                    flush(dev);
                }
                else {
                    float dx = px - mx, dy = py - my;
                    const float hw = (v.r - v.l) * 0.5f - e, hh = (v.b - v.t) * 0.5f - e;
                    const float k = fabsf(dx) * hh > fabsf(dy) * hw ? hw / (fabsf(dx) + 1e-6f) : hh / (fabsf(dy) + 1e-6f);
                    const float ex = mx + dx * k, ey = my + dy * k;
                    const float len = sqrtf(dx * dx + dy * dy) + 1e-6f;
                    dx /= len; dy /= len;
                    const float a = Look::kStainArrow * s;
                    const DWORD f8 = (DWORD)(fade * 255.0f + 0.5f);
                    setDrawStates(dev, g_shape[SHAPE_STAIN_ARROW], false);
                    quadDir(ex, ey, a, dx, dy, D3DCOLOR_ARGB(f8, f8, f8, f8));
                    flush(dev);
                }
            }

            // the hero: an arrow the way he faces
            if (heroHere) {
                float px, py;
                toScreen(g_hero[0], g_hero[2], px, py);
                float fx = -sinf(g_hero[3]), fz = -cosf(g_hero[3]);
                float dx, dy;
                worldDirToScreen(fx, fz, dx, dy);
                const float len = sqrtf(dx * dx + dy * dy);
                if (len > 1e-6f) { dx /= len; dy /= len; } else { dx = 0.0f; dy = -1.0f; }
                const DWORD f8 = (DWORD)(fade * 255.0f + 0.5f);
                setDrawStates(dev, g_shape[SHAPE_HERO], false);
                quadDir(px, py, Look::kHeroSize * s, dx, dy, D3DCOLOR_ARGB(f8, f8, f8, f8));
                flush(dev);
            }

            // title and the controls
            text(0, title, v.l + pad, v.t + pad * 0.6f, Look::kTitle, DT_LEFT | DT_TOP, fade);
            const wchar_t* help = padHelp
                ? (g_page == 1 ? L"RS  Move     LT / RT  Zoom     LB / RB  Area     R3  Area map"
                               : L"RS  Move     LT / RT  Zoom     LB / RB  Area     R3  World map")
                : (g_page == 1 ? L"Drag  Move     Wheel  Zoom     ,  /  .  Area     M  Area map"
                               : L"Drag  Move     Wheel  Zoom     ,  /  .  Area     M  World map");
            const float helpY = v.b - pad * 0.6f - Look::kHelpSize * s * 1.25f;
            text(2, help, v.l + pad, helpY, Look::kHelp, DT_LEFT | DT_TOP, fade);
            drawLegend(dev, v.l + pad, helpY - Look::kLegendIcon * s * 1.35f, s, fade);

            // the frame (two thin lines, like the game's windows)
            dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
            setDrawStates(dev, NULL, false);
            const float lw = floorf(s + 0.5f) < 1.0f ? 1.0f : floorf(s + 0.5f), gp = floorf(Look::kFrameGap * s + 0.5f);
            for (int k = 0; k < 2; ++k) {
                const float i = k ? gp : 0.0f;
                const D3DCOLOR c = pm(k ? Look::kFrameInner : Look::kFrameOuter, fade);
                quad(v.l + i, v.t + i, v.r - i, v.t + i + lw, c);
                quad(v.l + i, v.b - i - lw, v.r - i, v.b - i, c);
                quad(v.l + i, v.t + i + lw, v.l + i + lw, v.b - i - lw, c);
                quad(v.r - i - lw, v.t + i + lw, v.r - i, v.b - i - lw, c);
            }
            flush(dev);
            dev->EndScene();
        }
        if (oldRT) { dev->SetRenderTarget(0, oldRT); oldRT->Release(); }
        dev->SetDepthStencilSurface(oldDS);
        if (oldDS) oldDS->Release();
        g_sb->Apply();
        bb->Release();
    }

// =====================================================================================================================
// Every frame
// =====================================================================================================================
    bool g_prevR3 = false, g_prevLB = false, g_prevRB = false;
    bool g_prevM = false, g_prevComma = false, g_prevPeriod = false;
    bool g_dragging = false, g_prevLButton = false;
    POINT g_dragLast = { 0, 0 };

    bool gameHasFocus() {
        const HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        return fg && pid == GetCurrentProcessId();
    }

    void stepArea(int dir) {
        int list[kPlaceCount];
        const int n = visiblePlaces(list);
        if (n <= 0) return;
        int at = -1;
        for (int i = 0; i < n; ++i) if (list[i] == g_pagePlace) at = i;
        if (g_page == 1 || at < 0) {
            // from the world page: the hero's area first (or the first one)
            at = -1;
            for (int i = 0; i < n; ++i) if (list[i] == g_heroPlace) at = i;
            if (at < 0) at = dir > 0 ? n - 1 : 0;
            if (g_page == 1) { g_page = 0; g_pagePlace = list[at]; g_offX = g_offZ = 0; return; }
        }
        at = (at + dir + n) % n;
        g_page = 0;
        g_pagePlace = list[at];
        g_offX = g_offZ = 0;
    }
    void togglePage() {
        g_page = 1 - g_page;
        g_offX = g_offZ = 0;
        if (g_page == 0 && g_pagePlace < 0) g_pagePlace = g_heroPlace;
    }

    void handleInput(IDirect3DDevice9* dev, float dt) {
        // controller: all pads read in the last half second
        bool r3 = false, lb = false, rb = false;
        int lt = 0, rt = 0, rx = 0, ry = 0;
        const DWORD now = GetTickCount();
        for (int i = 0; i < 4; ++i) {
            if (now - g_padTick[i] > 500) continue;
            const PhysPad& p = g_pad[i];
            r3 |= p.r3; lb |= p.lb; rb |= p.rb;
            if (p.lt > lt) lt = p.lt;
            if (p.rt > rt) rt = p.rt;
            if (abs(p.rx) + abs(p.ry) > abs(rx) + abs(ry)) { rx = p.rx; ry = p.ry; }
        }
        if (r3 && !g_prevR3) togglePage();
        if (lb && !g_prevLB) stepArea(-1);
        if (rb && !g_prevRB) stepArea(+1);
        g_prevR3 = r3; g_prevLB = lb; g_prevRB = rb;
        const float trig = ((lt > 30 ? (lt - 30) / 225.0f : 0.0f) - (rt > 30 ? (rt - 30) / 225.0f : 0.0f));
        if (trig != 0.0f) g_zoom[g_page] *= powf(Look::kTriggerZoomPerSecond, trig * dt);
        const float dz = 9000.0f;
        float sx = 0.0f, sy = 0.0f;
        const float mag = sqrtf((float)rx * rx + (float)ry * ry);
        if (mag > dz) { const float k = (mag - dz) / (32767.0f - dz) / mag; sx = rx * k; sy = ry * k; }
        const float viewW = (g_view.r - g_view.l) * g_view.mppBb;
        if (sx != 0.0f || sy != 0.0f) {
            // the stick moves the map itself (like dragging it): right = the map goes right
            float wx, wz;
            screenDirToWorld(sx, -sy, wx, wz);              // the map moves this way on the screen
            g_offX -= wx * viewW * Look::kStickPanPerSecond * dt;
            g_offZ -= wz * viewW * Look::kStickPanPerSecond * dt;
        }

        // keyboard / mouse
        const bool focus = gameHasFocus();
        const bool m = focus && (GetAsyncKeyState('M') & 0x8000) != 0;
        const bool comma = focus && (GetAsyncKeyState(VK_OEM_COMMA) & 0x8000) != 0;
        const bool period = focus && (GetAsyncKeyState(VK_OEM_PERIOD) & 0x8000) != 0;
        if (m && !g_prevM) { togglePage(); g_lastKbmUse = now; }
        if (comma && !g_prevComma) { stepArea(-1); g_lastKbmUse = now; }
        if (period && !g_prevPeriod) { stepArea(+1); g_lastKbmUse = now; }
        g_prevM = m; g_prevComma = comma; g_prevPeriod = period;
        const LONG wheel = InterlockedExchange(&g_wheelAcc, 0);
        if (wheel) g_zoom[g_page] *= powf(Look::kWheelZoomStep, wheel / 120.0f);
        const bool lbtn = focus && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        POINT pt;
        RECT rc;
        D3DSURFACE_DESC bd;
        IDirect3DSurface9* bb = NULL;
        bool have = false;
        if (g_hwnd && GetCursorPos(&pt) && ScreenToClient(g_hwnd, &pt) && GetClientRect(g_hwnd, &rc) && rc.right > 0 && rc.bottom > 0 &&
            SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
            bb->GetDesc(&bd);
            bb->Release();
            pt.x = (LONG)(pt.x * (float)bd.Width / rc.right);
            pt.y = (LONG)(pt.y * (float)bd.Height / rc.bottom);
            have = true;
        }
        if (have) {
            const bool inside = pt.x >= g_view.l && pt.x < g_view.r && pt.y >= g_view.t && pt.y < g_view.b;
            if (lbtn && !g_prevLButton && inside) { g_dragging = true; g_dragLast = pt; g_lastKbmUse = now; }
            if (!lbtn) g_dragging = false;
            if (g_dragging) {
                float wx, wz;
                screenDirToWorld((float)(pt.x - g_dragLast.x), (float)(pt.y - g_dragLast.y), wx, wz);
                g_offX -= wx * g_view.mppBb;
                g_offZ -= wz * g_view.mppBb;
                g_dragLast = pt;
            }
        }
        g_prevLButton = lbtn;

        const float zmin = g_page ? Look::kWorldZoomMin : Look::kAreaZoomMin, zmax = g_page ? Look::kWorldZoomMax : Look::kAreaZoomMax;
        if (g_zoom[g_page] < zmin) g_zoom[g_page] = zmin;
        if (g_zoom[g_page] > zmax) g_zoom[g_page] = zmax;
    }

    void frameTick(IDirect3DDevice9* dev) {
        if (!g_on || !dev) return;
        const ULONGLONG ms = GetTickCount64();
        float dt = g_lastMs ? (float)(ms - g_lastMs) / 1000.0f : 0.0f;
        if (dt > 0.1f) dt = 0.1f;
        g_lastMs = ms;
        const DWORD now = (DWORD)ms;

        const DWORD chr = playerChr();
        const int loading = menuDword(0x7EC);
        DWORD slot = 0xFFFFFFFF, gm = 0;
        if (rdD(kGameMan, gm) && gm) rdD(gm + 0xA70, slot);
        if (!chr || (loading >= 1 && loading <= 4) || slot > 9) {
            if (g_hadPlayer) {
                g_hadPlayer = false;
                g_haveLastKey = false;
                if (g_dirty) saveFile();
            }
            g_shown = false;
            g_alpha = 0.0f;
            g_inputActive = false;
            g_dragging = false;
            return;
        }
        if (!g_hwnd) {
            D3DDEVICE_CREATION_PARAMETERS cp;
            if (SUCCEEDED(dev->GetCreationParameters(&cp))) g_hwnd = cp.hFocusWindow;
        }
        if (!g_hadPlayer) {
            g_hadPlayer = true;
            g_enterTick = now;
            // the character in play: slot and name
            DWORD gdm = 0, pd = 0;
            wchar_t name[16] = { 0 };
            if (rdD(kGameDataMan, gdm) && gdm && rdD(gdm + 8, pd) && pd) rdBlock(pd + 0xA0, name, 30);
            name[15] = 0;
            if ((int)slot != g_activeSlot || wcsncmp(name, g_activeName, 16) != 0) activate((int)slot, name);
            g_stainHas = false;
            readStain(false);
        }

        // where the hero is, and record it
        if (heroPos(chr, g_hero)) {
            // the game's area banner says where he is; the explored ground is recorded only with it (right after a loading
            // screen it is -1 for a moment: nothing is recorded then, so every cell carries the game's own area)
            const int banner = placeIndex((DWORD)menuDword(0xEB4));
            int place = banner;
            DWORD block = 0;
            if (place < 0 && now - g_enterTick > 3000 && currentBlock(block)) place = blockDefaultPlace(block);   // only for the page
            g_heroPlace = place;
            if (banner >= 0) {
                const unsigned long long k = cellKey(fl(g_hero[0], kCellSize[0]), fl(g_hero[2], kCellSize[0]), fl(g_hero[1], kCellHeight[0]));
                if (!g_haveLastKey || k != g_lastKey) {
                    g_lastKey = k;
                    g_haveLastKey = true;
                    if (reveal(g_hero[0], g_hero[1], g_hero[2], banner)) g_dirty = true;
                }
            }
        }
        if (now - g_stainCheckTick > 500) { g_stainCheckTick = now; readStain(true); }
        if (g_dirty && now - g_lastSaveTick > 60000) { g_lastSaveTick = now; saveFile(); }
        if (g_writing == 2) { InterlockedExchange(&g_writing, 0); SDLOG(0, "gameMap: could not write %s\n", g_filePath.c_str()); }

        // shown while the gesture window is open (7 / 9: closing)
        const int gw = menuDword(0xEC);
        const bool shownNow = gw != 0 && gw != 7 && gw != 9;
        if (shownNow && !g_shown) {
            readLitBonfires();
            for (int b = 0; b < kBonfireCount; ++b) {
                g_bonfirePlace[b] = placeIndex(kBonfires[b].place);
                const int p = placeAt(kBonfires[b].x, kBonfires[b].y, kBonfires[b].z);
                g_bonfirePlace2[b] = p != g_bonfirePlace[b] ? p : -1;
                g_bonfireGround[b] = placeAt(kBonfires[b].x, kBonfires[b].y, kBonfires[b].z, 1);   // within 6 m, +-3 m height
            }
            g_page = 0;
            g_pagePlace = g_heroPlace;
            if (g_pagePlace < 0) { int list[kPlaceCount]; if (visiblePlaces(list) > 0) g_pagePlace = list[0]; }
            g_offX = g_offZ = 0;
        }
        g_shown = shownNow;
        const float step = Look::kFadeSeconds > 0.0f ? dt / Look::kFadeSeconds : 1.0f;
        g_alpha += g_shown ? step : -step;
        if (g_alpha < 0.0f) g_alpha = 0.0f;
        if (g_alpha > 1.0f) g_alpha = 1.0f;
        g_inputActive = g_shown && g_inputHooks;
        if (g_alpha <= 0.0f) { g_dragging = false; return; }
        if (g_shown) {
            readLitBonfires();
            handleInput(dev, dt);
        }
        kbIcons::Shared* sh = kbIcons::shared();
        const bool padHelp = sh ? sh->kbmActive == 0 : g_lastPadUse >= g_lastKbmUse;
        draw(dev, padHelp);
    }

    // the bottom-left HUD's opacity is multiplied by this (1 = as set in DSfix.ini, 0 while the map shows)
    float hudBottomLeftFactor() { return g_on ? 1.0f - g_alpha : 1.0f; }

} // namespace gameMap

bool installGameMap() {
    if (!Settings::get().getGameMap()) return false;
    gameMap::g_on = true;
    gameMap::g_inputHooks = installGameMapInput(&gameMap::onPad, &gameMap::onWheel);
    SDLOG(0, "gameMap: on (with the gesture window%s)\n", gameMap::g_inputHooks ? "" : "; its controls are off: the game code is not as expected");
    return true;
}
