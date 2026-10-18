
#include "RenderstateManager.h"

#include <time.h>
#include <intsafe.h>
#include <io.h>
#include <fstream>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cmath>

#include "d3dutil.h"
#include "Settings.h"
#include "Hash.h"
#include "Detouring.h"
#include "WindowManager.h"
#include "FPS.h"
#include "resource1.h"
#include <atlbase.h>
#include "AspectFix.h"
#include "WeaponWearBars.h"
#include "TitleMark.h"
#include "Effect.h"
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

// ============================================================================================
// Per-draw HUD scaling (enableHudMod + hudDrawScale)
//
// The game's UI vertices are INT16[6] = { x, y, colorLo, colorHi, u, v } in a virtual 1280x720
// space with 8 units per pixel (a full-screen quad spans 0..10240 x 0..5760).
// While the HUD window is open, every draw call is classified by the position of its quad and
// drawn with a shrunken D3D viewport anchored at that zone's corner. Because this happens per draw
// call, all pieces of one element (boss bar frame, ruler, fill, ...) get exactly the same
// transform, no matter whether the draw ends up in the HUD buffer or goes straight to the frame.
// ============================================================================================
namespace {
	enum HudGroup { HG_NONE = 0, HG_TL, HG_BL, HG_BR, HG_BOSS };

	bool     g_hudScalePhase = false;   // true from the start of the HUD until Present / first menu draw
	bool     g_hudWindowSeen = false;   // a HUD window (buffer) was started in this frame
	HudGroup g_lastHudGroup = HG_NONE; // text/button glyphs follow the element drawn just before them

	const float kUiW = 10240.0f;
	const float kUiH = 5760.0f;

	float hudGroupScale(HudGroup g) {
		float s = 1.0f;
		if (g == HG_TL) s = Settings::get().getHudScaleTopLeft();
		else if (g == HG_BL) s = Settings::get().getHudScaleBottomLeft();
		else if (g == HG_BR) s = Settings::get().getHudScaleBottomRight();
		else if (g == HG_BOSS) s = Settings::get().getHudScaleBottomCenter();
		else return 1.0f;
		// a viewport can only shrink; scales >= 1 are handled by the compositor in Hud.cpp
		return (s < 1.0f && s > 0.05f) ? s : 1.0f;
	}

	void hudGroupAnchor(HudGroup g, const HudLayout& L, float& ax, float& ay) {
		ax = 0.0f; ay = 0.0f;
		if (g == HG_BL) { ax = 0.0f; ay = 1.0f; }
		else if (g == HG_BR) { ax = 1.0f; ay = 1.0f; }
		else if (g == HG_BOSS) { ax = 0.59f; ay = 1.0f; } // boss bar is centred around x~0.59
	}

	// Zones (centre of the quad / text string, normalized screen coordinates):
	//  TL   top-left strip, x < 0.60            (health, stamina, humanity, status)
	//  BL   left block, below mid-screen        (item cluster + its labels)
	//  BR   bottom-right corner, BELOW the boss bar (souls counter)
	//  BOSS lower screen, between BL and the right edge (boss bar + boss name; scale 1.0 by default)
	// Everything else (top-right: flame icon, quick menu; centre: prompts, banners) is never scaled.
	HudGroup zoneFor(float cx, float cy, float w, float h) {
		if (w > 0.7f || h > 0.45f) return HG_NONE; // fades, full-screen overlays
		const HudLayout L = getHudLayout();
		if (cy < L.tlH && cx < 0.60f) return HG_TL;
		if (cy > L.blTop && cx < L.blW) return HG_BL;
		if (cy > L.brTop && cx > L.brLeft) return HG_BR;
		if (cy > 0.70f && cx >= L.blW) return HG_BOSS;
		return HG_NONE;
	}

	// 'valid' (optional) reports whether the vertex data looked like sane UI coordinates.
	// 'outBox' (optional) receives the normalized bounding box { x0, y0, x1, y1 } of the quad.
	HudGroup classifyHudQuad(const INT16* v, UINT strideBytes, UINT first, UINT count, bool* valid = NULL, float* outBox = NULL) {
		if (valid) *valid = false;
		if (!v || strideBytes < 4 || count == 0) return HG_NONE;
		if (count > 4096) count = 4096;
		const UINT step = strideBytes / 2;
		float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
		for (UINT i = first; i < first + count; ++i) {
			const INT16* p = v + (size_t)i * step;
			const float x = (float)p[0], y = (float)p[1];
			if (x < minX) minX = x;
			if (x > maxX) maxX = x;
			if (y < minY) minY = y;
			if (y > maxY) maxY = y;
		}
		const float w = (maxX - minX) / kUiW, h = (maxY - minY) / kUiH;
		const float cx = (minX + maxX) * 0.5f / kUiW, cy = (minY + maxY) * 0.5f / kUiH;
		if (outBox) { outBox[0] = minX / kUiW; outBox[1] = minY / kUiH; outBox[2] = maxX / kUiW; outBox[3] = maxY / kUiH; }
		if (valid) *valid = (cx > -0.2f && cx < 1.2f && cy > -0.2f && cy < 1.2f && w < 1.5f && h < 1.5f && (w > 0.0f || h > 0.0f));
		return zoneFor(cx, cy, w, h);
	}

	// Text strings use a different vertex format: stride 24 = float x, y, z (pixels in the 1280x720 UI space),
	// DWORD colour, float u, v. Classifies the string by its bounding box.
	bool textBox(const void* data, UINT stride, UINT count, float& minX, float& minY, float& maxX, float& maxY) {
		if (!data || stride != 24 || count == 0 || count > 4096) return false;
		minX = minY = 1e9f; maxX = maxY = -1e9f;
		for (UINT i = 0; i < count; ++i) {
			const float* p = (const float*)((const BYTE*)data + (size_t)i * stride);
			const float x = p[0], y = p[1];
			if (!(x > -2000.0f && x < 4000.0f && y > -2000.0f && y < 3000.0f)) return false; // also rejects NaN
			if (x < minX) minX = x;
			if (x > maxX) maxX = x;
			if (y < minY) minY = y;
			if (y > maxY) maxY = y;
		}
		return (maxX >= minX && maxY >= minY);
	}

	// Moves a text string to where the shrunken element it belongs to is and shrinks the glyphs with it.
	// align: 0 = keep the string's centre, 1 = keep its left edge (left-aligned labels), 2 = keep its right edge (right-aligned numbers)
	void transformHudText(std::vector<BYTE>& out, const void* data, UINT stride, UINT count, HudGroup g,
		float minX, float minY, float maxX, float maxY, int align) {
		const float s = hudGroupScale(g);
		const float ts = s; // glyphs shrink together with the element they belong to
		const HudLayout L = getHudLayout();
		float ax, ay;
		hudGroupAnchor(g, L, ax, ay);
		const float axp = ax * 1280.0f, ayp = ay * 720.0f;
		const float cx = (minX + maxX) * 0.5f, cy = (minY + maxY) * 0.5f;
		const float refX = (align == 1) ? minX : ((align == 2) ? maxX : cx);
		const float nrefX = axp + (refX - axp) * s;
		const float ncy = ayp + (cy - ayp) * s;
		out.resize((size_t)count * stride);
		memcpy(&out[0], data, (size_t)count * stride);
		for (UINT i = 0; i < count; ++i) {
			float* p = (float*)(&out[0] + (size_t)i * stride);
			p[0] = nrefX + (p[0] - refX) * ts;
			p[1] = ncy + (p[1] - cy) * ts;
		}
	}

	// Shrinks the viewport around the group's anchor. Returns true if the viewport was changed
	// (caller must restore 'saved' after the draw).
	bool applyHudViewport(IDirect3DDevice9* dev, HudGroup g, D3DVIEWPORT9& saved) {
		const float s = hudGroupScale(g);
		if (s >= 1.0f || !dev) return false;
		if (FAILED(dev->GetViewport(&saved))) return false;
		const HudLayout L = getHudLayout();
		float ax, ay;
		hudGroupAnchor(g, L, ax, ay);
		const float W = (float)saved.Width, H = (float)saved.Height;
		D3DVIEWPORT9 vp = saved;
		vp.Width = (DWORD)(W * s + 0.5f);
		vp.Height = (DWORD)(H * s + 0.5f);
		vp.X = saved.X + (DWORD)(W * ax * (1.0f - s) + 0.5f);
		vp.Y = saved.Y + (DWORD)(H * ay * (1.0f - s) + 0.5f);
		if (vp.Width == 0 || vp.Height == 0) return false;
		return SUCCEEDED(dev->SetViewport(&vp));
	}


	float hudGroupOpacity(HudGroup g) {
		float o = 1.0f;
		if (g == HG_TL) o = Settings::get().getHudTopLeftOpacity();
		else if (g == HG_BL) o = Settings::get().getHudBottomLeftOpacity();
		else if (g == HG_BR) o = Settings::get().getHudBottomRightOpacity();
		if (o < 0.0f) o = 0.0f;
		if (o > 1.0f) o = 1.0f;
		return o;
	}

	// Draws that go straight to the frame (not through the HUD buffer) do not get the per-corner opacity when the buffer
	// is composited. The game's UI is drawn with shaders, so fixed-function texture-stage tricks have no effect on it,
	// but its shaders do use the vertex colour (the game fades UI pieces itself that way). So the opacity is applied by
	// scaling the alpha byte (D3DCOLOR, alpha = 4th byte) of a copy of the vertices.
	// 'out' may already hold the vertices (then 'data' must point into it) and is modified in place.
	const void* applyVertexOpacity(std::vector<BYTE>& out, const void* data, UINT stride, UINT first, UINT count,
		UINT colorOffset, float o, IDirect3DDevice9* dev) {
		if (!data || o >= 0.999f || count == 0 || count > 65536 || stride < colorOffset + 4) return data;
		if (o < 0.0f) o = 0.0f;
		DWORD srcBlend = D3DBLEND_SRCALPHA;
		if (dev) dev->GetRenderState(D3DRS_SRCBLEND, &srcBlend);
		const bool scaleRgb = (srcBlend == D3DBLEND_ONE); // additive / premultiplied blending: alpha alone would not fade it
		const UINT f = (UINT)(o * 256.0f + 0.5f);
		const size_t total = (size_t)(first + count) * stride;
		BYTE* base;
		if (!out.empty() && (const void*)&out[0] == data && out.size() >= total) {
			base = &out[0];
		}
		else {
			out.assign((const BYTE*)data, (const BYTE*)data + total);
			base = &out[0];
		}
		for (UINT i = first; i < first + count; ++i) {
			BYTE* c = base + (size_t)i * stride + colorOffset;
			c[3] = (BYTE)((c[3] * f) >> 8);
			if (scaleRgb) {
				c[0] = (BYTE)((c[0] * f) >> 8);
				c[1] = (BYTE)((c[1] * f) >> 8);
				c[2] = (BYTE)((c[2] * f) >> 8);
			}
		}
		return base;
	}

	// Byte offset of the colour inside a 24-byte text vertex. Expected layout is float x,y,z + colour + float u,v (offset 12);
	// float x,y,z + u,v + colour (offset 20) is detected too. Returns 0 if neither looks right (then no opacity is applied).
	UINT textColorOffset(const void* data) {
		if (!data) return 0;
		const BYTE* v = (const BYTE*)data;
		float f12, f16, f20;
		memcpy(&f12, v + 12, 4); memcpy(&f16, v + 16, 4); memcpy(&f20, v + 20, 4);
		auto isUv = [](float f) { return f > -1.0f && f < 2.0f; }; // NaN fails both comparisons
		if (isUv(f16) && isUv(f20)) return 12;
		if (isUv(f12) && isUv(f16)) return 20;
		return 0;
	}

	// If the draw uses alpha testing, its reference value is scaled with the opacity so faded pixels are not cut away.
	struct AlphaRefGuard {
		IDirect3DDevice9* dev;
		DWORD ref;
		bool active;
		AlphaRefGuard(IDirect3DDevice9* d, float o) : dev(d), ref(0), active(false) {
			if (!d || o >= 0.999f) return;
			DWORD at = FALSE;
			if (FAILED(d->GetRenderState(D3DRS_ALPHATESTENABLE, &at)) || !at) return;
			if (FAILED(d->GetRenderState(D3DRS_ALPHAREF, &ref))) return;
			d->SetRenderState(D3DRS_ALPHAREF, (DWORD)(ref * (o < 0.0f ? 0.0f : o)));
			active = true;
		}
		~AlphaRefGuard() { if (active) dev->SetRenderState(D3DRS_ALPHAREF, ref); }
	};


	// Validated effect settings, shared by initResources() and the reload hotkeys.
	// -1 means "effect off". Values above the highest preset are clamped instead of indexing past the shader macro tables.
	int ssaoStrengthIndex() {
		unsigned s = Settings::get().getSsaoStrength();
		if (s == 0) return -1;
		return (int)(s > 3 ? 3 : s) - 1;            // SSAO: low / medium / high
	}
	int aaQualityIndex() {
		unsigned q = Settings::get().getAAQuality();
		if (q == 0) return -1;
		return (int)(q > 4 ? 4 : q) - 1;            // FXAA and SMAA: low / medium / high / ultra
	}
	SSAO::Type ssaoTypeFromSettings() {
		const std::string& t = Settings::get().getSsaoType();
		// DSfix 3.0: VSSAO2 used to fall through to SCAO (it was never loaded)
		if (t == "VSSAO") return SSAO::VSSAO;
		if (t == "HBAO") return SSAO::HBAO;
		if (t == "VSSAO2") return SSAO::VSSAO2;
		return SSAO::SCAO;
	}
	void dofBufferSize(unsigned& w, unsigned& h) {
		const unsigned dofRes = Settings::get().getDOFOverrideResolution();
		w = dofRes == 0 ? 512 : dofRes * 16 / 9;
		h = dofRes == 0 ? 360 : dofRes;
	}

	// ---- one-frame HUD dump (hotkey, see Actions.def: dumpHudFrame) ----
	bool g_dumpRequested = false;
	bool g_dumpActive = false;
	std::vector<std::string> g_dumpLines;
	bool g_dumpIsRequested = false;  // the frame being recorded is the one asked for with the hotkey (-> hud_dump.txt)
	// menu-open capture: after the hotkey, the first frame in which a menu appears (and the 3 frames after it) are written
	// to menu_open_dump.txt, so the frames right after pressing Start can be seen. Armed for about 10 seconds.
	int  g_menuCapArmFrames = 0;
	// flight recorder: armed by the hotkey for about 30 seconds; it keeps the dumps of the last frames, and a second hotkey
	// press writes them to flight_dump.txt (for short glitches that are over before the hotkey can be pressed)
	int  g_recArmFrames = 0;
	bool g_recWrite = false;
	const size_t kRecFrames = 240;
	std::vector< std::vector<std::string> > g_recRing;
	size_t g_recNext = 0;
	int  g_menuCapLeft = 0;
	int  g_menuCapFrameNo = 0;
	bool g_frameHadMenu = false;
	bool g_prevFrameHadMenu = false;
	// frames since the last frame with a HUD window (in game); the title screen never has one
	int  g_framesSinceHud = 1000;

	// ---- AA/SSAO placement hold ----
	// DSfix finds the scene image and the depth image by the order of the frame's SetRenderTarget calls (1st and 11th).
	// Some menus (the Status page) change that order in some frames, so the effects were skipped in those frames and
	// SSAO blinked on and off. The pair found by the normal detection is remembered; in a frame where the normal
	// detection does not apply the effects, the remembered pair is used instead, but only when the game actually set
	// both surfaces as render targets in this frame (so they are alive and freshly drawn). Only frames the normal
	// detection handles update the pair, so it can never drift to a wrong surface, and normal frames are unchanged.
	IDirect3DSurface9* g_heldMainRT = NULL;
	IDirect3DSurface9* g_heldZ = NULL;
	unsigned g_heldMainUses = 0;
	bool g_heldZSeen = false;
	bool g_effectsDone = false;
	unsigned g_heldFramesUsed = 0;

	// ---- boss bar rows: the background quad of each boss bar ([0.308 0.815 0.827 0.831]: the full width of the bar) is
	// remembered, so the bar's semi-transparent fills drawn while the HUD buffer is paused (the golden "damage taken" part)
	// can be recognised by sitting exactly in that row, at any width. They used to be taken for an enemy health bar once
	// the boss was below about half health (narrower than 0.3 of the screen) and stayed at their unscaled place.
	struct BossRow { float x0, y0, x1, y1; unsigned frame; };
	BossRow g_bossRows[4] = {};
	unsigned g_frameNo = 0;
	void rememberBossRow(const float* b) {
		int slot = -1, oldest = 0;
		for (int i = 0; i < 4; ++i) {
			const BossRow& r = g_bossRows[i];
			if (r.frame && fabsf(r.y0 - b[1]) < 0.004f && fabsf(r.y1 - b[3]) < 0.004f && fabsf(r.x0 - b[0]) < 0.004f) { slot = i; break; }
			if (g_bossRows[i].frame < g_bossRows[oldest].frame) oldest = i;
		}
		if (slot < 0) slot = oldest;
		BossRow& r = g_bossRows[slot];
		r.x0 = b[0]; r.y0 = b[1]; r.x1 = b[2]; r.y1 = b[3]; r.frame = g_frameNo;
	}
	bool onBossRow(const float* b) {
		for (int i = 0; i < 4; ++i) {
			const BossRow& r = g_bossRows[i];
			if (!r.frame || g_frameNo - r.frame > 10) continue; // seen in the last few frames only
			if (fabsf(r.y0 - b[1]) < 0.004f && fabsf(r.y1 - b[3]) < 0.004f && fabsf(r.x0 - b[0]) < 0.004f && b[2] <= r.x1 + 0.004f)
				return true;
		}
		return false;
	}

	// ---- render target order dump (same hotkey as the HUD dump; the next frames go to rt_dump.txt) ----
	int g_rtDumpFrames = 0;
	std::vector<std::string> g_rtDumpLines;

	void rtDumpLine(const char* fmt, ...) {
		char buf[256];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
		va_end(ap);
		g_rtDumpLines.push_back(buf);
	}

	// Vertex position i of a UI draw in normalized screen coordinates. INT16 quads (stride 12) use the 10240x5760 space,
	// text (stride 24) floats in 1280x720 UI pixels. False for other formats or garbage.
	bool dumpVertexPos(const void* data, UINT stride, UINT i, float& x, float& y) {
		if (stride == 12) {
			const INT16* p = (const INT16*)((const BYTE*)data + (size_t)i * stride);
			x = p[0] / kUiW; y = p[1] / kUiH;
			return true;
		}
		if (stride == 24) {
			const float* p = (const float*)((const BYTE*)data + (size_t)i * stride);
			x = p[0] / 1280.0f; y = p[1] / 720.0f;
			return x > -2.0f && x < 3.0f && y > -2.0f && y < 3.0f; // also rejects NaN
		}
		return false;
	}

