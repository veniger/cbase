#include <string.h>

#ifdef CB_PLATFORM_POSIX
    #include <unistd.h>
    #include <fcntl.h>
    #include <poll.h>
    #include <errno.h>
    #include <signal.h>
#endif

/* --- Internal helpers --- */

#ifdef CB_PLATFORM_WINDOWS
    #define CB__INVALID_SOCK        INVALID_SOCKET
    #define CB__SOCK_CLOSE(s)       closesocket(s)
    #define CB__SOCK_LAST_ERR()     WSAGetLastError()
    #define CB__SOCK_WOULDBLOCK     WSAEWOULDBLOCK
#else
    #define CB__INVALID_SOCK        (-1)
    #define CB__SOCK_CLOSE(s)       close(s)
    #define CB__SOCK_LAST_ERR()     errno
    #define CB__SOCK_WOULDBLOCK     EAGAIN
#endif

static cb_info_t cb__set_nonblocking(cb__sockfd_t s)
{
#ifdef CB_PLATFORM_WINDOWS
    u_long nb = 1;
    if (ioctlsocket(s, FIONBIO, &nb) != 0) return CB_INFO_NET_NONBLOCK_FAILED;
    return CB_INFO_OK;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return CB_INFO_NET_NONBLOCK_FAILED;
    if (fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0) return CB_INFO_NET_NONBLOCK_FAILED;
    return CB_INFO_OK;
#endif
}

static void cb__addr_to_sockaddr(cb_net_addr_t in, struct sockaddr_in *out)
{
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(in.port);
    out->sin_addr.s_addr = htonl(in.ip);
}

static cb_net_addr_t cb__sockaddr_to_addr(const struct sockaddr_in *in)
{
    cb_net_addr_t a;
    a.info = CB_INFO_OK;
    a.ip   = ntohl(in->sin_addr.s_addr);
    a.port = ntohs(in->sin_port);
    return a;
}

/* ================================================================ */
/*  Lifecycle                                                       */
/* ================================================================ */

cb_info_t cb_net_init(void)
{
#ifdef CB_PLATFORM_WINDOWS
    WSADATA wsa;
    int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    return (rc == 0) ? CB_INFO_OK : CB_INFO_NET_INIT_FAILED;
#else
    /* Ignore SIGPIPE so writes to a closed peer return EPIPE instead of
       killing the process. Blunt but the right default for a game runtime. */
    signal(SIGPIPE, SIG_IGN);
    return CB_INFO_OK;
#endif
}

cb_info_t cb_net_shutdown(void)
{
#ifdef CB_PLATFORM_WINDOWS
    return (WSACleanup() == 0) ? CB_INFO_OK : CB_INFO_NET_INIT_FAILED;
#else
    return CB_INFO_OK;
#endif
}

/* ================================================================ */
/*  Address                                                         */
/* ================================================================ */

cb_net_addr_t cb_net_addr_v4(const char *ip_dotted, uint16_t port)
{
    cb_net_addr_t a;
    a.info = CB_INFO_OK;
    a.ip = 0;
    a.port = port;

    struct in_addr ia;
    if (inet_pton(AF_INET, ip_dotted, &ia) != 1)
    {
        a.info = CB_INFO_NET_ADDR_INVALID;
        return a;
    }
    a.ip = ntohl(ia.s_addr);
    return a;
}

/* ================================================================ */
/*  UDP                                                             */
/* ================================================================ */

cb_udp_socket_t cb_udp_open(uint16_t bind_port)
{
    cb_udp_socket_t s;
    s.info = CB_INFO_OK;
    s.handle = CB__INVALID_SOCK;

    cb__sockfd_t fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == CB__INVALID_SOCK)
    {
        s.info = CB_INFO_NET_SOCKET_FAILED;
        return s;
    }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(bind_port);

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0)
    {
        CB__SOCK_CLOSE(fd);
        s.info = CB_INFO_NET_BIND_FAILED;
        return s;
    }

    if (cb__set_nonblocking(fd) != CB_INFO_OK)
    {
        CB__SOCK_CLOSE(fd);
        s.info = CB_INFO_NET_NONBLOCK_FAILED;
        return s;
    }

    s.handle = fd;
    return s;
}

cb_info_t cb_udp_close(cb_udp_socket_t *s)
{
    if (s->handle != CB__INVALID_SOCK)
    {
        CB__SOCK_CLOSE(s->handle);
        s->handle = CB__INVALID_SOCK;
    }
    return CB_INFO_OK;
}

cb_net_io_result_t cb_udp_send(cb_udp_socket_t *s, cb_net_addr_t to,
                               const void *data, size_t size)
{
    cb_net_io_result_t r;
    r.info = CB_INFO_OK;
    r.bytes = 0;

    struct sockaddr_in sa;
    cb__addr_to_sockaddr(to, &sa);

#ifdef CB_PLATFORM_WINDOWS
    int sent = sendto(s->handle, (const char *)data, (int)size, 0,
                      (struct sockaddr *)&sa, sizeof(sa));
#else
    ssize_t sent = sendto(s->handle, data, size, 0,
                          (struct sockaddr *)&sa, sizeof(sa));
#endif
    if (sent < 0)
    {
        int err = CB__SOCK_LAST_ERR();
        r.info = (err == CB__SOCK_WOULDBLOCK) ? CB_INFO_NET_WOULD_BLOCK
                                              : CB_INFO_NET_SEND_FAILED;
        return r;
    }
    r.bytes = (size_t)sent;
    return r;
}

