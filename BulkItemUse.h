#pragma once

// bulkItemUse: use several soul items at once from the inventory menu.
// See BulkItemUse.cpp for how it works.

// Installs the hooks. Does nothing when the setting is off. Safe to call more than once.
// Returns true when the feature is active.
bool installBulkItemUse();

// True when the hooks are in place.
bool bulkItemUseActive();
