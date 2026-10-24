#pragma once

#include <d3d9.h>

// A small "DSfix 3.0" mark in the bottom-right corner of the title screen ("PRESS ANY BUTTON" and the main menu), so
// players can see which DSfix they run. It is shown only while the title screen is drawn (recognised by its copyright
// line, which is there from the first moment, and by the static title logo), fades in and out with it, and is never
// drawn anywhere else (gameplay, menus, loading screens). The look is in one block at the top of TitleMark.cpp.

// Called by RSManager when the game sets its title screen logo texture (MenuscreenLogo; only after the logo animation).
void titleMarkLogoSeen();

// Called by RSManager for each 2D quad (INT16 vertices, stride 12) with its bounding box in screen fractions (0..1 of the
// 1280x720 UI space). Recognises the pieces of the title screen's copyright line.
void titleMarkOnQuad(const float box[4]);

// Called by RSManager right before the frame is presented: draws the mark onto the back buffer while visible.
void titleMarkDraw(IDirect3DDevice9* dev);

// Called when the device releases its resources (Reset, Alt+Tab, shutdown).
void titleMarkRelease();

// True while the title screen is showing (the same test that shows the mark). Used by the quick load.
bool titleScreenShowing();
