# Research: NetBSD LAN rx-hook binding for the cluster port driver (FC-P0.3)

Status: DONE (P0 spike). Pure research — no code changes. Feeds FC-P0.4
(`exec_kbackend_netbsd.h` §14 LAN port binding).

## Source examined

NetBSD **10.1/amd64** `syssrc` (the pinned substrate,
`tests/netbsd/netbsd_version.env`), extracted from the cached tarball used by
the harness build
(`/data/nyquist/p4a-netbsd-cache/anita-netbsd-shared-10.1-amd64/download/source/sets/syssrc.tgz`,
matches the CDN's `NETBSD_SYSSRC_SHA512` pin). Files read:
`sys/net/if_ethersubr.c`, `sys/net/pfil.{c,h}`, `sys/net/if.{c,h}`,
`sys/net/if_bridge.c`, `sys/dev/qbus/if_qe.c`. Clean-room note (Rule 8): this
records facts about the **public NetBSD kernel API** for OVMX's own binding
code to call; no NetBSD source is copied into OVMX.

The design doc (`docs/design-faithful-cluster-executive.md` §3.2.1, §5.6)
already specifies the two candidate rx mechanisms and the questions the spike
must answer. This doc answers them.

## 1. Which rx hook exists for ethertype 0x6007 (SCA)

**`pfil(9)` on `ifp->if_pfil`, run from `ether_input()`, is present and
viable on NetBSD 10.1.**

- `ether_input()` runs the per-interface pfil hook chain *before* ethertype
  dispatch:
  `sys/net/if_ethersubr.c:749-757`
  ```c
  if ((m->m_flags & M_PROMISC) == 0) {
          if (pfil_run_hooks(ifp->if_pfil, &m, ifp, PFIL_IN) != 0)
                  return;
          if (m == NULL)
                  return;
          eh = mtod(m, struct ether_header *);
          etype = ntohs(eh->ether_type);
  }
  ```
  This runs strictly before the `switch (etype)` at
  `sys/net/if_ethersubr.c:907-955` that dispatches IP/ARP/etc. — 0x6007 is
  not a case there, so an unclaimed SCA frame would otherwise fall through
  to `noproto`/drop. The pfil hook is the only place to intercept it before
  that happens.
- `ifp->if_pfil` is a genuine **per-interface** filter point (distinct from
  the module-global `if_pfil` in `if.c`), created at interface attach and
  destroyed at detach:
  `sys/net/if.c:754-755` — `ifp->if_pfil = pfil_head_create(PFIL_TYPE_IFNET, ifp);`
  `sys/net/if.c:1526-1527` — destroyed in `if_detach()`.
  `sys/net/if.h:363` — `pfil_head_t *if_pfil; /* :: filtering point */` (a
  public `struct ifnet` member).
- Registration API (`sys/net/pfil.h:42,45,67-68`):
  ```c
  typedef int (*pfil_func_t)(void *, struct mbuf **, struct ifnet *, int);
  #define PFIL_IN  0x00000001
  int pfil_add_hook(pfil_func_t, void *, int, pfil_head_t *);
  int pfil_remove_hook(pfil_func_t, void *, int, pfil_head_t *);
  ```
  so the binding attaches with `pfil_add_hook(scap_pfil_rx, ctx, PFIL_IN, ifp->if_pfil)`
  and detaches with the matching `pfil_remove_hook`, exactly the shape
  `exec_lan_open`/`exec_lan_close` need.
- **Caveat found, not in the design doc's table**: pfil is skipped when the
  mbuf carries `M_PROMISC` (`if_ethersubr.c:742-751` — set when the
  interface is `IFF_PROMISC` and the frame's dest MAC doesn't match ours).
  This does **not** block us: OVMX never needs `IFF_PROMISC` on ETH0: — SCA
  frames arrive either unicast to our own MAC or to the joined cluster
  multicast group, both of which clear the `M_PROMISC` path and always hit
  the hook. It only matters if some *other* consumer later puts ETH0: into
  promiscuous mode; worth a one-line comment at the `exec_lan_open` call
  site.
- 0x6007 (24583) is safely above `ETHERMTU` (1500), so
  `if_ethersubr.c:495`'s length-vs-ethertype disambiguation (802.3 raw
  frames use lengths ≤ 1500) never misclassifies it — it's always treated
  as an EtherType field.

