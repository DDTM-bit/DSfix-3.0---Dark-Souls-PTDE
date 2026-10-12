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

#include "d3dutil.h"
#include "Settings.h"
#include "Hash.h"
#include "Detouring.h"
#include "WindowManager.h"
#include "FPS.h"
#include "resource1.h"
#include <atlbase.h>
#include "AspectFix.h"
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


	// With the 16:10 / 4:3 letterbox correction active, 2D UI draws (INT16 quads, stride 12, and text, stride 24) are
	// drawn inside the game's own 16:9 viewport so the HUD and menus are not stretched. Full-screen quads (fades,
	// menu backgrounds) keep the expanded viewport so they still cover the whole screen.
	bool enterUiViewport(IDirect3DDevice9* dev, UINT stride, const void* data, UINT first, UINT count, D3DVIEWPORT9& saved) {
		if (!dev || !isAspectActive() || (stride != 12 && stride != 24)) return false;
		if (stride == 12 && data && count) {
			float box[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			classifyHudQuad((const INT16*)data, stride, first, count, NULL, box);
			if ((box[2] - box[0]) > 0.95f && (box[3] - box[1]) > 0.95f) return false;
		}
		D3DVIEWPORT9 cur, game;
		if (FAILED(dev->GetViewport(&cur)) || !getGameViewport(cur, &game)) return false;
		saved = cur;
		return SUCCEEDED(dev->SetViewport(&game));
	}


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
		return (t == "VSSAO") ? SSAO::VSSAO : ((t == "HBAO") ? SSAO::HBAO : SSAO::SCAO);
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

	void dumpHudDraw(const char* api, const char* state, const char* tex, int grp, UINT prims, UINT stride, UINT first, UINT nv, const void* data) {
		char buf[600];
		float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
		const INT16* v = (const INT16*)data;
		if (v && stride >= 4 && nv > 0) {
			const UINT n = nv > 4096 ? 4096 : nv;
			const UINT step = stride / 2;
			x0 = y0 = 1e9f; x1 = y1 = -1e9f;
			for (UINT i = first; i < first + n; ++i) {
				const INT16* p = v + (size_t)i * step;
				const float x = p[0] / kUiW, y = p[1] / kUiH;
				if (x < x0) x0 = x;
				if (x > x1) x1 = x;
				if (y < y0) y0 = y;
				if (y > y1) y1 = y;
			}
		}
		INT16 r[6] = { 0, 0, 0, 0, 0, 0 };
		float f[3] = { 0.0f, 0.0f, 0.0f };
		if (v && stride >= 12) { for (int i = 0; i < 6; ++i) r[i] = v[i]; memcpy(f, data, sizeof(f)); }
		DWORD tail[3] = { 0, 0, 0 };
		if (v && stride >= 24) memcpy(tail, (const BYTE*)data + 12, sizeof(tail));
		sprintf_s(buf, sizeof(buf), "%s %s phase=%d tex=%s grp=%d prims=%u stride=%u nv=%u bbox=[%.3f %.3f %.3f %.3f] raw16=[%d %d %d %d %d %d] f32=[%g %g %g] tail=[%08lx %08lx %08lx]",
			api, state, g_hudScalePhase ? 1 : 0, tex, grp, prims, stride, nv, x0, y0, x1, y1, r[0], r[1], r[2], r[3], r[4], r[5], f[0], f[1], f[2], tail[0], tail[1], tail[2]);
		g_dumpLines.push_back(buf);
	}
}

// called from the hotkey action; the next frame that contains a HUD window is written to hud_dump.txt
void requestHudDump() { g_dumpRequested = true; g_rtDumpFrames = 8; }

extern bool g_Force30FPS;

RSManager RSManager::instance;

static const char* PIXEL_SHADER_DUMP_DIR = "dsfix/pixelshader_dump";
static const char* PIXEL_SHADER_OVERRIDE_DIR = "dsfix/pixelshader_override";
static const char* VERTEX_SHADER_DUMP_DIR = "dsfix/vertexshader_dump";
static const char* VERTEX_SHADER_OVERRIDE_DIR = "dsfix/vertexshader_override";

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
		}
		else SDLOG(0, "WARNING: SSAO skipped (buffers missing)\n");
	}
	if (wantDof) {
		if (haveDof) gauss = new GAUSS(d3ddev, dofW, dofH, sharedDofTex, sharedDofSurf);
		else SDLOG(0, "WARNING: DoF blur skipped (buffers missing)\n");
	}
	if (Settings::get().getEnableHudMod()) {
		if (haveCore) hud = new HUD(d3ddev, rw, rh);
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

	SDLOG(0, "RenderstateManager resource release completed\n");
}

