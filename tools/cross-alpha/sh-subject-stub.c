/*
 * sh-subject-stub.c -- a minimal static /bin/sh stand-in for the Alpha
 * qemu-system-alpha syssvc RUN harness (rd vms-341). The process-listing/
 * -control suites (test_syssvc_showproc / procnam / startup_service / delprc)
 * $CREPRC a named subject that execs SUBJECT_IMAGE = "/bin/sh" with stdin
 * redirected to a script that sleeps, so the running image is a live /bin/sh
 * the executive can list (SHOW SYSTEM/PROCESS), name-lookup, and $DELPRC.
 *
 * On x86_64/aarch64 that /bin/sh is busybox (the test harness init installs it).
 * Alpha has no busybox in the cross env, so this is the equivalent live-process
 * stand-in: drain stdin (the sleep script) exactly as a shell reading its input
 * would, then block forever so the subject stays resident for the executive to
 * observe and delete. It is a HARNESS SUBJECT, not a faked result -- every
 * assertion the suites make is a real executive facility (process table read,
 * name lookup, $DELPRC) exercised against this genuine second process.
 */
#include <unistd.h>

int main(void)
{
    char buf[512];
    /* Consume the CREPRC-redirected input script the way a shell would, then
     * block indefinitely so the process stays resident (the suite deletes it). */
    while (read(0, buf, sizeof buf) > 0) { /* drain */ }
    for (;;) pause();
    return 0;
}