	// One dump line per draw; draws made of several glyphs / quads also get one "run" line per row of neighbouring pieces,
	// so it shows where every string of a text batch is.
	void dumpHudDraw(const char* api, const char* state, const char* tex, int grp, UINT prims, UINT stride, UINT first, UINT nv, const void* data, UINT piece = 0) {
		char buf[600];
		float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
		const UINT n = nv > 8192 ? 8192 : nv;
		if (data && n > 0 && (stride == 12 || stride == 24)) {
			x0 = y0 = 1e9f; x1 = y1 = -1e9f;
			for (UINT i = first; i < first + n; ++i) {
				float x, y;
				if (!dumpVertexPos(data, stride, i, x, y)) continue;
				if (x < x0) x0 = x;
				if (x > x1) x1 = x;
				if (y < y0) y0 = y;
				if (y > y1) y1 = y;
			}
			if (x0 > x1) { x0 = y0 = x1 = y1 = 0.0f; }
		}
		const INT16* v = (const INT16*)data;
		INT16 r[6] = { 0, 0, 0, 0, 0, 0 };
		float f[3] = { 0.0f, 0.0f, 0.0f };
		if (v && stride >= 12) { for (int i = 0; i < 6; ++i) r[i] = v[i]; memcpy(f, data, sizeof(f)); }
		DWORD tail[3] = { 0, 0, 0 };
		if (v && stride >= 24) memcpy(tail, (const BYTE*)data + 12, sizeof(tail));
		sprintf_s(buf, sizeof(buf), "%s %s phase=%d tex=%s grp=%d prims=%u stride=%u nv=%u bbox=[%.3f %.3f %.3f %.3f] raw16=[%d %d %d %d %d %d] f32=[%g %g %g] tail=[%08lx %08lx %08lx]",
			api, state, g_hudScalePhase ? 1 : 0, tex, grp, prims, stride, nv, x0, y0, x1, y1, r[0], r[1], r[2], r[3], r[4], r[5], f[0], f[1], f[2], tail[0], tail[1], tail[2]);
		g_dumpLines.push_back(buf);

		// pieces: 6 vertices per glyph for text triangle lists, 4 per quad otherwise
		if (!piece) piece = 4;
		if (!data || n < piece * 2 || (stride != 12 && stride != 24)) return;
		const UINT pieces = n / piece;
		float rx0 = 0, ry0 = 0, rx1 = 0, ry1 = 0;
		UINT rcount = 0, runs = 0;
		auto flush = [&]() {
			if (!rcount) return;
			if (runs < 200) {
				char rb[160];
				sprintf_s(rb, sizeof(rb), "    run %u: [%.3f %.3f %.3f %.3f] pieces=%u", runs, rx0, ry0, rx1, ry1, rcount);
				g_dumpLines.push_back(rb);
			}
			++runs;
			rcount = 0;
			};
		for (UINT k = 0; k < pieces; ++k) {
			float px0 = 1e9f, py0 = 1e9f, px1 = -1e9f, py1 = -1e9f;
			bool ok = true;
			for (UINT j = 0; j < piece; ++j) {
				float x, y;
				if (!dumpVertexPos(data, stride, first + k * piece + j, x, y)) { ok = false; break; }
				if (x < px0) px0 = x;
				if (x > px1) px1 = x;
				if (y < py0) py0 = y;
				if (y > py1) py1 = y;
			}
			if (!ok) continue;
			if (rcount) {
				// same row (vertical overlap) and close to the previous piece -> same run
				const float ph = py1 - py0, rh = ry1 - ry0;
				const float overlap = (py1 < ry1 ? py1 : ry1) - (py0 > ry0 ? py0 : ry0);
				const float gap = px0 - rx1;
				const float lim = (ph > rh ? ph : rh);
				if (overlap > 0.5f * (ph < rh ? ph : rh) && gap < 1.5f * lim && gap > -lim) {
					if (px0 < rx0) rx0 = px0;
					if (px1 > rx1) rx1 = px1;
					if (py0 < ry0) ry0 = py0;
					if (py1 > ry1) ry1 = py1;
					++rcount;
					continue;
				}
				flush();
			}
			rx0 = px0; ry0 = py0; rx1 = px1; ry1 = py1;
			rcount = 1;
		}
		flush();
		if (runs > 200) {
			char rb[64];
			sprintf_s(rb, sizeof(rb), "    (%u more runs not listed)", runs - 200);
			g_dumpLines.push_back(rb);
		}
	}

	// "Name/WxH/pointer" for the dump, so textures the code has no name for can still be told apart
	std::string dumpTexInfo(IDirect3DBaseTexture9* t, const char* name) {
		char b[96];
		UINT w = 0, h = 0;
		if (t && t->GetType() == D3DRTYPE_TEXTURE) {
			D3DSURFACE_DESC d;
			if (SUCCEEDED(((IDirect3DTexture9*)t)->GetLevelDesc(0, &d))) { w = d.Width; h = d.Height; }
		}
		sprintf_s(b, sizeof(b), "%s/%ux%u/%p", name ? name : "none", w, h, (void*)t);
		return b;
	}


	// ---- window scaling (main menu, quick menu, list windows, prompts / popups, subtitles) ----
	// Only active with enableHudMod and only in frames where the HUD window was started (in game; the title screen menus are
	// left alone). Each window starts with one background quad; when it is seen (after the HUD, in draw order) the window
	// is active for the rest of the frame. The main menu then owns every later UI draw (its "View" explanation darkens
	// the screen with huge quads around the highlighted line, so it cannot be limited to a rectangle); the other windows own
	// the later UI draws whose centre lies inside their rectangle. Owned draws are drawn directly with their vertices moved
	// toward the window's anchor: the pieces and the game's own font are rasterized smaller (no buffer, no resampling of an
	// image, no viewport clipping). Window pieces are never scaled as HUD pieces.
	enum WinKind { WK_PANEL = 0, WK_QUICK, WK_LIST, WK_POPUP, WK_PROMPT, WK_DIALOG, WK_COUNT };
	struct WinState { bool active; float r[4]; float ax, ay; unsigned order; };
	WinState g_win[WK_COUNT] = {};
	unsigned g_winOrder = 0; // windows detected so far in this frame: a later window is drawn on top of an earlier one
	const char* const kWinNames[WK_COUNT] = { "menu", "quick menu (unused)", "list window", "popup", "prompt", "quantity dialog" };
	const float kMenuAnchorX = 0.977f, kMenuAnchorY = 0.019f;   // top-right corner of the main menu
	const float kPromptAnchorX = 0.5f, kPromptAnchorY = 0.90f;  // bottom centre of the prompt / popup area
	const float kDialogAnchorX = 0.5f, kDialogAnchorY = 0.5f;   // middle of the screen: the quantity dialog stays centred

	void resetWindows() {
		for (int k = 0; k < WK_COUNT; ++k) g_win[k].active = false;
		g_winOrder = 0;
	}

	// An opaque quad covering the whole screen (the black background of the loading screen after "Quit Game") ends all
	// windows for the rest of the frame: what is drawn after it belongs to the next screen. Without this the loading
	// screen's item window was shrunk while the menu was still fading out in the corner, then popped back to full size.
	// A see-through full-screen layer counts too while a window is open; with no window open (the fade at the end of the
	// "BONFIRE LIT" banner, about 20 % opaque) it is ignored, so the prompt drawn after it is still a prompt.
	bool g_screenCovered = false;
	bool isWholeScreen(const float* b) {
		return b[0] > -0.01f && b[1] > -0.01f && b[2] < 1.01f && b[3] < 1.01f && (b[2] - b[0]) > 0.97f && (b[3] - b[1]) > 0.97f;
	}
	bool anyWindowActive() { for (int k = 0; k < WK_COUNT; ++k) if (g_win[k].active) return true; return false; }

	float clampScale(float s) {
		if (!(s < 1.0f)) return 1.0f; // also NaN
		return s < 0.5f ? 0.5f : s;
	}
	float windowScale(int k) {
		if (k == WK_PANEL || k == WK_QUICK) return clampScale(Settings::get().getMenuScale());
		if (k == WK_LIST) return clampScale(Settings::get().getListWindowScale());
		if (k == WK_POPUP || k == WK_PROMPT) return clampScale(Settings::get().getPromptScale());
		if (k == WK_DIALOG) return clampScale(Settings::get().getMenuScale());
		return 1.0f;
	}

	void activateWindow(int k, float x0, float y0, float x1, float y1, float ax, float ay) {
		WinState& w = g_win[k];
		w.active = true;
		w.r[0] = x0; w.r[1] = y0; w.r[2] = x1; w.r[3] = y1;
		w.ax = ax; w.ay = ay;
		w.order = ++g_winOrder;
		if (g_dumpActive) {
			char b[160];
			sprintf_s(b, sizeof(b), "--- WINDOW DETECTED: %s [%.3f %.3f %.3f %.3f] anchor %.3f %.3f scale %.2f ---", kWinNames[k], x0, y0, x1, y1, ax, ay, windowScale(k));
			g_dumpLines.push_back(b);
		}
	}

	bool texSize(IDirect3DBaseTexture9* tex, UINT& w, UINT& h) {
		w = h = 0;
		if (!tex || tex->GetType() != D3DRTYPE_TEXTURE) return false;
		D3DSURFACE_DESC d;
		if (FAILED(((IDirect3DTexture9*)tex)->GetLevelDesc(0, &d))) return false;
		w = d.Width; h = d.Height;
		return true;
	}

	// A popup or prompt can start only once per frame, except when a list window was drawn after it: then a new one drawn
	// on top of the list is a second window of that kind (an item pickup popup left open under the bonfire menu, then the
	// "Warp to selected bonfire" question over the warp list). Popups and prompts share scale and anchor, so taking over
	// the slot is fine: the earlier one is completely drawn by then.
	bool canStartAgain(int k) {
		if (!g_win[k].active) return true;
		return g_win[WK_LIST].active && g_win[WK_LIST].order > g_win[k].order;
	}

	// Called for single INT16 quads drawn after the HUD. 'b' = normalized bounding box. 'healthbarTex' / 'guiTex': the quad
	// uses the HudHealthbar / GuiElements1 texture.
	void detectWindow(const float* b, IDirect3DBaseTexture9* tex, bool healthbarTex, bool guiTex, bool hudDone) {
		if (g_screenCovered) return;          // the next screen (loading screen) is being drawn
		if (g_win[WK_PANEL].active) return; // inside the main menu everything belongs to the menu
		UINT tw, th;
		if (!texSize(tex, tw, th)) return;
		const float w = b[2] - b[0], h = b[3] - b[1], cx = (b[0] + b[2]) * 0.5f, cy = (b[1] + b[3]) * 0.5f;
		// Quick menu (Start), top-right corner: backdrop [0.647 0.072 0.919 0.247] (HudHealthbar texture), frame lines
		// (GuiElements1), icon tiles (512x256 menu icon texture). It is drawn first in every menu frame, so from its first
		// piece on everything belongs to the menu, also while the quick menu itself grows in and while the main menu grows
		// out of it (the pieces do not have their final size and place yet then; the menu used to pop to the scaled size).
		// Any of its pieces counts, at any size, as long as it is in the top-right corner (enemy health bars, the only
		// other HudHealthbar quads, are thin and drawn inside the HUD window).
		if (cx > 0.60f && cx < 1.35f && cy > -0.30f && cy < 0.32f && w < 0.60f && h < 0.40f &&
			(guiTex || (tw == 512 && th == 256) ||
				(healthbarTex && (hudDone ? (w > 0.05f && h > 0.03f)
					// inside the HUD only the exact backdrop (HUD pieces use this texture too)
					: (b[0] > 0.63f && b[0] < 0.67f && b[1] > 0.055f && b[1] < 0.09f && b[2] > 0.90f && b[2] < 0.94f && b[3] > 0.23f && b[3] < 0.27f))))) {
			activateWindow(WK_PANEL, 0.0f, 0.0f, 1.0f, 1.0f, kMenuAnchorX, kMenuAnchorY);
			return;
		}
		// The quantity dialog in the middle of the screen: the number picker the game shows when it asks "how many?"
		// (discarding a stack, and with bulkItemUse also using several items at once). It is drawn as a dark panel
		// [0.266 0.292 0.735 0.708] first, then a frame on top [0.282 0.319 0.709 0.694], then the up/down arrows, the
		// number and the OK / CANCEL texts. The panel comes first and must already belong to the window, otherwise it
		// is drawn at full size and only its contents shrink. So this is matched on any big box in the middle of the
		// screen, not on one texture, and before the 1024x1024 test below (the frame is on a 1024x512 texture).
		// It shrinks toward the middle of the screen so it stays centred.
		if (hudDone && canStartAgain(WK_DIALOG) &&
			cx > 0.46f && cx < 0.54f && cy > 0.45f && cy < 0.55f &&
			w > 0.38f && w < 0.60f && h > 0.33f && h < 0.52f) {
			activateWindow(WK_DIALOG, b[0] - 0.03f, b[1] - 0.03f, b[2] + 0.03f, b[3] + 0.03f, kDialogAnchorX, kDialogAnchorY);
			return;
		}

		// the other windows are only ever drawn after the HUD
		if (!hudDone || tw != 1024 || th != 1024) return;
		// main menu, left background piece: [0.028 0.022 0.827 0.992] (Inventory, Status, Level Up...) or [0.028 0.021 0.827 0.293] (System)
		if (b[0] > 0.015f && b[0] < 0.045f && b[1] > 0.005f && b[1] < 0.04f && b[2] > 0.80f && b[2] < 0.85f && h > 0.2f) {
			activateWindow(WK_PANEL, 0.0f, 0.0f, 1.0f, 1.0f, kMenuAnchorX, kMenuAnchorY);
			return;
		}
		// gesture menu (Back / Select button), a wide window along the top: drawn after the HUD as two background halves,
		// left [0.048 0.036 0.479 0.319] and right [0.525 0.036 0.956 0.319]. Like the main menu it owns every later UI
		// draw of the frame (portrait, gesture figures, texts, button hints) and shrinks with menuScale, toward its own
		// top-left corner. Without this its pieces were taken for HUD pieces (top-left corner, hudScaleTopLeft) until a
		// piece on the right started the main menu (top-right corner): the window came apart in two halves.
		if (b[0] > 0.035f && b[0] < 0.065f && b[1] > 0.02f && b[1] < 0.055f && b[3] > 0.29f && b[3] < 0.35f && w > 0.40f && w < 0.46f) {
			activateWindow(WK_PANEL, 0.0f, 0.0f, 1.0f, 1.0f, b[0], b[1]);
			return;
		}
		// list window on the left: bonfire menu [0.058 0.155 0.433 0.878]
		if (!g_win[WK_LIST].active && b[0] > 0.03f && b[0] < 0.10f && w > 0.30f && w < 0.45f && h > 0.50f && h < 0.80f) {
			activateWindow(WK_LIST, b[0] - 0.01f, b[1] - 0.01f, b[2] + 0.01f, b[3] + 0.01f, b[0], b[1]);
			return;
		}
		// interaction prompt [0.258 0.782 0.743 0.846] (+ a second piece below, to 0.887) and the yes/no question [0.259 0.782 0.741 0.922]
		if (canStartAgain(WK_PROMPT) && cx > 0.45f && cx < 0.55f && w > 0.40f && w < 0.60f && h > 0.035f && h < 0.20f && b[1] > 0.70f && b[1] < 0.85f) {
			const float y1 = (b[3] > b[1] + 0.105f) ? b[3] : b[1] + 0.105f;
			activateWindow(WK_PROMPT, b[0] - 0.01f, b[1] - 0.01f, b[2] + 0.01f, y1 + 0.01f, kPromptAnchorX, kPromptAnchorY);
			return;
		}
		// item popup above the prompt, left piece [0.277 0.643 0.641 0.785] (taller with several items)
		if (canStartAgain(WK_POPUP) && b[0] > 0.20f && b[0] < 0.35f && w > 0.25f && w < 0.50f && h > 0.08f && h < 0.45f && b[1] > 0.30f && b[1] < 0.75f && b[3] < 0.83f) {
			activateWindow(WK_POPUP, 0.20f, b[1] - 0.02f, 0.80f, b[3] + 0.015f, kPromptAnchorX, kPromptAnchorY);
			return;
		}
	}

	// The window a UI draw belongs to (-1 = none). 'b' = normalized bounding box of the draw.
	int windowFor(const float* b, bool hudDone, bool opaque) {
		if (isWholeScreen(b)) {
			// A see-through fade with no window open yet (the "BONFIRE LIT" banner ending) is ignored, so a prompt drawn
			// after it is still detected. With a window open (the menu while "Quit Game" fades to the loading screen)
			// any full-screen layer ends the windows, as before.
			if (!opaque && !anyWindowActive()) return -1;
			if (anyWindowActive()) {
				resetWindows();
				if (g_dumpActive) g_dumpLines.push_back("--- WHOLE SCREEN COVERED: windows ended for this frame ---");
			}
			g_screenCovered = true;
			return -1;
		}
		if (g_screenCovered) return -1;
		if (g_win[WK_PANEL].active && !hudDone) {
			// The quick menu was drawn before the HUD was finished (the first frame after pressing Start can do that):
			// only its own corner belongs to the menu, the HUD pieces drawn after it are left to the HUD code.
			const float cx = (b[0] + b[2]) * 0.5f, cy = (b[1] + b[3]) * 0.5f;
			return (cx > 0.60f && cx < 1.35f && cy > -0.30f && cy < 0.32f && (b[2] - b[0]) < 0.6f) ? WK_PANEL : -1;
		}
		if (!hudDone) return -1;
		if (g_win[WK_PANEL].active) return WK_PANEL;
		if ((b[2] - b[0]) > 0.97f && (b[3] - b[1]) > 0.97f) return -1;
		const float cx = (b[0] + b[2]) * 0.5f, cy = (b[1] + b[3]) * 0.5f;
		// Where windows overlap, the one drawn on top wins, i.e. the one detected later in the frame. A popup or prompt
		// drawn over a list window (the bonfire's "Warp to selected bonfire" confirmation over the warp list) wins over the
		// list: its left half and the OK button used to be shrunk with the list. But an item pickup popup that is still open
		// when the bonfire menu opens is drawn first, under the list: there the list wins, otherwise the list's lower pieces
		// (its highlight bar on the last entries, its bottom edge) were moved with the popup. (Popup and prompt share scale
		// and anchor, so their order between themselves does not matter.)
		int best = -1;
		for (int k = WK_QUICK; k < WK_COUNT; ++k) {
			const WinState& win = g_win[k];
			if (win.active && cx >= win.r[0] && cx <= win.r[2] && cy >= win.r[1] && cy <= win.r[3] &&
				(best < 0 || win.order > g_win[best].order)) best = k;
		}
		return best;
	}

	INT16 toInt16(float v) {
		if (v < -32768.0f) return -32768;
		if (v > 32767.0f) return 32767;
		return (INT16)(v < 0.0f ? v - 0.5f : v + 0.5f);
	}

	// Copies the vertices [0, first + count) into 'out' and moves [first, first + count) toward the anchor (ax, ay, normalized)
	// by s. INT16 quads (stride 12) live in the 10240x5760 space, text (stride 24) in 1280x720 UI pixels.
	const void* scaleUiVertices(std::vector<BYTE>& out, const void* data, UINT stride, UINT first, UINT count, float s, float ax, float ay) {
		if (!data || s >= 1.0f || count == 0 || count > 65536 || (stride != 12 && stride != 24)) return data;
		const size_t total = (size_t)(first + count) * stride;
		out.assign((const BYTE*)data, (const BYTE*)data + total);
		if (stride == 12) {
			const float axv = ax * kUiW, ayv = ay * kUiH;
			for (UINT i = first; i < first + count; ++i) {
				INT16* p = (INT16*)(&out[0] + (size_t)i * stride);
				p[0] = toInt16(axv + (p[0] - axv) * s);
				p[1] = toInt16(ayv + (p[1] - ayv) * s);
			}
		}
		else {
			const float axp = ax * 1280.0f, ayp = ay * 720.0f;
			for (UINT i = first; i < first + count; ++i) {
				float* p = (float*)(&out[0] + (size_t)i * stride);
				p[0] = axp + (p[0] - axp) * s;
				p[1] = ayp + (p[1] - ayp) * s;
			}
		}
		return &out[0];
	}

