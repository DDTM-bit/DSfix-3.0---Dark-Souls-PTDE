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