cb_udp_recv_result_t cb_udp_recv(cb_udp_socket_t *s, void *buf, size_t buf_size)
{
    cb_udp_recv_result_t r;
    r.info = CB_INFO_OK;
    r.size = 0;
    r.from.info = CB_INFO_OK;
    r.from.ip = 0;
    r.from.port = 0;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
#ifdef CB_PLATFORM_WINDOWS
    int slen = sizeof(sa);
    int got = recvfrom(s->handle, (char *)buf, (int)buf_size, 0,
                       (struct sockaddr *)&sa, &slen);
#else
    socklen_t slen = sizeof(sa);
    ssize_t got = recvfrom(s->handle, buf, buf_size, 0,
                           (struct sockaddr *)&sa, &slen);
#endif
    if (got < 0)
    {
        int err = CB__SOCK_LAST_ERR();
        r.info = (err == CB__SOCK_WOULDBLOCK) ? CB_INFO_NET_WOULD_BLOCK
                                              : CB_INFO_NET_RECV_FAILED;
        return r;
    }
    r.size = (size_t)got;
    r.from = cb__sockaddr_to_addr(&sa);
    return r;
}

/* ================================================================ */
/*  TCP                                                             */
/* ================================================================ */

cb_tcp_listener_t cb_tcp_listen(uint16_t port, int backlog)
{
    cb_tcp_listener_t l;
    l.info = CB_INFO_OK;
    l.handle = CB__INVALID_SOCK;

    cb__sockfd_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == CB__INVALID_SOCK)
    {
        l.info = CB_INFO_NET_SOCKET_FAILED;
        return l;
    }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0)
    {
        CB__SOCK_CLOSE(fd);
        l.info = CB_INFO_NET_BIND_FAILED;
        return l;
    }

    if (listen(fd, backlog) != 0)
    {
        CB__SOCK_CLOSE(fd);
        l.info = CB_INFO_NET_LISTEN_FAILED;
        return l;
    }

    if (cb__set_nonblocking(fd) != CB_INFO_OK)
    {
        CB__SOCK_CLOSE(fd);
        l.info = CB_INFO_NET_NONBLOCK_FAILED;
        return l;
    }

    l.handle = fd;
    return l;
}

cb_info_t cb_tcp_listener_close(cb_tcp_listener_t *l)
{
    if (l->handle != CB__INVALID_SOCK)
    {
        CB__SOCK_CLOSE(l->handle);
        l->handle = CB__INVALID_SOCK;
    }
    return CB_INFO_OK;
}

cb_tcp_socket_t cb_tcp_accept(cb_tcp_listener_t *l)
{
    cb_tcp_socket_t s;
    s.info = CB_INFO_OK;
    s.handle = CB__INVALID_SOCK;

#ifdef CB_PLATFORM_WINDOWS
    SOCKET fd = accept(l->handle, NULL, NULL);
    if (fd == CB__INVALID_SOCK)
    {
        int err = CB__SOCK_LAST_ERR();
        s.info = (err == CB__SOCK_WOULDBLOCK) ? CB_INFO_NET_WOULD_BLOCK
                                              : CB_INFO_NET_ACCEPT_FAILED;
        return s;
    }
#else
    int fd = accept(l->handle, NULL, NULL);
    if (fd < 0)
    {
        int err = CB__SOCK_LAST_ERR();
        if (err == EAGAIN || err == EWOULDBLOCK)
        {
            s.info = CB_INFO_NET_WOULD_BLOCK;
        }
        else
        {
            s.info = CB_INFO_NET_ACCEPT_FAILED;
        }
        return s;
    }
#endif

    if (cb__set_nonblocking(fd) != CB_INFO_OK)
    {
        CB__SOCK_CLOSE(fd);
        s.info = CB_INFO_NET_NONBLOCK_FAILED;
        return s;
    }

    s.handle = fd;
    return s;
}

cb_tcp_socket_t cb_tcp_connect(cb_net_addr_t to)
{
    cb_tcp_socket_t s;
    s.info = CB_INFO_OK;
    s.handle = CB__INVALID_SOCK;

    cb__sockfd_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == CB__INVALID_SOCK)
    {
        s.info = CB_INFO_NET_SOCKET_FAILED;
        return s;
    }

    struct sockaddr_in sa;
    cb__addr_to_sockaddr(to, &sa);

    /* Blocking connect; flip to non-blocking once the handshake is done. */
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0)
    {
        CB__SOCK_CLOSE(fd);
        s.info = CB_INFO_NET_CONNECT_FAILED;
        return s;
    }

    if (cb__set_nonblocking(fd) != CB_INFO_OK)
    {
        CB__SOCK_CLOSE(fd);
        s.info = CB_INFO_NET_NONBLOCK_FAILED;
        return s;
    }

    s.handle = fd;
    return s;
}

