/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_kif_bg_ops.h - the BGn: (INET pseudo-device) executive-service marshalling,
 * ONE source shared by two transports (rd vms-99f).
 *
 * NOT a normal header: it is a FRAGMENT of function definitions, included exactly
 * once by each of
 *   - src/libvmssys/vms_kif.c        (LIBVMSSYS$SHR, the sys$ service layer), and
 *   - src/vmstcpip/sockets/ovmx_decc_sockets.c (DECC$SHR, the C RTL sockets).
 * Each includer supplies, before the #include:
 *   bg_bind_ok()        -> nonzero when this process holds a /dev/vms descriptor
 *   KIF_CALL(req, args) -> issue one ioctl; return its VMS status from the
 *                          enclosing function if it could not be delivered
 *   vms_memset/vms_memcpy/vms_strncpy, and the vms_ioctl.h / ssdef.h definitions.
 * WHY TWO INCLUDERS, NOT A CALL ACROSS IMAGES. DECC$SHR must stay a non-TLS
 * producer (vms-212), and vms_kif.c keeps its /dev/vms descriptor in __thread
 * storage, so DECC$SHR cannot link vms_kif.c; and DECC$SHR has no --use of
 * LIBVMSSYS$SHR (every OVMX image is built against DECC$SHR first). Sharing the
 * marshalling text keeps ONE definition of each BGn: request while each image keeps
 * its own descriptor. Closing or opening /dev/vms is nothing to the process
 * (vms_dev_release, rd vms-9f32), so two descriptors in one process are harmless.
 */
/* ================================================================
 * INET pseudo-device BGn: (vms-527)
 *
 * Thin marshalling wrappers over the VMS_IOCTL_BG_* driver in vms.ko
 * (src/kernel/vms_bg.c), the exact analogue of the vms_kif_mbx_* wrappers
 * above: bind to /dev/vms (honest SS$_NOSUCHDEV if the executive is absent --
 * INV-6, never a userspace socket fallback), issue one ioctl, hand back the
 * driver's own status. There is NO userspace socket here; the socket lives in
 * the executive.
 * ================================================================ */


uint32_t vms_kif_bg_create(uint32_t *exec_chan, uint32_t *unit,
                           char *devnam, uint32_t devnam_sz)
{
    struct vms_bg_create_args args;

    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));

    KIF_CALL(VMS_IOCTL_BG_CREATE, &args);

    if (args.status & 1) {
        if (exec_chan) *exec_chan = args.chan;
        if (unit) *unit = args.unit;
        if (devnam && devnam_sz > 0) {
            vms_strncpy(devnam, args.devnam, devnam_sz - 1);
            devnam[devnam_sz - 1] = '\0';
        }
    }
    return args.status;
}

uint32_t vms_kif_bg_setmode(uint32_t exec_chan)
{
    struct vms_bg_chanonly_args args;

    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;

    KIF_CALL(VMS_IOCTL_BG_SETMODE, &args);

    return args.status;
}

uint32_t vms_kif_bg_setmode_icmp(uint32_t exec_chan)
{
    struct vms_bg_chanonly_args args;

    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;

    KIF_CALL(VMS_IOCTL_BG_SETMODE_ICMP, &args);

    return args.status;
}

uint32_t vms_kif_bg_setmode_udp(uint32_t exec_chan)
{
    struct vms_bg_chanonly_args args;

    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;

    KIF_CALL(VMS_IOCTL_BG_SETMODE_UDP, &args);

    return args.status;
}

uint32_t vms_kif_bg_connect(uint32_t exec_chan, uint16_t family,
                            uint16_t port, uint32_t addr)
{
    struct vms_bg_connect_args args;

    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;
    args.sin_family = family;
    args.sin_port = port;   /* network byte order, caller's responsibility */
    args.sin_addr = addr;   /* network byte order */

    KIF_CALL(VMS_IOCTL_BG_CONNECT, &args);

    return args.status;
}

uint32_t vms_kif_bg_send(uint32_t exec_chan, const void *buf, uint32_t len,
                         uint32_t *actlen)
{
    struct vms_bg_io_args args;

    if (!buf)
        return SS$_BADPARAM;
    if (len > VMS_BG_IOCTL_MAXLEN)
        return SS$_BADPARAM;
    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;
    args.len = len;
    vms_memcpy(args.data, buf, len);

    KIF_CALL(VMS_IOCTL_BG_SEND, &args);

    if ((args.status & 1) && actlen) *actlen = args.len;
    return args.status;
}

