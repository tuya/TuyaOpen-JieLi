#include "tkl_network.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include <lwip/inet.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

static struct sockaddr_in jieli_ipv4_sockaddr(TUYA_IP_ADDR_T addr, uint16_t port)
{
    struct sockaddr_in result;

    memset(&result, 0, sizeof(result));
    result.sin_family = AF_INET;
    result.sin_port = htons(port);
    result.sin_addr.s_addr = htonl(addr);
    return result;
}

static OPERATE_RET jieli_copy_sockaddr(const struct sockaddr_in *source, TUYA_IP_ADDR_T *addr, uint16_t *port)
{
    if (addr != NULL) {
        *addr = ntohl(source->sin_addr.s_addr);
    }
    if (port != NULL) {
        *port = ntohs(source->sin_port);
    }
    return OPRT_OK;
}

static fd_set *jieli_fdset(TUYA_FD_SET_T *fds)
{
    return (fd_set *)fds;
}

TUYA_ERRNO tkl_net_get_errno(void)
{
    return errno;
}

OPERATE_RET tkl_net_fd_set(const int fd, TUYA_FD_SET_T *fds)
{
    if (fds == NULL || fd < 0) {
        return OPRT_INVALID_PARM;
    }
    FD_SET(fd, jieli_fdset(fds));
    return OPRT_OK;
}

OPERATE_RET tkl_net_fd_clear(const int fd, TUYA_FD_SET_T *fds)
{
    if (fds == NULL || fd < 0) {
        return OPRT_INVALID_PARM;
    }
    FD_CLR(fd, jieli_fdset(fds));
    return OPRT_OK;
}

OPERATE_RET tkl_net_fd_isset(const int fd, TUYA_FD_SET_T *fds)
{
    if (fds == NULL || fd < 0) {
        return FALSE;
    }
    return FD_ISSET(fd, jieli_fdset(fds)) ? TRUE : FALSE;
}

OPERATE_RET tkl_net_fd_zero(TUYA_FD_SET_T *fds)
{
    if (fds == NULL) {
        return OPRT_INVALID_PARM;
    }
    FD_ZERO(jieli_fdset(fds));
    return OPRT_OK;
}

int tkl_net_select(const int maxfd, TUYA_FD_SET_T *readfds, TUYA_FD_SET_T *writefds, TUYA_FD_SET_T *errorfds,
                   const uint32_t ms_timeout)
{
    struct timeval timeout = {
        .tv_sec = (long)(ms_timeout / 1000U),
        .tv_usec = (long)((ms_timeout % 1000U) * 1000U),
    };
    return lwip_select(maxfd, jieli_fdset(readfds), jieli_fdset(writefds), jieli_fdset(errorfds), &timeout);
}

int tkl_net_get_nonblock(const int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return (flags & O_NONBLOCK) ? 1 : 0;
}

OPERATE_RET tkl_net_set_block(const int fd, const BOOL_T block)
{
    unsigned long value = block ? 0UL : 1UL;
    return ioctlsocket(fd, FIONBIO, &value) == 0 ? OPRT_OK : errno;
}

TUYA_ERRNO tkl_net_close(const int fd)
{
    return lwip_close(fd);
}

TUYA_ERRNO tkl_net_shutdown(const int fd, const int how)
{
    return lwip_shutdown(fd, how);
}

int tkl_net_socket_create(const TUYA_PROTOCOL_TYPE_E type)
{
    int socket_type = type == PROTOCOL_UDP ? SOCK_DGRAM : (type == PROTOCOL_RAW ? SOCK_RAW : SOCK_STREAM);
    return lwip_socket(AF_INET, socket_type, 0);
}

TUYA_ERRNO tkl_net_connect(const int fd, const TUYA_IP_ADDR_T addr, const uint16_t port)
{
    struct sockaddr_in peer = jieli_ipv4_sockaddr(addr, port);
    return lwip_connect(fd, (const struct sockaddr *)&peer, sizeof(peer));
}

TUYA_ERRNO tkl_net_connect_raw(const int fd, void *p_socket_addr, const int len)
{
    return lwip_connect(fd, (const struct sockaddr *)p_socket_addr, (socklen_t)len);
}

TUYA_ERRNO tkl_net_bind(const int fd, const TUYA_IP_ADDR_T addr, const uint16_t port)
{
    struct sockaddr_in local = jieli_ipv4_sockaddr(addr, port);
    return lwip_bind(fd, (const struct sockaddr *)&local, sizeof(local));
}

