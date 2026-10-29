
#include "KeyActions.h"

#include <fstream>
#include <string>
#include <sstream>
#include <vector>
using namespace std;

#include "main.h"
#include "WindowManager.h"
#include "Settings.h"
#include "RenderstateManager.h"

void requestHudDump(); // defined in RenderstateManager.cpp
void toggleSharpenPass(); // defined in RenderstateManager.cpp

// DSfix 3.0: Global flag for smart physics toggle
bool g_Force30FPS = false;

KeyActions KeyActions::instance;

void KeyActions::load() {
	std::ifstream sfile;
	sfile.open(GetDirectoryFile("DSfixKeys.ini"), std::ios::in);
	char buffer[256];
	while(!sfile.eof()) {
		sfile.getline(buffer, 256);
		if(buffer[0] == '#') continue;
		if(sfile.gcount() <= 1) continue;
		string bstring(buffer);
		
		size_t pos = bstring.npos;
		char postChar;
		#define KEY(_name, _val) \
		pos = bstring.find(#_name); \
		if(pos != bstring.npos) { \
			postChar = buffer[pos + strlen(#_name)]; \
			if(postChar == '\r' || postChar == '\n' || postChar == ' ' || postChar == '\0') { \
				string action; stringstream ss(bstring); ss >> action; \
				keyBindingMap.insert(make_pair(_val, action)); \
			} \
		}
		#include "Keys.def"
		#undef KEY
	}
	sfile.close();
}

void KeyActions::report() {
	SDLOG(0, "= Loaded Keybindings:\n");
	for(IntStrMap::const_iterator i = keyBindingMap.begin(); i != keyBindingMap.end(); ++i) {
		SDLOG(0, " - %p => %s\n", i->first, i->second.c_str());
	}
	SDLOG(0, "=============\n");
	
	SDLOG(5, "= Possible Actions:\n");
	#define ACTION(_name, _action) \
	SDLOG(5, "%s, ", #_name);
 	#include "Actions.def"
	#undef ACTION
	SDLOG(5, "=============\n");
	
	SDLOG(5, "= Possible Keys:\n");
	#define KEY(_name, _val) \
	SDLOG(5, "%s, ", #_name);
 	#include "Keys.def"
	#undef KEY
	SDLOG(5, "=============\n");
}

void KeyActions::performAction(const char* name) {
	#define ACTION(_name, _action) \
	if(strcmp(#_name, name) == 0) _name();
 	#include "Actions.def"
	#undef ACTION
}

// DSfix 3.0: toggle30FPSLimit does nothing while a menu is open, on the title or loading screen, while aiming
// with a bow, or while a bow or crossbow is two-handed. There the same key is easily pressed by accident (for
// example BACKSPACE in a menu), and the player would only notice the 30 FPS limit later, back in the game.
// Only read when the key is pressed. Game addresses are checked byte for byte first; if they don't match
// (another game version), the toggle always works as before.
namespace {
	bool readDword(DWORD addr, DWORD& out) {
		if (addr < 0x10000) return false;
		__try { out = *(DWORD*)addr; return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	bool toggle30FPSAddressesOk() {
		static int state = -1; // -1 not checked, 0 mismatch, 1 ok
		if (state >= 0) return state == 1;
		static const struct { DWORD addr; unsigned char bytes[19]; size_t len; } sigs[] = {
			{ 0x00C2ABE0, { 0x56, 0x8B, 0x35, 0xD0, 0x86, 0x37, 0x01, 0x57, 0x33, 0xFF, 0x39, 0x7E, 0x24, 0x74, 0x05, 0xBF }, 16 }, // IsShowMenu
			{ 0x00EFC61E, { 0x8B, 0x0D, 0xC8, 0xD6, 0x37, 0x01, 0x8B, 0x3D, 0xA0, 0x84, 0x37, 0x01, 0x89, 0x81, 0x24, 0x10, 0x00, 0x00 }, 18 }, // camera holder +0x1024
			{ 0x00F148BD, { 0x8B, 0x43, 0x74, 0x83, 0xF8, 0x04, 0x0F, 0x87, 0x2C, 0x19, 0x00, 0x00, 0xFF, 0x24, 0x85, 0xD4, 0x62, 0xF1, 0x00 }, 19 }, // camera mode +0x74
		};
		state = 1;
		for (const auto& s : sigs) {
			__try { if (memcmp((const void*)s.addr, s.bytes, s.len) != 0) state = 0; }
			__except (EXCEPTION_EXECUTE_HANDLER) { state = 0; }
		}
		if (state == 0) SDLOG(0, "toggle30FPSLimit: game addresses don't match, the menu/bow check is off\n");
		return state == 1;
	}

	// Returns the reason the toggle is blocked, or NULL when it may run.
	const char* toggle30FPSBlockedReason() {
		if (!toggle30FPSAddressesOk()) return NULL;
		DWORD v = 0, player = 0, chrCam = 0, gameData = 0;
		if (!readDword(0x0137D644, v) || !readDword(v + 0x3C, player) || player == 0) return "no player (title or loading screen)";

		typedef DWORD(__cdecl* IsShowMenuFn)();
		DWORD menus = 0;
		__try { menus = ((IsShowMenuFn)0x00C2ABE0)(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { menus = 0; }
		if (menus != 0) return "a menu is open";

		DWORD mode = 0;
		if (readDword(0x0137D6C8, v) && readDword(v + 0x1024, chrCam) && chrCam != 0 && readDword(chrCam + 0x74, mode) && mode >= 1 && mode <= 3)
			return "aiming";

		// player game data: +0x230 how the weapons are held (1 = normal), +0x234/+0x238 active left/right slot,
		// left weapons at +0x24C + 8*slot, right weapons at +0x250 + 8*slot. Bows and crossbows are 1200000..1299999.
		DWORD stance = 1, ls = 0, rs = 0, left = 0, right = 0;
		if (readDword(0x01378700, v) && readDword(v + 0x8, gameData) && gameData != 0 && readDword(gameData + 0x230, stance) && stance != 1) {
			if (readDword(gameData + 0x234, ls) && ls <= 1) readDword(gameData + 0x24C + 8 * ls, left);
			if (readDword(gameData + 0x238, rs) && rs <= 1) readDword(gameData + 0x250 + 8 * rs, right);
			if ((left >= 1200000 && left < 1300000) || (right >= 1200000 && right < 1300000)) return "bow or crossbow two-handed";
		}
		return NULL;
	}
}

void KeyActions::processIO() {
		HWND fgWindow = ::GetForegroundWindow();
		DWORD fgPid = 0;
		if (fgWindow != NULL) {
			::GetWindowThreadProcessId(fgWindow, &fgPid);
		}
		// Only process inputs if the currently focused window belongs to this game process
		if (fgWindow != NULL && fgPid == ::GetCurrentProcessId()) {
			g_Force30FPS = false; // Reset the state every single frame

			for (IntStrMap::const_iterator i = keyBindingMap.begin(); i != keyBindingMap.end(); ++i) {

				// DSfix 3.0: Call GetAsyncKeyState exactly ONCE and store the result to prevent flag erasure
				short keyState = GetAsyncKeyState(i->first);
				bool isDown = (keyState & 0x8000) != 0;
				bool justPressed = (keyState & 1) != 0;

				// Convert the old clunky toggle into the modern DSfix 3.0 Smart Hold
				if (i->second == "toggleFPS" || i->second == "toggleFPSlimit") {
					if (isDown) g_Force30FPS = true;
				}
				else if (justPressed) {
					if (i->second == "toggle30FPSLimit") {
						const char* reason = toggle30FPSBlockedReason();
						if (reason) { SDLOG(0, "Action ignored: toggle30FPSLimit (%s)\n", reason); continue; }
					}
					SDLOG(0, "Action triggered: %s\n", i->second.c_str());
					performAction(i->second.c_str());
				}
			}
		}
}


#define ACTION(_name, _action) \
void KeyActions::##_name() { _action; };
#include "Actions.def"
#undef ACTION


