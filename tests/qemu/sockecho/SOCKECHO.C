/*
 * SOCKECHO.C - a VMS-native sockets client/server for the DECC$SHR sockets proof
 * (rd vms-99f). Compiled to an object and LINKED THE VMS WAY (LINK.EXE
 * --executable --use DECC$SHR, PT_INTERP = IMGACT.EXE) by mk_sockecho_native.sh;
 * every socket call below binds to DECC$SHR's universals at activation. It is
 * driven, under a system-call audit, by tests/qemu/test_syssvc_decc_socket.c.
 *
 *   SOCKECHO client <port>   connect to the driver's listener on 127.0.0.1:<port>,
 *                            round-trip bytes, then run a server leg of its own
 *                            (bind/listen/accept) -- exit 0 only if all held.
 *   SOCKECHO noexec          run with no /dev/vms: socket() must FAIL (ENODEV),
 *                            never succeed through the host.
 *
 * Every verdict line starts "SOCKECHO: PASS" or "SOCKECHO: FAIL"; the driver
 * greps them. Nothing here knows about BGn:, $QIO or the executive -- it is an
 * ordinary BSD-sockets program, which is the point.
 */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

static int fails;

static void verdict(int ok, const char *what)
{
    printf("SOCKECHO: %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        fails++;
}

/* What the kernel says this fd is: an executive socket is vms.ko's
 * "anon_inode:[bgconn]"; a Linux socket would be "socket:[inode]". */
static void fd_kind(int fd, char *out, size_t n)
{
    char path[64];
    ssize_t k;

    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    k = readlink(path, out, n - 1);
    out[k > 0 ? k : 0] = '\0';
}

static int recv_exact(int fd, char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = recv(fd, buf + off, len - off, 0);
        if (n <= 0)
            return -1;
        off += (size_t)n;
    }
    return 0;
}