TUYA_ERRNO tkl_net_listen(const int fd, const int backlog)
{
    return lwip_listen(fd, backlog);
}

TUYA_ERRNO tkl_net_accept(const int fd, TUYA_IP_ADDR_T *addr, uint16_t *port)
{
    struct sockaddr_in peer;
    socklen_t length = sizeof(peer);
    int result = lwip_accept(fd, (struct sockaddr *)&peer, &length);
    if (result >= 0) {
        jieli_copy_sockaddr(&peer, addr, port);
    }
    return result;
}

TUYA_ERRNO tkl_net_send(const int fd, const void *buf, const uint32_t nbytes)
{
    return lwip_send(fd, buf, nbytes, 0);
}

TUYA_ERRNO tkl_net_send_to(const int fd, const void *buf, const uint32_t nbytes, const TUYA_IP_ADDR_T addr,
                           const uint16_t port)
{
    struct sockaddr_in peer = jieli_ipv4_sockaddr(addr, port);
    return lwip_sendto(fd, buf, nbytes, 0, (const struct sockaddr *)&peer, sizeof(peer));
}

TUYA_ERRNO tkl_net_recv(const int fd, void *buf, const uint32_t nbytes)
{
    return lwip_recv(fd, buf, nbytes, 0);
}

int tkl_net_recv_nd_size(const int fd, void *buf, const uint32_t buf_size, const uint32_t nd_size)
{
    uint32_t received = 0;
    if (buf == NULL || buf_size == 0U || nd_size == 0U) {
        return 0;
    }
    while (received < nd_size && received < buf_size) {
        uint32_t request_size = nd_size - received;
        if (request_size > buf_size - received) {
            request_size = buf_size - received;
        }
        int result = lwip_recv(fd, (uint8_t *)buf + received, request_size, 0);
        if (result <= 0) {
            return result;
        }
        received += (uint32_t)result;
    }
    return (int)received;
}

TUYA_ERRNO tkl_net_recvfrom(const int fd, void *buf, const uint32_t nbytes, TUYA_IP_ADDR_T *addr, uint16_t *port)
{
    struct sockaddr_in peer;
    socklen_t length = sizeof(peer);
    int result = lwip_recvfrom(fd, buf, nbytes, 0, (struct sockaddr *)&peer, &length);
    if (result >= 0) {
        jieli_copy_sockaddr(&peer, addr, port);
    }
    return result;
}

OPERATE_RET tkl_net_gethostbyname(const char *domain, TUYA_IP_ADDR_T *addr)
{
    struct hostent *host;
    if (domain == NULL || addr == NULL) {
        return OPRT_INVALID_PARM;
    }
    host = lwip_gethostbyname(domain);
    if (host == NULL || host->h_addr_list == NULL || host->h_addr_list[0] == NULL) {
        return OPRT_COM_ERROR;
    }
    *addr = ntohl(*(const uint32_t *)host->h_addr_list[0]);
    return OPRT_OK;
}

OPERATE_RET tkl_net_socket_bind(const int fd, const char *ip)
{
    struct in_addr address;
    if (ip == NULL || !inet_aton(ip, &address)) {
        return OPRT_INVALID_PARM;
    }
    return lwip_bind(fd, (const struct sockaddr *)&(struct sockaddr_in){
                         .sin_family = AF_INET,
                         .sin_addr = address,
                     }, sizeof(struct sockaddr_in)) == 0
               ? OPRT_OK
               : errno;
}

OPERATE_RET tkl_net_set_cloexec(const int fd)
{
    (void)fd;
    return OPRT_OK;
}

OPERATE_RET tkl_net_get_socket_ip(const int fd, TUYA_IP_ADDR_T *addr)
{
    return tkl_net_getsockname(fd, addr, NULL);
}

TUYA_IP_ADDR_T tkl_net_str2addr(const char *ip_str)
{
    return ip_str == NULL ? 0 : ntohl(inet_addr(ip_str));
}

char *tkl_net_addr2str(const TUYA_IP_ADDR_T ipaddr)
{
    static char buffer[16];
    (void)snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u", (unsigned)((ipaddr >> 24) & 0xffU),
                   (unsigned)((ipaddr >> 16) & 0xffU), (unsigned)((ipaddr >> 8) & 0xffU),
                   (unsigned)(ipaddr & 0xffU));
    return buffer;
}