cb_info_t cb_tcp_close(cb_tcp_socket_t *s)
{
    if (s->handle != CB__INVALID_SOCK)
    {
        CB__SOCK_CLOSE(s->handle);
        s->handle = CB__INVALID_SOCK;
    }
    return CB_INFO_OK;
}

cb_net_io_result_t cb_tcp_send(cb_tcp_socket_t *s, const void *data, size_t size)
{
    cb_net_io_result_t r;
    r.info = CB_INFO_OK;
    r.bytes = 0;

#ifdef CB_PLATFORM_WINDOWS
    int sent = send(s->handle, (const char *)data, (int)size, 0);
#else
    ssize_t sent = send(s->handle, data, size, 0);
#endif
    if (sent < 0)
    {
        int err = CB__SOCK_LAST_ERR();
        r.info = (err == CB__SOCK_WOULDBLOCK) ? CB_INFO_NET_WOULD_BLOCK
                                              : CB_INFO_NET_SEND_FAILED;
        return r;
    }
    r.bytes = (size_t)sent;
    return r;
}

cb_net_io_result_t cb_tcp_recv(cb_tcp_socket_t *s, void *buf, size_t buf_size)
{
    cb_net_io_result_t r;
    r.info = CB_INFO_OK;
    r.bytes = 0;

#ifdef CB_PLATFORM_WINDOWS
    int got = recv(s->handle, (char *)buf, (int)buf_size, 0);
#else
    ssize_t got = recv(s->handle, buf, buf_size, 0);
#endif
    if (got < 0)
    {
        int err = CB__SOCK_LAST_ERR();
        r.info = (err == CB__SOCK_WOULDBLOCK) ? CB_INFO_NET_WOULD_BLOCK
                                              : CB_INFO_NET_RECV_FAILED;
        return r;
    }
    if (got == 0)
    {
        r.info = CB_INFO_NET_CLOSED;
        return r;
    }
    r.bytes = (size_t)got;
    return r;
}

/* ================================================================ */
/*  Polling                                                         */
/* ================================================================ */

cb_info_t cb_net_poll(cb_net_pollable_t *items, size_t count, int timeout_ms)
{
#ifdef CB_PLATFORM_WINDOWS
    /* WSAPoll uses the same struct layout as pollfd, but named WSAPOLLFD. */
    WSAPOLLFD *pfds = (WSAPOLLFD *)malloc(sizeof(WSAPOLLFD) * count);
    if (!pfds) return CB_INFO_ALLOC_FAILED;

    for (size_t i = 0; i < count; i++)
    {
        pfds[i].fd = items[i].handle;
        pfds[i].events = 0;
        if (items[i].events & CB_NET_POLL_READ)  pfds[i].events |= POLLRDNORM;
        if (items[i].events & CB_NET_POLL_WRITE) pfds[i].events |= POLLWRNORM;
        pfds[i].revents = 0;
    }

    int rc = WSAPoll(pfds, (ULONG)count, timeout_ms);
    if (rc < 0)
    {
        free(pfds);
        return CB_INFO_NET_POLL_FAILED;
    }

    for (size_t i = 0; i < count; i++)
    {
        items[i].revents = 0;
        if (pfds[i].revents & POLLRDNORM) items[i].revents |= CB_NET_POLL_READ;
        if (pfds[i].revents & POLLWRNORM) items[i].revents |= CB_NET_POLL_WRITE;
        if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL))
            items[i].revents |= CB_NET_POLL_ERROR;
    }

    free(pfds);
    return CB_INFO_OK;
#else
    struct pollfd *pfds = (struct pollfd *)malloc(sizeof(struct pollfd) * count);
    if (!pfds) return CB_INFO_ALLOC_FAILED;

    for (size_t i = 0; i < count; i++)
    {
        pfds[i].fd = items[i].handle;
        pfds[i].events = 0;
        if (items[i].events & CB_NET_POLL_READ)  pfds[i].events |= POLLIN;
        if (items[i].events & CB_NET_POLL_WRITE) pfds[i].events |= POLLOUT;
        pfds[i].revents = 0;
    }

    int rc = poll(pfds, (nfds_t)count, timeout_ms);
    if (rc < 0)
    {
        free(pfds);
        return CB_INFO_NET_POLL_FAILED;
    }

    for (size_t i = 0; i < count; i++)
    {
        items[i].revents = 0;
        if (pfds[i].revents & POLLIN)  items[i].revents |= CB_NET_POLL_READ;
        if (pfds[i].revents & POLLOUT) items[i].revents |= CB_NET_POLL_WRITE;
        if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL))
            items[i].revents |= CB_NET_POLL_ERROR;
    }

    free(pfds);
    return CB_INFO_OK;
#endif
}