**`ifp->if_input`/`_if_input` interposition is the in-tree alternative**,
used by `bridge(4)` (the design doc's cited precedent), confirmed at
`sys/net/if_bridge.c`:
- Member add: `sys/net/if_bridge.c:852` guards `ifs->_if_input != ether_input`
  (refuses to add a member already claimed by something else), then
  `sys/net/if_bridge.c:911` sets `ifs->_if_input = bridge_input;`
- Member remove restores it: `sys/net/if_bridge.c:737` — `ifs->_if_input = ether_input;`
- `_if_input` is the actual driver-facing input pointer
  (`sys/net/if.h:326` — `void (*_if_input)(...) /* :: input routine (from h/w driver) */`)
  invoked by the generic `if_input()` dispatcher
  (`sys/net/if.c:1131-1140`, and the percpuq drain path `if.c:849`).
- **This mechanism is heavier than we need**: `bridge(4)` uses it because a
  bridge must see *every* frame regardless of dest MAC, so it additionally
  forces `ifpromisc(ifs, 1)` on the member (`if_bridge.c:888`). OVMX only
  wants one ethertype and doesn't need promiscuous reception. Interposing
  `_if_input` would also fully own the interface's receive path — nothing
  else (including a future `bridge`/`agr` use of ETH0:, or the normal
  IP stack if OVMX ever runs both TCP/IP and cluster on the same NIC) could
  coexist. `pfil` composes with other pfil consumers and leaves IP/ARP
  dispatch on the interface untouched.

**Recommendation: use path (a), `pfil(9)`.** It's present on the pinned
10.1 rail, needs no promiscuous mode, doesn't monopolize the interface, and
its call shape (`pfil_add_hook`/`pfil_remove_hook`) maps directly onto
`exec_lan_open`/`exec_lan_close`. Reserve `_if_input` interposition as a
fallback only if a future NetBSD rail version removes `if_pfil` from
`ether_input` (not the case in 10.1).

## 2. IPL at which `qe(4)` (Qbus DEQNA/DELQA-T, the VAX-relevant driver)
   delivers input

Note: the pinned syssrc's `sys/dev/qbus/` has **`if_qe.c`** (DEQNA/DELQA
Qbus Ethernet) but no `if_xq.c` — `xq` is not part of this tree; `qe` is the
one Qbus Ethernet driver present and is what the NetBSD-VAX rail (SIMH
`qe`/DEQNA emulation) uses.

`qe(4)`'s interrupt handler enqueues received mbufs onto the **generic
`if_percpuq` framework** rather than calling `ether_input()` directly:

```
sys/dev/qbus/if_qe.c:565     qeintr(void *arg)              <- UBA interrupt handler
sys/dev/qbus/if_qe.c:335-336 uba_intr_establish(ua->ua_icookie, ua->ua_cvec, qeintr, sc, ...)
sys/dev/qbus/if_qe.c:583-592   m = sc->sc_rxmbuf[...]; ...
                                if_percpuq_enqueue(ifp->if_percpuq, m);
```

`if_percpuq_enqueue()` only queues the mbuf and schedules a softint; the
actual `ether_input()` call (and therefore the pfil hook) happens later, out
of the hardware-interrupt context, in the softint drain routine:

```
sys/net/if.c:838-850   if_percpuq_softint(void *arg) {
                            ...
                            ifp->_if_input(ifp, m);   /* == ether_input by default */
                        }
sys/net/if.c:863-872   if_percpuq_create(): flags = SOFTINT_NET;
                        ipq->ipq_si = softint_establish(flags, if_percpuq_softint, ipq);
```

So the answer to "at what IPL does `qe` deliver input" has two parts:

- The **hardware interrupt** (`qeintr`, running at the Qbus/UBA device
  interrupt priority established by `uba_intr_establish`) only copies the
  descriptor into an mbuf and calls `if_percpuq_enqueue` — no protocol code
  runs there.
- The **pfil hook itself runs at `SOFTINT_NET`** (i.e. `IPL_SOFTNET`), the
  same softint level as every other `if_percpuq`-based NIC on this NetBSD
  version. `qe` is not a "runs ether_input from the raw interrupt" driver —
  this corrects/narrows the design doc's §3.2.1 note ("`rx_cb` runs at
  softint … or in the driver interrupt on the VAX `qe`/`xq` drivers"): on
  the actual pinned 10.1 `qe.c`, it is softint, not the raw driver
  interrupt.