	// If the draw uses a scissor rectangle (lists that scroll), it is moved and shrunk the same way.
	// Returns true if it was changed; the caller restores 'saved' after the draw.
	bool scaleWindowScissor(IDirect3DDevice9* dev, float s, float ax, float ay, RECT& saved) {
		if (!dev || s >= 1.0f) return false;
		DWORD sc = FALSE;
		if (FAILED(dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &sc)) || !sc) return false;
		D3DVIEWPORT9 vp;
		if (FAILED(dev->GetViewport(&vp)) || FAILED(dev->GetScissorRect(&saved))) return false;
		const float axp = vp.X + ax * vp.Width, ayp = vp.Y + ay * vp.Height;
		RECT r;
		r.left = (LONG)floorf(axp + (saved.left - axp) * s);
		r.top = (LONG)floorf(ayp + (saved.top - ayp) * s);
		r.right = (LONG)ceilf(axp + (saved.right - axp) * s);
		r.bottom = (LONG)ceilf(ayp + (saved.bottom - ayp) * s);
		return SUCCEEDED(dev->SetScissorRect(&r));
	}

	// ---- swapConfirmIcons / swapXYIcons: two button icons swap places in the button texture (ButtonsEffects).
	// The face buttons are cells of one row (v = 0): Y / triangle u = 0, X / square 56/1024, B / circle 112/1024,
	// A / cross 168/1024 (found in the menu hints, e.g. "A:Enter B:Close X:Unequip Item Y:Toggle Status").
	// A glyph is drawn as one quad (6 vertices: float x y z, u v, colour). Glyphs whose cell is one of a swapped pair
	// are moved to the other cell of the pair. Only the texture coordinates change, so a replaced texture
	// (PlayStation icons from tex_override) is swapped just the same.
	const float kIconYU = 0.0f;         // Y / triangle
	const float kIconXU = 0.0546875f;   // X / square
	const float kIconBU = 0.109375f;    // B / circle
	const float kIconAU = 0.1640625f;   // A / cross
	const float kIconRowV = 0.0f;
	void swapButtonIcons(BYTE* verts, UINT count, D3DPRIMITIVETYPE type, bool swapAB, bool swapXY) {
		const UINT per = (type == D3DPT_TRIANGLELIST) ? 6 : ((type == D3DPT_TRIANGLESTRIP || type == D3DPT_TRIANGLEFAN) ? 4 : 0);
		if (!per || count < per) return;
		for (UINT g = 0; g + per <= count; g += per) {
			float minU = 1e9f, maxU = -1e9f, minV = 1e9f;
			for (UINT i = g; i < g + per; ++i) {
				const float* uv = (const float*)(verts + (size_t)i * 24 + 12);
				if (!(uv[0] > -0.01f && uv[0] < 1.01f && uv[1] > -0.01f && uv[1] < 1.01f)) { minU = 1e9f; break; } // not a UV pair
				if (uv[0] < minU) minU = uv[0];
				if (uv[0] > maxU) maxU = uv[0];
				if (uv[1] < minV) minV = uv[1];
			}
			if (minU > 1.0f || maxU - minU > 0.06f || fabsf(minV - kIconRowV) > 0.0005f) continue;
			float delta = 0.0f;
			if (swapAB && fabsf(minU - kIconAU) < 0.0005f) delta = kIconBU - kIconAU;
			else if (swapAB && fabsf(minU - kIconBU) < 0.0005f) delta = kIconAU - kIconBU;
			else if (swapXY && fabsf(minU - kIconXU) < 0.0005f) delta = kIconYU - kIconXU;
			else if (swapXY && fabsf(minU - kIconYU) < 0.0005f) delta = kIconXU - kIconYU;
			if (delta == 0.0f) continue;
			for (UINT i = g; i < g + per; ++i) ((float*)(verts + (size_t)i * 24 + 12))[0] += delta;
		}
	}

	// Subtitles (NPC dialogue, Text00 font, lower part of the screen) shrink toward the bottom centre.
	float subtitleScale() { return clampScale(Settings::get().getSubtitleScale()); }

	// ---- mouse: the game reads the cursor with GetCursorPos (hooked in main.cpp). While a shrunk main menu or list window
	// is open, the cursor position is moved back to where that spot was before the shrink, so hovering and clicking hit
	// what is drawn under the cursor. Written on the render thread at every Present, read on the game's thread.
	struct MouseMapData {
		bool panel, list, pop, popOnTop;
		float pax, pay, ps;           // main menu anchor and scale
		float lax, lay, ls, lr[4];    // list window anchor, scale and (unscaled) rectangle
		float oax, oay, os, orr[4];   // popup / prompt open over the list window: anchor, scale and (unscaled) rectangle
		LONG cx, cy, cw, ch;          // client area of the game window, screen coordinates
	};
	volatile LONG g_mouseSeq = 0;     // odd while being written
	MouseMapData g_mouseMap = {};

	void publishMouseMap(IDirect3DDevice9* dev) {
		MouseMapData m = {};
		m.panel = g_win[WK_PANEL].active && windowScale(WK_PANEL) < 1.0f;
		m.list = !m.panel && g_win[WK_LIST].active && windowScale(WK_LIST) < 1.0f;
		if (m.panel || m.list) {
			HWND hwnd = NULL;
			D3DDEVICE_CREATION_PARAMETERS cp;
			if (dev && SUCCEEDED(dev->GetCreationParameters(&cp))) hwnd = cp.hFocusWindow;
			RECT rc;
			POINT o = { 0, 0 };
			if (!hwnd || !::GetClientRect(hwnd, &rc) || !::ClientToScreen(hwnd, &o) || rc.right <= 0 || rc.bottom <= 0) {
				m.panel = m.list = false;
			}
			else {
				m.cx = o.x; m.cy = o.y; m.cw = rc.right; m.ch = rc.bottom;
				m.pax = g_win[WK_PANEL].ax; m.pay = g_win[WK_PANEL].ay; m.ps = windowScale(WK_PANEL);
				m.lax = g_win[WK_LIST].ax; m.lay = g_win[WK_LIST].ay; m.ls = windowScale(WK_LIST);
				for (int i = 0; i < 4; ++i) m.lr[i] = g_win[WK_LIST].r[i];
				// a confirmation popup over the list window: the cursor over it must not be moved as if it were over the list
				if (m.list && (g_win[WK_POPUP].active || g_win[WK_PROMPT].active)) {
					m.pop = true;
					m.popOnTop = false;
					m.orr[0] = m.orr[1] = 1e9f; m.orr[2] = m.orr[3] = -1e9f;
					for (int k = WK_POPUP; k <= WK_PROMPT; ++k) {
						if (!g_win[k].active) continue;
						if (g_win[k].order > g_win[WK_LIST].order) m.popOnTop = true; // drawn over the list
						m.oax = g_win[k].ax; m.oay = g_win[k].ay; m.os = windowScale(k);
						if (g_win[k].r[0] < m.orr[0]) m.orr[0] = g_win[k].r[0];
						if (g_win[k].r[1] < m.orr[1]) m.orr[1] = g_win[k].r[1];
						if (g_win[k].r[2] > m.orr[2]) m.orr[2] = g_win[k].r[2];
						if (g_win[k].r[3] > m.orr[3]) m.orr[3] = g_win[k].r[3];
					}
				}
			}
		}
		if (!m.panel && !m.list && !g_mouseMap.panel && !g_mouseMap.list) return; // nothing to change
		InterlockedIncrement(&g_mouseSeq);
		g_mouseMap = m;
		InterlockedIncrement(&g_mouseSeq);
	}
}

// Called from the GetCursorPos hook (main.cpp) with the real cursor position in screen coordinates.
void remapMenuCursor(POINT* p) {
	if (!p) return;
	MouseMapData m = {};
	LONG a = 0, b = 1;
	for (int tries = 0; tries < 4 && (a != b || (a & 1)); ++tries) {
		a = g_mouseSeq;
		MemoryBarrier();
		m = g_mouseMap;
		MemoryBarrier();
		b = g_mouseSeq;
	}
	if (a != b || (a & 1) || (!m.panel && !m.list) || m.cw <= 0 || m.ch <= 0) return;
	// screen -> normalized UI coordinates
	float u = (float)(p->x - m.cx) / m.cw;
	float v = (float)(p->y - m.cy) / m.ch;
	if (m.panel) {
		u = m.pax + (u - m.pax) / m.ps;
		v = m.pay + (v - m.pay) / m.ps;
	}
	else {
		// the shrunk list window
		const float x0 = m.lax + (m.lr[0] - m.lax) * m.ls, x1 = m.lax + (m.lr[2] - m.lax) * m.ls;
		const float y0 = m.lay + (m.lr[1] - m.lay) * m.ls, y1 = m.lay + (m.lr[3] - m.lay) * m.ls;
		const bool overList = !(u < x0 || u > x1 || v < y0 || v > y1);
		// a popup over the list window: over the (shrunk) popup the cursor follows the popup, not the list. A popup under
		// the list (an item pickup popup left open when the bonfire menu opened) only where the list does not cover it.
		if (m.pop && (m.popOnTop || !overList)) {
			const float px0 = m.oax + (m.orr[0] - m.oax) * m.os, px1 = m.oax + (m.orr[2] - m.oax) * m.os;
			const float py0 = m.oay + (m.orr[1] - m.oay) * m.os, py1 = m.oay + (m.orr[3] - m.oay) * m.os;
			if (u >= px0 && u <= px1 && v >= py0 && v <= py1) {
				if (m.os >= 1.0f) return; // popup not shrunk: the real position is right
				u = m.oax + (u - m.oax) / m.os;
				v = m.oay + (v - m.oay) / m.os;
				p->x = m.cx + (LONG)floorf(u * m.cw + 0.5f);
				p->y = m.cy + (LONG)floorf(v * m.ch + 0.5f);
				return;
			}
		}
		// only over the shrunk list window itself
		if (!overList) return;
		u = m.lax + (u - m.lax) / m.ls;
		v = m.lay + (v - m.lay) / m.ls;
	}
	// normalized UI -> screen
	p->x = m.cx + (LONG)floorf(u * m.cw + 0.5f);
	p->y = m.cy + (LONG)floorf(v * m.ch + 0.5f);
}

namespace {

}

// called from the hotkey action; the whole next frame (all UI draws, with or without the HUD) is written to hud_dump.txt
void requestHudDump() {
	if (g_recArmFrames > 0) { g_recWrite = true; return; } // second press: write what the recorder kept
	g_dumpRequested = true;
	g_rtDumpFrames = 8;
	g_menuCapArmFrames = 1500;
	g_menuCapLeft = 0;
	g_recArmFrames = 3600;
	g_recRing.clear();
	g_recNext = 0;
}

extern bool g_Force30FPS;

RSManager RSManager::instance;

static const char* PIXEL_SHADER_DUMP_DIR = "dsfix/pixelshader_dump";
static const char* PIXEL_SHADER_OVERRIDE_DIR = "dsfix/pixelshader_override";
static const char* VERTEX_SHADER_DUMP_DIR = "dsfix/vertexshader_dump";
static const char* VERTEX_SHADER_OVERRIDE_DIR = "dsfix/vertexshader_override";

// ============================================================================================
// sharpenStrength: contrast-adaptive sharpening (the AMD FidelityFX CAS formula, as in ReShade's CAS.fx).
// FXAA / SMAA soften the image a little; this pass brings fine detail back. Edges that are already sharp and strong
// contrasts get less sharpening than flat detail, so it does not draw bright halos. It runs on the finished 3D image
// right after AA and before SSAO, the HUD and the menus, so text and UI are never sharpened. 0 (default) = off: no
// shader is even created. The shader is built into the DLL, no extra file is needed.
// ============================================================================================
namespace {
	const char* kSharpenFx = R"FX(
texture frameTex2D;
sampler frameSampler = sampler_state {
	Texture = <frameTex2D>;
	AddressU = CLAMP; AddressV = CLAMP;
	MinFilter = POINT; MagFilter = POINT; MipFilter = NONE;
	SRGBTexture = FALSE;
};
float2 pixelSize;
float sharpness;

struct VSOUT { float4 pos : POSITION; float2 uv : TEXCOORD0; };
VSOUT SharpenVS(float3 pos : POSITION, float2 uv : TEXCOORD0) {
	VSOUT o;
	o.pos = float4(pos, 1.0);
	o.uv = uv;
	return o;
}
float3 px(float2 uv, float dx, float dy) {
	return tex2Dlod(frameSampler, float4(uv + float2(dx, dy) * pixelSize, 0.0, 0.0)).rgb;
}
float4 SharpenPS(float2 uv : TEXCOORD0) : COLOR0 {
	float3 a = px(uv, -1.0, -1.0), b = px(uv, 0.0, -1.0), c = px(uv, 1.0, -1.0);
	float3 d = px(uv, -1.0,  0.0), e = px(uv, 0.0,  0.0), f = px(uv, 1.0,  0.0);
	float3 g = px(uv, -1.0,  1.0), h = px(uv, 0.0,  1.0), i = px(uv, 1.0,  1.0);
	// soft minimum and maximum of the neighbourhood (cross + full 3x3)
	float3 mn = min(min(min(d, e), min(f, b)), h);
	float3 mn2 = min(mn, min(min(a, c), min(g, i)));
	mn += mn2;
	float3 mx = max(max(max(d, e), max(f, b)), h);
	float3 mx2 = max(mx, max(max(a, c), max(g, i)));
	mx += mx2;
	// amount: high in flat areas, low where the contrast is already high
	float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 0.00001)));
	float peak = -1.0 / lerp(8.0, 5.0, saturate(sharpness));
	float3 w = amp * peak;
	float3 o = (b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w);
	return float4(saturate(o), 1.0);
}
technique Sharpen {
	pass P0 {
		VertexShader = compile vs_3_0 SharpenVS();
		PixelShader = compile ps_3_0 SharpenPS();
	}
}
)FX";

	class SharpenPass : public Effect {
	public:
		SharpenPass(IDirect3DDevice9* dev, int w, int h) : Effect(dev), width(w), height(h), effect(NULL),
			texHandle(NULL), pixelHandle(NULL), sharpHandle(NULL) {
			ID3DXBuffer* errors = NULL;
			const DWORD flags = D3DXFX_NOT_CLONEABLE | D3DXSHADER_OPTIMIZATION_LEVEL3;
			HRESULT hr = D3DXCreateEffect(dev, kSharpenFx, (UINT)strlen(kSharpenFx), NULL, NULL, flags, NULL, &effect, &errors);
			if (FAILED(hr) || !effect) {
				SDLOG(0, "Sharpening: shader could not be created (error %08X) %s\n", (unsigned)hr,
					errors ? (const char*)errors->GetBufferPointer() : "");
				effect = NULL;
			}
			else {
				texHandle = effect->GetParameterByName(NULL, "frameTex2D");
				pixelHandle = effect->GetParameterByName(NULL, "pixelSize");
				sharpHandle = effect->GetParameterByName(NULL, "sharpness");
			}
			SAFERELEASE(errors);
		}
		virtual ~SharpenPass() { SAFERELEASE(effect); }
		bool ok() const { return effect != NULL && texHandle && pixelHandle && sharpHandle; }

		void go(IDirect3DTexture9* src, IDirect3DSurface9* dst, float amount) {
			device->SetVertexDeclaration(vertexDeclaration);
			device->SetRenderTarget(0, dst);
			const float px[2] = { 1.0f / (float)width, 1.0f / (float)height };
			effect->SetTexture(texHandle, src);
			effect->SetFloatArray(pixelHandle, px, 2);
			effect->SetFloat(sharpHandle, amount);
			UINT passes = 0;
			effect->Begin(&passes, 0);
			effect->BeginPass(0);
			quad(width, height);
			effect->EndPass();
			effect->End();
		}

	private:
		int width, height;
		ID3DXEffect* effect;
		D3DXHANDLE texHandle, pixelHandle, sharpHandle;
	};

	SharpenPass* g_sharpen = NULL;
	bool g_doSharpen = true; // toggleSharpen hotkey

	// sharpenStrength clamped to 0..1 (0 = off)
	float sharpenAmount() {
		const float s = Settings::get().getSharpenStrength();
		if (!(s > 0.0f)) return 0.0f;
		return s > 1.0f ? 1.0f : s;
	}
}

// hotkey action (Actions.def: toggleSharpen), to compare with and without sharpening
void toggleSharpenPass() {
	g_doSharpen = !g_doSharpen;
	SDLOG(0, "Sharpening %s\n", g_doSharpen ? "on" : "off");
}

// ---- HUD buffer ----
// The game draws the HUD and the menus into its "1280x720" image, which DSfix creates at the present resolution
// (presentWidth / presentHeight). The HUD buffer must have that size too. When the present resolution is the render
// resolution (the usual case) it is rgbaBuffer1 itself, exactly as before. When they differ (render 3840x2160,
// present 1920x1080: downsampling), the HUD image was never recognised (it was compared with the render size), so the
// HUD and menu scaling did nothing; now it gets its own buffer at the present size.
namespace {
	IDirect3DTexture9* g_hudBufTex = NULL;
	IDirect3DSurface9* g_hudBufSurf = NULL;
	bool g_hudBufOwn = false; // true: a separate buffer (present size != render size), released with the resources
	bool isHudTargetSize(const D3DSURFACE_DESC& d) {
		return d.Width == Settings::get().getPresentWidth() && d.Height == Settings::get().getPresentHeight();
	}
}

void RSManager::initResources() {
	SDLOG(0, "RenderstateManager resource initialization started\n");
	unsigned rw = Settings::get().getRenderWidth(), rh = Settings::get().getRenderHeight();
	unsigned dofW, dofH;
	dofBufferSize(dofW, dofH);
	const int aaIdx = aaQualityIndex();
	const int ssaoIdx = ssaoStrengthIndex();

	// DSfix 3.0: every buffer is checked. If video memory runs short (e.g. right after Alt+Tab), the effect that needs the
	// missing buffer is simply skipped for this session instead of crashing the game on a NULL pointer.
	auto createRT = [&](unsigned w, unsigned h, IDirect3DTexture9** tex, IDirect3DSurface9** surf, const char* name) -> bool {
		*tex = NULL;
		*surf = NULL;
		HRESULT hr = d3ddev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, tex, NULL);
		if (SUCCEEDED(hr) && *tex) hr = (*tex)->GetSurfaceLevel(0, surf);
		if (FAILED(hr) || !*tex || !*surf) {
			SDLOG(0, "WARNING: could not create %s (%ux%u), error %08X\n", name, w, h, (unsigned)hr);
			SAFERELEASE(*surf);
			SAFERELEASE(*tex);
			return false;
		}
		return true;
		};

	// Core resources used by every effect and by the HUD mod: intermediate buffer, depth buffer, state block
	const bool haveRgba = createRT(rw, rh, &rgbaBuffer1Tex, &rgbaBuffer1Surf, "rgbaBuffer1");
	const unsigned pw = Settings::get().getPresentWidth(), ph = Settings::get().getPresentHeight();
	g_hudBufOwn = false;
	g_hudBufTex = NULL;
	g_hudBufSurf = NULL;
	if (pw == rw && ph == rh) { g_hudBufTex = rgbaBuffer1Tex; g_hudBufSurf = rgbaBuffer1Surf; }
	else if (Settings::get().getEnableHudMod() && createRT(pw, ph, &g_hudBufTex, &g_hudBufSurf, "hudBuffer")) g_hudBufOwn = true;
	depthStencilSurf = NULL;
	HRESULT dsHr = d3ddev->CreateDepthStencilSurface(rw, rh, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, false, &depthStencilSurf, NULL);
	if (FAILED(dsHr) || !depthStencilSurf) {
		SDLOG(0, "WARNING: could not create depth/stencil surface (%ux%u), error %08X\n", rw, rh, (unsigned)dsHr);
		depthStencilSurf = NULL;
	}
	prevStateBlock = NULL;
	HRESULT sbHr = d3ddev->CreateStateBlock(D3DSBT_ALL, &prevStateBlock);
	if (FAILED(sbHr) || !prevStateBlock) {
		SDLOG(0, "WARNING: could not create state block, error %08X\n", (unsigned)sbHr);
		prevStateBlock = NULL;
	}
	const bool haveCore = haveRgba && depthStencilSurf && prevStateBlock;
	if (!haveCore) SDLOG(0, "WARNING: core resources missing - AA, SSAO, DoF blur and HUD mod disabled until the next device reset\n");

	// Shared buffer pool
	const bool wantAA = aaIdx >= 0;
	const bool wantSsao = ssaoIdx >= 0;
	const bool wantDof = Settings::get().getDOFBlurAmount() != 0;
	// Only the buffers an enabled effect actually uses are created (VRAM):
	//   sharedBuffer1: FXAA, SMAA, SSAO     sharedBuffer2: SMAA, SSAO     sharedDofBuffer: DoF blur
	const bool wantSmaa = wantAA && Settings::get().getAAType() == "SMAA";
	const bool needShared1 = wantAA || wantSsao;
	const bool needShared2 = wantSmaa || wantSsao;
	bool haveShared1 = false, haveShared2 = false, haveDof = false;
	if (haveCore) {
		if (needShared1) haveShared1 = createRT(rw, rh, &sharedBuffer1Tex, &sharedBuffer1Surf, "sharedBuffer1");
		if (needShared2) haveShared2 = createRT(rw, rh, &sharedBuffer2Tex, &sharedBuffer2Surf, "sharedBuffer2");
		if (wantDof) haveDof = createRT(dofW, dofH, &sharedDofTex, &sharedDofSurf, "sharedDofBuffer");
	}
	SDLOG(0, "Shared buffers: buffer1 %s, buffer2 %s, DoF %s\n", haveShared1 ? "created" : "not needed/failed",
		haveShared2 ? "created" : "not needed/failed", haveDof ? "created" : "not needed/failed");

	if (wantAA) {
		if (Settings::get().getAAType() == "SMAA") {
			if (haveShared1 && haveShared2) {
				SMAA::ExternalStorage storage(sharedBuffer1Tex, sharedBuffer1Surf, sharedBuffer2Tex, sharedBuffer2Surf);
				smaa = new SMAA(d3ddev, rw, rh, (SMAA::Preset)aaIdx, storage);
			}
			else SDLOG(0, "WARNING: SMAA skipped (buffers missing)\n");
		}
		else {
			if (haveShared1) fxaa = new FXAA(d3ddev, rw, rh, (FXAA::Quality)aaIdx, sharedBuffer1Tex, sharedBuffer1Surf);
			else SDLOG(0, "WARNING: FXAA skipped (buffers missing)\n");
		}
	}
	if (wantSsao) {
		if (haveShared1 && haveShared2) {
			ssao = new SSAO(d3ddev, rw, rh, (unsigned)ssaoIdx, ssaoTypeFromSettings(),
				sharedBuffer1Tex, sharedBuffer1Surf, sharedBuffer2Tex, sharedBuffer2Surf);
			if (!ssao->ok()) {
				SDLOG(0, "WARNING: SSAO effect could not be loaded - SSAO off\n");
				SAFEDELETE(ssao);
			}
		}
		else SDLOG(0, "WARNING: SSAO skipped (buffers missing)\n");
	}
	if (wantDof) {
		if (haveDof) gauss = new GAUSS(d3ddev, dofW, dofH, sharedDofTex, sharedDofSurf);
		else SDLOG(0, "WARNING: DoF blur skipped (buffers missing)\n");
	}
	if (sharpenAmount() > 0.0f) {
		if (haveCore) {
			g_sharpen = new SharpenPass(d3ddev, rw, rh);
			if (g_sharpen->ok()) {
				SDLOG(0, "Sharpening: strength %.2f\n", sharpenAmount());
			}
			else {
				SAFEDELETE(g_sharpen);
			}
		}
		else SDLOG(0, "WARNING: sharpening skipped (buffers missing)\n");
	}
	if (Settings::get().getEnableHudMod()) {
		if (haveCore && g_hudBufSurf) hud = new HUD(d3ddev, pw, ph);
		else SDLOG(0, "WARNING: HUD mod skipped (buffers missing)\n");
	}
	if (Settings::get().getEnableTextureOverride() && Settings::get().getEnableTexturePrefetch()) {
		if (!inited) { // DSfix 3.0: Only prefetch textures on the very first boot to prevent Alt+Tab RAM leaks
			prefetchTextures();
		}
	}

	if (Settings::get().getEnableShaderDumping()) {
		createDirectory(PIXEL_SHADER_DUMP_DIR);
		createDirectory(VERTEX_SHADER_DUMP_DIR);
	}

	SDLOG(0, "RenderstateManager resource initialization completed\n");
	if (!inited) startDetour(); // on first init only
	inited = true;
}

