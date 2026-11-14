#pragma once

// Replaces one entry of a COM function table. This is the only place in the mod that writes
// into a table it does not own; every later hook must go through it.
//
// Order matters: the entry's current value is published through *original BEFORE the entry is
// switched to `replacement`, so a call that arrives in between always finds a function to call.
// Returns false if the page could not be made writable or another program changed the table
// under us; the table is then left alone.
bool PatchSlot(void** table, int slot, void* replacement, void* volatile* original);
