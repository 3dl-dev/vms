/*
 * ovmx_identity.h - System-identity single source of truth (INV-1)
 *
 * ONE module owns system identity. Every surface that names the system --
 * SHOW SYSTEM, the DCL/SSH/LOGINOUT banners, the boot banner, F$GETSYI
 * VERSION, MONITOR headers -- reads it here. Nothing anywhere else may
 * hardcode a version string; the identity gate test enforces that
 * (tests/libvms/test_identity.c, "no hardcoded version" pass).
 *
 * Spec: docs/design-authenticity-roadmap.md sec 4.5 INV-1 (rd vms-e652),
 * constrained by INV-0 trademark ceiling (rd vms-e8f).
 *
 * ---------------------------------------------------------------------
 * DUAL IDENTITY (operator ruling D1, 2026-07-27)
 * ---------------------------------------------------------------------
 *
 * 1. OVMX PRODUCT VERSION -- the brand, ours, shown to HUMANS.
 *    OVMX tags its own version (V0.1 at first release -> V1 eventually).
 *    Human-facing surfaces show it badged per INV-0:
 *        "OpenVMX V0.1 - OpenVMS-compatible"
 *    A human surface must NEVER answer "OpenVMS Vx.y" bare: that claims to
 *    BE VSI's product as-to-source, which is the passing-off line INV-0
 *    draws. "OpenVMS"/"VMS" are VSI trademarks -- clean-room RE licenses
 *    the copyright, not the mark.
 *
 * 2. VMS-COMPAT VERSION -- the token MACHINES read to interoperate.
 *    F$GETSYI("VERSION") / SYI$_VERSION report the real VSI version for the
 *    arch actually running. This is functional interoperability (the
 *    Wine/Samba/ReactOS posture), and it is derived from the arch -- it is
 *    NOT a global constant.
 *
 * 3. IRON RULE -- NEVER LIE TO THE METAL.
 *    OVMX reports the arch it actually runs on. Where that arch has a VMS
 *    lineage (x86-64), the compat token matches it. Where it does NOT
 *    (aarch64 -- OpenVMS never ran on ARM), OVMX does not fabricate a VMS
 *    arch: it answers with its own honest identity. OVMX-on-ARM is a NEW
 *    frontier to celebrate, not a tell to paper over.
 *
 * ---------------------------------------------------------------------
 * PURITY (INV-5, [[vms-purity-guardrail]])
 * ---------------------------------------------------------------------
 * OVMX_VMS_COMPAT_VERSION_X86_64 is a VMS-authentic value and carries the
 * standing purity obligation: it must be pinned to the oracle (public VSI
 * release documentation / the ~/vax lab) with operator sign-off, never
 * self-certified. The V9.2-x FAMILY is operator-ruled (D1); the patch level
 * and the field's blank-padding width are open -- see rd vms-e652 notes.
 *
 * ---------------------------------------------------------------------
 * DUAL-IDENTITY REBRAND, GNU/Linux-STYLE (operator ruling, rd vms-700 /
 * vms-296 / vms-3de, 2026-08-11)
 * ---------------------------------------------------------------------
 * The single word "OVMX" used to name two different things at once: the
 * VMS-compatible PRODUCT a human logs into, and the SYSKRNL layer (the Linux
 * kernel underneath) it runs on. This split follows the GNU/Linux convention -- "GNU/Linux"
 * names the base (kernel + userland underneath), while a distribution built
 * on it carries its own separate brand:
 *
 *   PRODUCT (OVMX_PRODUCT_NAME) = "OpenVMX" -- the VMS-compatible
 *   environment a human touches: login banner, SHOW SYSTEM, MONITOR, DCL
 *   identity. This is what changed here.
 *
 *   SYSKRNL (OVMX_SYSKRNL_NAME) = "OVMX/Linux" -- the Linux base layer
 *   underneath, WITH THE SLASH, exactly like "GNU/Linux": an operating
 *   ENVIRONMENT layered on a KERNEL, not the environment itself. Surfaced
 *   at early boot (before STARTUP.EXE hands off to the OpenVMX/VMS
 *   environment -- src/ovmx_init/ovmx_init.c) and in distro build metadata
 *   (distro/Dockerfile.bootable LABELs, distro/rootfs/etc/os-release).
 *   Never shown on a VMS-facing surface -- see INV-4's "no Linux leak on a
 *   VMS-facing surface" note on ovmx_node_name() below; this is the mirror
 *   image, an intentionally LINUX-facing surface.
 *
 * The bare, un-slashed "OVMX" token remains load-bearing as a NON-BRAND
 * machine identifier in many places this rebrand must NOT touch: VMS
 * facility/status codes (OVMX$_* in src/libvms/status.c), the IMGACT
 * ELF-note owner (imgact_activate.h), the SCS/cluster wire OS-name field
 * (src/vmsscs/scsd.c), nodename fallbacks (OVMX_DEFAULT_NODENAME, below),
 * and the kit/product-database producer field (OVMX_VENDOR_TOKEN, below --
 * deliberately NOT OVMX_PRODUCT_NAME, so this rebrand does not silently
 * reflow into on-disk kit/product records). See
 * tests/integration/test_frozen_identity_tokens.sh, the tripwire gate that
 * pins these byte values against a future blind rename.
 */
