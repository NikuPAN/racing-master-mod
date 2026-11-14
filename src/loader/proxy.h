#pragma once

// Loads the genuine version.dll from the Windows system folder and resolves all 17 exports.
// Returns false if anything is missing, in which case the DLL refuses to load.
bool LoadRealVersionDll();
