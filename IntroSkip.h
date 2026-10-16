#pragma once

// skipIntro: the start-up autosave notice and the Bandai Namco,
// FromSoftware and Havok logos are never opened, so the title screen comes up straight away. Two title-screen steps are
// changed in memory; nothing depends on timing. Called once from DllMain when skipIntro is on (with or without unlockFPS).
bool installIntroSkip();
