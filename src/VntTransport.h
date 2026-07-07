/*
 * VntTransport.h — vnt transport wrapper for USE_VNT mode.
 *
 * When USE_VNT is defined, streaming sockets route through the vnt P2P
 * virtual network via the vn-socket-ffi C ABI instead of OS sockets.
 * This header declares the wrapper API that PlatformSockets.c and the
 * stream files use to indirect through vnt.
 *
 * The wrapper manages a single vnt_ctx_t* (g_VntCtx) initialised in
 * LiStartConnection and freed in LiStopConnection.
 */
#ifndef VNT_TRANSPORT_H
#define VNT_TRANSPORT_H

#include "Limelight.h"
#include "Platform.h"
#include "PlatformSockets.h"

#ifdef USE_VNT
#include "vn_socket.h"

/* --- Global vnt context (set by vntTransportInit, cleared by vntTransportFree) --- */
extern vnt_ctx_t* g_VntCtx;

/* --- Lifecycle --- */
int vntTransportInit(const char* configJson, int timeoutMs);
void vntTransportFree(void);
uint32_t vntTransportVirtualIp(void);

/* --- Socket wrappers (replace OS socket calls when USE_VNT) --- */
/* SOCKET is 'int' on Unix. We reuse the SOCKET type for compatibility. */
/* vnt fds are >= 3; INVALID_SOCKET is -1 on Unix. */

SOCKET vntCreateSocket(int addressFamily, int socketType, int protocol, bool nonBlocking);
SOCKET vntConnectTcpSocket(struct sockaddr_storage* dstaddr, SOCKADDR_LEN addrlen, unsigned short port, int timeoutSec);
SOCKET vntBindUdpSocket(int addressFamily, struct sockaddr_storage* localAddr, SOCKADDR_LEN addrLen, int bufferSize, int socketQosType);
int vntRecvUdpSocket(SOCKET s, char* buffer, int size, bool useSelect);
int vntSendMtuSafe(SOCKET s, char* buffer, int size);
int vntPollSockets(struct pollfd* pollFds, int pollFdsCount, int timeoutMs);
bool vntIsSocketReadable(SOCKET s);
int vntSetNonFatalRecvTimeoutMs(SOCKET s, int timeoutMs);
void vntCloseSocket(SOCKET s);
void vntShutdownTcpSocket(SOCKET s);
int vntEnableNoDelay(SOCKET s);

/* --- Direct sendto wrapper for Video/Audio/Mic ping+data --- */
int vntSendto(SOCKET s, const char* buf, int len, int flags,
              const struct sockaddr* to, SOCKADDR_LEN tolen);

/* --- ENet socket wrappers (Phase 4) --- */
SOCKET vntEnetSocketCreate(int af, int type);
int vntEnetSocketBind(SOCKET socket, const struct sockaddr_storage* addr, SOCKADDR_LEN addrLen);
int vntEnetSocketConnect(SOCKET socket, const struct sockaddr_storage* addr, SOCKADDR_LEN addrLen);
int vntEnetSocketSend(SOCKET socket, const struct sockaddr_storage* addr, SOCKADDR_LEN addrLen,
                      const uint8_t* buf, size_t len);
int vntEnetSocketReceive(SOCKET socket, struct sockaddr_storage* outAddr, SOCKADDR_LEN* outAddrLen,
                         uint8_t* buf, size_t bufLen, int* outTruncated);
int vntEnetSocketWait(SOCKET socket, uint32_t* condition, uint32_t timeoutMs);
int vntEnetSocketSetOption(SOCKET socket, int option, int value);
int vntEnetSocketGetOption(SOCKET socket, int option, int* value);
int vntEnetSocketGetAddress(SOCKET socket, struct sockaddr_storage* addr, SOCKADDR_LEN* addrLen);
void vntEnetSocketDestroy(SOCKET socket);

/* --- Address helpers --- */
/* Convert sockaddr_storage host/port to vnt host-byte-order u32/u16 */
uint32_t vntSockaddrToIp(const struct sockaddr_storage* addr);
uint16_t vntSockaddrToPort(const struct sockaddr_storage* addr);
/* Fill a sockaddr_in with the given host-byte-order ip + port */
void vntIpToSockaddr(uint32_t ip, uint16_t port, struct sockaddr_storage* outAddr, SOCKADDR_LEN* outAddrLen);

/* --- MTU --- */
int vntTransportMtu(void);

#endif /* USE_VNT */
#endif /* VNT_TRANSPORT_H */