#ifndef OVMX_IDENTITY_H
#define OVMX_IDENTITY_H

#include <stddef.h>
#include <string.h>

#include "sysgen_params.h"

/* ---- Brand identity (human surfaces, INV-0) ---------------------- */

#define OVMX_PRODUCT_NAME       "OpenVMX"
#define OVMX_PRODUCT_VERSION    "V0.4-6"

/*
 * INV-0 badge. Attached to human-facing identity so the answer to "what is
 * this" is honest: an OpenVMS-COMPATIBLE system, not OpenVMS. If VSI ever
 * objects to the mark in the badge, the fallback is the hacker-tradition
 * dodge spelling ("0penVMS-compatible", cf. un*x) -- change it HERE, once.
 */
#define OVMX_COMPAT_BADGE       "OpenVMS-compatible"

/* "OpenVMX V0.1" -- brand + version, no badge (tight columns, e.g. MONITOR). */
#define OVMX_PRODUCT_ID         OVMX_PRODUCT_NAME " " OVMX_PRODUCT_VERSION

/* "OpenVMX V0.1 - OpenVMS-compatible" -- the full honest human answer. */
#define OVMX_PRODUCT_BANNER     OVMX_PRODUCT_ID " - " OVMX_COMPAT_BADGE

/*
 * OVMX_VENDOR_TOKEN -- the machine-facing vendor/producer identifier baked
 * into on-disk formats OVMX invented itself: the kit-container producer
 * field and the product-database producer field
 * (tools/ovmx_kit_pack.c, src/product/ovmx_product_db.h, PRODUCT SHOW
 * PRODUCT's "Producer:" column). DELIBERATELY NOT OVMX_PRODUCT_NAME: this
 * is a stable machine-read identifier, already baked into shipped kit
 * files, and must not silently reflow every time the human-facing brand
 * changes (see the DUAL-IDENTITY REBRAND note above).
 */
#define OVMX_VENDOR_TOKEN        "OVMX"

/* ---- SYSKRNL identity (GNU/Linux-style dual identity) ----------
 *
 * "OVMX/Linux" -- WITH THE SLASH, exactly like "GNU/Linux" -- names the
 * SYSKRNL layer (the host kernel underneath) OVMX_PRODUCT_NAME ("OpenVMX")
 * runs on top of. See the DUAL-IDENTITY REBRAND note above for the full
 * rationale and the two surfaces this is shown on (early boot, distro
 * metadata). Distinct from, and always printed BEFORE, the product banner.
 *
 * SUBSTRATE-TRUE (INV-6 / Rule 9). The SYSKRNL name must state the kernel this
 * image ACTUALLY runs on -- a boot that prints "OVMX/Linux" while running on
 * NetBSD is a false statement about the running system, exactly the LARP the
 * authenticity rules forbid. OVMX has a second SYSKRNL, OVMX/NetBSD, that
 * captures the VAX (docs/design-ovmx-netbsd-syskrnl.md, epic vms-8e8); when an
 * image is built for a NetBSD substrate this reports OVMX/NetBSD. Selected at
 * COMPILE time from the target's own predefined macro (__NetBSD__ vs the
 * Linux/glibc default), so a binary can only ever name the kernel it was built
 * to run on. */
#if defined(__NetBSD__)
#define OVMX_SYSKRNL_NAME     "OVMX/NetBSD"
#define OVMX_SYSKRNL_BANNER   OVMX_SYSKRNL_NAME " -- SYSKRNL (NetBSD kernel)"
#else
#define OVMX_SYSKRNL_NAME     "OVMX/Linux"
#define OVMX_SYSKRNL_BANNER   OVMX_SYSKRNL_NAME " -- SYSKRNL (Linux kernel)"
#endif

