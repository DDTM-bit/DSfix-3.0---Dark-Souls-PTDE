#pragma once

#include <windows.h>
#include <d3d9.h>

// durabilityBars: a thin bar under each weapon slot of the HUD shows the durability of the weapon in that hand
// (Dark Souls 3 style). Research on where the game keeps the durability: the Dark Souls Overhaul team (metal-crow &
// Sean Pesce). The bars are drawn right after each slot's frame, in the same place and the same way, so they belong to
// the bottom-left HUD group: hudScaleBottomLeft and hudBottomLeftOpacity apply, they hide whenever the HUD does, and
// they work the same with enableHudMod 0. The look (size, position, colours) is in one block at the top of
// DurabilityBars.cpp.

// Installs the hook that reads the durability of the equipped weapons. Called once at start-up when durabilityBars is on.
bool installDurabilityBars();

// True when the hook is installed (durabilityBars on and the game build matches).
bool durabilityBarsActive();

// Called once per frame (Present).
void durabilityBarsNewFrame();

// Called by RSManager before each 2D HUD piece is drawn (quads and text), with its bounding box in HUD fractions and
// whether it uses the HUD frame texture (HudHealthbar). Returns how far to move it down (UI units, 1280x720 x 8; 0 = not at
// all): the arrow / bolt boxes under a bow or crossbow slot and their count number, so that they clear the bar.
int durabilityBarsAmmoShift(const float box[4], bool frameTexture);

// Called by RSManager after each 2D HUD quad (INT16 vertices, stride 12, one quad) was drawn.
// box: the quad's bounding box in HUD fractions (0..1 of the 1280x720 UI space); frameTexture: drawn with the HUD
// frame texture (HudHealthbar); vertexAlpha: the alpha of its first vertex; opacity: the HUD group opacity DSfix applied
// to this draw (1 when it went into the HUD buffer, which gets the opacity when it is composited); hudBuffer: the draw went
// into DSfix's HUD buffer.
void durabilityBarsOnHudQuad(IDirect3DDevice9* dev, const float box[4], bool frameTexture, BYTE vertexAlpha, float opacity,
    bool hudBuffer);
