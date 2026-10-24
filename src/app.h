// The tray application: panel, settings, trigger flow, updates.
#pragma once
#include <windows.h>

namespace tt {

int runTrayApp(HINSTANCE instance);

// True while a typing run or an update install is in progress.
bool appBusy();

}
