#ifndef SPAGHETTI_NETPLAY_H
#define SPAGHETTI_NETPLAY_H
// C interface between the game (C code) and the lockstep netplay layer (C++).
#include <libultraship.h>

#ifdef __cplusplus
extern "C" {
#endif

// Called from read_controllers() right after the physical controllers are read into `pads` (4 entries).
// During a netplay session this blocks until every player's input for this frame has arrived, then
// overwrites `pads` with the synchronized inputs (player N = pad N).
void Netplay_OnControllersRead(OSContPad* pads);

// Called at the end of the game's soft reset (ApplyPendingReset). Applies the host's snapshot so every
// player's game starts the session from the same state.
void Netplay_OnReset(void);

// True while a lockstep session is running.
int Netplay_InSession(void);

// Saving to disk is disabled during a session (other players' save files are used temporarily).
int Netplay_BlocksSaving(void);

#ifdef __cplusplus
}
#endif

#endif