/*
 * ovmx_syskrnl_banner - The early-boot SYSKRNL identity line. Printed
 * BEFORE ovmx_product_banner() so a boot reads the SYSKRNL layer coming up
 * ("OVMX/Linux" or "OVMX/NetBSD"), THEN "OpenVMX" product -- see
 * src/ovmx_init/ovmx_init.c main(), which prints this literally first.
 */
static inline const char *ovmx_syskrnl_banner(void)
{
    return OVMX_SYSKRNL_BANNER;
}

/* ---- VMS-compat identity (machine surfaces, true-to-arch) -------- */

/*
 * Real VSI OpenVMS version for an arch with VMS lineage. x86-64 is the only
 * lineage arch OVMX targets today. PURITY: operator-ruled family (D1);
 * exact patch level pending oracle pin -- see the purity note above.
 */
#define OVMX_VMS_COMPAT_VERSION_X86_64  "V9.2-3"

/* Node-name fallback when SYSGEN SCSNODE is unconfigured. Matches the
 * cluster daemon's fallback (src/vmsscs/scsd.c resolve_node_identity) so
 * the wire and the display agree on who we are. */
#define OVMX_DEFAULT_NODENAME   "OVMX"

/* Longest string any accessor writes, incl. NUL. */
#define OVMX_IDENTITY_MAXLEN    64

/*
 * ovmx_hw_arch - The architecture OVMX was actually built for, as the
 * VMS-style uppercase token used by SYI$_ARCH_NAME-class surfaces.
 * Compile-time, not uname(): this is what the code IS, and the iron rule
 * says we report the metal truthfully.
 */
static inline const char *ovmx_hw_arch(void)
{
#if defined(__x86_64__)
    return "X86_64";
#elif defined(__aarch64__)
    return "AARCH64";
#else
    return "UNKNOWN";
#endif
}

/*
 * ovmx_arch_has_vms_lineage - Nonzero when the running arch is one OpenVMS
 * actually shipped on (VAX, Alpha, IA-64, x86-64). ARM is deliberately 0:
 * OpenVMS never ran on ARM and OVMX will not pretend otherwise.
 */
static inline int ovmx_arch_has_vms_lineage(void)
{
#if defined(__x86_64__)
    return 1;
#else
    return 0;
#endif
}

/*
 * ovmx_compat_version - The version token MACHINES read (F$GETSYI VERSION,
 * SYI$_VERSION). True-to-arch per the iron rule:
 *   - lineage arch (x86-64) -> the real VSI version for that arch
 *   - no lineage  (aarch64) -> OVMX's own version, honestly
 * Never a global constant, never a fabricated VMS arch.
 */
static inline const char *ovmx_compat_version(void)
{
    return ovmx_arch_has_vms_lineage()
           ? OVMX_VMS_COMPAT_VERSION_X86_64
           : OVMX_PRODUCT_VERSION;
}

/*
 * ovmx_product_version - The version HUMANS are shown (brand, ours).
 */
static inline const char *ovmx_product_version(void)
{
    return OVMX_PRODUCT_VERSION;
}

/*
 * ovmx_product_banner - The full badged human identity,
 * "OpenVMX V0.3 - OpenVMS-compatible" (INV-0).
 */
static inline const char *ovmx_product_banner(void)
{
    return OVMX_PRODUCT_BANNER;
}

/*
 * ovmx_node_name - The system's VMS node name for DISPLAY surfaces
 * (SHOW SYSTEM header, banners). Reads SYSGEN SCSNODE -- the configured
 * cluster identity -- falling back to OVMX_DEFAULT_NODENAME.
 *
 * Deliberately NOT the Linux hostname: a hostname reaching a VMS-facing
 * display is exactly the Linux leak INV-4 forbids. (F$GETSYI("NODENAME")
 * keeps its own established semantics -- see dcl_lexical.c.)
 *
 * Writes at most out_len-1 chars plus NUL. No-op if out is NULL or
 * out_len is 0.
 */
static inline void ovmx_node_name(char *out, size_t out_len)
{
    if (!out || out_len == 0)
        return;

    char configured[SYSGEN_STRVAL_LEN];
    const char *src = OVMX_DEFAULT_NODENAME;

    if (sysgen_read_string("SCSNODE", configured, sizeof(configured)) == 0
            && configured[0] != '\0') {
        src = configured;
    }

    strncpy(out, src, out_len - 1);
    out[out_len - 1] = '\0';
}

#endif /* OVMX_IDENTITY_H */
