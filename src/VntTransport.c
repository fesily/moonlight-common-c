/*
 * VntTransport.c — vnt transport wrapper implementation.
 *
 * Routes streaming socket calls through the vn-socket-ffi C ABI when
 * USE_VNT is defined. All functions here are NO-OPs or unavailable when
 * USE_VNT is not defined (the real-socket path in PlatformSockets.c is
 * used instead).
 */
#include "Limelight-internal.h"

#ifdef USE_VNT

#include "VntTransport.h"
#include "vn_socket.h"
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>

vnt_ctx_t* g_VntCtx = NULL;

/* Default vnt MTU — vnt default is 1380, but we use a conservative value */
#define VNT_DEFAULT_MTU 1380

/* Track which fds are "non-blocking" (for ENET_SOCKOPT_NONBLOCK emulation) */
#define MAX_VNT_FDS 64
static struct {
    SOCKET fd;
    bool nonBlocking;
} s_fdFlags[MAX_VNT_FDS];
static int s_fdFlagCount = 0;

static bool getFdFlag(SOCKET fd) {
    for (int i = 0; i < s_fdFlagCount; i++) {
        if (s_fdFlags[i].fd == fd) return s_fdFlags[i].nonBlocking;
    }
    return false;
}

static void setFdFlag(SOCKET fd, bool nonBlocking) {
    for (int i = 0; i < s_fdFlagCount; i++) {
        if (s_fdFlags[i].fd == fd) {
            s_fdFlags[i].nonBlocking = nonBlocking;
            return;
        }
    }
    if (s_fdFlagCount < MAX_VNT_FDS) {
        s_fdFlags[s_fdFlagCount].fd = fd;
        s_fdFlags[s_fdFlagCount].nonBlocking = nonBlocking;
        s_fdFlagCount++;
    }
}

static void removeFdFlag(SOCKET fd) {
    for (int i = 0; i < s_fdFlagCount; i++) {
        if (s_fdFlags[i].fd == fd) {
            s_fdFlags[i] = s_fdFlags[s_fdFlagCount - 1];
            s_fdFlagCount--;
            return;
        }
    }
}

/* --- Lifecycle --- */

int vntTransportInit(const char* configJson, int timeoutMs) {
    if (g_VntCtx != NULL) {
        return 0;
    }
    g_VntCtx = vnt_init(configJson);
    if (g_VntCtx == NULL) {
        Limelog("VNT: vnt_init failed\n");
        return -1;
    }
    int rc = vnt_wait_ready(g_VntCtx, timeoutMs);
    if (rc != 0) {
        Limelog("VNT: vnt_wait_ready failed: %d\n", rc);
        vnt_free(g_VntCtx);
        g_VntCtx = NULL;
        return -2;
    }
    uint32_t vip = vnt_virtual_ip(g_VntCtx);
    if (vip == 0) {
        Limelog("VNT: virtual IP is 0\n");
        vnt_free(g_VntCtx);
        g_VntCtx = NULL;
        return -3;
    }
    {
        struct in_addr a;
        a.s_addr = htonl(vip);
        char ipb[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &a, ipb, sizeof(ipb));
        Limelog("VNT: ready, virtual IP %s\n", ipb);
    }
    s_fdFlagCount = 0;
    return 0;
}

void vntTransportFree(void) {
    if (g_VntCtx != NULL) {
        vnt_free(g_VntCtx);
        g_VntCtx = NULL;
    }
    s_fdFlagCount = 0;
}

uint32_t vntTransportVirtualIp(void) {
    if (g_VntCtx == NULL) return 0;
    return vnt_virtual_ip(g_VntCtx);
}

/* --- Address helpers --- */

uint32_t vntSockaddrToIp(const struct sockaddr_storage* addr) {
    if (addr->ss_family == AF_INET) {
        const struct sockaddr_in* sin = (const struct sockaddr_in*)addr;
        return ntohl(sin->sin_addr.s_addr);
    }
    return 0;
}