void RSManager::prefetchTextures()
{
	SDLOG(0, "Prefetch overwrite textures to memory started\n");
	double startTime = getElapsedTime();
	WIN32_FIND_DATA textureFile;
	const char* texturePath = "dsfix\\tex_override\\";
	char tmp[128];
	sprintf_s(tmp, 128, "%s*", texturePath);
	HANDLE searchHandle = FindFirstFile(tmp, &textureFile);
	if (searchHandle != INVALID_HANDLE_VALUE) {
		do {
			SDLOG(0, "initTexture: %s ", textureFile.cFileName);
			sprintf_s(tmp, 128, "%s%s", texturePath, textureFile.cFileName);
			std::FILE* fp = std::fopen(tmp, "rb");
			if (fp)
			{
				std::string filename = textureFile.cFileName;
				const size_t period_idx = filename.rfind('.');
				std::string extension = "";
				if (std::string::npos != period_idx)
				{
					extension = filename.substr(period_idx);
					filename = filename.erase(period_idx);
					SDLOG(0, "filename: %s extension: %s ", filename.c_str(), extension.c_str());
				}
				if (extension == ".bmp" ||
					extension == ".dds" ||
					extension == ".dib" ||
					extension == ".hdr" ||
					extension == ".jpg" ||
					extension == ".pfm" ||
					extension == ".png" ||
					extension == ".ppm" ||
					extension == ".tga")
				{
					MemData data;
					data.size = textureFile.nFileSizeLow;
					SDLOG(0, "size: %ld ", data.size);
					data.buffer = new char[data.size];
					std::fread(&data.buffer[0], 1, data.size, fp);
					std::fclose(fp);
					UINT32 decimalValue;
					sscanf(filename.c_str(), "%08x", &decimalValue);
					SDLOG(0, "texture hash: %s hex: %08x\n", filename.c_str(), decimalValue);
					cachedTexFiles.insert(std::pair<UINT32, MemData>(decimalValue, data));
				}
			}
		} while (FindNextFile(searchHandle, &textureFile));
	}
	SDLOG(0, "Prefetch overwrite textures to memory ended, time: %f\n", getElapsedTime() - startTime);
}

RSManager::~RSManager()
{
	for (auto& texData : cachedTexFiles)
	{
		if (texData.second.buffer) {
			delete[] texData.second.buffer;
			texData.second.buffer = NULL;
		}
	}
}

void RSManager::releaseResources() {
	SDLOG(0, "RenderstateManager releasing resources\n");
	titleMarkRelease();
	if (g_hudBufOwn) {
		SAFERELEASE(g_hudBufSurf);
		SAFERELEASE(g_hudBufTex);
	}
	g_hudBufSurf = NULL;
	g_hudBufTex = NULL;
	g_hudBufOwn = false;
	SAFERELEASE(rgbaBuffer1Surf);
	SAFERELEASE(rgbaBuffer1Tex);
	SAFERELEASE(sharedBuffer1Surf);
	SAFERELEASE(sharedBuffer1Tex);
	SAFERELEASE(sharedBuffer2Surf);
	SAFERELEASE(sharedBuffer2Tex);
	SAFERELEASE(sharedDofSurf);
	SAFERELEASE(sharedDofTex);
	SAFERELEASE(depthStencilSurf);
	SAFERELEASE(prevStateBlock);
	SAFEDELETE(smaa);
	SAFEDELETE(fxaa);
	SAFEDELETE(g_sharpen);
	SAFEDELETE(ssao);
	SAFEDELETE(gauss);
	SAFEDELETE(hud);

	// DSfix 3.0: Plug the tracking map leaks during Alt+Tab / Device Lost events
	mainRenderTexIndices.clear();
	mainRenderSurfIndices.clear();
	mainRenderTexIndex = 0;
	mainRenderSurfIndex = 0;

	zSurf = NULL;
	mainRT = NULL;
	prevVDecl = NULL;
	prevDepthStencilSurf = NULL;
	prevRenderTarget = NULL;
	prevRenderTex = NULL;

	// DSfix 3.0: a device reset can happen in the middle of a frame (Alt+Tab). Clear every per-frame state so the first
	// frame after the reset starts clean and nothing points at the released HUD buffer.
	onHudRT = false;
	pausedHudRT = false;
	hudStarted = false;
	hddp = 0;
	rddp = 0;
	nrts = 0;
	mainRTuses = 0;
	doft[0] = 0; doft[1] = 0; doft[2] = 0;
	g_hudScalePhase = false;
	g_hudWindowSeen = false;
	g_lastHudGroup = HG_NONE;
	g_dumpActive = false;
	g_dumpLines.clear();
	g_heldMainRT = NULL; // the surfaces are released with the device
	g_heldZ = NULL;
	g_heldMainUses = 0;
	g_heldZSeen = false;
	g_effectsDone = false;
	g_rtDumpFrames = 0;
	g_rtDumpLines.clear();
	resetWindows();

	SDLOG(0, "RenderstateManager resource release completed\n");
}

HRESULT RSManager::redirectPresent(CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion) {
	weaponWearBarsNewFrame();
	capturing = false;
	if (captureNextFrame) {
		capturing = true;
		captureNextFrame = false;
		SDLOG(0, "== CAPTURING FRAME ==\n")
	}
	if (g_dumpActive) {
		if (g_dumpIsRequested) {
			std::ofstream df(GetDirectoryFile("hud_dump.txt"), std::ios::app);
			df << "=== HUD FRAME DUMP ===\n";
			for (size_t i = 0; i < g_dumpLines.size(); ++i) df << g_dumpLines[i] << "\n";
			df << "=== END ===\n\n";
			g_dumpIsRequested = false;
		}
		bool writeCap = false;
		if (g_menuCapLeft > 0) {
			writeCap = true;
			--g_menuCapLeft;
		}
		else if (g_menuCapArmFrames > 0) {
			if (g_frameHadMenu && !g_prevFrameHadMenu && g_menuCapArmFrames < 1500) { // the first frame with a menu
				writeCap = true;
				g_menuCapLeft = 3;
				g_menuCapArmFrames = 0;
				g_menuCapFrameNo = 0;
			}
			else {
				--g_menuCapArmFrames;
			}
		}
		if (writeCap) {
			std::ofstream mf(GetDirectoryFile("menu_open_dump.txt"), std::ios::app);
			mf << "=== MENU OPEN CAPTURE, frame " << ++g_menuCapFrameNo << " ===\n";
			for (size_t i = 0; i < g_dumpLines.size(); ++i) mf << g_dumpLines[i] << "\n";
			mf << "=== END ===\n\n";
		}
		if (g_recArmFrames > 0) {
			if (g_recRing.size() < kRecFrames) {
				g_recRing.push_back(std::vector<std::string>());
				g_recRing.back().swap(g_dumpLines);
			}
			else {
				g_recRing[g_recNext].swap(g_dumpLines); // overwrite the oldest frame
				g_recNext = (g_recNext + 1) % kRecFrames;
			}
			if (g_recWrite) {
				std::ofstream rf(GetDirectoryFile("flight_dump.txt"), std::ios::app);
				rf << "##### FLIGHT RECORDER: last " << g_recRing.size() << " frames, oldest first #####\n\n";
				const size_t n = g_recRing.size();
				const size_t start = (n < kRecFrames) ? 0 : g_recNext; // oldest frame
				for (size_t k = 0; k < n; ++k) {
					const std::vector<std::string>& fr = g_recRing[(start + k) % n];
					rf << "=== FRAME -" << (n - 1 - k) << " ===\n";
					for (size_t i = 0; i < fr.size(); ++i) rf << fr[i] << "\n";
					rf << "=== END ===\n\n";
				}
				g_recWrite = false;
				g_recArmFrames = 0;
				g_recRing.clear();
				g_recNext = 0;
			}
			else {
				--g_recArmFrames;
				if (g_recArmFrames == 0) { g_recRing.clear(); g_recNext = 0; }
			}
		}
		g_dumpActive = false;
		g_dumpLines.clear();
	}
	g_prevFrameHadMenu = g_frameHadMenu;
	g_frameHadMenu = false;
	g_framesSinceHud = g_hudWindowSeen ? 0 : (g_framesSinceHud < 1000 ? g_framesSinceHud + 1 : 1000);
	if (g_dumpRequested) { // record the whole next frame
		g_dumpRequested = false;
		g_dumpIsRequested = true;
		g_dumpActive = true;
	}
	if (g_menuCapArmFrames > 0 || g_menuCapLeft > 0 || g_recArmFrames > 0) g_dumpActive = true; // captures record every frame
	if (g_dumpActive) g_dumpLines.push_back("--- FRAME START ---");
	hudStarted = false;
	g_hudScalePhase = false;
	g_hudWindowSeen = false;
	g_lastHudGroup = HG_NONE;
	if (g_rtDumpFrames > 0) {
		rtDumpLine("--- PRESENT: %u render target calls, effects %s, mainRTuses %u, held uses %u, held depth seen %d ---",
			nrts, g_effectsDone ? "applied" : "NOT applied", mainRTuses, g_heldMainUses, g_heldZSeen ? 1 : 0);
		if (--g_rtDumpFrames == 0) {
			std::ofstream df(GetDirectoryFile("rt_dump.txt"), std::ios::app);
			df << "=== RENDER TARGET DUMP (held pair used in " << g_heldFramesUsed << " frames so far) ===\n";
			for (size_t i = 0; i < g_rtDumpLines.size(); ++i) df << g_rtDumpLines[i] << "\n";
			df << "=== END ===\n\n";
			g_rtDumpLines.clear();
		}
	}
	nrts = 0;
	++g_frameNo;
	doft[0] = 0; doft[1] = 0; doft[2] = 0;
	mainRT = NULL;
	mainRTuses = 0;
	zSurf = NULL;
	g_heldMainUses = 0;
	g_heldZSeen = false;
	g_effectsDone = false;
	publishMouseMap(d3ddev); // this frame's shrunk windows, for the cursor mapping until the next Present
	resetWindows();
	g_screenCovered = false;


	// "DSfix 3.0" in the corner of the title screen (only while the title screen shows)
	titleMarkDraw(d3ddev);

	// Pace the frame BEFORE submitting to the D3D presentation queue
	frameTimeManagement();

	HRESULT hr = d3ddev->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);

	return hr;
}

D3DPRESENT_PARAMETERS RSManager::adjustPresentationParameters(const D3DPRESENT_PARAMETERS* pPresentationParameters) {
	D3DPRESENT_PARAMETERS ret;
	memcpy(&ret, pPresentationParameters, sizeof(D3DPRESENT_PARAMETERS));
	SDLOG(0, " - requested mode:\n");
	SDLOG(0, " - - Backbuffer(s): %4u x %4u %16s *%d \n", pPresentationParameters->BackBufferWidth, pPresentationParameters->BackBufferHeight, D3DFormatToString(pPresentationParameters->BackBufferFormat), pPresentationParameters->BackBufferCount);
	SDLOG(0, " - - PresentationInterval: %2u   Windowed: %5s    Refresh: %3u Hz\n", pPresentationParameters->PresentationInterval, pPresentationParameters->Windowed ? "true" : "false", pPresentationParameters->FullScreen_RefreshRateInHz);
	//if(Settings::get().getEnableTripleBuffering()) {
	//	ret.SwapEffect = D3DSWAPEFFECT_FLIPEX;
	//	ret.PresentationInterval = D3DPRESENT_FORCEIMMEDIATE;
	//	ret.BackBufferCount = 2;
	//	ret.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
	//	ret.SwapEffect = D3DSWAPEFFECT_DISCARD;
	//}
	//if(Settings::get().getUnlockFPS()) {
	//	ret.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
	//	ret.SwapEffect = D3DSWAPEFFECT_DISCARD;
	//}
	return ret;
}

// DSfix 3.0: fullscreenHz and enableVsync. Both act only in the game's own fullscreen mode (exclusive fullscreen). In a
// window or borderless window Windows composes the picture at the desktop refresh rate and never tears, so there they
// are left alone. Called after adjustPresentationParameters when the device is created and on every Reset (Alt+Tab).
// With the defaults (fullscreenHz 0, enableVsync -1) nothing is changed.
void applyDisplayOverrides(IDirect3D9* d3d, UINT adapter, D3DPRESENT_PARAMETERS* pp) {
	if (!pp) return;
	const int vsync = Settings::get().getEnableVsync();
	const unsigned wantHz = Settings::get().getFullscreenHz();
	if (vsync < 0 && wantHz == 0) return;
	if (pp->Windowed) {
		SDLOG(0, " - windowed / borderless mode: enableVsync and fullscreenHz not applied (they only act in the game's fullscreen mode)\n");
		return;
	}

	if (wantHz > 0) {
		const UINT gameHz = pp->FullScreen_RefreshRateInHz;
		UINT best = 0, lowest = 0;
		char rates[256] = "";
		if (d3d) {
			// Display modes are listed by display format: the back buffer format without its alpha channel
			D3DFORMAT fmt = pp->BackBufferFormat;
			if (fmt == D3DFMT_A8R8G8B8 || fmt == D3DFMT_UNKNOWN) fmt = D3DFMT_X8R8G8B8;
			else if (fmt == D3DFMT_A1R5G5B5) fmt = D3DFMT_X1R5G5B5;
			UINT seen[64]; UINT nSeen = 0;
			const UINT n = d3d->GetAdapterModeCount(adapter, fmt);
			for (UINT i = 0; i < n; ++i) {
				D3DDISPLAYMODE m;
				if (FAILED(d3d->EnumAdapterModes(adapter, fmt, i, &m))) continue;
				if (m.Width != pp->BackBufferWidth || m.Height != pp->BackBufferHeight || m.RefreshRate == 0) continue;
				bool dup = false;
				for (UINT k = 0; k < nSeen; ++k) if (seen[k] == m.RefreshRate) dup = true;
				if (dup) continue;
				if (nSeen < 64) seen[nSeen++] = m.RefreshRate;
				char one[16];
				sprintf_s(one, "%s%u", rates[0] ? " " : "", m.RefreshRate);
				strcat_s(rates, one);
				// the highest rate not above the request; if every rate is above it, the lowest one
				if (m.RefreshRate <= wantHz && m.RefreshRate > best) best = m.RefreshRate;
				if (lowest == 0 || m.RefreshRate < lowest) lowest = m.RefreshRate;
			}
		}
		if (best == 0) best = lowest;
		if (best) {
			pp->FullScreen_RefreshRateInHz = best;
			SDLOG(0, " - fullscreenHz: wanted %u Hz, the game asked %u Hz, using %u Hz (available at %ux%u: %s)\n",
				wantHz, gameHz, best, pp->BackBufferWidth, pp->BackBufferHeight, rates);
		}
		else {
			SDLOG(0, " - fullscreenHz: no display mode found for %ux%u, keeping the game's %u Hz\n",
				pp->BackBufferWidth, pp->BackBufferHeight, gameHz);
		}
	}

	if (vsync >= 0) {
		const UINT gameInterval = pp->PresentationInterval;
		pp->PresentationInterval = vsync > 0 ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
		SDLOG(0, " - enableVsync %d: presentation interval %08X (the game asked %08X)\n", vsync, pp->PresentationInterval, gameInterval);
	}
}

void RSManager::dumpSurface(const char* name, IDirect3DSurface9* surface) {
	char fullname[128];
	sprintf_s(fullname, 128, "dump%03d_%s.tga", dumpCaptureIndex++, name);
	D3DXSaveSurfaceToFile(fullname, D3DXIFF_TGA, surface, NULL, NULL);
}

void RSManager::registerMainRenderTexture(IDirect3DTexture9* pTexture) {
	if (pTexture) {
		mainRenderTexIndices.insert(std::make_pair(pTexture, mainRenderTexIndex));
		SDLOG(4, "Registering main render tex: %p as #%d\n", pTexture, mainRenderTexIndex);
		mainRenderTexIndex++;
	}
}

void RSManager::registerMainRenderSurface(IDirect3DSurface9* pSurface) {
	if (pSurface) {
		mainRenderSurfIndices.insert(std::make_pair(pSurface, mainRenderSurfIndex));
		SDLOG(4, "Registering main render surface: %p as #%d\n", pSurface, mainRenderSurfIndex);
		mainRenderSurfIndex++;
	}
}