static int client(const char *port)
{
    static const char msg1[] = "OVMX-DECC-SOCKET-ROUNDTRIP-0123456789";
    static const char msg2[] = "second-message-over-write-and-read";
    struct addrinfo hints, *ai = NULL;
    char kind[128], buf[128];
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    int s, rc, one = 1, got = 0;
    socklen_t glen = sizeof(got);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    rc = getaddrinfo("127.0.0.1", port, &hints, &ai);
    verdict(rc == 0 && ai != NULL && ai->ai_family == AF_INET,
            "getaddrinfo resolves the numeric 127.0.0.1");
    if (rc != 0 || ai == NULL)
        return 1;
    {
        struct addrinfo *n = NULL;
        int r2 = getaddrinfo("ovmx-no-such-host.invalid", port, &hints, &n);
        verdict(r2 == EAI_FAIL && n == NULL,
                "getaddrinfo of a host NAME fails EAI_FAIL (no resolver until vms-a9e7; never the host's DNS)");
        verdict(gethostbyname("ovmx-no-such-host.invalid") == NULL,
                "gethostbyname of a host NAME fails (no host DNS)");
    }

    s = socket(ai->ai_family, ai->ai_socktype, 0);
    verdict(s >= 0, "socket(AF_INET, SOCK_STREAM) returns a descriptor");
    if (s < 0) {
        printf("SOCKECHO: INFO socket errno=%d\n", errno);
        return 1;
    }
    fd_kind(s, kind, sizeof(kind));
    printf("SOCKECHO: INFO socket fd %d is %s\n", s, kind);
    verdict(strcmp(kind, "anon_inode:[bgconn]") == 0,
            "the socket descriptor is the executive's BG connection (anon_inode:[bgconn]), not a Linux socket");

    rc = connect(s, ai->ai_addr, ai->ai_addrlen);
    verdict(rc == 0, "connect() to the in-guest listener");
    if (rc != 0) {
        printf("SOCKECHO: INFO connect errno=%d\n", errno);
        return 1;
    }
    rc = getpeername(s, (struct sockaddr *)&peer, &plen);
    verdict(rc == 0 && peer.sin_family == AF_INET &&
            peer.sin_port == ((struct sockaddr_in *)ai->ai_addr)->sin_port &&
            peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK),
            "getpeername() answers the true peer 127.0.0.1:<port>");
    rc = setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    verdict(rc == 0 && getsockopt(s, IPPROTO_TCP, TCP_NODELAY, &got, &glen) == 0 && got != 0,
            "setsockopt/getsockopt TCP_NODELAY reach the executive socket");

    verdict(send(s, msg1, sizeof(msg1) - 1, 0) == (ssize_t)(sizeof(msg1) - 1),
            "send() the whole message");
    {
        struct pollfd p = { .fd = s, .events = POLLIN, .revents = 0 };
        verdict(poll(&p, 1, 10000) == 1 && (p.revents & POLLIN),
                "poll() reports the socket readable when the echo arrives");
    }
    memset(buf, 0, sizeof(buf));
    verdict(recv_exact(s, buf, sizeof(msg1) - 1) == 0 &&
            memcmp(buf, msg1, sizeof(msg1) - 1) == 0,
            "recv() returns the echoed message BYTE-EXACT");

    verdict(write(s, msg2, sizeof(msg2) - 1) == (ssize_t)(sizeof(msg2) - 1),
            "write() on the socket descriptor");
    {
        fd_set r;
        struct timeval tv = { 10, 0 };
        FD_ZERO(&r);
        FD_SET(s, &r);
        verdict(select(s + 1, &r, NULL, NULL, &tv) == 1 && FD_ISSET(s, &r),
                "select() reports the socket readable");
    }
    memset(buf, 0, sizeof(buf));
    {
        size_t off = 0;
        while (off < sizeof(msg2) - 1) {
            ssize_t n = read(s, buf + off, sizeof(msg2) - 1 - off);
            if (n <= 0) break;
            off += (size_t)n;
        }
        verdict(off == sizeof(msg2) - 1 && memcmp(buf, msg2, off) == 0,
                "read() returns the second echo BYTE-EXACT");
    }
    verdict(close(s) == 0, "close() the connection");
    freeaddrinfo(ai);

    /* Server leg: this image's own listener, an executive socket too. */
    {
        struct sockaddr_in a;
        socklen_t al = sizeof(a);
        int l, c, x;

        l = socket(AF_INET, SOCK_STREAM, 0);
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        verdict(l >= 0 && bind(l, (struct sockaddr *)&a, sizeof(a)) == 0 &&
                listen(l, 4) == 0 &&
                getsockname(l, (struct sockaddr *)&a, &al) == 0 && a.sin_port != 0,
                "bind()/listen()/getsockname() an ephemeral listener");
        c = socket(AF_INET, SOCK_STREAM, 0);
        verdict(c >= 0 && connect(c, (struct sockaddr *)&a, sizeof(a)) == 0,
                "connect() a second socket to this image's own listener");
        x = accept(l, NULL, NULL);
        fd_kind(x, kind, sizeof(kind));
        verdict(x >= 0 && strcmp(kind, "anon_inode:[bgconn]") == 0,
                "accept() returns an executive BG connection descriptor");
        memset(buf, 0, sizeof(buf));
        verdict(send(c, "ping", 4, 0) == 4 && recv_exact(x, buf, 4) == 0 &&
                memcmp(buf, "ping", 4) == 0,
                "bytes cross from the connecting socket to the accepted one");
        close(c);
        verdict(recv(x, buf, sizeof(buf), 0) == 0,
                "close() of the peer is seen as end-of-stream on the accepted socket");
        close(x);
        close(l);
    }
    return fails ? 1 : 0;
}

static int noexec(void)
{
    int s;

    errno = 0;
    s = socket(AF_INET, SOCK_STREAM, 0);
    printf("SOCKECHO: INFO no-executive socket() = %d errno=%d\n", s, errno);
    verdict(s < 0 && errno == ENODEV,
            "with no executive socket() FAILS with ENODEV (no host-socket fallback)");
    return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    int rc;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc >= 3 && strcmp(argv[1], "client") == 0)
        rc = client(argv[2]);
    else if (argc >= 2 && strcmp(argv[1], "noexec") == 0)
        rc = noexec();
    else {
        printf("SOCKECHO: FAIL usage: SOCKECHO client <port> | noexec\n");
        rc = 2;
    }
    printf("SOCKECHO: DONE %s\n", rc == 0 ? "ok" : "failed");
    return rc;
}
