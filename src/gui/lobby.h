/** @file src/gui/lobby.h Multiplayer lobby UI. */

#ifndef LOBBY_H
#define LOBBY_H

#include "../../include/types.h"

/**
 * Show the multiplayer lobby screen.
 * Returns true if the game should start, false if the user cancels.
 * On return, g_netConfig is populated and network is initialised.
 */
extern bool GUI_Lobby_Show(void);

#endif /* LOBBY_H */