HRESULT RSManager::redirectSetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget) {
	nrts++;
	//if(RenderTargetIndex==0) lastReplacement = -1;
	//SurfSurfMap::iterator it = renderTexSurfTargets.find(pRenderTarget);
	//if(it != renderTexSurfTargets.end()) {
	//	SDLOG(1, "Redirected SetRenderTarget(%d, %p) to %p\n", RenderTargetIndex, pRenderTarget, it->second);
	//	if(capturing) dumpSurface("redirectRenderTarget", it->second);
	//	lastReplacement = renderTexSurfIndices[pRenderTarget];
	//	return d3ddev->SetRenderTarget(RenderTargetIndex, it->second);
	//}
	//if(capturing && hudStarted) {
	//	//SurfIntMap::iterator it = renderTexSurfIndices.find(pRenderTarget);
	//	//if(it != renderTexSurfIndices.end()) {
	//		dumpSurface("hudTarget", pRenderTarget);
	//		capturing = false;
	//	//}
	//}
	// determine where we are rendering to
	//{
	//	SurfIntMap::iterator it = mainRenderSurfIndices.find(pRenderTarget);
	//	if(it != mainRenderSurfIndices.end()) {
	//		SDLOG(4, " - rendersurface #%d\n", it->second);
	//	} else {
	//		if(IDirect3DTexture9* tex = getSurfTexture(pRenderTarget)) {
	//			TexIntMap::iterator it = mainRenderTexIndices.find(tex);
	//			if(it != mainRenderTexIndices.end()) {
	//				SDLOG(4, " - rendertexture #%d\n", it->second);
	//			}
	//			tex->Release();
	//		} else {
	//			IDirect3DSurface9* backBuffer;
	//			d3ddev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
	//			if(pRenderTarget == backBuffer) {
	//				SDLOG(4, " - backbuffer\n");
	//			}
	//			backBuffer->Release();
	//		}
	//	}
	//}
	if (capturing) {
		IDirect3DSurface9* oldRenderTarget, * depthStencilSurface;
		d3ddev->GetRenderTarget(0, &oldRenderTarget);
		d3ddev->GetDepthStencilSurface(&depthStencilSurface);
		char buffer[64];
		sprintf(buffer, "%03d_oldRenderTarget_%p_.tga", nrts, oldRenderTarget);
		SDLOG(0, "Capturing surface %p as %s\n", oldRenderTarget, buffer);
		D3DXSaveSurfaceToFile(buffer, D3DXIFF_TGA, oldRenderTarget, NULL, NULL);
		if (depthStencilSurface) {
			sprintf(buffer, "%03d_oldRenderTargetDepth_%p_.tga", nrts, oldRenderTarget);
			SDLOG(0, "Capturing depth surface %p as %s\n", depthStencilSurface, buffer);
			D3DXSaveSurfaceToFile(buffer, D3DXIFF_TGA, depthStencilSurface, NULL, NULL);
		}
		SAFERELEASE(oldRenderTarget);
		SAFERELEASE(depthStencilSurface);
	}

	if (nrts == 1) { // we are switching to the RT that will be the main rendering target for this frame
		// store it for later use
		mainRT = pRenderTarget;
		SDLOG(3, "Storing RT as main RT: %p\n", mainRT); // was level 0: one line every frame at logLevel 1
		if (!haveOcclusionScale) {
			measureOcclusionScale();
		}
	}

	if (nrts == 11) { // we are switching to the RT used to store the Z value in the 24 RGB bits (among other things)
		// lets store it for later use
		zSurf = pRenderTarget;
		SDLOG(3, "Storing RT as Z buffer RT: %p\n", zSurf);
	}

	if (mainRT && pRenderTarget == mainRT) {
		SDLOG(3, "MainRT uses: %u + 1\n", mainRTuses);
		++mainRTuses;
	}

	// the remembered pair (see g_heldMainRT): count its uses in this frame independently of the call order
	if (g_heldMainRT && pRenderTarget == g_heldMainRT) ++g_heldMainUses;
	if (g_heldZ && pRenderTarget == g_heldZ) g_heldZSeen = true;

	if (g_rtDumpFrames > 0) {
		unsigned w = 0, h = 0, fmt = 0;
		if (pRenderTarget) {
			D3DSURFACE_DESC d;
			if (SUCCEEDED(pRenderTarget->GetDesc(&d))) { w = d.Width; h = d.Height; fmt = (unsigned)d.Format; }
		}
		rtDumpLine("#%02u idx%lu %p %ux%u fmt%u%s%s%s%s", nrts, RenderTargetIndex, pRenderTarget, w, h, fmt,
			(pRenderTarget && pRenderTarget == mainRT) ? " =main" : "",
			(pRenderTarget && pRenderTarget == zSurf) ? " =z" : "",
			(pRenderTarget && pRenderTarget == g_heldMainRT) ? " =heldMain" : "",
			(pRenderTarget && pRenderTarget == g_heldZ) ? " =heldZ" : "");
	}

	// Runs AA and SSAO on the finished 3D image `sceneRT` with the depth image `depthRT`. False if the surfaces are not
	// what they should be (not a texture, wrong size), in which case nothing was drawn.
	auto runMainEffects = [&](IDirect3DSurface9* sceneRT, IDirect3DSurface9* depthRT) -> bool {
		IDirect3DTexture9* tex = getSurfTexture(sceneRT); // final renderbuffer has to be from texture
		if (!tex) return false;
		D3DSURFACE_DESC desc;
		sceneRT->GetDesc(&desc); // check size just to make even more sure
		if (desc.Width != Settings::get().getRenderWidth() || desc.Height != Settings::get().getRenderHeight()) {
			tex->Release();
			return false;
		}
		const bool wantSsao = ssao && doSsao && !bonfireDisableSSAO;
		IDirect3DTexture9* zTex = wantSsao ? getSurfTexture(depthRT) : NULL;
		if (wantSsao && !zTex) { // SSAO without its depth image would draw garbage
			tex->Release();
			return false;
		}
		storeRenderState();
		d3ddev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
		d3ddev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
		d3ddev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
		// perform AA processing
		if (!lowFPSmode && doAA && (smaa || fxaa)) {
			if (smaa) smaa->go(tex, tex, rgbaBuffer1Surf, SMAA::INPUT_COLOR);
			else fxaa->go(tex, rgbaBuffer1Surf);
			d3ddev->StretchRect(rgbaBuffer1Surf, NULL, sceneRT, NULL, D3DTEXF_NONE);
		}
		// sharpening (sharpenStrength): on the anti-aliased image, before SSAO so the ambient occlusion is not sharpened
		if (g_sharpen && g_doSharpen) {
			g_sharpen->go(tex, rgbaBuffer1Surf, sharpenAmount());
			d3ddev->StretchRect(rgbaBuffer1Surf, NULL, sceneRT, NULL, D3DTEXF_NONE);
		}
		// perform SSAO
		if (wantSsao) {
			ssao->go(tex, zTex, rgbaBuffer1Surf);
			d3ddev->StretchRect(rgbaBuffer1Surf, NULL, sceneRT, NULL, D3DTEXF_NONE);
		}
		restoreRenderState();
		SAFERELEASE(zTex);
		tex->Release();
		return true;
		};

	// we are switching away from the initial 3D-rendered image, do AA and SSAO
	const bool wantEffects = (ssao && doSsao && !bonfireDisableSSAO) || (doAA && (smaa || fxaa)) || (g_sharpen && g_doSharpen);
	if (wantEffects && !g_effectsDone) {
		const bool normalReady = mainRTuses == 2 && mainRT && zSurf;
		const bool heldReady = g_heldMainRT && g_heldZ && g_heldZSeen && g_heldMainUses == 2;
		if (normalReady || heldReady) {
			IDirect3DSurface9* oldRenderTarget = NULL;
			d3ddev->GetRenderTarget(0, &oldRenderTarget);
			if (oldRenderTarget) {
				// 1. the normal detection (unchanged): it also refreshes the remembered pair
				if (normalReady && oldRenderTarget == mainRT && runMainEffects(oldRenderTarget, zSurf)) {
					g_effectsDone = true;
					g_heldMainRT = mainRT;
					g_heldZ = zSurf;
					if (g_rtDumpFrames > 0) rtDumpLine("    -> effects applied (normal detection)");
				}
				// 2. the call order changed in this frame: use the remembered pair at the same point of the frame
				if (!g_effectsDone && heldReady && oldRenderTarget == g_heldMainRT && runMainEffects(oldRenderTarget, g_heldZ)) {
					g_effectsDone = true;
					++g_heldFramesUsed;
					if (g_heldFramesUsed == 1) SDLOG(0, "AA/SSAO: the frame's render target order changed (menu); using the remembered scene/depth pair\n");
					if (g_rtDumpFrames > 0) rtDumpLine("    -> effects applied (remembered pair)");
				}
				oldRenderTarget->Release();
			}
		}
	}

	// DoF blur stuff
	if (gauss && doDofGauss) {
		IDirect3DSurface9* oldRenderTarget;
		d3ddev->GetRenderTarget(0, &oldRenderTarget);
		D3DSURFACE_DESC desc;
		oldRenderTarget->GetDesc(&desc);
		unsigned dofIndex = isDof(desc.Width, desc.Height);
		if (dofIndex) {
			doft[dofIndex]++;
			SDLOG(6, "DOF index: %u, doft: %u\n", dofIndex, doft[dofIndex]);
			//if(takeScreenshot) {
			//	char buffer[256];
			//	sprintf(buffer, "dof%1u_doft%1u.bmp", dofIndex, doft[dofIndex]);
			//	D3DXSaveSurfaceToFile(buffer, D3DXIFF_BMP, oldRenderTarget, NULL, NULL);
			//}
			if (dofIndex == 1 && doft[1] == 4) {
				IDirect3DTexture9* oldRTtex = getSurfTexture(oldRenderTarget);
				if (oldRTtex) {
					storeRenderState();
					for (size_t i = 0; i < Settings::get().getDOFBlurAmount(); ++i) gauss->go(oldRTtex, oldRenderTarget);
					restoreRenderState();
					oldRTtex->Release();
				}
			}
		}
		oldRenderTarget->Release();
	}

	// Timing for hudless screenshots
	if (mainRTuses == 11 && takeScreenshot) {
		IDirect3DSurface9* oldRenderTarget;
		d3ddev->GetRenderTarget(0, &oldRenderTarget);
		if (oldRenderTarget != mainRT) {
			static int toggleSS = 0;
			toggleSS = (toggleSS + 1) % 2;
			if (!toggleSS) {
				takeScreenshot = false;
				SDLOG(0, "Capturing screenshot\n");
				char timebuf[128], buffer[512];
				time_t ltime;
				time(&ltime);
				struct tm* timeinfo;
				timeinfo = localtime(&ltime);
				strftime(timebuf, 128, "screenshot_%Y-%m-%d_%H-%M-%S.png", timeinfo);
				sprintf(buffer, "%s\\%s", Settings::get().getScreenshotDir().c_str(), timebuf);
				SDLOG(0, " - to %s\n", buffer);

				D3DSURFACE_DESC desc;
				oldRenderTarget->GetDesc(&desc);
				IDirect3DSurface9* convertedSurface;
				d3ddev->CreateRenderTarget(desc.Width, desc.Height, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, true, &convertedSurface, NULL);
				D3DXLoadSurfaceFromSurface(convertedSurface, NULL, NULL, oldRenderTarget, NULL, NULL, D3DX_FILTER_POINT, 0);
				D3DXSaveSurfaceToFile(buffer, D3DXIFF_PNG, convertedSurface, NULL, NULL);
				convertedSurface->Release();
			}
		}
		oldRenderTarget->Release();
	}

	if (rddp >= 4) { // we just finished rendering the frame (pre-HUD)
		IDirect3DSurface9* oldRenderTarget;
		d3ddev->GetRenderTarget(0, &oldRenderTarget);
		// final renderbuffer has to be from texture, just making sure here
		if (IDirect3DTexture9* tex = getSurfTexture(oldRenderTarget)) {
			// check size just to make even more sure
			D3DSURFACE_DESC desc;
			oldRenderTarget->GetDesc(&desc);
			if (isHudTargetSize(desc)) {
				// HUD stuff
				if (hud && doHud && rddp == 9 && !g_hudWindowSeen) {
					SDLOG(0, "Starting HUD rendering\n");
					hddp = 0;
					onHudRT = true;
					g_hudScalePhase = true;
					g_hudWindowSeen = true;
					g_lastHudGroup = HG_NONE;
					if (g_dumpActive) g_dumpLines.push_back("--- WINDOW START ---");
					d3ddev->SetRenderTarget(0, g_hudBufSurf);
					d3ddev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_RGBA(0, 0, 0, 0), 0.0f, 0);
					prevRenderTex = tex;
					prevRenderTarget = pRenderTarget;
					tex->Release();
					oldRenderTarget->Release();

					d3ddev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
					d3ddev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_ADD);
					d3ddev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
					d3ddev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
					return S_OK;
				}
			}
			tex->Release();
		}
		oldRenderTarget->Release();
	}
	if (onHudRT) {
		finishHudRendering();
	}
	if (rddp < 4 || rddp > 8) rddp = 0;
	else rddp++;
	return d3ddev->SetRenderTarget(RenderTargetIndex, pRenderTarget);
}

HRESULT RSManager::redirectStretchRect(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect, IDirect3DSurface9* pDestSurface, CONST RECT* pDestRect, D3DTEXTUREFILTERTYPE Filter) {
	//SurfSurfMap::iterator it = renderTexSurfTargets.find(pSourceSurface);
	//if(it != renderTexSurfTargets.end()) {
	//	SDLOG(1, "Redirected StretchRect %p -> %p to %p -> %p\n", pSourceSurface, pDestSurface, it->second, pDestSurface);
	//	if(capturing) dumpSurface("redirectStretchRect", it->second);
	//	return d3ddev->StretchRect(it->second, pSourceRect, pDestSurface, pDestRect, Filter);
	//}
	return d3ddev->StretchRect(pSourceSurface, pSourceRect, pDestSurface, pDestRect, D3DTEXF_LINEAR);
}

HRESULT RSManager::redirectSetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture) {
	if (pTexture == NULL) return d3ddev->SetTexture(Stage, pTexture);
	//TexIntMap::iterator it = renderTexIndices.find((IDirect3DTexture9*)pTexture);
	//if(it != renderTexIndices.end() && it->second == 2) {
	//	IDirect3DSurface9* surf0;
	//	((IDirect3DTexture9*)pTexture)->GetSurfaceLevel(0, &surf0);
	//	SDLOG(1, "Redirected SetTexture(%d, %p) -- StretchRect from %p to %p\n", Stage, pTexture, renderTexSurfTargets[surf0], surf0);
	//	if(capturing) dumpSurface("redirectSetTexture", renderTexSurfTargets[surf0]);
	//	d3ddev->StretchRect(renderTexSurfTargets[surf0], NULL, surf0, NULL, D3DTEXF_LINEAR);
	//}
	//int index = getTextureIndex((IDirect3DTexture9*)pTexture);
	//if(dumpedTextures.find(index) == dumpedTextures.end()) {
	//	dumpedTextures.insert(index);
	//	IDirect3DSurface9* surf;
	//	((IDirect3DTexture9*)pTexture)->GetSurfaceLevel(0, &surf);
	//	char buffer[16];
	//	sprintf(buffer, "tex%4d__", index);
	//	dumpSurface(buffer, surf);
	//}
	if (!hudStarted && isTextureHudHealthbar(pTexture)) {
		SDLOG(1, "HUD started!\n");
		hudStarted = true;
	}
	if (isTextureMenuscreenLogo(pTexture)) titleMarkLogoSeen(); // the title screen is showing
	//if(mainRT && mainRTuses > 3) {
	//	IDirect3DTexture9* tex;
	//	pTexture->QueryInterface(IID_IDirect3DTexture9, (void**)&tex);
	//	if(tex) {
	//		IDirect3DSurface9* surf;
	//		if(tex->GetSurfaceLevel(0, &surf) == D3D_OK) {
	//			bool main = surf == mainRT;
	//			surf->Release();
	//			if(main) {
	//				d3ddev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
	//			}
	//		}
	//	}
	//}
	if (pTexture && rddp == 0 && Stage == 0) { ++rddp; }
	else if (pTexture && rddp == 1 && Stage == 1) { ++rddp; }
	else if (pTexture && rddp == 2 && Stage == 2) { ++rddp; }
	else if (pTexture && rddp == 3 && Stage == 3) { ++rddp; }
	else rddp = 0;
	return d3ddev->SetTexture(Stage, pTexture);
}

HRESULT RSManager::redirectSetDepthStencilSurface(IDirect3DSurface9* pNewZStencil) {
	//if(lastReplacement >= 0) {
	//	SDLOG(1, "Redirected SetDepthStencilSurface(%p) to %p\n", pNewZStencil, renderTexDSBuffers[lastReplacement]);
	//	d3ddev->SetDepthStencilSurface(renderTexDSBuffers[lastReplacement]);
	//}
	//lastReplacement = -1;
	return d3ddev->SetDepthStencilSurface(pNewZStencil);
}

unsigned RSManager::getTextureIndex(IDirect3DTexture9* ppTexture) {
	TexIntMap::iterator it = texIndices.find(ppTexture);
	if (it != texIndices.end()) return it->second;
	return UINT_MAX;
}

void RSManager::registerD3DXCreateTextureFromFileInMemory(LPCVOID pSrcData, UINT SrcDataSize, LPDIRECT3DTEXTURE9 pTexture) {
	SDLOG(1, "RenderstateManager: registerD3DXCreateTextureFromFileInMemory %p\n", pTexture);
	if (Settings::get().getEnableTextureDumping()) {
		UINT32 hash = SuperFastHash((char*)const_cast<void*>(pSrcData), SrcDataSize);
		SDLOG(1, " - size: %8u, hash: %8x\n", SrcDataSize, hash);

		IDirect3DSurface9* surf;
		((IDirect3DTexture9*)pTexture)->GetSurfaceLevel(0, &surf);
		char buffer[128];
		sprintf_s(buffer, "dsfix/tex_dump/%08x.tga", hash);
		D3DXSaveSurfaceToFile(GetDirectoryFile(buffer), D3DXIFF_TGA, surf, NULL, NULL);
		surf->Release();
	}
	registerKnowTexture(pSrcData, SrcDataSize, pTexture);
}

