#pragma once

// covenantOfferings: offer several items (or several humanity) to a covenant at once.
// Returns true when the feature is on and every place it needs was found in the game.
bool installCovenantOffer();

// True when the feature is running.
bool covenantOfferActive();

// BonfireCovenants: "Enter Covenant" in the bonfire list, to switch to a covenant you already joined in this playthrough.
// Nothing is patched when the setting is off.
bool installBonfireCovenants();

// BonfireCovenants: the short fade after Yes, drawn at Present (nothing when off or not fading), and its release.
struct IDirect3DDevice9;
void bonfireCovenantsDraw(IDirect3DDevice9* dev);
void bonfireCovenantsRelease();