OPERATE_RET tkl_net_setsockopt(const int fd, const TUYA_OPT_LEVEL level, const TUYA_OPT_NAME optname,
                               const void *optval, const int optlen)
{
    return lwip_setsockopt(fd, level, optname, optval, (socklen_t)optlen) == 0 ? OPRT_OK : errno;
}

OPERATE_RET tkl_net_getsockopt(const int fd, const TUYA_OPT_LEVEL level, const TUYA_OPT_NAME optname, void *optval,
                               int *optlen)
{
    socklen_t length = optlen == NULL ? 0 : (socklen_t)*optlen;
    int result = lwip_getsockopt(fd, level, optname, optval, &length);
    if (optlen != NULL) {
        *optlen = (int)length;
    }
    return result == 0 ? OPRT_OK : errno;
}

OPERATE_RET tkl_net_set_timeout(const int fd, const int ms_timeout, const TUYA_TRANS_TYPE_E type)
{
    struct timeval timeout = {
        .tv_sec = ms_timeout / 1000,
        .tv_usec = (ms_timeout % 1000) * 1000,
    };
    int option = type == TRANS_SEND ? SO_SNDTIMEO : SO_RCVTIMEO;
    return tkl_net_setsockopt(fd, SOL_SOCKET, option, &timeout, sizeof(timeout));
}

OPERATE_RET tkl_net_set_bufsize(const int fd, const int buf_size, const TUYA_TRANS_TYPE_E type)
{
    int option = type == TRANS_SEND ? SO_SNDBUF : SO_RCVBUF;
    return tkl_net_setsockopt(fd, SOL_SOCKET, option, &buf_size, sizeof(buf_size));
}

OPERATE_RET tkl_net_set_reuse(const int fd)
{
    int enabled = 1;
    return tkl_net_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
}

OPERATE_RET tkl_net_disable_nagle(const int fd)
{
    int enabled = 0;
    return tkl_net_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
}

OPERATE_RET tkl_net_set_broadcast(const int fd)
{
    int enabled = 1;
    return tkl_net_setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled));
}

OPERATE_RET tkl_net_set_keepalive(int fd, const BOOL_T alive, const uint32_t idle, const uint32_t intr,
                                  const uint32_t cnt)
{
    int enabled = alive ? 1 : 0;
    OPERATE_RET result = tkl_net_setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled));
    if (result != OPRT_OK || !alive) {
        return result;
    }
    /* lwip's sockets.h always defines TCP_KEEPIDLE/TCP_KEEPINTVL/TCP_KEEPCNT,
     * but the stack only honours them when built with LWIP_TCP_KEEPALIVE -
     * which the wl83 SDK ships disabled (lwipopts.h) and, being a prebuilt
     * lwip_2_2_0.a, we cannot rebuild. Guarding on the macros alone therefore
     * always takes a path that returns ENOPROTOOPT, and the transport turns
     * any error here into OPRT_MID_TRANSPORT_SOCK_SET_KEEP_ALIVE_FAILED,
     * aborting the AI service's connect outright. Gate on the feature flag
     * instead: SO_KEEPALIVE above already enabled keepalive, so only the
     * tuning is skipped and lwip's own defaults apply. */
#if defined(LWIP_TCP_KEEPALIVE) && LWIP_TCP_KEEPALIVE
    result = tkl_net_setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    result = tkl_net_setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intr, sizeof(intr));
    result = tkl_net_setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#else
    (void)idle;
    (void)intr;
    (void)cnt;
#endif
    return result;
}

OPERATE_RET tkl_net_getsockname(int fd, TUYA_IP_ADDR_T *addr, uint16_t *port)
{
    struct sockaddr_in local;
    socklen_t length = sizeof(local);
    int result = lwip_getsockname(fd, (struct sockaddr *)&local, &length);
    if (result == 0) {
        jieli_copy_sockaddr(&local, addr, port);
        return OPRT_OK;
    }
    return errno;
}

OPERATE_RET tkl_net_getpeername(int fd, TUYA_IP_ADDR_T *addr, uint16_t *port)
{
    struct sockaddr_in peer;
    socklen_t length = sizeof(peer);
    int result = lwip_getpeername(fd, (struct sockaddr *)&peer, &length);
    if (result == 0) {
        jieli_copy_sockaddr(&peer, addr, port);
        return OPRT_OK;
    }
    return errno;
}

OPERATE_RET tkl_net_sethostname(const char *hostname)
{
    (void)hostname;
    return OPRT_NOT_SUPPORTED;
}
