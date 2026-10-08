/*
 * crtl_fd_test.c - the C RTL file layer over RMS (vms-b90), driven the way an
 * ordinary DEC C program -- GCC's cc1 among them -- drives it: the standard
 * headers, the default 32-bit pointer size, stdio and the POSIX descriptor
 * calls, on files of the Files-11 ODS-2 volume.
 *
 * Built by the crtl-fd gate (run-module-gp-activation-alpha.sh) against the
 * RMS-backed DECC$SHR (JOINT_CRTL_RMS_FD=1). The boot's SYSTARTUP first writes
 * VDA0:[SYSTMP]CFDIN.TXT with DCL OPEN/WRITE (a record file DCL owns), runs this
 * image, then an independent reader -- DCL TYPE over RMS -- shows the file this
 * image wrote. Every check prints; the first failure picks the exit value, and
 * success is the distinctive 7.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DIR   "VDA0:[SYSTMP]"
#define OUTF  DIR "CFDOUT.TXT"
#define INF   DIR "CFDIN.TXT"
#define NLINE 120

static int fails;
static int first_fail;

static void check(int ok, int code, const char *what)
{
    printf("CFD: %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        fails++;
        if (!first_fail)
            first_fail = code;
    }
}

static void line_text(int i, char *buf, size_t n)
{
    snprintf(buf, n, "line %03d of the OVMX C RTL file layer over RMS\n", i);
}

int main(int argc, char **argv)
{
    char want[128], got[256];
    long total = 0;
    (void)argv;
    setvbuf(stdout, NULL, _IONBF, 0);   /* every line reaches the console */

    /* 1. stdio writes a multi-block file of unaligned lines. */
    FILE *f = fopen(OUTF, "w");
    check(f != NULL, 10, "fopen(\"" OUTF "\", \"w\") creates the file over RMS");
    if (!f) {
        printf("CFD: errno=%d\n", errno);
        return 10;
    }
    for (int i = 1; i <= NLINE; i++) {
        line_text(i, want, sizeof want);
        fputs(want, f);
        total += (long)strlen(want);
    }
    check(fclose(f) == 0, 11, "fclose flushes and $CLOSEs");

    /* 2. stat reports the byte-exact size and a regular file. */
    struct stat st;
    int sr = stat(OUTF, &st);
    printf("CFD: stat -> %d size=%ld (want %ld) mode=0%o ino=%llu\n", sr,
           (long)st.st_size, total, (unsigned)st.st_mode,
           (unsigned long long)st.st_ino);
    check(sr == 0 && st.st_size == total && S_ISREG(st.st_mode) && st.st_ino != 0,
          12, "stat: size == bytes written, S_IFREG, File ID as st_ino");

    /* 3. open/read in odd-sized pieces reproduces every byte. */
    int fd = open(OUTF, O_RDONLY);
    check(fd >= 0, 13, "open(O_RDONLY)");
    if (fd >= 0) {
        static char all[NLINE * 64];
        long have = 0;
        ssize_t r;
        while ((r = read(fd, all + have, 7)) > 0)
            have += r;
        int same = have == total;
        long off = 0;
        for (int i = 1; same && i <= NLINE; i++) {
            line_text(i, want, sizeof want);
            size_t l = strlen(want);
            same = memcmp(all + off, want, l) == 0;
            off += (long)l;
        }
        check(same, 14, "read() in 7-byte pieces returns the file byte-exact, then 0 at EOF");

        off_t p = lseek(fd, 1000, SEEK_SET);
        r = read(fd, got, 40);
        check(p == 1000 && r == 40 && memcmp(got, all + 1000, 40) == 0, 15,
              "lseek(1000) + read(40) lands mid-block on the right bytes");
        check(lseek(fd, 0, SEEK_END) == total, 16, "lseek(0, SEEK_END) == size");
        struct stat fst;
        check(fstat(fd, &fst) == 0 && fst.st_size == total && fst.st_ino == st.st_ino,
              17, "fstat on the descriptor agrees with stat");

        /* 4. A descriptor dup()ed onto stdout writes into the same file. */
        close(fd);
    }

    /* 5. fgets reads it back line by line. */
    f = fopen(OUTF, "r");
    int lines = 0, good = 1;
    if (f) {
        while (fgets(got, sizeof got, f)) {
            lines++;
            line_text(lines, want, sizeof want);
            if (strcmp(got, want) != 0)
                good = 0;
        }
        fclose(f);
    }
    check(f != NULL && lines == NLINE && good, 18, "fgets returns every line");

    /* 6. Append mode extends it (O_APPEND: every write at the end of file). */
    f = fopen(OUTF, "a");
    if (f) {
        fputs("APPENDED BY THE C RTL\n", f);
        fclose(f);
    }
    check(f != NULL && stat(OUTF, &st) == 0 && st.st_size == total + 22, 19,
          "fopen(\"a\") appends at the end of file");

    /* 7. A descriptor dup2()ed onto stdout: write(1) lands in an RMS file. */
    fflush(stdout);
    int save = dup(1);
    int wfd = open(DIR "CFDDUP.TXT", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int dok = save >= 0 && wfd >= 0 && dup2(wfd, 1) == 1;
    if (dok) {
        write(1, "WRITTEN THROUGH FD 1\n", 21);
        dup2(save, 1);
        close(wfd);
        close(save);
    }
    check(dok && stat(DIR "CFDDUP.TXT", &st) == 0 && st.st_size == 21, 20,
          "dup2(rms_fd, 1): write(1) goes to the RMS file; stdout restored");

    /* 8. A record file DCL wrote (OPEN/WRITE) reads as lines. */
    f = fopen(INF, "r");
    char l1[80] = "", l2[80] = "";
    if (f) {
        if (!fgets(l1, sizeof l1, f)) l1[0] = 0;
        if (!fgets(l2, sizeof l2, f)) l2[0] = 0;
        fclose(f);
    }
    printf("CFD: DCL file: \"%.*s\" \"%.*s\"\n", (int)strcspn(l1, "\n"), l1,
           (int)strcspn(l2, "\n"), l2);
    check(f != NULL && strcmp(l1, "WRITTEN BY DCL\n") == 0 &&
              strcmp(l2, "SECOND RECORD\n") == 0,
          21, "a DCL OPEN/WRITE record file reads back as newline-terminated lines");

    /* 9. UNIX syntax names the same file. */
    f = fopen("/vda0/systmp/cfdout.txt", "r");
    check(f != NULL && fgets(got, sizeof got, f) && strncmp(got, "line 001", 8) == 0,
          22, "fopen(\"/vda0/systmp/cfdout.txt\") is VDA0:[SYSTMP]CFDOUT.TXT");
    if (f)
        fclose(f);

    /* 10. Missing file: NULL and ENOENT, never a silent success. */
    errno = 0;
    f = fopen(DIR "NO_SUCH_FILE.TXT", "r");
    check(f == NULL && errno == ENOENT, 23, "a missing file fails with ENOENT");

    /* 11. rename + unlink are RMS $RENAME / $ERASE. */
    check(rename(DIR "CFDDUP.TXT", DIR "CFDREN.TXT") == 0 &&
              access(DIR "CFDREN.TXT", F_OK) == 0 &&
              access(DIR "CFDDUP.TXT", F_OK) != 0,
          24, "rename moves the directory entry");
    check(unlink(DIR "CFDREN.TXT") == 0 && access(DIR "CFDREN.TXT", F_OK) != 0, 25,
          "unlink erases it");

    /* 12. The kernel's namespaces stay the kernel's. */
    int nfd = open("/dev/null", O_WRONLY);
    check(nfd >= 0 && write(nfd, "x", 1) == 1, 26, "/dev/null is still the kernel device");
    if (nfd >= 0)
        close(nfd);

    if (fails) {
        printf("OVMX CRTL-FD test: %d check(s) FAILED (first %d)\n", fails, first_fail);
        return first_fail;
    }
    printf("OVMX CRTL-FD test: OK (stdio + descriptors over RMS on ODS-2) argc=%d\n", argc);
    return 7;
}