**Consequence for contract rule 1**: `exec_lan_open`'s rx callback
(`scap_pfil_rx`) is invoked at `IPL_SOFTNET`/`SOFTINT_NET` on this rail. The
rx-queue spinlock the binding initializes for it should be
`mutex_init(&lock, MUTEX_DEFAULT, IPL_SOFTNET)` (or the numerically
equal/higher `IPL_NET` if the fork-thread side also touches it from a
lower-priority context — the binding picks the ceiling of both sides, per
the existing accessor discipline). No code needed for the spike; this is
the fact FC-P0.4 consumes when it writes `exec_kbackend_netbsd.h`.

## 3. `if_transmit`/`if_output`: does it accept a pre-built frame?

**Yes — `if_transmit()`/`if_transmit_lock()` takes an mbuf with a complete
Ethernet header already installed and does no ARP/route/sockaddr work**,
whether or not the underlying driver implements `if_transmit` natively.

- `bridge(4)` is the in-tree precedent the design doc cites, and it
  confirms the exact call shape: it forwards an already-Ethernet-framed
  mbuf straight to `if_transmit_lock()`, bypassing `if_output`/ARP
  entirely:
  `sys/net/if_bridge.c:1531` — `error = if_transmit_lock(dst_ifp, m);`
  (inside `bridge_enqueue()`, `if_bridge.c:1486-1531`).
- `qe(4)` is a **legacy `if_start`-model driver** — it sets
  `ifp->if_start = qestart` (`if_qe.c:343`) and never assigns
  `ifp->if_transmit` itself. `if_attach()` auto-populates a generic
  wrapper in that case:
  `sys/net/if.c:810-811`
  ```c
  if (ifp->if_transmit == NULL || ifp->if_transmit == if_nulltransmit)
          ifp->if_transmit = if_transmit;
  ```
  and that generic `if_transmit()` (`sys/net/if.c:3777-3803`) just
  `IFQ_ENQUEUE`s the mbuf onto `ifp->if_snd` **as-is** and kicks
  `if_start_lock()` → `qestart()`, which DMAs the mbuf's own bytes to the
  wire. No header is built or rewritten anywhere in this path — the caller
  must have already set `ether_dhost`/`ether_shost`/`ether_type`.
- `if_transmit_lock()` (`sys/net/if.c:3806-3828`) is the uniform entry
  point regardless of whether the driver implements `if_transmit` natively
  or falls back to the generic wrapper — it's what `exec_lan_xmit` should
  call: `(void)if_transmit_lock(ifp, m)` after `m_gethdr`/`m_copyback` of
  the fully-built SCA frame, matching the design doc's binding row exactly.
- This is callable from the fork thread (process/kthread context) with no
  `KERNEL_LOCK` requirement in the non-ALTQ path (`if.c:3822-3825` — the
  default `#else /* !ALTQ */` branch is a direct call, no lock).

## 4. Multicast-add path (cluster HELLO multicast join)

**`if_mcast_op(ifp, SIOCADDMULTI, sa)` is the documented, sleep-capable
entry point** — confirmed present and exactly as the design doc specifies:

```
sys/net/if.h:1138       int if_mcast_op(ifnet_t *, const unsigned long, const struct sockaddr *);
sys/net/if.c:3965-3994  if_mcast_op(): "Use this, not if_ioctl, for the
                         multicast commands." / "May sleep." Builds an
                         ifreq via ifreq_setaddr(cmd, &ifr, sa) and calls
                         if_ioctl(ifp, cmd, &ifr).
```

`sa` is a `struct sockaddr_dl` (`AF_LINK`, `sdl_alen = 6`, the group MAC in
`sdl_data`) — the standard shape every NetBSD SIOCADDMULTI caller uses;
`if_ioctl` reaches the driver's ioctl routine, which for `qe(4)` is:

```
sys/dev/qbus/if_qe.c:695-704   case SIOCADDMULTI: case SIOCDELMULTI:
                                    if ((error = ether_ioctl(ifp, cmd, data)) == ENETRESET) {
                                        if (ifp->if_flags & IFF_RUNNING)
                                            qe_setup(sc);   /* reprograms the hw filter */
                                        error = 0;
                                    }
```