/* now != 0: IO$M_NOW -- SS$_ENDOFFILE and no bytes when nothing is queued. */
uint32_t vms_kif_bg_recv_ex(uint32_t exec_chan, void *buf, uint32_t bufsz,
                            uint32_t *actlen, int now)
{
    struct vms_bg_io_args args;
    uint32_t n;

    if (!buf)
        return SS$_BADPARAM;
    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;
    args.len = (bufsz > VMS_BG_IOCTL_MAXLEN) ? VMS_BG_IOCTL_MAXLEN : bufsz;
    args.flags = now ? 1u : 0u;

    KIF_CALL(VMS_IOCTL_BG_RECV, &args);

    if (args.status & 1) {
        n = args.len;
        if (n > bufsz) n = bufsz;
        if (n > VMS_BG_IOCTL_MAXLEN) n = VMS_BG_IOCTL_MAXLEN;
        vms_memcpy(buf, args.data, n);
        if (actlen) *actlen = args.len;
    }
    return args.status;
}

uint32_t vms_kif_bg_recv(uint32_t exec_chan, void *buf, uint32_t bufsz,
                         uint32_t *actlen)
{
    return vms_kif_bg_recv_ex(exec_chan, buf, bufsz, actlen, 0);
}

uint32_t vms_kif_bg_deaccess(uint32_t exec_chan)
{
    struct vms_bg_chanonly_args args;

    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;

    KIF_CALL(VMS_IOCTL_BG_DEACCESS, &args);

    return args.status;
}

uint32_t vms_kif_bg_dassgn(uint32_t exec_chan)
{
    struct vms_bg_chanonly_args args;

    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;

    KIF_CALL(VMS_IOCTL_BG_DASSGN, &args);

    return args.status;
}

uint32_t vms_kif_bg_pollfd(uint32_t exec_chan, int *out_fd)
{
    struct vms_bg_pollfd_args args;

    if (out_fd)
        *out_fd = -1;
    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;
    args.fd = -1;

    KIF_CALL(VMS_IOCTL_BG_POLLFD, &args);

    if ((args.status & 1) && out_fd)
        *out_fd = args.fd;
    return args.status;
}

/*
 * Materialize the channel's executive socket as a REAL, data-carrying fd (RUNG-3b).
 * Unlike vms_kif_bg_pollfd's readiness-only fd, the fd returned here has real
 * read/write that route to the executive socket, and NO O_CLOEXEC so it survives
 * execve -- the primitive the --wrap dup2/dup uses to hand a BGn: connection to a
 * ported daemon's exec'd child. Fail-honest: no /dev/vms -> SS$_NOSUCHDEV; bad
 * channel / no socket -> SS$_IVCHAN (from the executive). Never a fabricated fd.
 */
uint32_t vms_kif_bg_materialize_fd(uint32_t exec_chan, int *out_fd)
{
    struct vms_bg_datafd_args args;

    if (out_fd)
        *out_fd = -1;
    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;
    args.fd = -1;

    KIF_CALL(VMS_IOCTL_BG_MATERIALIZE_FD, &args);

    if ((args.status & 1) && out_fd)
        *out_fd = args.fd;
    return args.status;
}

uint32_t vms_kif_bg_getname(uint32_t exec_chan, uint32_t which,
                            uint16_t *family, uint16_t *port, uint32_t *addr)
{
    struct vms_bg_name_args args;

    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;
    args.which = which;

    KIF_CALL(VMS_IOCTL_BG_GETNAME, &args);

    if (args.status & 1) {
        if (family) *family = args.sin_family;
        if (port)   *port   = args.sin_port;
        if (addr)   *addr   = args.sin_addr;
    }
    return args.status;
}

/* IO$_SENSEMODE interface list (rd vms-99f) -- the executive's own list of the
 * node's network interfaces, read from the stack the channel's socket lives in
 * (TCPIP SHOW INTERFACE). The channel must have its socket (IO$_SETMODE first).
 * `out` takes at most `max` entries (clamped to VMS_BG_IFLIST_MAX); *count gets
 * the number filled. /dev/vms absent -> SS$_NOSUCHDEV, never a host listing. */
