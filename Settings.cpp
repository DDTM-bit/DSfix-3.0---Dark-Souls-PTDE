#include "Settings.h"

#include <fstream>
#include <string>

#include "main.h"
#include "WindowManager.h"
#include <windows.h>

Settings Settings::instance;

void Settings::load() {
	std::ifstream sfile;
	sfile.open(GetDirectoryFile("DSfix.ini"), std::ios::in);
	char buffer[128];
	while(!sfile.eof()) {
		sfile.getline(buffer, 128);
		if(buffer[0] == '#') continue;
		if(sfile.gcount() <= 1) continue;
		std::string bstring(buffer);

		#define SETTING(_type, _var, _inistring, _defaultval) \
		if(bstring.find(_inistring) == 0) { \
			read(buffer + strlen(_inistring) + 1, _var); \
		}
		#include "Settings.def"
		#undef SETTING
		// NEW: Manually parse custom save folder path and expand Windows environment variables
		if (bstring.find("customSaveFolder ") == 0) {
			char* valStart = buffer + strlen("customSaveFolder ");
			while (*valStart == ' ') valStart++; // trim leading spaces
			std::string rawPath = std::string(valStart);
			rawPath.erase(rawPath.find_last_not_of(" \n\r\t") + 1); // trim trailing whitespace

			if (rawPath == "none") {
				customSaveFolder = "none";
			}
			else {
				char expandedPath[MAX_PATH];
				// Expand environment variables like %localappdata%
				if (ExpandEnvironmentStrings(rawPath.c_str(), expandedPath, MAX_PATH) != 0) {
					customSaveFolder = std::string(expandedPath);
				}
				else {
					customSaveFolder = rawPath; // Fallback if expansion fails
				}
			}
		}
	}
	sfile.close();
	
	if(getBackupInterval() < 600) {
		BackupInterval = 600;
	}
	
	if(getPresentWidth() == 0) PresentWidth = getRenderWidth();
	if(getPresentHeight() == 0) PresentHeight = getRenderHeight();

	if(getOverrideLanguage().length() >= 2 && getOverrideLanguage().find("none") != 0) {
		performLanguageOverride();
	}

	curFPSlimit = getFPSLimit();
}

void Settings::report() {
	SDLOG(0, "= Settings read:\n");
	#define SETTING(_type, _var, _inistring, _defaultval) \
	log(_inistring, _var);
	#include "Settings.def"
	#undef SETTING
	SDLOG(0, "=============\n");
}

void Settings::init() {
	if(!inited) {
		if(getDisableCursor()) WindowManager::get().toggleCursorVisibility();
		if(getCaptureCursor()) WindowManager::get().toggleCursorCapture();
		if(getBorderlessFullscreen()) WindowManager::get().toggleBorderlessFullscreen();

		WindowManager::get().resize(NULL, NULL);
		inited = true;
	}
}

void Settings::shutdown() {
	if(inited) {		
		inited = false;
	}
}

unsigned Settings::getCurrentFPSLimit() {
	if (curFPSlimit == 0) {
		return 60; // Global fallback safe clamp
	}
	return curFPSlimit;
}
void Settings::setCurrentFPSLimit(unsigned limit) {
	curFPSlimit = limit;
}
void Settings::toggle30FPSLimit() {
	if(curFPSlimit == 30) curFPSlimit = getFPSLimit();
	else curFPSlimit = 30;
}


// reading --------------------------------------------------------------------

void Settings::read(char* source, bool& value) {
	std::string ss(source);
	if(ss.find("true")==0 || ss.find("1")==0 || ss.find("TRUE")==0 || ss.find("enable")==0) value = true;
	else value = false;
}

void Settings::read(char* source, int& value) {
	sscanf_s(source, "%d", &value);
}

void Settings::read(char* source, unsigned& value) {
	sscanf_s(source, "%u", &value);
}

void Settings::read(char* source, float& value) {
	sscanf_s(source, "%f", &value);
}

void Settings::read(char* source, std::string& value) {
	value.assign(source);
}


// logging --------------------------------------------------------------------

void Settings::log(const char* name, bool value) {
	SDLOG(0, " - %s : %s\n", name, value ? "true" : "false");
}

void Settings::log(const char* name, int value) {
	SDLOG(0, " - %s : %d\n", name, value);
}

void Settings::log(const char* name, unsigned value) {
	SDLOG(0, " - %s : %u\n", name, value);
}

void Settings::log(const char* name, float value) {
	SDLOG(0, " - %s : %f\n", name, value);
}

void Settings::log(const char* name, const std::string& value) {
	SDLOG(0, " - %s : %s\n", name, value.c_str());
}

// language override --------------------------------------------------------------------

void Settings::performLanguageOverride() {
	// The registry is no longer used for the override; the locale hooks in
	// Detouring.cpp handle it. This only restores anything an older DSfix
	// version left behind (it acts only if a PrevLocaleName value exists).
	undoLanguageOverride();
}

void Settings::undoLanguageOverride() {
	HKEY key;
	// reading operations
	if(RegOpenKeyEx(HKEY_CURRENT_USER, "Control Panel\\International", 0, KEY_READ, &key) != ERROR_SUCCESS) {
		SDLOG(0, "ERROR opening language registry key for reading (restore)\n");
		return;
	}
	BYTE prevLang[64] = { 0 }; // previous locale registry key
	DWORD prevLangSize = sizeof(prevLang) - 1;
	// load previous locale
	if(RegQueryValueEx(key, "PrevLocaleName", 0, 0, prevLang, &prevLangSize) != ERROR_SUCCESS) {
		RegCloseKey(key);
		SDLOG(1, "No leftover language registry value to restore\n");
		return;
	}
	RegFlushKey(key);
	RegCloseKey(key);

	// Writing operations
	if(RegOpenKeyEx(HKEY_CURRENT_USER, "Control Panel\\International", 0, KEY_WRITE, &key) != ERROR_SUCCESS) {
		SDLOG(0, "ERROR opening language registry key for restoring\n");
		return;
	}
	// restore previous locale
	if(RegSetValueEx(key, "LocaleName", 0, REG_SZ, prevLang, prevLangSize) != ERROR_SUCCESS) {
		RegCloseKey(key);
		SDLOG(0, "ERROR restoring language registry key\n");
		return;
	}
	// remove PrevLocaleName value
	if(RegDeleteValue(key, "PrevLocaleName") != ERROR_SUCCESS) {
		SDLOG(0, "ERROR deleting PrevLocaleName registry key\n");
	}
	SDLOG(0, "Restored previous language value %s\n", prevLang);
	RegFlushKey(key);
	RegCloseKey(key);
}