`ether_ioctl` → `ether_addmulti` (`sys/net/if_ethersubr.c:1345`) maintains
the `ethercom` multicast list; `qe_setup()` reprograms the DEQNA's hardware
address filter from that list (`if_qe.c:790-833`).

**Gotcha found, worth recording for FC-P0.4/P0.9**: the DEQNA hardware
filter in `qe_setup()` holds at most 12 explicit multicast addresses
(`if_qe.c` comment at 798: "The DEQNA can handle up to 12 direct ethernet
addresses"); past that limit — or on any address-*range* multicast entry —
the driver falls back to `IFF_ALLMULTI`, and because it doesn't know how to
put the DEQNA into a true ALLMULTI mode it further escalates to
`IFF_PROMISC` (`if_qe.c:826-830`, comment: "How is the DEQNA turned in
ALLMULTI mode??? … fall back to PROMISC when more than 12 ethernet
addresses"). Recall from §1 above that `IFF_PROMISC` is exactly the
condition that makes `ether_input()` set `M_PROMISC` and **skip the pfil
hook** for frames not addressed to us. OVMX's cluster join adds exactly one
multicast group (`AB-00-04-01-<group>`), well under the 12-address limit,
so this does not bite in isolation — but if some other consumer on the same
`qe` NIC later pushes the combined multicast count over 12, cluster rx
would silently go dark. Not a P0.3 blocker; flag it as a one-line comment
at the `exec_lan_mc_add` NetBSD binding call site so a future multicast
consumer on ETH0: doesn't reintroduce it silently.

`if_mcast_op` "may sleep" (`if.c:3972`) — consistent with it being called
only from `exec_lan_open` in the fork thread's setup path, never from
`rx_cb` (contract rule 1 already forbids sleeping there for unrelated
reasons; this confirms the multicast join call site is correctly placed
outside the rx path).

## Recommendation for FC-P0.4

1. **rx**: `pfil(9)` on `ifp->if_pfil` in `ether_input()`. Attach in
   `exec_lan_open` via `pfil_add_hook(scap_pfil_rx, ctx, PFIL_IN,
   ifp->if_pfil)`; detach in `exec_lan_close` via the matching
   `pfil_remove_hook`. `scap_pfil_rx` filters on
   `ntohs(eh->ether_type) == 0x6007`; on match, copies into the core's
   `exec_lanbuf_t` pool buffer, enqueues under the rx-IPL lock, wakes the
   fork thread, and consumes the mbuf (`m_freem` + return nonzero) so it
   never reaches IP/ARP dispatch; on no match, returns 0 (`m` untouched) so
   normal dispatch continues — this is what lets ETH0: keep serving TCP/IP
   or other consumers unmodified.
2. **rx-queue lock IPL**: `IPL_SOFTNET` (the `if_percpuq`/`SOFTINT_NET`
   level `qe(4)` delivers at, per §2). Never allocate or sleep in
   `scap_pfil_rx`, per contract rule 1.
3. **tx**: `m_gethdr(M_DONTWAIT, MT_DATA)` + `m_copyback` of the complete,
   pre-built SCA frame (dhost/shost/ethertype already set by
   `vms_pe.c`/codec), then `if_transmit_lock(ifp, m)`. No `ether_output`
   involvement — matches the design doc's binding row and what `bridge(4)`
   does.
4. **multicast**: `exec_lan_mc_add`/`_del` call
   `if_mcast_op(ifp, SIOCADDMULTI/SIOCDELMULTI, (const struct sockaddr *)&sdl)`
   with a `sockaddr_dl` built from the group MAC, from the fork thread's
   `exec_lan_open` setup path (never from `rx_cb`). One-line comment at the
   call site noting the DEQNA 12-address/ALLMULTI→PROMISC fallback (§4).

Neither outcome changes `kernel-core`; both are the one-screen adapters
§3.2.1 already sized (`exec_kbackend_netbsd.h` §14, ~300 lines total for
the whole NetBSD LAN binding, not just this part).

## Blockers

None. The spike answered every fact FC-P0.4 needs from the pinned NetBSD
10.1 tree; no ambiguity requiring an escalation.