HRESULT RSManager::redirectPresent(CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion) {
	capturing = false;
	if (captureNextFrame) {
		capturing = true;
		captureNextFrame = false;
		SDLOG(0, "== CAPTURING FRAME ==\n")
	}
	if (timingIntroMode) {
		skippedPresents++;
		if (skippedPresents >= 1200u && !Settings::get().getUnlockFPS()) {
			SDLOG(1, "Intro mode ended (timeout)!\n");
			timingIntroMode = false;
		}
		if (skippedPresents >= 3000u) {
			SDLOG(1, "Intro mode ended (full timeout)!\n");
			timingIntroMode = false;
		}
		return S_OK;
	}
	skippedPresents = 0;
	if (g_dumpActive) {
		std::ofstream df(GetDirectoryFile("hud_dump.txt"), std::ios::app);
		df << "=== HUD FRAME DUMP ===\n";
		for (size_t i = 0; i < g_dumpLines.size(); ++i) df << g_dumpLines[i] << "\n";
		df << "=== END ===\n\n";
		g_dumpActive = false;
		g_dumpLines.clear();
	}
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
	doft[0] = 0; doft[1] = 0; doft[2] = 0;
	mainRT = NULL;
	mainRTuses = 0;
	zSurf = NULL;
	g_heldMainUses = 0;
	g_heldZSeen = false;
	g_effectsDone = false;

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
	if (Settings::get().getForceWindowed()) {
		SDLOG(0, " - OVERRIDING to user-specified windowed mode:\n");
		WindowManager::get().resize(Settings::get().getPresentWidth(), Settings::get().getPresentHeight());
		ret.Windowed = TRUE;
		ret.FullScreen_RefreshRateInHz = 0;
	}
	else if (Settings::get().getForceFullscreen()) {
		SDLOG(0, " - OVERRIDING to user-specified fullscreen mode:\n");
		ret.Windowed = FALSE;
		ret.FullScreen_RefreshRateInHz = Settings::get().getFullscreenHz();
	}
	if (Settings::get().getForceFullscreen() || Settings::get().getForceWindowed()) {
		ret.PresentationInterval = Settings::get().getEnableVsync() ? D3DPRESENT_INTERVAL_DEFAULT : D3DPRESENT_INTERVAL_IMMEDIATE;
		ret.BackBufferWidth = Settings::get().getPresentWidth();
		ret.BackBufferHeight = Settings::get().getPresentHeight();
		SDLOG(0, " - - Backbuffer(s): %4u x %4u %16s *%d \n", ret.BackBufferWidth, ret.BackBufferHeight, D3DFormatToString(ret.BackBufferFormat), ret.BackBufferCount);
		SDLOG(0, " - - PresentationInterval: %2u   Windowed: %5s    Refresh: %3u Hz\n", ret.PresentationInterval, ret.Windowed ? "true" : "false", ret.FullScreen_RefreshRateInHz);
	}
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
	const bool wantEffects = (ssao && doSsao && !bonfireDisableSSAO) || (doAA && (smaa || fxaa));
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
			if (desc.Width == Settings::get().getRenderWidth() && desc.Height == Settings::get().getRenderHeight()) {
				// HUD stuff
				if (hud && doHud && rddp == 9 && !g_hudWindowSeen) {
					SDLOG(0, "Starting HUD rendering\n");
					hddp = 0;
					onHudRT = true;
					g_hudScalePhase = true;
					g_hudWindowSeen = true;
					g_lastHudGroup = HG_NONE;
					if (g_dumpRequested) { g_dumpRequested = false; g_dumpActive = true; g_dumpLines.clear(); g_dumpLines.push_back("--- WINDOW START ---"); }
					d3ddev->SetRenderTarget(0, rgbaBuffer1Surf);
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
	if (Settings::get().getSkipIntro() && !timingIntroMode && isTextureBandainamcoLogo(pTexture)) {
		SDLOG(1, "Intro mode started!\n");
		timingIntroMode = true;
	}
	if (timingIntroMode && (isTextureGuiElements1(pTexture) || isTextureMenuscreenLogo(pTexture) || isTextureText(pTexture))) {
		SDLOG(1, "Intro mode ended due to texture!\n");
		timingIntroMode = false;
	}
	if (!hudStarted && isTextureHudHealthbar(pTexture)) {
		SDLOG(1, "HUD started!\n");
		hudStarted = true;
	}
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
	SDLOG(0, "Reloaded SSAO (VSSAO)\n");
}
void RSManager::reloadHbao() {
	if (!reloadSsaoAllowed(sharedBuffer1Tex, sharedBuffer2Tex)) return;
	SAFEDELETE(ssao);
	ssao = new SSAO(d3ddev, Settings::get().getRenderWidth(), Settings::get().getRenderHeight(), (unsigned)ssaoStrengthIndex(), SSAO::HBAO, sharedBuffer1Tex, sharedBuffer1Surf, sharedBuffer2Tex, sharedBuffer2Surf);
	SDLOG(0, "Reloaded SSAO (HBAO)\n");
}
void RSManager::reloadScao() {
	if (!reloadSsaoAllowed(sharedBuffer1Tex, sharedBuffer2Tex)) return;
	SAFEDELETE(ssao);
	ssao = new SSAO(d3ddev, Settings::get().getRenderWidth(), Settings::get().getRenderHeight(), (unsigned)ssaoStrengthIndex(), SSAO::SCAO, sharedBuffer1Tex, sharedBuffer1Surf, sharedBuffer2Tex, sharedBuffer2Surf);
	SDLOG(0, "Reloaded SSAO (SCAO)\n");
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
				if (ctex && desc.Width == Settings::get().getRenderWidth() && desc.Height == Settings::get().getRenderHeight()) {
					SDLOG(1, "Starting HUD rendering (fallback)\n");
					hddp = 0;
					onHudRT = true;
					g_hudScalePhase = true;
					g_hudWindowSeen = true;
					g_lastHudGroup = HG_NONE;
					if (g_dumpRequested) { g_dumpRequested = false; g_dumpActive = true; g_dumpLines.clear(); g_dumpLines.push_back("--- WINDOW START (fallback) ---"); }
					prevRenderTex = ctex;
					prevRenderTarget = cur;
					d3ddev->SetRenderTarget(0, rgbaBuffer1Surf);
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
		// The dialogue vignette (a wide gradient over the lower half of the screen) must not go through the HUD buffer,
		// otherwise the corner opacity regions cut visible edges into it. (The full-screen fade-to-black is NOT affected.)
		wideBottomOverlay = (qb[2] - qb[0]) > 0.7f && (qb[3] - qb[1]) < 0.7f && (qb[1] + qb[3]) * 0.5f > 0.6f;
		// A full-screen quad after the HUD (fade from/to black after loading, death fade, ...). hddp >= 7 means the
		// health bars and humanity counter have already been drawn, i.e. the HUD itself is done.
		fullScreenFade = (onHudRT || pausedHudRT) && hddp >= 7 && (qb[2] - qb[0]) > 0.95f && (qb[3] - qb[1]) > 0.95f;
		if (pausedHudRT) {
			// The game also "pauses" for any semi-transparent health-bar textured quad. That covers world-anchored things
			// (lock-on marker, enemy health bars: must NOT be scaled) but also HUD pieces of our own: the boss bar fill and the
			// golden flash of the item squares. Tell them apart by size/position.
			const float qw = qb[2] - qb[0], qh = qb[3] - qb[1];
			bool own = false;
			if (hudGrp == HG_BOSS) own = qw > 0.3f;                    // wide boss bar
			else if (hudGrp == HG_BL) own = qw >= 0.03f && qb[0] > 0.05f && qb[2] < 0.31f && qb[1] > 0.49f && qb[3] < 0.97f; // item cluster: flash squares, fading frames, underlines (lock-on marker is ~0.026 wide)
			else if (hudGrp == HG_BR) own = qw > 0.09f && qb[0] > 0.75f; // souls box
			else if (hudGrp == HG_TL) own = qb[0] > 0.05f && qb[0] < 0.16f; // the player's own bars start at x ~ 0.137
			if (!own) hudGrp = HG_NONE;
		}
		// the boss zone only ever scales health-bar textured quads (not dialogue/prompt boxes)
		if (hudGrp == HG_BOSS && !idxIsHealthbarTex) hudGrp = HG_NONE;
		g_lastHudGroup = hudGrp;
	}
	if (g_dumpActive) {
		IDirect3DBaseTexture9* dt = NULL;
		d3ddev->GetTexture(0, &dt);
		dumpHudDraw("IDX", onHudRT ? "WINDOW" : (pausedHudRT ? "PAUSED" : "DIRECT"), dt ? getTextureName(dt) : "none", (int)hudGrp, PrimitiveCount, VertexStreamZeroStride, MinIndex, NumVertices, pVertexStreamZeroData);
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
	D3DVIEWPORT9 uiSavedVp;
	const bool uiVp = enterUiViewport(d3ddev, VertexStreamZeroStride, pVertexStreamZeroData, MinIndex, NumVertices, uiSavedVp);
	D3DVIEWPORT9 savedVp;
	const bool vpChanged = applyHudViewport(d3ddev, hudGrp, savedVp);
	HRESULT hr;
	{
		// draws that bypass the HUD buffer get their corner's opacity here (buffered ones get it when the buffer is composited)
		const float op = (!onHudRT && hudGrp != HG_NONE && VertexStreamZeroStride == 12) ? hudGroupOpacity(hudGrp) : 1.0f;
		std::vector<BYTE> vcopy;
		const void* vdata = applyVertexOpacity(vcopy, pVertexStreamZeroData, VertexStreamZeroStride, MinIndex, NumVertices, 4, op, d3ddev);
		AlphaRefGuard arg(d3ddev, op);
		hr = d3ddev->DrawIndexedPrimitiveUP(PrimitiveType, MinIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, vdata, VertexStreamZeroStride);
	}
	if (vpChanged) d3ddev->SetViewport(&savedVp);
	if (uiVp) d3ddev->SetViewport(&uiSavedVp);
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
	UINT dumpNv = 0;   // for the dump (INT16 layout only)
	UINT primNv = 0;   // vertex count of this draw
	if (PrimitiveType == D3DPT_TRIANGLELIST) primNv = PrimitiveCount * 3;
	else if (PrimitiveType == D3DPT_TRIANGLESTRIP || PrimitiveType == D3DPT_TRIANGLEFAN) primNv = PrimitiveCount + 2;
	if (VertexStreamZeroStride == 12) dumpNv = primNv;
	std::vector<BYTE> textCopy;
	CONST void* drawData = pVertexStreamZeroData;
	bool textDirect = false;
	if (g_hudScalePhase && hud && doHud) {
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
		dumpHudDraw("PRIM", onHudRT ? "WINDOW" : (pausedHudRT ? "PAUSED" : "DIRECT"), dt ? getTextureName(dt) : "none", (int)hudGrp, PrimitiveCount, VertexStreamZeroStride, 0, dumpNv, pVertexStreamZeroData);
		if (dt) dt->Release();
	}
	D3DVIEWPORT9 uiSavedVp;
	const bool uiVp = enterUiViewport(d3ddev, VertexStreamZeroStride, pVertexStreamZeroData, 0, primNv, uiSavedVp);
	D3DVIEWPORT9 savedVp;
	// float text was already moved above; other formats (unknown/INT16) still use the viewport transform
	const bool vpChanged = (VertexStreamZeroStride == 24) ? false : applyHudViewport(d3ddev, hudGrp, savedVp);
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
	if (vpChanged) d3ddev->SetViewport(&savedVp);
	if (uiVp) d3ddev->SetViewport(&uiSavedVp);
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

HRESULT RSManager::redirectD3DXCreateTextureFromFileInMemoryEx(LPDIRECT3DDEVICE9 pDevice, LPCVOID pSrcData, UINT SrcDataSize, UINT Width, UINT Height, UINT MipLevels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, DWORD Filter, DWORD MipFilter, D3DCOLOR ColorKey, D3DXIMAGE_INFO* pSrcInfo, PALETTEENTRY* pPalette, LPDIRECT3DTEXTURE9* ppTexture) {
	if (Settings::get().getEnableTextureOverride()) {
		UINT32 hash = SuperFastHash((char*)const_cast<void*>(pSrcData), SrcDataSize);
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
	if (takeScreenshot) dumpSurface("HUD_end", rgbaBuffer1Surf);
	d3ddev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
	d3ddev->SetRenderTarget(0, prevRenderTarget);
	onHudRT = false;
	// draw HUD to screen
	storeRenderState();
	hud->go(rgbaBuffer1Tex, prevRenderTarget);
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
	d3ddev->SetRenderTarget(0, rgbaBuffer1Surf);
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