uint32_t vms_kif_bg_iflist(uint32_t exec_chan, struct vms_bg_ifent *out,
                           uint32_t max, uint32_t *count)
{
    struct vms_bg_iflist_args args;
    uint32_t i;

    if (count) *count = 0;
    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;
    args.max = (max < VMS_BG_IFLIST_MAX) ? max : VMS_BG_IFLIST_MAX;

    KIF_CALL(VMS_IOCTL_BG_IFLIST, &args);

    if (args.status & 1) {
        uint32_t n = (args.count < args.max) ? args.count : args.max;
        for (i = 0; i < n; i++)
            if (out) vms_memcpy(&out[i], &args.ent[i], sizeof(out[i]));
        if (count) *count = n;
    }
    return args.status;
}

/* IO$_SENSEMODE readiness wait (rd vms-99f): the C RTL's select()/poll() on BG
 * channels. ent[0..n-1] name the channels and the VMS_BG_RDY_* conditions each
 * waits for; on return their revents are the executive's live readiness and
 * *nready the count ready. timeout_ms 0 = sense only, < 0 = no limit.
 * /dev/vms absent -> SS$_NOSUCHDEV. */
uint32_t vms_kif_bg_select(struct vms_bg_selent *ent, uint32_t n,
                           int32_t timeout_ms, uint32_t *nready)
{
    struct vms_bg_select_args args;
    uint32_t i;

    if (nready) *nready = 0;
    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;
    if (n > VMS_BG_SELECT_MAX)
        return SS$_BADPARAM;

    vms_memset(&args, 0, sizeof(args));
    args.n = n;
    args.timeout_ms = timeout_ms;
    for (i = 0; i < n; i++)
        args.ent[i] = ent[i];

    KIF_CALL(VMS_IOCTL_BG_SELECT, &args);

    for (i = 0; i < n; i++)
        ent[i].revents = args.ent[i].revents;
    if (nready) *nready = args.nready;
    return args.status;
}

/* Server path (vms-698). bind reads the effective local address back (ephemeral
 * port); accept installs the connection onto a second, pre-$ASSIGNed BG channel
 * (accept_exec_chan) and returns the peer address. /dev/vms absent -> SS$_NOSUCHDEV. */
uint32_t vms_kif_bg_bind(uint32_t exec_chan, uint16_t family, uint16_t port,
                         uint32_t addr, uint16_t *out_port, uint32_t *out_addr)
{
    struct vms_bg_bind_args args;

    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;
    args.sin_family = family;
    args.sin_port = port;   /* network byte order (0 = ephemeral) */
    args.sin_addr = addr;   /* network byte order (0 = INADDR_ANY) */

    KIF_CALL(VMS_IOCTL_BG_BIND, &args);

    if (args.status & 1) {  /* effective local address read back */
        if (out_port) *out_port = args.sin_port;
        if (out_addr) *out_addr = args.sin_addr;
    }
    return args.status;
}

uint32_t vms_kif_bg_listen(uint32_t exec_chan, int backlog)
{
    struct vms_bg_listen_args args;

    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;
    args.backlog = backlog;

    KIF_CALL(VMS_IOCTL_BG_LISTEN, &args);

    return args.status;
}

uint32_t vms_kif_bg_accept(uint32_t listen_exec_chan, uint32_t accept_exec_chan,
                           uint16_t *family, uint16_t *port, uint32_t *addr)
{
    struct vms_bg_accept_args args;

    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.listen_chan = listen_exec_chan;
    args.accept_chan = accept_exec_chan;

    KIF_CALL(VMS_IOCTL_BG_ACCEPT, &args);

    if (args.status & 1) {  /* peer address */
        if (family) *family = args.sin_family;
        if (port)   *port   = args.sin_port;
        if (addr)   *addr   = args.sin_addr;
    }
    return args.status;
}

uint32_t vms_kif_bg_setsockopt(uint32_t exec_chan, int level, int optname,
                               int optval)
{
    struct vms_bg_sockopt_args args;

    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;
    args.op = 0;                        /* set */
    args.level = level;
    args.optname = optname;
    args.optval = optval;

    KIF_CALL(VMS_IOCTL_BG_SOCKOPT, &args);

    return args.status;
}

uint32_t vms_kif_bg_getsockopt(uint32_t exec_chan, int level, int optname,
                               int *out_optval)
{
    struct vms_bg_sockopt_args args;

    if (!bg_bind_ok())
        return SS$_NOSUCHDEV;

    vms_memset(&args, 0, sizeof(args));
    args.chan = exec_chan;
    args.op = 1;                        /* get */
    args.level = level;
    args.optname = optname;

    KIF_CALL(VMS_IOCTL_BG_SOCKOPT, &args);

    if ((args.status & 1) && out_optval)
        *out_optval = args.optval;
    return args.status;
}
