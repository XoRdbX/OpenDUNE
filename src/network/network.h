/** @file src/network/network.h UDP network layer for multiplayer. */

#ifndef NETWORK_H
#define NETWORK_H

#include "../../include/types.h"

#define NET_MAX_PLAYERS     3
#define NET_MAX_PACKET_SIZE 512
#define NET_DEFAULT_PORT    19740

/** Network player slot. */
typedef struct NetPeer {
	char     ip[64];
	uint16   port;
	bool     connected;
	uint32   lastSeen;   /* g_timerGUI value of last received packet */
} NetPeer;

/** Global network configuration, set before game start. */
typedef struct NetConfig {
	bool     active;                    /*!< Multiplayer mode is enabled. */
	bool     isHost;                    /*!< We are the session host. */
	uint8    localPlayerIndex;          /*!< Our index in peers[]. */
	uint8    playerCount;               /*!< Total human players. */
	uint8    humanHouseIDs[NET_MAX_PLAYERS]; /*!< HouseID for each player slot. */
	NetPeer  peers[NET_MAX_PLAYERS];    /*!< Peer list (all players including self). */
	uint32   sharedSeed;                /*!< Shared RNG seed, agreed in lobby. */
} NetConfig;

extern NetConfig g_netConfig;

extern bool   Net_Init(uint16 port);
extern void   Net_Uninit(void);
extern bool   Net_Send(uint8 peerIndex, const uint8 *data, uint16 len);
extern int16  Net_Recv(uint8 *peerIndexOut, uint8 *buf, uint16 bufLen);
extern int16  Net_RecvAny(uint8 *buf, uint16 bufLen, char *srcIP, uint16 *srcPort);
extern bool   Net_SetPeerAddr(uint8 peerIndex, const char *ip, uint16 port);
extern uint32 Net_GetTime(void);

#endif /* NETWORK_H */