void RSManager::registerKnowTexture(LPCVOID pSrcData, UINT SrcDataSize, LPDIRECT3DTEXTURE9 pTexture) {
	if (foundKnownTextures < numKnownTextures) {
		UINT32 hash = SuperFastHash((char*)const_cast<void*>(pSrcData), SrcDataSize);
#define TEXTURE(_name, _hash) \
		if(hash == _hash) { \
			texture##_name = pTexture; \
			++foundKnownTextures; \
			SDLOG(1, "RenderstateManager: recognized known texture %s at %u\n", #_name, pTexture); \
		}
#include "Textures.def"
#undef TEXTURE
		if (foundKnownTextures == numKnownTextures) {
			SDLOG(1, "RenderstateManager: all known textures found!\n");
		}
	}
}

void RSManager::registerD3DXCompileShader(LPCSTR pSrcData, UINT srcDataLen, const D3DXMACRO* pDefines, LPD3DXINCLUDE pInclude, LPCSTR pFunctionName, LPCSTR pProfile, DWORD Flags, LPD3DXBUFFER* ppShader, LPD3DXBUFFER* ppErrorMsgs, LPD3DXCONSTANTTABLE* ppConstantTable) {
	SDLOG(0, "RenderstateManager: registerD3DXCompileShader %p, fun: %s, profile: %s", *ppShader, pFunctionName, pProfile);
	SDLOG(0, "============= source:\n%s\n====================", pSrcData);
}

IDirect3DTexture9* RSManager::getSurfTexture(IDirect3DSurface9* pSurface) {
	IUnknown* pContainer = NULL;
	HRESULT hr = pSurface->GetContainer(IID_IDirect3DTexture9, (void**)&pContainer);
	if (D3D_OK == hr) return (IDirect3DTexture9*)pContainer;
	SAFERELEASE(pContainer);
	return NULL;
}

void RSManager::enableSingleFrameCapture() {
	captureNextFrame = true;
}

void RSManager::enableTakeScreenshot() {
	takeScreenshot = true;
	SDLOG(0, "takeScreenshot: %s\n", takeScreenshot ? "true" : "false");
}

// Dev hotkeys: rebuild an effect from its .fx file. Each one is skipped (with a log line) when the effect is off in the
// ini or its buffers were not created, instead of building it with invalid settings or missing buffers.
static bool reloadSsaoAllowed(IDirect3DTexture9* b1, IDirect3DTexture9* b2) {
	if (ssaoStrengthIndex() < 0) { SDLOG(0, "SSAO reload skipped: SSAO is off (ssaoStrength 0)\n"); return false; }
	if (!b1 || !b2) { SDLOG(0, "SSAO reload skipped: SSAO buffers not allocated\n"); return false; }
	return true;
}
void RSManager::reloadVssao() {
	if (!reloadSsaoAllowed(sharedBuffer1Tex, sharedBuffer2Tex)) return;
	SAFEDELETE(ssao);
	ssao = new SSAO(d3ddev, Settings::get().getRenderWidth(), Settings::get().getRenderHeight(), (unsigned)ssaoStrengthIndex(), SSAO::VSSAO, sharedBuffer1Tex, sharedBuffer1Surf, sharedBuffer2Tex, sharedBuffer2Surf);
	if (!ssao->ok()) { SAFEDELETE(ssao); SDLOG(0, "SSAO reload failed - SSAO off\n"); return; }
	SDLOG(0, "Reloaded SSAO (VSSAO)\n");
}
void RSManager::reloadHbao() {
	if (!reloadSsaoAllowed(sharedBuffer1Tex, sharedBuffer2Tex)) return;
	SAFEDELETE(ssao);
	ssao = new SSAO(d3ddev, Settings::get().getRenderWidth(), Settings::get().getRenderHeight(), (unsigned)ssaoStrengthIndex(), SSAO::HBAO, sharedBuffer1Tex, sharedBuffer1Surf, sharedBuffer2Tex, sharedBuffer2Surf);
	if (!ssao->ok()) { SAFEDELETE(ssao); SDLOG(0, "SSAO reload failed - SSAO off\n"); return; }
	SDLOG(0, "Reloaded SSAO (HBAO)\n");
}
void RSManager::reloadScao() {
	if (!reloadSsaoAllowed(sharedBuffer1Tex, sharedBuffer2Tex)) return;
	SAFEDELETE(ssao);
	ssao = new SSAO(d3ddev, Settings::get().getRenderWidth(), Settings::get().getRenderHeight(), (unsigned)ssaoStrengthIndex(), SSAO::SCAO, sharedBuffer1Tex, sharedBuffer1Surf, sharedBuffer2Tex, sharedBuffer2Surf);
	if (!ssao->ok()) { SAFEDELETE(ssao); SDLOG(0, "SSAO reload failed - SSAO off\n"); return; }
	SDLOG(0, "Reloaded SSAO (SCAO)\n");
}
void RSManager::reloadSsao() {
	if (!reloadSsaoAllowed(sharedBuffer1Tex, sharedBuffer2Tex)) return;
	SAFEDELETE(ssao);
	ssao = new SSAO(d3ddev, Settings::get().getRenderWidth(), Settings::get().getRenderHeight(), (unsigned)ssaoStrengthIndex(), ssaoTypeFromSettings(), sharedBuffer1Tex, sharedBuffer1Surf, sharedBuffer2Tex, sharedBuffer2Surf);
	if (!ssao->ok()) {
		SDLOG(0, "SSAO reload failed (see the errors above) - SSAO off until the next reload\n");
		SAFEDELETE(ssao);
		return;
	}
	SDLOG(0, "Reloaded SSAO (%s)\n", Settings::get().getSsaoType().c_str());
}

void RSManager::reloadGauss() {
	if (Settings::get().getDOFBlurAmount() == 0) { SDLOG(0, "DoF reload skipped: DoF blur is off (dofBlurAmount 0)\n"); return; }
	if (!sharedDofTex) { SDLOG(0, "DoF reload skipped: DoF buffer not allocated\n"); return; }
	unsigned w, h;
	dofBufferSize(w, h); // same size as the buffer (512x360 when dofOverrideResolution is 0)
	SAFEDELETE(gauss);
	gauss = new GAUSS(d3ddev, w, h, sharedDofTex, sharedDofSurf);
	SDLOG(0, "Reloaded GAUSS\n");
}

void RSManager::reloadAA() {
	const int aaIdx = aaQualityIndex();
	const bool isSmaa = Settings::get().getAAType() == "SMAA";
	if (aaIdx < 0) { SDLOG(0, "AA reload skipped: AA is off (aaQuality 0)\n"); return; }
	if (!sharedBuffer1Tex || (isSmaa && !sharedBuffer2Tex)) { SDLOG(0, "AA reload skipped: AA buffers not allocated\n"); return; }
	SAFEDELETE(smaa);
	SAFEDELETE(fxaa);
	if (isSmaa) {
		SMAA::ExternalStorage storage(sharedBuffer1Tex, sharedBuffer1Surf, sharedBuffer2Tex, sharedBuffer2Surf);
		smaa = new SMAA(d3ddev, Settings::get().getRenderWidth(), Settings::get().getRenderHeight(), (SMAA::Preset)aaIdx, storage);
	}
	else {
		fxaa = new FXAA(d3ddev, Settings::get().getRenderWidth(), Settings::get().getRenderHeight(), (FXAA::Quality)aaIdx, sharedBuffer1Tex, sharedBuffer1Surf);
	}
	SDLOG(0, "Reloaded AA\n");
}


HRESULT RSManager::redirectDrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinIndex, UINT NumVertices, UINT PrimitiveCount, CONST void* pIndexData, D3DFORMAT IndexDataFormat, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride) {
	//if(onHudRT) {
	//	SDLOG(0, "HUD render: primitive type: %d, MinIndex: %u, NumVertices: %u, PrimitiveCount: %u, IndexDataFormat: %s\n", PrimitiveType, MinIndex, NumVertices, PrimitiveCount, D3DFormatToString(IndexDataFormat));

	//	// Print vertex decl type
	//	IDirect3DVertexDeclaration9* vDecl;
	//	d3ddev->GetVertexDeclaration(&vDecl);
	//	D3DVERTEXELEMENT9 decl[MAXD3DDECLLENGTH];
	//	UINT numElements;
	//	vDecl->GetDeclaration(decl, &numElements);
	//	for(size_t i=0; i<numElements; ++i) {
	//		SDLOG(0, "element %u -- stream: %d, offset: %d, type: %s, usage: %s\n", i, decl->Stream, decl->Offset, D3DDeclTypeToString((D3DDECLTYPE)decl->Type), D3DDeclUsageToString((D3DDECLUSAGE)decl->Usage));
	//	}
	//	vDecl->Release();
	//	
	//	// Print indices
	//	//SDLOG(0, "Indices: \n");
	//	//INT16 *indices = (INT16*)pIndexData;
	//	//for(size_t i=0; i<PrimitiveCount+2; ++i) {
	//	//	SDLOG(0, "%8hd, ", indices[i]);
	//	//}
	//	//SDLOG(0, "\n");

	//	// Print vertices
	//	SDLOG(0, "Vertices: \n");
	//	INT16 *values = (INT16*)pVertexStreamZeroData;
	//	for(size_t i=0; i<NumVertices*4*2; ++i) {
	//		SDLOG(0, "%8hd, ", values[i]);
	//		if((i+1)%2 == 0) SDLOG(0, "; ");
	//		if((i+1)%8 == 0) SDLOG(0, "\n");
	//	}

	//	//return d3ddev->DrawIndexedPrimitiveUP(PrimitiveType, MinIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, hudVertices, VertexStreamZeroStride);
	//}
	if (hudStarted && hideHud) {
		return D3D_OK;
	}
	// title screen mark: the title screen is recognised by the pieces of its copyright line
	if (VertexStreamZeroStride == 12 && PrimitiveCount == 2 && NumVertices == 4 && pVertexStreamZeroData) {
		float tb[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		classifyHudQuad((const INT16*)pVertexStreamZeroData, VertexStreamZeroStride, MinIndex, NumVertices, NULL, tb);
		titleMarkOnQuad(tb);
	}
	// weaponWearBars: the arrow / bolt boxes under a bow or crossbow slot move down below the bar (a moved copy is drawn)
	std::vector<BYTE> ammoCopy;
	if (weaponWearBarsActive() && VertexStreamZeroStride == 12 && pVertexStreamZeroData && NumVertices && NumVertices <= 4096) {
		float ab[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		classifyHudQuad((const INT16*)pVertexStreamZeroData, VertexStreamZeroStride, MinIndex, NumVertices, NULL, ab);
		IDirect3DBaseTexture9* at = NULL;
		d3ddev->GetTexture(0, &at);
		const bool atFrame = at && isTextureHudHealthbar(at);
		if (at) at->Release();
		const int shift = weaponWearBarsAmmoShift(ab, atFrame);
		if (shift) {
			ammoCopy.assign((const BYTE*)pVertexStreamZeroData, (const BYTE*)pVertexStreamZeroData + (size_t)(MinIndex + NumVertices) * 12);
			for (UINT i = MinIndex; i < MinIndex + NumVertices; ++i) {
				INT16* y = (INT16*)(&ammoCopy[0] + (size_t)i * 12 + 2);
				*y = (INT16)(*y + shift);
			}
			pVertexStreamZeroData = &ammoCopy[0];
		}
	}
	// Fallback: the render-pass counter (rddp) that normally detects the start of the HUD gets confused by some
	// scenes (e.g. looking at the far horizon), and then the whole HUD would stay at scale 1. In that case start the
	// HUD window here, at the first HUD draw of the frame (the item-cluster backdrop quad, always drawn first).
	if (hud && doHud && !g_hudWindowSeen && !onHudRT && !pausedHudRT && PrimitiveCount == 2 && VertexStreamZeroStride == 12 && pVertexStreamZeroData) {
		const INT16* sv = (const INT16*)pVertexStreamZeroData;
		if (abs((int)sv[0] - 695) <= 64 && abs((int)sv[1] - 3256) <= 64) {
			IDirect3DBaseTexture9* ft = NULL;
			d3ddev->GetTexture(0, &ft);
			const bool firstHudDraw = ft && isTextureHudHealthbar(ft);
			if (ft) ft->Release();
			IDirect3DSurface9* cur = NULL;
			if (firstHudDraw && SUCCEEDED(d3ddev->GetRenderTarget(0, &cur)) && cur) {
				IDirect3DTexture9* ctex = getSurfTexture(cur);
				D3DSURFACE_DESC desc;
				cur->GetDesc(&desc);
				if (ctex && isHudTargetSize(desc)) {
					SDLOG(1, "Starting HUD rendering (fallback)\n");
					hddp = 0;
					onHudRT = true;
					g_hudScalePhase = true;
					g_hudWindowSeen = true;
					g_lastHudGroup = HG_NONE;
					if (g_dumpActive) g_dumpLines.push_back("--- WINDOW START (fallback) ---");
					prevRenderTex = ctex;
					prevRenderTarget = cur;
					d3ddev->SetRenderTarget(0, g_hudBufSurf);
					d3ddev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_RGBA(0, 0, 0, 0), 0.0f, 0);
					d3ddev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
					d3ddev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_ADD);
					d3ddev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
					d3ddev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
				}
				if (ctex) ctex->Release();
				cur->Release();
			}
		}
	}
	bool isTargetIndicator = false;
	if (pausedHudRT) {
		IDirect3DBaseTexture9* t;
		d3ddev->GetTexture(0, &t);
		// check for target indicator
		if (isTextureHudHealthbar(t)) {
			INT16* vertices = (INT16*)pVertexStreamZeroData;
			if (vertices[3] > -2000) {
				resumeHudRendering();
			}
		}
		else {
			resumeHudRendering();
		}
		t->Release();
	}
	if (onHudRT) {
		IDirect3DBaseTexture9* t;
		d3ddev->GetTexture(0, &t);
		SDLOG(4, "On HUD, redirectDrawIndexedPrimitiveUP texture: %s\n", getTextureName(t));
		if ((hddp < 5 && isTextureHudHealthbar(t)) || (hddp >= 5 && hddp < 7 && isTextureCategoryIconsHumanityCount(t)) || (hddp >= 7 && !isTextureCategoryIconsHumanityCount(t))) hddp++;
		// check for target indicator
		if (isTextureHudHealthbar(t)) {
			INT16* vertices = (INT16*)pVertexStreamZeroData;
			if (vertices[3] < -2000) {
				isTargetIndicator = true;
				pauseHudRendering();
			}
		}
		t->Release();
		if (hddp == 8) {
			finishHudRendering();
		}
		else if (!isTargetIndicator) {
			//d3ddev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
			//d3ddev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_ADD);
			//d3ddev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
			//d3ddev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
		}
	}
	bool idxIsHealthbarTex = false;
	if (g_hudScalePhase) {
		// menu draws end the HUD scaling phase; they must never be scaled
		IDirect3DBaseTexture9* mt = NULL;
		d3ddev->GetTexture(0, &mt);
		if (mt && isTextureGuiElements1(mt)) {
			g_hudScalePhase = false;
			g_lastHudGroup = HG_NONE;
			if (g_dumpActive) g_dumpLines.push_back("--- PHASE END (menu texture) ---");
		}
		idxIsHealthbarTex = mt && isTextureHudHealthbar(mt);
		if (mt) mt->Release();
	}
	HudGroup hudGrp = HG_NONE;
	bool wideBottomOverlay = false;
	bool fullScreenFade = false;
	if (g_hudScalePhase && hud && doHud && pVertexStreamZeroData) {
		float qb[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		hudGrp = classifyHudQuad((const INT16*)pVertexStreamZeroData, VertexStreamZeroStride, MinIndex, NumVertices, NULL, qb);
		// The small right end cap of a long health / stamina bar ([0.615 0.100 0.620 0.128] with a high-HP character) lies
		// right of the top-left zone's x 0.60 limit and stayed at its unscaled place. A thin health-bar textured piece in the
		// rows of the bars belongs to its bar. (The quick menu's backdrop, the only other such quad up there, is wide.)
		if (hudGrp == HG_NONE && !pausedHudRT && idxIsHealthbarTex) {
			const float qw = qb[2] - qb[0], qh = qb[3] - qb[1];
			if (qw < 0.015f && qh < 0.05f && qb[1] > 0.05f && qb[3] < 0.20f && qb[0] > 0.30f && qb[2] < 0.85f) hudGrp = HG_TL;
		}
		// The dialogue vignette (a wide gradient over the lower half of the screen) must not go through the HUD buffer,
		// otherwise the corner opacity regions cut visible edges into it. (The full-screen fade-to-black is NOT affected.)
		wideBottomOverlay = (qb[2] - qb[0]) > 0.7f && (qb[3] - qb[1]) < 0.7f && (qb[1] + qb[3]) * 0.5f > 0.6f;
		// A full-screen quad after the HUD (fade from/to black after loading, death fade, ...). hddp >= 7 means the
		// health bars and humanity counter have already been drawn, i.e. the HUD itself is done.
		fullScreenFade = (onHudRT || pausedHudRT) && hddp >= 7 && (qb[2] - qb[0]) > 0.95f && (qb[3] - qb[1]) > 0.95f;
		if (hudGrp == HG_BOSS && idxIsHealthbarTex && (qb[2] - qb[0]) > 0.3f && (qb[3] - qb[1]) < 0.03f) rememberBossRow(qb);
		if (pausedHudRT) {
			// The game also "pauses" for any semi-transparent health-bar textured quad. That covers world-anchored things
			// (lock-on marker, enemy health bars: must NOT be scaled) but also HUD pieces of our own: the boss bar fill and the
			// golden flash of the item squares. Tell them apart by size/position.
			const float qw = qb[2] - qb[0], qh = qb[3] - qb[1];
			bool own = false;
			if (hudGrp == HG_BOSS) own = qw > 0.3f || onBossRow(qb);   // wide boss bar, or a fill in a boss bar's row
			else if (hudGrp == HG_BL) own = qw >= 0.03f && qb[0] > 0.05f && qb[2] < 0.31f && qb[1] > 0.49f && qb[3] < 0.97f; // item cluster: flash squares, fading frames, underlines (lock-on marker is ~0.026 wide)
			else if (hudGrp == HG_BR) own = qw > 0.09f && qb[0] > 0.75f; // souls box
			else if (hudGrp == HG_TL) own = qb[0] > 0.05f && qb[0] < 0.16f; // the player's own bars start at x ~ 0.137
			if (!own) hudGrp = HG_NONE;
		}
		// the boss zone only ever scales health-bar textured quads (not dialogue/prompt boxes)
		if (hudGrp == HG_BOSS && !idxIsHealthbarTex) hudGrp = HG_NONE;
		g_lastHudGroup = hudGrp;
	}
	// windows (main menu, list windows, prompts): drawn after the HUD, never part of it
	int winK = -1;
	// "in game": a HUD window in this frame or in one of the last frames (the quick menu can come before the HUD)
	const bool hudDoneNow = g_hudWindowSeen && !onHudRT && !pausedHudRT;
	if (hud && doHud && (g_hudWindowSeen || g_framesSinceHud < 30) && VertexStreamZeroStride == 12 && pVertexStreamZeroData) {
		float wb[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		bool wok = false;
		classifyHudQuad((const INT16*)pVertexStreamZeroData, VertexStreamZeroStride, MinIndex, NumVertices, &wok, wb);
		if (wok && PrimitiveCount == 2) {
			IDirect3DBaseTexture9* wt = NULL;
			d3ddev->GetTexture(0, &wt);
			if (wt) detectWindow(wb, wt, isTextureHudHealthbar(wt), isTextureGuiElements1(wt), hudDoneNow);
			if (wt) wt->Release();
		}
		// the "View" explanation darkens the screen with quads reaching far off-screen (not "valid" UI boxes); inside the
		// open menu they are menu pieces like everything else
		if (NumVertices && (wok || g_win[WK_PANEL].active || isWholeScreen(wb))) {
			// opaque = every vertex at least ~94 % alpha (D3DCOLOR at byte 4, alpha in its top byte)
			bool opaque = isWholeScreen(wb);
			const UINT n = NumVertices > 64 ? 64 : NumVertices;
			for (UINT i = MinIndex; opaque && i < MinIndex + n; ++i) {
				const BYTE* c = (const BYTE*)pVertexStreamZeroData + (size_t)i * 12 + 4;
				if (c[3] < 0xF0) opaque = false;
			}
			winK = windowFor(wb, hudDoneNow, opaque);
		}
		if (winK >= 0) {
			hudGrp = HG_NONE;
			g_lastHudGroup = HG_NONE;
		}
	}
	if (g_dumpActive) {
		IDirect3DBaseTexture9* dt = NULL;
		d3ddev->GetTexture(0, &dt);
		dumpHudDraw("IDX", onHudRT ? "WINDOW" : (pausedHudRT ? "PAUSED" : "DIRECT"), dumpTexInfo(dt, dt ? getTextureName(dt) : "none").c_str(), winK >= 0 ? 10 + winK : (int)hudGrp, PrimitiveCount, VertexStreamZeroStride, MinIndex, NumVertices, pVertexStreamZeroData);
		// menu-open capture: does this frame show a menu? (GuiElements1 or menu icon quads in the top-right corner)
		if ((g_menuCapArmFrames > 0 || g_menuCapLeft > 0) && dt && VertexStreamZeroStride == 12 && pVertexStreamZeroData && NumVertices) {
			float mb[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			classifyHudQuad((const INT16*)pVertexStreamZeroData, VertexStreamZeroStride, MinIndex, NumVertices, NULL, mb);
			UINT mw, mh;
			const bool menuTex = isTextureGuiElements1(dt) || (texSize(dt, mw, mh) && mw == 512 && mh == 256);
			const float mcx = (mb[0] + mb[2]) * 0.5f, mcy = (mb[1] + mb[3]) * 0.5f;
			if (menuTex && mcx > 0.60f && mcx < 1.35f && mcy > -0.30f && mcy < 0.32f) g_frameHadMenu = true;
		}
		if (dt) dt->Release();
	}
	// The boss bar (frame, ruler and fill) and the souls box are drawn directly onto the frame, in the game's own
	// order, instead of through the HUD buffer. That keeps them identical to vanilla (no fading with the corner opacity
	// settings, no separate fade on death) and keeps the souls text on top of its box.
	if (fullScreenFade) {
		// In vanilla the fade covers the whole HUD. It used to be drawn into the HUD buffer instead, where it covered only the
		// buffered parts: the directly drawn text showed through it on the loading screen (whenever a corner opacity was
		// below 1), and the HUD flickered while the fade-in after loading ran. So the HUD buffer is composited now and the
		// fade is drawn on top of everything, like in vanilla. Everything after it (loading screen) is not HUD: no scaling.
		if (g_dumpActive) g_dumpLines.push_back("--- FULL-SCREEN FADE: HUD finished before it ---");
		if (pausedHudRT) resumeHudRendering();
		finishHudRendering();
		pausedHudRT = false;
		g_hudScalePhase = false;
		g_lastHudGroup = HG_NONE;
		hudGrp = HG_NONE;
	}
	bool routedDirect = false;
	if (onHudRT && (hudGrp == HG_BOSS || hudGrp == HG_BR || wideBottomOverlay)) {
		pauseHudRendering();
		routedDirect = true;
	}
	D3DVIEWPORT9 savedVp;
	const bool vpChanged = applyHudViewport(d3ddev, hudGrp, savedVp); // no-op for window draws (hudGrp is HG_NONE)
	// window draws: vertices moved toward the window's anchor (and the scissor rectangle with them)
	std::vector<BYTE> wcopy;
	const void* srcData = pVertexStreamZeroData;
	RECT savedSc;
	bool scChanged = false;
	if (winK >= 0) {
		const float ws = windowScale(winK);
		srcData = scaleUiVertices(wcopy, pVertexStreamZeroData, VertexStreamZeroStride, MinIndex, NumVertices, ws, g_win[winK].ax, g_win[winK].ay);
		scChanged = scaleWindowScissor(d3ddev, ws, g_win[winK].ax, g_win[winK].ay, savedSc);
	}
	HRESULT hr;
	{
		// draws that bypass the HUD buffer get their corner's opacity here (buffered ones get it when the buffer is composited)
		const float op = (!onHudRT && hudGrp != HG_NONE && VertexStreamZeroStride == 12) ? hudGroupOpacity(hudGrp) : 1.0f;
		std::vector<BYTE> vcopy;
		const void* vdata = applyVertexOpacity(vcopy, srcData, VertexStreamZeroStride, MinIndex, NumVertices, 4, op, d3ddev);
		AlphaRefGuard arg(d3ddev, op);
		hr = d3ddev->DrawIndexedPrimitiveUP(PrimitiveType, MinIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, vdata, VertexStreamZeroStride);
	}
	// weaponWearBars: watches the weapon slots' pictures and frames; right after a frame it draws the bar under it, into the
	// same target with the same (possibly bottom-left scaled) viewport, and with the same opacity this draw got
	if (weaponWearBarsActive() && winK < 0 && VertexStreamZeroStride == 12 && PrimitiveCount == 2 && NumVertices == 4 && pVertexStreamZeroData) {
		float db[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		classifyHudQuad((const INT16*)pVertexStreamZeroData, VertexStreamZeroStride, MinIndex, NumVertices, NULL, db);
		const BYTE vertexAlpha = ((const BYTE*)pVertexStreamZeroData + (size_t)MinIndex * 12 + 4)[3];
		IDirect3DBaseTexture9* dbt = NULL;
		d3ddev->GetTexture(0, &dbt);
		const bool frameTex = dbt && isTextureHudHealthbar(dbt);
		if (dbt) dbt->Release();
		const float dbOp = (!onHudRT && hudGrp != HG_NONE) ? hudGroupOpacity(hudGrp) : 1.0f;
		weaponWearBarsOnHudQuad(d3ddev, db, frameTex, vertexAlpha, dbOp, onHudRT);
	}
	if (scChanged) d3ddev->SetScissorRect(&savedSc);
	if (vpChanged) d3ddev->SetViewport(&savedVp);
	if (routedDirect) resumeHudRendering();
	//if(onHudRT) {
	//	if(takeScreenshot) dumpSurface("HUD_IndexPrimUP", rgbaBuffer1Surf);
	//}
	return hr;
}

HRESULT RSManager::redirectDrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride) {
	if (hudStarted && hideHud) {
		IDirect3DBaseTexture9* t;
		d3ddev->GetTexture(0, &t);
		bool hide = isTextureText(t);
		hide = hide || isTextureButtonsEffects(t);
		hide = hide || isTextureHudEffectIcons(t);
		t->Release();
		if (hide) return D3D_OK;
	}
	// weaponWearBars: the count number of an arrow / bolt box moves down with its box (see redirectDrawIndexedPrimitiveUP)
	std::vector<BYTE> ammoCopy;
	if (weaponWearBarsActive() && VertexStreamZeroStride == 24 && pVertexStreamZeroData) {
		UINT n = 0;
		if (PrimitiveType == D3DPT_TRIANGLELIST) n = PrimitiveCount * 3;
		else if (PrimitiveType == D3DPT_TRIANGLESTRIP || PrimitiveType == D3DPT_TRIANGLEFAN) n = PrimitiveCount + 2;
		float x0, y0, x1, y1;
		if (n && n <= 4096 && textBox(pVertexStreamZeroData, 24, n, x0, y0, x1, y1)) {
			const float ab[4] = { x0 / 1280.0f, y0 / 720.0f, x1 / 1280.0f, y1 / 720.0f };
			const int shift = weaponWearBarsAmmoShift(ab, false);
			if (shift) {
				ammoCopy.assign((const BYTE*)pVertexStreamZeroData, (const BYTE*)pVertexStreamZeroData + (size_t)n * 24);
				for (UINT i = 0; i < n; ++i) ((float*)(&ammoCopy[0] + (size_t)i * 24))[1] += (float)shift / 8.0f;
				pVertexStreamZeroData = &ammoCopy[0];
			}
		}
	}
	if (pausedHudRT) {
		IDirect3DBaseTexture9* t;
		d3ddev->GetTexture(0, &t);
		bool isText = isTextureText(t);
		SDLOG(4, "On HUD, PAUSED, redirectDrawPrimitiveUP texture: %s\n", getTextureName(t));
		t->Release();
		//// Print vertices
		//SDLOG(0, "Vertices: \n");
		//INT16 *values = (INT16*)pVertexStreamZeroData;
		//for(size_t i=0; i<PrimitiveCount+2; ++i) {
		//	SDLOG(0, "%8hd, ", values[i]);
		//	if((i+1)%2 == 0) SDLOG(0, "; ");
		//	if((i+1)%8 == 0) SDLOG(0, "\n");
		//}
		if (isText && PrimitiveCount >= 12) resumeHudRendering();
	}
	bool subbed = false;
	if (onHudRT) {
		IDirect3DBaseTexture9* t;
		d3ddev->GetTexture(0, &t);
		bool isText = isTextureText(t);
		bool isSub = isTextureText00(t);
		SDLOG(4, "On HUD, redirectDrawPrimitiveUP texture: %s\n", getTextureName(t));
		t->Release();
		//if(isText && PrimitiveCount <= 10) pauseHudRendering();
		if (isSub) {
			pauseHudRendering();
			subbed = true;
		}
	}
	// Text / button glyphs (vertex format: float x,y,z in 1280x720 UI pixels, colour, u, v; stride 24).
	// They are moved to where their element ended up, but drawn at hudTextScale so they stay sharp.
	HudGroup hudGrp = HG_NONE;
	UINT primNv = 0;   // vertex count of this draw
	if (PrimitiveType == D3DPT_TRIANGLELIST) primNv = PrimitiveCount * 3;
	else if (PrimitiveType == D3DPT_TRIANGLESTRIP || PrimitiveType == D3DPT_TRIANGLEFAN) primNv = PrimitiveCount + 2;
	std::vector<BYTE> textCopy;
	CONST void* drawData = pVertexStreamZeroData;
	bool textDirect = false;
	// pieces and text of a window (main menu, list window, prompt) detected earlier in this frame
	int winK = -1;
	const bool hudDoneNow = g_hudWindowSeen && !onHudRT && !pausedHudRT;
	if (hud && doHud && primNv && pVertexStreamZeroData && anyWindowActive()) {
		float wb[4];
		bool wok = false;
		if (VertexStreamZeroStride == 24) {
			float x0, y0, x1, y1;
			if (textBox(pVertexStreamZeroData, VertexStreamZeroStride, primNv, x0, y0, x1, y1)) {
				wb[0] = x0 / 1280.0f; wb[1] = y0 / 720.0f; wb[2] = x1 / 1280.0f; wb[3] = y1 / 720.0f;
				wok = true;
			}
		}
		else if (VertexStreamZeroStride == 12) {
			classifyHudQuad((const INT16*)pVertexStreamZeroData, VertexStreamZeroStride, 0, primNv, &wok, wb);
			if (g_win[WK_PANEL].active) wok = true; // off-screen pieces of the open menu too
		}
		if (wok) winK = windowFor(wb, hudDoneNow, false);
	}
	if (winK < 0 && g_hudScalePhase && hud && doHud) {
		IDirect3DBaseTexture9* tt = NULL;
		d3ddev->GetTexture(0, &tt);
		const bool sub = tt && isTextureText00(tt);
		const bool menu = tt && isTextureGuiElements1(tt);
		const bool isTxt = tt && isTextureText(tt);
		if (tt) tt->Release();
		if (!sub && !menu && primNv && pVertexStreamZeroData) {
			bool placed = false;
			if (VertexStreamZeroStride == 24) {
				float x0, y0, x1, y1;
				if (textBox(pVertexStreamZeroData, VertexStreamZeroStride, primNv, x0, y0, x1, y1)) {
					const float w = (x1 - x0) / 1280.0f, h = (y1 - y0) / 720.0f;
					HudGroup g = zoneFor((x0 + x1) * 0.5f / 1280.0f, (y0 + y1) * 0.5f / 720.0f, w, h);
					// A long label on the left side (an item or spell name) reaches far enough to the right that the
					// MIDDLE of the text leaves the bottom-left zone, even though the text starts well inside it.
					// It was then taken for boss-zone text and scaled toward the middle of the screen, or for nothing
					// at all and left unscaled. Seen with "Large Soul of a Proud Knight": its middle lands at 0.301
					// against a zone that ends at 0.300. Names are longer in German and Russian, so this is easy to
					// hit there. These labels always start at the same left edge, so for text the LEFT EDGE decides,
					// not the middle: a label that starts inside the bottom-left zone belongs to it however long it
					// grows. Quads are not affected, only text, and the boss name is drawn centred around x 0.59, so
					// it never starts that far left.
					// The size test from zoneFor is repeated here on purpose: without it this rule could take hold of
					// a wide or tall piece of text that zoneFor turns down as a fade or a full-screen overlay.
					if (w <= 0.7f && h <= 0.45f) {
						const HudLayout TL = getHudLayout();
						const float textLeft = x0 / 1280.0f, textMidY = (y0 + y1) * 0.5f / 720.0f;
						if (g != HG_BL && textMidY > TL.blTop && textLeft < TL.blW) g = HG_BL;
					}
					// boss-zone text (boss name) only counts inside the HUD buffer window, never dialogue/prompts
					if (g == HG_BOSS && !onHudRT) g = HG_NONE;
					// while paused (fading item frames): only the cluster / souls texts count, never enemy names or damage numbers
					if (pausedHudRT && g != HG_BL && g != HG_BR) g = HG_NONE;
					hudGrp = g;
					placed = true;
					// Font glyphs are drawn directly onto the frame (like the subtitles). Going through the HUD buffer made
					// them noticeably thinner than vanilla.
					if (isTxt && onHudRT && (g == HG_BL || g == HG_BR || g == HG_BOSS)) textDirect = true;
					if (hudGroupScale(g) < 1.0f) {
						// counts are right-aligned numbers, item names are left-aligned, the souls number is right-aligned
						int align = 1;
						if (g == HG_BR) align = 2;
						else if (g == HG_BL && (x1 - x0) < 45.0f) align = 2;
						transformHudText(textCopy, pVertexStreamZeroData, VertexStreamZeroStride, primNv, g, x0, y0, x1, y1, align);
						drawData = &textCopy[0];
					}
				}
			}
			else if (VertexStreamZeroStride == 12) {
				bool ok = false;
				const HudGroup g = classifyHudQuad((const INT16*)pVertexStreamZeroData, VertexStreamZeroStride, 0, primNv, &ok);
				if (ok) { hudGrp = g; placed = true; }
			}
			// unknown vertex format: follow the element drawn just before it, only inside the HUD buffer window
			if (!placed) hudGrp = onHudRT ? g_lastHudGroup : HG_NONE;
		}
	}
	if (g_dumpActive) {
		IDirect3DBaseTexture9* dt = NULL;
		d3ddev->GetTexture(0, &dt);
		dumpHudDraw("PRIM", onHudRT ? "WINDOW" : (pausedHudRT ? "PAUSED" : "DIRECT"), dumpTexInfo(dt, dt ? getTextureName(dt) : "none").c_str(), winK >= 0 ? 10 + winK : (int)hudGrp, PrimitiveCount, VertexStreamZeroStride, 0, primNv, pVertexStreamZeroData, PrimitiveType == D3DPT_TRIANGLELIST ? 6 : 4);
		if (dt) dt->Release();
	}
	D3DVIEWPORT9 savedVp;
	// float HUD text was already moved above; other formats (unknown/INT16) still use the viewport transform
	const bool vpChanged = (VertexStreamZeroStride == 24 || winK >= 0) ? false : applyHudViewport(d3ddev, hudGrp, savedVp);
	RECT savedSc;
	bool scChanged = false;
	if (winK >= 0) {
		// window pieces and window text: vertices moved toward the window's anchor
		const float ws = windowScale(winK);
		drawData = scaleUiVertices(textCopy, pVertexStreamZeroData, VertexStreamZeroStride, 0, primNv, ws, g_win[winK].ax, g_win[winK].ay);
		scChanged = scaleWindowScissor(d3ddev, ws, g_win[winK].ax, g_win[winK].ay, savedSc);
	}
	else if (hud && doHud && VertexStreamZeroStride == 24 && primNv && pVertexStreamZeroData && subtitleScale() < 1.0f) {
		// subtitles (Text00 font in the lower part of the screen) shrink toward the bottom centre
		IDirect3DBaseTexture9* st = NULL;
		d3ddev->GetTexture(0, &st);
		const bool isSubText = st && isTextureText00(st);
		if (st) st->Release();
		float x0, y0, x1, y1;
		if (isSubText && textBox(pVertexStreamZeroData, VertexStreamZeroStride, primNv, x0, y0, x1, y1) &&
			(y0 + y1) * 0.5f / 720.0f > 0.70f && (x1 - x0) / 1280.0f < 0.98f) {
			drawData = scaleUiVertices(textCopy, pVertexStreamZeroData, VertexStreamZeroStride, 0, primNv, subtitleScale(), 0.5f, 0.925f);
		}
	}
	const bool swapAB = Settings::get().getSwapConfirmIcons(), swapXY = Settings::get().getSwapXYIcons();
	if ((swapAB || swapXY) && VertexStreamZeroStride == 24 && primNv && primNv <= 65536 && drawData) {
		IDirect3DBaseTexture9* bt = NULL;
		d3ddev->GetTexture(0, &bt);
		const bool isButton = bt && isTextureButtonsEffects(bt);
		if (bt) bt->Release();
		if (isButton) {
			if (textCopy.empty() || drawData != (const void*)&textCopy[0]) {
				std::vector<BYTE> tmp((const BYTE*)drawData, (const BYTE*)drawData + (size_t)primNv * 24);
				textCopy.swap(tmp);
				drawData = &textCopy[0];
			}
			swapButtonIcons(&textCopy[0], primNv, PrimitiveType, swapAB, swapXY);
		}
	}
	if (textDirect) pauseHudRendering();
	HRESULT hr;
	{
		// text drawn directly onto the frame gets its corner's opacity through the vertex colour
		float op = (!onHudRT && hudGrp != HG_NONE && primNv) ? hudGroupOpacity(hudGrp) : 1.0f;
		UINT colOff = 0;
		if (op < 0.999f) {
			if (VertexStreamZeroStride == 24) colOff = textColorOffset(drawData);
			else if (VertexStreamZeroStride == 12) colOff = 4;
			if (!colOff) op = 1.0f;
		}
		if (op < 0.999f) drawData = applyVertexOpacity(textCopy, drawData, VertexStreamZeroStride, 0, primNv, colOff, op, d3ddev);
		AlphaRefGuard arg(d3ddev, op);
		hr = d3ddev->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, drawData, VertexStreamZeroStride);
	}
	if (textDirect) resumeHudRendering();
	if (scChanged) d3ddev->SetScissorRect(&savedSc);
	if (vpChanged) d3ddev->SetViewport(&savedVp);
	if (subbed) resumeHudRendering();
	//if(onHudRT) {
	//	if(takeScreenshot) dumpSurface("HUD_PrimUP", rgbaBuffer1Surf);
	//}
	return hr;
}

void RSManager::reloadHudVertices() {
	SDLOG(0, "Reloading HUD vertices\n");
	std::ifstream vfile;
	vfile.open(GetDirectoryFile("hudvertices.txt"), std::ios::in);
	char buffer[256];
	size_t index = 0;
	SDLOG(0, "- starting\n");
	while (!vfile.eof()) {
		vfile.getline(buffer, 256);
		if (buffer[0] == '#') continue;
		if (vfile.gcount() <= 4) continue;

		sscanf(buffer, "%hd %hd", &hudVertices[index], &hudVertices[index + 1]);
		SDLOG(0, "- read %hd, %hd\n", hudVertices[index], hudVertices[index + 1]);
		index += 2;
	}
	vfile.close();
}

bool RSManager::isTextureText(IDirect3DBaseTexture9* t) {
	return isTextureText00(t) || isTextureText01(t) || isTextureText02(t) || isTextureText03(t)
		|| isTextureText04(t) || isTextureText05(t) || isTextureText06(t) || isTextureText07(t)
		|| isTextureText08(t) || isTextureText09(t) || isTextureText10(t) || isTextureText11(t)
		|| isTextureText12(t);
}

unsigned RSManager::isDof(unsigned width, unsigned height) {
	unsigned dofRes = Settings::get().getDOFOverrideResolution();
	unsigned topWidth = dofRes == 0 ? 512 : dofRes * 16 / 9;
	unsigned topHeight = dofRes == 0 ? 360 : dofRes;
	if (width == topWidth && height == topHeight) return 1;
	if (width == topWidth / 2 && height == topHeight / 2) return 2;
	return 0;
}

extern HMODULE g_hModule; // Bring in the module handle from main.cpp

// ---- controllerIcons: button prompt textures for the connected controller ----
// The two controller icon textures (40fbc4ad, the game's ButtonsEffects, and 43a2b23a) are built into the DLL in one
// set per controller type. The font textures (db8a58fa / f9d8db89) are not touched by this.
// With controllerIcons auto, the type is found once, the first time the game loads one of them (on the game's own
// thread, never in DllMain): the HID game controllers Windows knows (Raw Input device list, by vendor: Sony,
// Nintendo, Microsoft), then XInput as a last resort. DirectInput is not used at all, so it works the
// same with or without a dinput8 wrapper chained by DSfix. With a type set (auto or forced), files with those two
// names in tex_override are ignored; with off, or with auto and no controller found, everything works as before.
// The icon sets must keep the layout of the original textures (the game picks each icon by its position).
namespace {
	enum PadType { PAD_NONE = 0, PAD_XBOX, PAD_PLAYSTATION, PAD_SWITCH };
	const char* padName(int t) {
		return t == PAD_XBOX ? "Xbox" : t == PAD_PLAYSTATION ? "PlayStation" : t == PAD_SWITCH ? "Switch Pro" : "none";
	}

	bool xinputConnected() {
		typedef DWORD(WINAPI* XInputGetState_t)(DWORD, void*);
		static const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
		for (int d = 0; d < 3; ++d) {
			HMODULE h = LoadLibraryA(dlls[d]);
			if (!h) continue;
			XInputGetState_t get = (XInputGetState_t)GetProcAddress(h, "XInputGetState");
			bool any = false;
			if (get) {
				BYTE state[16]; // XINPUT_STATE is 16 bytes
				for (DWORD i = 0; i < 4 && !any; ++i) any = get(i, state) == ERROR_SUCCESS;
			}
			FreeLibrary(h);
			if (get) return any;
		}
		return false;
	}

	int detectPadType() {
		bool sony = false, nintendo = false, microsoft = false;
		UINT n = 0;
		if (GetRawInputDeviceList(NULL, &n, sizeof(RAWINPUTDEVICELIST)) == 0 && n > 0 && n < 512) {
			std::vector<RAWINPUTDEVICELIST> list(n);
			const UINT got = GetRawInputDeviceList(list.data(), &n, sizeof(RAWINPUTDEVICELIST));
			for (UINT i = 0; i < got && got != (UINT)-1; ++i) {
				if (list[i].dwType != RIM_TYPEHID) continue;
				RID_DEVICE_INFO info = {};
				info.cbSize = sizeof(info);
				UINT sz = sizeof(info);
				if (GetRawInputDeviceInfoA(list[i].hDevice, RIDI_DEVICEINFO, &info, &sz) == (UINT)-1) continue;
				const RID_DEVICE_INFO_HID& hid = info.hid;
				if (hid.usUsagePage != 0x01 || (hid.usUsage != 0x04 && hid.usUsage != 0x05)) continue; // joystick / gamepad
				if (hid.dwVendorId == 0x054C) sony = true;
				else if (hid.dwVendorId == 0x057E) nintendo = true;
				else if (hid.dwVendorId == 0x045E) microsoft = true;
				SDLOG(0, "controllerIcons: game controller %04X:%04X\n", hid.dwVendorId, hid.dwProductId);
			}
		}
		// a Sony or Nintendo pad wins over an Xbox one: tools like Steam Input add a virtual Xbox pad next to it
		if (sony) return PAD_PLAYSTATION;
		if (nintendo) return PAD_SWITCH;
		if (microsoft || xinputConnected()) return PAD_XBOX;
		return PAD_NONE;
	}

	// -1 = setting off, otherwise the PadType (PAD_NONE: auto found no controller, nothing is replaced)
	int controllerIconType() {
		static int type = -2;
		if (type != -2) return type;
		const std::string& v = Settings::get().getControllerIcons();
		if (v.find("auto") == 0) type = detectPadType();
		else if (v.find("xbox") == 0) type = PAD_XBOX;
		else if (v.find("playstation") == 0) type = PAD_PLAYSTATION;
		else if (v.find("switch") == 0) type = PAD_SWITCH;
		else type = -1;
		if (type >= 0) SDLOG(0, "controllerIcons %s: using the %s icons\n", v.c_str(), padName(type));
		return type;
	}

	const UINT32 kIconTexA = 0x40fbc4ad, kIconTexB = 0x43a2b23a;
	int iconResource(UINT32 hash, int type) {
		const bool a = hash == kIconTexA;
		switch (type) {
		case PAD_XBOX:        return a ? IDR_TEX_XBOX_40FBC4AD : IDR_TEX_XBOX_43A2B23A;
		case PAD_PLAYSTATION: return a ? IDR_TEX_PS_40FBC4AD : IDR_TEX_PS_43A2B23A;
		case PAD_SWITCH:      return a ? IDR_TEX_SWITCH_40FBC4AD : IDR_TEX_SWITCH_43A2B23A;
		default:              return 0;
		}
	}

	HRESULT loadEmbeddedTexture(int resourceId, LPDIRECT3DDEVICE9 pDevice, UINT MipLevels, DWORD Usage, D3DPOOL Pool, DWORD Filter, DWORD MipFilter, D3DCOLOR ColorKey, D3DXIMAGE_INFO* pSrcInfo, PALETTEENTRY* pPalette, LPDIRECT3DTEXTURE9* ppTexture) {
		HRSRC hRes = FindResource(g_hModule, MAKEINTRESOURCE(resourceId), RT_RCDATA);
		if (!hRes) return E_FAIL;
		HGLOBAL hData = LoadResource(g_hModule, hRes);
		const DWORD resSize = SizeofResource(g_hModule, hRes);
		void* pResData = hData ? LockResource(hData) : NULL;
		if (!pResData || resSize == 0) return E_FAIL;
		return TrueD3DXCreateTextureFromFileInMemoryEx(pDevice, pResData, resSize, D3DX_DEFAULT, D3DX_DEFAULT, MipLevels, Usage, D3DFMT_FROM_FILE, Pool, Filter, MipFilter, ColorKey, pSrcInfo, pPalette, ppTexture);
	}
}

HRESULT RSManager::redirectD3DXCreateTextureFromFileInMemoryEx(LPDIRECT3DDEVICE9 pDevice, LPCVOID pSrcData, UINT SrcDataSize, UINT Width, UINT Height, UINT MipLevels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, DWORD Filter, DWORD MipFilter, D3DCOLOR ColorKey, D3DXIMAGE_INFO* pSrcInfo, PALETTEENTRY* pPalette, LPDIRECT3DTEXTURE9* ppTexture) {
	const bool iconsOn = Settings::get().getControllerIcons().find("off") != 0;
	const bool overrideOn = Settings::get().getEnableTextureOverride();
	const UINT32 texHash = (iconsOn || overrideOn) ? SuperFastHash((char*)const_cast<void*>(pSrcData), SrcDataSize) : 0;

	// controllerIcons: the two icon textures come from the set of the detected controller, before tex_override
	if (iconsOn && (texHash == kIconTexA || texHash == kIconTexB)) {
		const int res = iconResource(texHash, controllerIconType());
		if (res != 0) {
			const HRESULT hr = loadEmbeddedTexture(res, pDevice, MipLevels, Usage, Pool, Filter, MipFilter, ColorKey, pSrcInfo, pPalette, ppTexture);
			if (SUCCEEDED(hr)) return hr;
		}
		// no controller found (or the set is missing from the DLL): as before
	}

	if (overrideOn) {
		UINT32 hash = texHash;
		SDLOG(4, "Trying texture override size: %8u, hash: %8x\n", SrcDataSize, hash);

		// 1. Check Prefetch Cache (loads from tex_override on boot)
		if (Settings::get().getEnableTexturePrefetch() && cachedTexFiles.find(hash) != cachedTexFiles.end())
		{
			SDLOG(4, "Cached texture file found! size: %ld, hash: %8x \n", cachedTexFiles[hash].size, hash);
			return TrueD3DXCreateTextureFromFileInMemoryEx(pDevice, cachedTexFiles[hash].buffer, cachedTexFiles[hash].size, D3DX_DEFAULT, D3DX_DEFAULT, MipLevels, Usage, D3DFMT_FROM_FILE, Pool, Filter, MipFilter, ColorKey, pSrcInfo, pPalette, ppTexture);
		}
		else
		{
			// 2. Check Disk for User Overrides (.png and .dds)
			char buffer[128];
			sprintf_s(buffer, "dsfix/tex_override/%08x.png", hash);
			if (fileExists(buffer)) {

				SDLOG(4, "Texture override (png)! hash: %08x\n", hash);
				return D3DXCreateTextureFromFileEx(pDevice, buffer, D3DX_DEFAULT, D3DX_DEFAULT, MipLevels, Usage, D3DFMT_FROM_FILE, Pool, Filter, MipFilter, ColorKey, pSrcInfo, pPalette, ppTexture);
			}
			sprintf_s(buffer, "dsfix/tex_override/%08x.dds", hash);
			if (fileExists(buffer)) {

				SDLOG(4, "Texture override (dds)! hash: %08x\n", hash);
				return D3DXCreateTextureFromFileEx(pDevice, buffer, D3DX_DEFAULT, D3DX_DEFAULT, MipLevels, Usage, D3DFMT_FROM_FILE, Pool, Filter, MipFilter, ColorKey, pSrcInfo, pPalette, ppTexture);
			}
		}

		// 3. Fallback to Embedded DLL Textures
		int resourceId = 0;
		if (hash == 0xdb8a58fa) resourceId = IDR_TEX_DB8A58FA;
		else if (hash == 0xf9d8db89) resourceId = IDR_TEX_F9D8DB89;

		if (resourceId != 0) {
			HRSRC hRes = FindResource(g_hModule, MAKEINTRESOURCE(resourceId), RT_RCDATA);
			if (hRes) {
				HGLOBAL hData = LoadResource(g_hModule, hRes);
				DWORD resSize = SizeofResource(g_hModule, hRes);
				void* pResData = LockResource(hData);

				if (pResData && resSize > 0) {
					SDLOG(4, "Loaded embedded texture override for hash: %08x\n", hash);
					return TrueD3DXCreateTextureFromFileInMemoryEx(pDevice, pResData, resSize, D3DX_DEFAULT, D3DX_DEFAULT, MipLevels, Usage, D3DFMT_FROM_FILE, Pool, Filter, MipFilter, ColorKey, pSrcInfo, pPalette, ppTexture);
				}
			}
		}
	}

	// 4. Fallback to Original Game Texture
	return TrueD3DXCreateTextureFromFileInMemoryEx(pDevice, pSrcData, SrcDataSize, Width, Height, MipLevels, Usage, Format, Pool, Filter, MipFilter, ColorKey, pSrcInfo, pPalette, ppTexture);
}

void RSManager::storeRenderState() {
	if (prevStateBlock) prevStateBlock->Capture();
	prevVDecl = NULL;
	prevDepthStencilSurf = NULL;
	d3ddev->GetVertexDeclaration(&prevVDecl);
	d3ddev->GetDepthStencilSurface(&prevDepthStencilSurf);
	d3ddev->SetDepthStencilSurface(depthStencilSurf);
}

void RSManager::restoreRenderState() {
	if (prevVDecl) {
		d3ddev->SetVertexDeclaration(prevVDecl);
		prevVDecl->Release();
	}
	d3ddev->SetDepthStencilSurface(prevDepthStencilSurf); // also restore NULL!
	if (prevDepthStencilSurf) {
		prevDepthStencilSurf->Release();
	}
	if (prevStateBlock) prevStateBlock->Apply();
}

const char* RSManager::getTextureName(IDirect3DBaseTexture9* pTexture) {
#define TEXTURE(_name, _hash) \
	if(texture##_name == pTexture) return #_name;
#include "Textures.def"
#undef TEXTURE
	return "Unknown";
}

void RSManager::finishHudRendering() {
	SDLOG(2, "FinishHudRendering\n");
	g_lastHudGroup = HG_NONE; // note: the scaling phase itself continues until Present or the first menu draw
	if (g_dumpActive) g_dumpLines.push_back("--- FINISH (HUD buffer composited) ---");
	if (takeScreenshot) dumpSurface("HUD_end", g_hudBufSurf);
	d3ddev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
	d3ddev->SetRenderTarget(0, prevRenderTarget);
	onHudRT = false;
	// The HUD is done for this frame: nothing may resume drawing into its buffer any more. It could be finished while
	// "paused": in the second frame of the quick menu's grow-in, its backdrop (HudHealthbar texture, a vertex colour that
	// looks like the lock-on marker's) paused the buffer and finished the HUD in the same draw. The next menu piece then
	// resumed the finished buffer, the menu was drawn into it and the whole HUD was composited a second time (the
	// bottom-left items flashed once, brighter where their opacity is below 1).
	pausedHudRT = false;
	// draw HUD to screen
	storeRenderState();
	hud->go(g_hudBufTex, prevRenderTarget);
	restoreRenderState();
}

void RSManager::pauseHudRendering() {
	SDLOG(3, "PauseHudRendering\n");
	if (g_dumpActive) g_dumpLines.push_back("--- PAUSE ---");
	d3ddev->SetRenderTarget(0, prevRenderTarget);
	d3ddev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
	d3ddev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_BLENDTEXTUREALPHA);
	onHudRT = false;
	pausedHudRT = true;
}

void RSManager::resumeHudRendering() {
	SDLOG(3, "ResumeHudRendering\n");
	if (g_dumpActive) g_dumpLines.push_back("--- RESUME ---");
	d3ddev->SetRenderTarget(0, g_hudBufSurf);
	d3ddev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
	d3ddev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	onHudRT = true;
	pausedHudRT = false;
}

HRESULT RSManager::redirectSetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) {
	//if(allowStateChanges()) {
	return d3ddev->SetTextureStageState(Stage, Type, Value);
	//} else {
	//	SDLOG(3, "SetTextureStageState suppressed: %u  -  %u  -  %u\n", Stage, Type, Value);
	//}
	//return D3D_OK;
}

HRESULT RSManager::redirectSetRenderState(D3DRENDERSTATETYPE State, DWORD Value) {
	if (State == D3DRS_COLORWRITEENABLE && !allowStateChanges()) return D3D_OK;
	//if(allowStateChanges()) {
	return d3ddev->SetRenderState(State, Value);
	//} else {
	//	SDLOG(3, "SetRenderState suppressed: %u  -  %u\n", State, Value);
	//}
	//return D3D_OK;
}

HRESULT RSManager::redirectCreatePixelShader(CONST DWORD* pFunction, IDirect3DPixelShader9** ppShader)
{
	bool shouldDump = Settings::get().getEnableShaderDumping();
	bool shouldOverride = Settings::get().getEnableShaderOverride();
	LPD3DXBUFFER pAssemblerBuffer = nullptr;
	LPD3DXBUFFER pFunctionBuffer = nullptr;
	UINT32 hash;

	if (shouldDump || shouldOverride) {
		if (disassembleShader(pFunction, &pAssemblerBuffer)) {
			hash = SuperFastHash(static_cast<char*>(pAssemblerBuffer->GetBufferPointer()), pAssemblerBuffer->GetBufferSize());
		}
		else {
			shouldDump = false;
			shouldOverride = false;
		}
	}

	if (shouldDump) {
		dumpShader(hash, PIXEL_SHADER_DUMP_DIR, pAssemblerBuffer);
	}

	if (shouldOverride) {
		if (getOverrideShader(hash, PIXEL_SHADER_OVERRIDE_DIR, &pFunctionBuffer)) {
			pFunction = static_cast<DWORD*>(pFunctionBuffer->GetBufferPointer());
		}
	}

	HRESULT result = d3ddev->CreatePixelShader(pFunction, ppShader);
	if (shouldDump || shouldOverride) {
		SDLOG(1, "Created pixel shader for hash %08x: 0x%p\n", hash, *ppShader);
	}
	SAFERELEASE(pAssemblerBuffer);
	SAFERELEASE(pFunctionBuffer);
	return result;
}

HRESULT RSManager::redirectCreateVertexShader(CONST DWORD* pFunction, IDirect3DVertexShader9** ppShader)
{
	bool shouldDump = Settings::get().getEnableShaderDumping();
	bool shouldOverride = Settings::get().getEnableShaderOverride();
	LPD3DXBUFFER pAssemblerBuffer = nullptr;
	LPD3DXBUFFER pFunctionBuffer = nullptr;
	UINT32 hash;

	if (shouldDump || shouldOverride) {
		if (disassembleShader(pFunction, &pAssemblerBuffer)) {
			hash = SuperFastHash(static_cast<char*>(pAssemblerBuffer->GetBufferPointer()), pAssemblerBuffer->GetBufferSize());
		}
		else {
			shouldDump = false;
			shouldOverride = false;
		}
	}

	if (shouldDump) {
		dumpShader(hash, VERTEX_SHADER_DUMP_DIR, pAssemblerBuffer);
	}

	if (shouldOverride) {
		if (getOverrideShader(hash, VERTEX_SHADER_OVERRIDE_DIR, &pFunctionBuffer)) {
			pFunction = static_cast<DWORD*>(pFunctionBuffer->GetBufferPointer());
		}
	}

	HRESULT result = d3ddev->CreateVertexShader(pFunction, ppShader);
	if (shouldDump || shouldOverride) {
		SDLOG(1, "Created vertex shader for hash %08x: 0x%p\n", hash, *ppShader);
	}
	SAFERELEASE(pAssemblerBuffer);
	SAFERELEASE(pFunctionBuffer);
	return result;
}

bool RSManager::disassembleShader(CONST DWORD* pFunction, LPD3DXBUFFER* ppBuffer) {
	LPD3DXBUFFER buffer = nullptr;
	HRESULT result = D3DXDisassembleShader(pFunction, false, NULL, ppBuffer);
	if (result != D3D_OK) {
		SDLOG(0, "Failed to disassemble shader\n");
	}
	return result == D3D_OK;
}

void RSManager::dumpShader(UINT32 hash, const char* directory, LPD3DXBUFFER pBuffer) {
	char fileNameBuffer[64];
	sprintf_s(fileNameBuffer, "%s/%08x.asm", directory, hash);
	const char* fileName = GetDirectoryFile(fileNameBuffer);
	if (writeFile(fileName, static_cast<char*>(pBuffer->GetBufferPointer()), pBuffer->GetBufferSize() - 1)) {
		SDLOG(0, "Wrote disassembled shader to %s\n", fileNameBuffer);
	}
	else {
		SDLOG(0, "Failed to write disassembled shader to %s\n", fileNameBuffer);
	}
}

bool RSManager::getOverrideShader(UINT32 hash, const char* directory, LPD3DXBUFFER* ppBuffer) {
	char fileNameBuffer[64];
	sprintf_s(fileNameBuffer, "%s/%08x.asm", directory, hash);
	const char* fileName = GetDirectoryFile(fileNameBuffer);
	if (fileExists(fileName)) {
		SDLOG(1, "Shader override: %s\n", fileName);
		LPD3DXBUFFER errors = nullptr;
		HRESULT assembleResult = D3DXAssembleShaderFromFile(fileName, nullptr, nullptr, 0, ppBuffer, &errors);
		if (assembleResult != D3D_OK) {
			SDLOG(0, "Failed to assemble replacement shader:\n%s\n", errors->GetBufferPointer());
		}
		SAFERELEASE(errors);
		return assembleResult == D3D_OK;
	}
	return false;
}

void RSManager::frameTimeManagement() {
	// Timer setup, done once.
	// Windows 10 1803+ / 11: high-resolution waitable timer (unchanged behaviour, wakes 1.5 ms early).
	// Older Windows (7, 8.1, early 10): that flag does not exist. A normal waitable timer only fires on the ~15.6 ms
	// system tick there, so the system timer resolution is raised to 1 ms and the timer wakes a little earlier.
	static bool timerSetupDone = false;
	static bool highResTimer = false;
	static double wakeMargin = 1.5;
	if (!timerSetupDone) {
		timerSetupDone = true;
		// CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (0x2) for Windows 10 build 1803+ and Windows 11
		frameTimer = CreateWaitableTimerExW(NULL, NULL, 0x00000002, TIMER_ALL_ACCESS);
		if (frameTimer) {
			highResTimer = true;
			SDLOG(0, "Frame pacing: high-resolution waitable timer\n");
		}
		else {
			if (timeBeginPeriod(1) == TIMERR_NOERROR) {
				SDLOG(0, "Frame pacing: high-resolution timer unavailable, system timer resolution raised to 1 ms\n");
			}
			else {
				SDLOG(0, "Frame pacing: high-resolution timer unavailable, could not raise system timer resolution\n");
			}
			frameTimer = CreateWaitableTimer(NULL, TRUE, NULL);
			wakeMargin = 2.0; // legacy timers can fire up to ~1 ms late even at 1 ms resolution
			if (!frameTimer) SDLOG(0, "Frame pacing: no waitable timer available, using Sleep(1)\n");
		}
	}

	double renderTime = getElapsedTime() - lastPresentTime;

	// Maintain low FPS detection threshold
	double thresholdRenderTime = (1000.0f / Settings::get().getFPSThreshold()) + 0.2;
	if (renderTime > thresholdRenderTime) lowFPSmode = true;
	else if (renderTime < thresholdRenderTime - 1.0f) lowFPSmode = false;

	if (Settings::get().getUnlockFPS()) {
		double currentLimit = g_Force30FPS ? 30.0 : (double)Settings::get().getCurrentFPSLimit();
		double desiredRenderTime = 1000.0 / currentLimit;

		double timeRemaining = desiredRenderTime - renderTime;

		// 1. Coarse sleep: If we have more than (margin + 0.5) ms left, yield to the OS using the timer.
		// Wake up early (1.5 ms with the high-res timer, 2 ms with legacy timers) so OS scheduling variance
		// cannot cause an overshoot.
		if (timeRemaining > wakeMargin + 0.5) {
			if (frameTimer) {
				LARGE_INTEGER li;
				// 10,000 units = 1 millisecond (negative indicates relative time)
				li.QuadPart = (LONGLONG)(-(timeRemaining - wakeMargin) * 10000.0);
				if (SetWaitableTimer(frameTimer, &li, 0, NULL, NULL, FALSE)) {
					WaitForSingleObject(frameTimer, INFINITE);
				}
			}
			else {
				// last resort: 1 ms sleeps (accurate thanks to timeBeginPeriod(1))
				while (desiredRenderTime - (getElapsedTime() - lastPresentTime) > wakeMargin) Sleep(1);
			}
			renderTime = getElapsedTime() - lastPresentTime;
		}

		// 2. Fine-grained spin: Burn the final ~1.5ms with low-latency thread yielding
		while (renderTime < desiredRenderTime) {
			YieldProcessor(); // Emits PAUSE instruction on x86, preventing pipeline stalls
			renderTime = getElapsedTime() - lastPresentTime;
		}
	}

	lastPresentTime = getElapsedTime();
}

void RSManager::measureOcclusionScale() {
	haveOcclusionScale = true;
	float scale = (static_cast<float>(Settings::get().getRenderWidth()) *
		static_cast<float>(Settings::get().getRenderHeight())) / (1024.0f * 720.0f);
	if (isAspectActive()) {
		scale *= getAspectRatio() / (16.0f / 9.0f);
	}
	occlusionScale = scale > 0.01f ? scale : 1.0f;
	SDLOG(2, "occlusionScale (calculated) = %f\n", occlusionScale);
}