uint16_t vntSockaddrToPort(const struct sockaddr_storage* addr) {
    if (addr->ss_family == AF_INET) {
        const struct sockaddr_in* sin = (const struct sockaddr_in*)addr;
        return ntohs(sin->sin_port);
    }
    return 0;
}

void vntIpToSockaddr(uint32_t ip, uint16_t port, struct sockaddr_storage* outAddr, SOCKADDR_LEN* outAddrLen) {
    struct sockaddr_in* sin = (struct sockaddr_in*)outAddr;
    memset(sin, 0, sizeof(*sin));
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = htonl(ip);
    sin->sin_port = htons(port);
    *outAddrLen = sizeof(*sin);
}

/* --- Socket wrappers --- */

SOCKET vntCreateSocket(int addressFamily, int socketType, int protocol, bool nonBlocking) {
    (void)addressFamily;
    (void)protocol;
    /* vnt sockets are created on bind/connect, not upfront.
     * Return a placeholder that vntBindUdpSocket / vntConnectTcpSocket will replace. */
    LC_ASSERT(g_VntCtx != NULL);
    return INVALID_SOCKET;
}

SOCKET vntConnectTcpSocket(struct sockaddr_storage* dstaddr, SOCKADDR_LEN addrlen, unsigned short port, int timeoutSec) {
    LC_ASSERT(g_VntCtx != NULL);
    if (g_VntCtx == NULL) return INVALID_SOCKET;

    uint32_t dstIp = vntSockaddrToIp(dstaddr);
    if (dstIp == 0) return INVALID_SOCKET;

    int timeoutMs = timeoutSec > 0 ? timeoutSec * 1000 : 10000;
    int fd = vnt_tcp_connect(g_VntCtx, dstIp, port, timeoutMs);
    if (fd < 0) {
        Limelog("VNT: vnt_tcp_connect failed: %d\n", fd);
        return INVALID_SOCKET;
    }
    setFdFlag(fd, false);
    return fd;
}

SOCKET vntBindUdpSocket(int addressFamily, struct sockaddr_storage* localAddr, SOCKADDR_LEN addrLen, int bufferSize, int socketQosType) {
    LC_ASSERT(g_VntCtx != NULL);
    if (g_VntCtx == NULL) return INVALID_SOCKET;
    (void)addressFamily;
    (void)bufferSize;
    (void)socketQosType;

    uint16_t port = 0;
    if (localAddr != NULL) {
        port = vntSockaddrToPort(localAddr);
    }

    int fd = vnt_udp_bind(g_VntCtx, port);
    if (fd < 0) {
        Limelog("VNT: vnt_udp_bind failed: %d\n", fd);
        return INVALID_SOCKET;
    }
    setFdFlag(fd, false);

    /* Fill in the actual bound address */
    if (localAddr != NULL) {
        uint32_t vip = vntTransportVirtualIp();
        vntIpToSockaddr(vip, port, localAddr, &addrLen);
    }

    return fd;
}

int vntRecvUdpSocket(SOCKET s, char* buffer, int size, bool useSelect) {
    LC_ASSERT(g_VntCtx != NULL);
    if (g_VntCtx == NULL) return -1;
    (void)useSelect; /* vnt fds don't support select; we use timeout instead */

    int timeoutMs = getFdFlag(s) ? 0 : 60000;
    uint32_t srcIp = 0;
    uint16_t srcPort = 0;
    int truncated = 0;
    long n = vnt_udp_recvfrom_timeout(g_VntCtx, s, (uint8_t*)buffer, (uintptr_t)size,
                                       &srcIp, &srcPort, &truncated, timeoutMs);
    if (n < 0) {
        if (n == VNT_E_TIMEOUT) {
            SetLastSocketError(EAGAIN);
            return -1;
        }
        return -1;
    }
    return (int)n;
}

