/*
 * imgact_boundary_audit.h -- freestanding install hook for the executive-
 * boundary AUDIT tracer at the real IMGACT activation site (vms-617, Phase A).
 *
 * IMGACT.EXE is -nostdlib/-ffreestanding, so the hosted pthread supervisor
 * (src/boundary_audit/boundary_audit.c) cannot be linked here. This is the
 * FREESTANDING port: a raw-clone() supervisor (no pthread_create, no libc) that
 * installs the SAME seccomp-BPF classifier + emits the SAME JSON finding format
 * (both shared verbatim via boundary_audit_filter.{h,c}) as the hosted module.
 *
 * Observe-only (SECCOMP_USER_NOTIF_FLAG_CONTINUE): behaviour is byte-identical
 * with the tracer on or off. Gated by the caller behind OVMX_BOUNDARY_AUDIT=1 --
 * never a default runtime change.
 */
#ifndef OVMX_IMGACT_BOUNDARY_AUDIT_H
#define OVMX_IMGACT_BOUNDARY_AUDIT_H

/*
 * Install the tracer on the CALLING thread, immediately before IMGACT transfers
 * control to the activated image. Spawns a freestanding supervisor (raw clone,
 * CLONE_FILES) that services seccomp notifications and flushes findings to
 * `log_path` when the image exits. `image` labels every finding; `log_path` is
 * the JSON-line sink (NULL/"" => no file sink).
 *
 * Returns 0 if the filter was armed (the image thread is now audited), or -1 if
 * the tracer could not be installed (unsupported arch, clone/pipe/seccomp
 * refused by the kernel). On -1 the image runs UNFILTERED -- i.e. exactly its
 * normal, un-audited behaviour: fail honest, never a fake (INV-6). The filter
 * is one-way and inherited by the image code that runs after transfer.
 */
int imgact_boundary_audit_install(const char *image, const char *log_path);

#endif /* OVMX_IMGACT_BOUNDARY_AUDIT_H */
