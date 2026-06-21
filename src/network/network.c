/** @file src/network/network.c UDP network layer (POSIX sockets). */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../include/types.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#define NET_CLOSE closesocket
#define NET_EAGAIN WSAEWOULDBLOCK
static int net_wsa_init = 0;
static int Net_WsaError(void) { return WSAGetLastError(); }
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#define SOCKET int
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR   (-1)
#define NET_CLOSE close
#define NET_EAGAIN EAGAIN
static int Net_WsaError(void) { return errno; }
#endif

#include "network.h"

NetConfig g_netConfig;

static SOCKET s_sock = INVALID_SOCKET;
static struct sockaddr_in s_peerAddr[NET_MAX_PLAYERS];

bool Net_Init(uint16 port)
{
	struct sockaddr_in addr;
	int i;
#if defined(_WIN32)
	if (!net_wsa_init) {
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) return false;
		net_wsa_init = 1;
	}
#endif

	s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s_sock == INVALID_SOCKET) return false;

	/* Non-blocking */
#if defined(_WIN32)
	{
		u_long mode = 1;
		ioctlsocket(s_sock, FIONBIO, &mode);
	}
#else
	fcntl(s_sock, F_SETFL, fcntl(s_sock, F_GETFL, 0) | O_NONBLOCK);
#endif

	memset(&addr, 0, sizeof(addr));
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port        = htons(port);

	if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
		NET_CLOSE(s_sock);
		s_sock = INVALID_SOCKET;
		return false;
	}

	/* Pre-resolve peer addresses */
	for (i = 0; i < NET_MAX_PLAYERS; i++) {
		if (!g_netConfig.peers[i].connected) continue;
		memset(&s_peerAddr[i], 0, sizeof(s_peerAddr[i]));
		s_peerAddr[i].sin_family = AF_INET;
		s_peerAddr[i].sin_port   = htons(g_netConfig.peers[i].port);
		inet_pton(AF_INET, g_netConfig.peers[i].ip, &s_peerAddr[i].sin_addr);
	}

	return true;
}

void Net_Uninit(void)
{
	if (s_sock != INVALID_SOCKET) {
		NET_CLOSE(s_sock);
		s_sock = INVALID_SOCKET;
	}
#if defined(_WIN32)
	if (net_wsa_init) { WSACleanup(); net_wsa_init = 0; }
#endif
}

bool Net_Send(uint8 peerIndex, const uint8 *data, uint16 len)
{
	int sent;
	if (s_sock == INVALID_SOCKET) return false;
	if (peerIndex >= NET_MAX_PLAYERS) return false;
	if (!g_netConfig.peers[peerIndex].connected) return false;

	sent = (int)sendto(s_sock, (const char *)data, len, 0,
	                   (struct sockaddr *)&s_peerAddr[peerIndex],
	                   sizeof(s_peerAddr[peerIndex]));
	return sent == (int)len;
}

/* Returns number of bytes received, -1 if nothing, fills peerIndexOut with sender. */
int16 Net_Recv(uint8 *peerIndexOut, uint8 *buf, uint16 bufLen)
{
	struct sockaddr_in from;
	socklen_t fromLen = sizeof(from);
	int i;
	int r;

	if (s_sock == INVALID_SOCKET) return -1;

	r = (int)recvfrom(s_sock, (char *)buf, bufLen, 0,
	                  (struct sockaddr *)&from, &fromLen);
	if (r <= 0) return -1;

	/* Match sender IP:port to known peers */
	for (i = 0; i < NET_MAX_PLAYERS; i++) {
		if (!g_netConfig.peers[i].connected) continue;
		if (s_peerAddr[i].sin_addr.s_addr == from.sin_addr.s_addr &&
		    s_peerAddr[i].sin_port == from.sin_port) {
			*peerIndexOut = (uint8)i;
			return (int16)r;
		}
	}

	return -1; /* Unknown sender */
}

uint32 Net_GetTime(void)
{
#if defined(_WIN32)
	return (uint32)GetTickCount();
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}