int vntSendMtuSafe(SOCKET s, char* buffer, int size) {
    LC_ASSERT(g_VntCtx != NULL);
    if (g_VntCtx == NULL) return -1;
    /* vnt UDP sendto handles MTU internally; just send the whole thing */
    return size;
}

int vntSendto(SOCKET s, const char* buf, int len, int flags,
              const struct sockaddr* to, SOCKADDR_LEN tolen) {
    LC_ASSERT(g_VntCtx != NULL);
    if (g_VntCtx == NULL) return -1;
    (void)flags;

    struct sockaddr_storage ss;
    memcpy(&ss, to, tolen);
    uint32_t dstIp = vntSockaddrToIp(&ss);
    uint16_t dstPort = vntSockaddrToPort(&ss);

    long n = vnt_udp_sendto(g_VntCtx, s, dstIp, dstPort, (const uint8_t*)buf, (uintptr_t)len);
    if (n < 0) {
        return -1;
    }
    return (int)n;
}

int vntPollSockets(struct pollfd* pollFds, int pollFdsCount, int timeoutMs) {
    LC_ASSERT(g_VntCtx != NULL);
    if (g_VntCtx == NULL) return -1;
    if (pollFdsCount <= 0) return 0;

    struct VntPollFd* vpf = (struct VntPollFd*)malloc(sizeof(struct VntPollFd) * pollFdsCount);
    if (vpf == NULL) return -1;

    for (int i = 0; i < pollFdsCount; i++) {
        vpf[i].fd = pollFds[i].fd;
        vpf[i].events = VNT_POLLIN;
        if (pollFds[i].events & POLLOUT) vpf[i].events |= VNT_POLLOUT;
        vpf[i].revents = 0;
    }

    int ret = vnt_poll(g_VntCtx, vpf, (uintptr_t)pollFdsCount, timeoutMs);
    if (ret > 0) {
        for (int i = 0; i < pollFdsCount; i++) {
            pollFds[i].revents = 0;
            if (vpf[i].revents & VNT_POLLIN) pollFds[i].revents |= POLLIN;
            if (vpf[i].revents & VNT_POLLOUT) pollFds[i].revents |= POLLOUT;
        }
    }

    free(vpf);
    return ret;
}

bool vntIsSocketReadable(SOCKET s) {
    LC_ASSERT(g_VntCtx != NULL);
    if (g_VntCtx == NULL) return false;

    struct VntPollFd pf;
    pf.fd = s;
    pf.events = VNT_POLLIN;
    pf.revents = 0;
    int ret = vnt_poll(g_VntCtx, &pf, 1, 0);
    return ret > 0 && (pf.revents & VNT_POLLIN);
}

int vntSetNonFatalRecvTimeoutMs(SOCKET s, int timeoutMs) {
    (void)s;
    (void)timeoutMs;
    /* vnt uses its own timeout in recvfrom_timeout; this is a no-op */
    return 0;
}

void vntCloseSocket(SOCKET s) {
    LC_ASSERT(g_VntCtx != NULL);
    if (g_VntCtx == NULL) return;
    vnt_close(g_VntCtx, s);
    removeFdFlag(s);
}

void vntShutdownTcpSocket(SOCKET s) {
    LC_ASSERT(g_VntCtx != NULL);
    if (g_VntCtx == NULL) return;
    vnt_shutdown(g_VntCtx, s);
}

int vntEnableNoDelay(SOCKET s) {
    (void)s;
    return 0;
}

int vntTransportMtu(void) {
    return VNT_DEFAULT_MTU;
}

/* --- ENet socket wrappers (Phase 4) --- */

SOCKET vntEnetSocketCreate(int af, int type) {
    (void)af;
    (void)type;
    /* ENet creates sockets then binds them. We defer to bind. */
    return 0;
}

int vntEnetSocketBind(SOCKET socket, const struct sockaddr_storage* addr, SOCKADDR_LEN addrLen) {
    LC_ASSERT(g_VntCtx != NULL);
    if (g_VntCtx == NULL) return -1;
    (void)addrLen;

    uint16_t port = vntSockaddrToPort(addr);
    int fd = vnt_udp_bind(g_VntCtx, port);
    if (fd < 0) return -1;
    setFdFlag(fd, false);
    /* Return the new fd via the socket parameter — ENet stores it */
    /* This is a bit of a hack: ENet passes the socket by value, so we
     * need to handle this differently. The caller should use the return
     * value as the new socket. */
    return fd;
}

int vntEnetSocketConnect(SOCKET socket, const struct sockaddr_storage* addr, SOCKADDR_LEN addrLen) {
    (void)socket;
    (void)addr;
    (void)addrLen;
    /* UDP is connectionless in vnt; connect is a no-op */
    return 0;
}

int vntEnetSocketSend(SOCKET socket, const struct sockaddr_storage* addr, SOCKADDR_LEN addrLen,
                      const uint8_t* buf, size_t len) {
    LC_ASSERT(g_VntCtx != NULL);
    if (g_VntCtx == NULL) return -1;
    (void)addrLen;

    uint32_t dstIp = vntSockaddrToIp(addr);
    uint16_t dstPort = vntSockaddrToPort(addr);
    long n = vnt_udp_sendto(g_VntCtx, socket, dstIp, dstPort, buf, len);
    return n < 0 ? -1 : (int)n;
}

int vntEnetSocketReceive(SOCKET socket, struct sockaddr_storage* outAddr, SOCKADDR_LEN* outAddrLen,
                         uint8_t* buf, size_t bufLen, int* outTruncated) {
    LC_ASSERT(g_VntCtx != NULL);
    if (g_VntCtx == NULL) return -1;

    bool nonBlock = getFdFlag(socket);
    int timeoutMs = nonBlock ? 0 : 100;

    uint32_t srcIp = 0;
    uint16_t srcPort = 0;
    int truncated = 0;
    long n = vnt_udp_recvfrom_timeout(g_VntCtx, socket, buf, bufLen,
                                       &srcIp, &srcPort, &truncated, timeoutMs);
    if (n < 0) {
        if (outTruncated) *outTruncated = 0;
        if (n == VNT_E_TIMEOUT) return 0;
        return -1;
    }
    if (outAddr && outAddrLen) {
        vntIpToSockaddr(srcIp, srcPort, outAddr, outAddrLen);
    }
    if (outTruncated) *outTruncated = truncated;
    return (int)n;
}

int vntEnetSocketWait(SOCKET socket, uint32_t* condition, uint32_t timeoutMs) {
    LC_ASSERT(g_VntCtx != NULL);
    if (g_VntCtx == NULL) return -1;

    struct VntPollFd pf;
    pf.fd = socket;
    pf.events = VNT_POLLIN;
    pf.revents = 0;

    int ret = vnt_poll(g_VntCtx, &pf, 1, (int)timeoutMs);
    if (ret < 0) return -1;

    *condition = 0;
    if (pf.revents & VNT_POLLIN) *condition |= 1;
    return 0;
}

int vntEnetSocketSetOption(SOCKET socket, int option, int value) {
    (void)socket;
    /* ENET_SOCKOPT_NONBLOCK = 1 */
    if (option == 1) {
        setFdFlag(socket, value != 0);
    }
    return 0;
}

int vntEnetSocketGetOption(SOCKET socket, int option, int* value) {
    (void)socket;
    (void)option;
    if (value) *value = 0;
    return 0;
}

int vntEnetSocketGetAddress(SOCKET socket, struct sockaddr_storage* addr, SOCKADDR_LEN* addrLen) {
    LC_ASSERT(g_VntCtx != NULL);
    if (g_VntCtx == NULL) return -1;
    uint32_t vip = vntTransportVirtualIp();
    vntIpToSockaddr(vip, 0, addr, addrLen);
    return 0;
}

void vntEnetSocketDestroy(SOCKET socket) {
    vntCloseSocket(socket);
}

#endif /* USE_VNT */
