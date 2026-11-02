/*
 * crtl_fd_test.c - the C RTL file layer over RMS (vms-b90), driven the way an
 * ordinary DEC C program -- GCC's cc1 among them -- drives it: the standard
 * headers, the default 32-bit pointer size, stdio and the POSIX descriptor
 * calls, on files of the Files-11 ODS-2 volume.
 *
 * Built by the crtl-fd gate (run-module-gp-activation-alpha.sh) against the
 * RMS-backed DECC$SHR (JOINT_CRTL_RMS_VENEER=1). The boot's SYSTARTUP first writes
 * VDA0:[SYSTMP]CFDIN.TXT with DCL OPEN/WRITE (a record file DCL owns), runs this
 * image, then an independent reader -- DCL TYPE over RMS -- shows the file this
 * image wrote. Every check prints; the first failure picks the exit value, and
 * success is the distinctive 7.
 *
 * struct stat is the DEC C RTL's default layout (vms-28d part 2): st_ino[3] is
 * the File ID and st_fab_rfm/rat/mrs the file's RMS record attributes -- what
 * GCC's libcpp (STAT_SIZE_RELIABLE) and incpath (INO_T_EQ) read under VMS.
 *
 * Run as a foreign command with "ret N" (vms-1c83), main returns N after an
 * atexit() registration and an unflushed line.
 * Run as a foreign command with "exit N" or "pexit N" (vms-599), it instead ends
 * through DEC C exit(N) or __posix_exit(N), after printing a line it does not
 * flush; the SYSTARTUP prints the $STATUS DCL got, which the gate compares with
 * what OpenVMS Alpha V8.4 gives (tests/lab/captures/decc-exit-alpha-20261021).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <unixlib.h>

#define DIRSPEC   "VDA0:[SYSTMP]"
#define OUTF  DIRSPEC "CFDOUT.TXT"
#define INF   DIRSPEC "CFDIN.TXT"
#define NLINE 120
#define FAB_C_VAR   2       /* libcpp files.cc spells these out the same way */
#define FAB_C_STMLF 5
#define STAT_SIZE_RELIABLE(ST) ((ST).st_fab_rfm != FAB_C_VAR)

void __posix_exit(int);     /* DEC C; <stdlib.h> declares it only under _POSIX_EXIT */
char *getname(int, char *, ...);    /* DEC C <unixio.h>: a descriptor's file or device */

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

static const char *ret_arg;
static void ret_atexit(void)
{
    printf("CFDX: ret %s atexit-ran", ret_arg);
}

int main(int argc, char **argv)
{
    char want[128], got[256];
    long total = 0;
    if (argc >= 3 && strcmp(argv[1], "ret") == 0) {
        /* vms-1c83: main RETURNS. Its atexit function and its unflushed line
         * appear only if $EXIT runs the C RTL's exit handler. */
        ret_arg = argv[2];
        atexit(ret_atexit);
        setvbuf(stdout, NULL, _IOFBF, BUFSIZ);
        printf("CFDX: ret %s flushed-by-exit", argv[2]);
        return (int)strtol(argv[2], NULL, 0);
    }
    if (argc >= 2 && strcmp(argv[1], "time") == 0) {
        /* vms-593: the C RTL's clock and zone. time() is the system time; the
         * zone is SYS$TIMEZONE_DIFFERENTIAL/_NAME, which SYSTARTUP redefines
         * between runs (no TZ variable). The struct tm stays the C RTL's own:
         * strftime formats every field this checks. jan = 2026-01-15 12:00 UTC. */
        time_t now = time(NULL), jan = 1768478400;
        char u[64], l[64], j[64];
        strftime(u, sizeof u, "%Y-%m-%d %H:%M:%S", gmtime(&now));
        strftime(l, sizeof l, "%Z %z", localtime(&now));
        strftime(j, sizeof j, "%Y-%m-%d %H:%M %Z %z", localtime(&jan));
        printf("CRTLTIME: utc=%s zone=%s jan=%s ctime=%s", u, l, j, ctime(&jan));
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "dev") == 0) {
        /* vms-a66e: the C RTL's terminal and pipe descriptors are VMS channels,
         * and no C RTL name reaches the substrate's file system. stdout is
         * SYS$OUTPUT, a channel to the terminal (getname names it); pipe() is a
         * mailbox pair that SHOW DEVICE, run by system() while it is open, finds
         * in the executive's device table; each write is one message and close
         * of the write end is end of file; /proc is an RMS name RMS does not
         * have. (openat() is no DEC C RTL entry; test_syssvc_crtl_devfd
         * covers a directory descriptor.) */
        char so[256], mb[256], cmd[300], b[64];
        setvbuf(stdout, NULL, _IONBF, 0);   /* in order with system()'s output */
        int p[2], tty = isatty(1);
        if (!getname(1, so, 1))
            strcpy(so, "<none>");
        int pr = pipe(p);
        if (pr != 0 || !getname(p[0], mb, 1))
            strcpy(mb, "<none>");
        int w1 = pr ? -1 : (int)write(p[1], "abc", 3);
        int w2 = pr ? -1 : (int)write(p[1], "defg", 4);
        int r1 = pr ? -1 : (int)read(p[0], b, sizeof b);
        int r2 = pr ? -1 : (int)read(p[0], b + 8, sizeof b - 8);
        int msgs = w1 == 3 && w2 == 4 && r1 == 3 && memcmp(b, "abc", 3) == 0 &&
                   r2 == 4 && memcmp(b + 8, "defg", 4) == 0;
        printf("CFDDEV: stdout=%s isatty=%d\n", so, tty);
        printf("CFDDEV: pipe=%s messages=%s\n", mb, msgs ? "abc|defg" : "WRONG");
        printf("CFDDEV: === SHOW DEVICE %s ===\n", mb);
        snprintf(cmd, sizeof cmd, "SHOW DEVICE %s", mb);
        int sst = system(cmd);
        printf("CFDDEV: === END SHOW DEVICE (%d) ===\n", sst);
        int eof = pr ? -1 : (close(p[1]) == 0 ? (int)read(p[0], b, sizeof b) : -1);
        if (!pr)
            close(p[0]);
        errno = 0;
        int pf = open("/proc/self/status", O_RDONLY);
        int pe = errno;
        printf("CFDDEV: eof=%s proc=%s\n", eof == 0 ? "yes" : "no",
               pf < 0 && pe == ENOENT ? "ENOENT" : "OPENED");
        return 0;
    }
    if (argc >= 3 && (strcmp(argv[1], "exit") == 0 || strcmp(argv[1], "pexit") == 0)) {
        int n = (int)strtol(argv[2], NULL, 0);
        /* fully buffered and no newline: only exit()'s stdio flush prints it */
        setvbuf(stdout, NULL, _IOFBF, BUFSIZ);
        printf("CFDX: %s %s flushed-by-exit", argv[1], argv[2]);
        if (argv[1][0] == 'p')
            __posix_exit(n);
        exit(n);
    }
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
    printf("CFD: stat -> %d size=%ld (want %ld) mode=0%o fid=(%u,%u,%u) rfm=%d rat=%d\n", sr,
           (long)st.st_size, total, (unsigned)st.st_mode,
           (unsigned)st.st_ino[0], (unsigned)st.st_ino[1], (unsigned)st.st_ino[2],
           st.st_fab_rfm, st.st_fab_rat);
    unsigned long out_fid = sr == 0 ? (unsigned long)st.st_ino[0] : 0;
    check(sr == 0 && st.st_size == total && S_ISREG(st.st_mode) && st.st_ino[0] != 0 &&
              st.st_ino[1] != 0,
          12, "stat: size == bytes written, S_IFREG, st_ino[3] is the File ID");
    check(st.st_fab_rfm == FAB_C_STMLF && STAT_SIZE_RELIABLE(st), 27,
          "stat: st_fab_rfm says Stream_LF, so the size is reliable");

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
        check(fstat(fd, &fst) == 0 && fst.st_size == total &&
                  memcmp(fst.st_ino, st.st_ino, sizeof st.st_ino) == 0 &&
                  fst.st_fab_rfm == st.st_fab_rfm,
              17, "fstat on the descriptor agrees with stat (File ID, record format)");

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
    int wfd = open(DIRSPEC "CFDDUP.TXT", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int dok = save >= 0 && wfd >= 0 && dup2(wfd, 1) == 1;
    if (dok) {
        write(1, "WRITTEN THROUGH FD 1\n", 21);
        dup2(save, 1);
        close(wfd);
        close(save);
    }
    check(dok && stat(DIRSPEC "CFDDUP.TXT", &st) == 0 && st.st_size == 21, 20,
          "dup2(rms_fd, 1): write(1) goes to the RMS file; stdout restored");

    /* 8. A record file DCL wrote (OPEN/WRITE) reads as lines. */
    struct stat ist;
    int isr = stat(INF, &ist);
    printf("CFD: DCL file stat -> %d size=%ld rfm=%d rat=%d\n", isr,
           isr == 0 ? (long)ist.st_size : -1L, ist.st_fab_rfm, ist.st_fab_rat);
    check(isr == 0 && ist.st_fab_rfm == FAB_C_VAR && !STAT_SIZE_RELIABLE(ist), 28,
          "stat of the DCL record file: st_fab_rfm says VAR, so its size is not the read size");
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

    /* 8a. The same record file the way libiberty's expandargv reads a GCC
     *     response file (@file, rd vms-465): fseek to the end, ftell, back to
     *     the start, fread that many bytes. The position at the end of a record
     *     file is not the byte count fread delivers (DEC C: an end-of-file
     *     position, the stored bytes), but it is no smaller, so the short read
     *     returns the whole text, without an error. */
    f = fopen(INF, "r");
    char whole[256];
    long endpos = -1;
    size_t rd = 0;
    int sk_end = -1, sk_set = -1, rerr = 1;
    memset(whole, 0, sizeof whole);
    if (f) {
        sk_end = fseek(f, 0L, SEEK_END);
        endpos = ftell(f);
        sk_set = fseek(f, 0L, SEEK_SET);
        if (endpos > 0 && endpos < (long)sizeof whole)
            rd = fread(whole, 1, (size_t)endpos, f);
        rerr = ferror(f);
        fclose(f);
    }
    printf("CFD: DCL file as @file: fseek(END)=%d ftell=%ld fseek(SET)=%d fread=%lu ferror=%d\n",
           sk_end, endpos, sk_set, (unsigned long)rd, rerr);
    check(f != NULL && sk_end == 0 && sk_set == 0 && endpos >= 29 && rd == 29 && !rerr &&
              memcmp(whole, "WRITTEN BY DCL\nSECOND RECORD\n", 29) == 0,
          48, "a DCL record file: fseek(SEEK_END) + ftell + rewind + fread reads it whole");

    /* 8a2. The machine's memory, as libiberty's physmem_total() asks it (GCC's
     *      garbage-collector heuristics; rd vms-465): the boot gives the guest
     *      1 GB, so more than half of that is physical memory. */
    long phys = sysconf(_SC_PHYS_PAGES), psz = sysconf(_SC_PAGESIZE);
    double mem = (double)phys * (double)psz;
    printf("CFD: sysconf(_SC_PHYS_PAGES)=%ld _SC_PAGESIZE=%ld -> %.0f MB\n", phys, psz, mem / 1048576.0);
    check(phys > 0 && psz == 8192 && mem >= 512.0 * 1048576.0 && mem <= 64.0 * 1073741824.0, 49,
          "sysconf(_SC_PHYS_PAGES) * _SC_PAGESIZE is the machine's memory");

    /* 8a3. Many small sequential writes -- an assembler file as cc1 writes it,
     *      through stdio's buffer -- land whole (the layer buffers them into
     *      multiblock $WRITEs, rd vms-465). */
    {
        int wfd = open(DIRSPEC "CFDWB.TXT", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        char chunk[37];
        unsigned long total = 0, bad = 0;
        int i;
        for (i = 0; wfd >= 0 && i < 4000; i++) {
            int k;
            for (k = 0; k < 36; k++)
                chunk[k] = (char)('a' + (i + k) % 26);
            chunk[36] = '\n';
            if (write(wfd, chunk, sizeof chunk) != (long)sizeof chunk)
                break;
            total += sizeof chunk;
        }
        if (wfd >= 0)
            close(wfd);
        FILE *rf2 = fopen(DIRSPEC "CFDWB.TXT", "r");
        char rl[64];
        long lines = 0;
        while (rf2 && fgets(rl, sizeof rl, rf2)) {
            int k;
            for (k = 0; k < 36; k++)
                if (rl[k] != (char)('a' + (lines + k) % 26))
                    bad++;
            lines++;
        }
        if (rf2)
            fclose(rf2);
        struct stat ws;
        int wst = stat(DIRSPEC "CFDWB.TXT", &ws);
        printf("CFD: 4000 writes of 37 bytes -> %lu bytes, read back %ld lines, %lu wrong, size %ld\n",
               total, lines, bad, wst == 0 ? (long)ws.st_size : -1L);
        check(total == 4000ul * 37 && lines == 4000 && bad == 0 && wst == 0 && ws.st_size == (long)total, 50,
              "4000 small sequential writes read back whole, byte for byte");
    }

    /* 8b. fgetname (vms-4ba3): the stream's full file specification, in VMS
     *     form and, with format 0, UNIX form -- what GCC's vmsdbgout.cc asks. */
    f = fopen(OUTF, "r");
    char spec[256] = "", uspec[256] = "";
    char *gs = f ? fgetname(f, spec, 1) : NULL;
    char *gu = f ? fgetname(f, uspec, 0) : NULL;
    if (f)
        fclose(f);
    printf("CFD: fgetname -> \"%s\" / \"%s\"\n", gs ? spec : "(null)", gu ? uspec : "(null)");
    check(gs == spec && strstr(spec, "[SYSTMP]CFDOUT.TXT;") != NULL && strchr(spec, ':') != NULL,
          29, "fgetname(f, buf, 1) returns the full VMS file specification");
    /* The UNIX form is the C RTL's default translation: lowercased, the
     * version kept as ".N" -- the real Alpha V8.4 C RTL turns
     * [TEST]MixedCase.Txt;2 into /sys$disk/test/mixedcase.txt.2
     * (tests/lab/captures/ods5-alpha-20261022/CONSOLE-5.txt, RUN-DEFAULT). */
    check(gu == uspec && strstr(uspec, "/systmp/cfdout.txt.") != NULL && uspec[0] == '/' &&
              uspec[strlen(uspec) - 1] >= '0' && uspec[strlen(uspec) - 1] <= '9', 30,
          "fgetname(f, buf, 0) returns its UNIX form");
    check(fgetname(stdout, spec, 1) == NULL, 31, "fgetname of a non-RMS stream is a null pointer");

    /* 8c. A floating argument through a varargs C RTL routine (the
     *     OTS$HOME_ARGS home area carries F registers too). */
    char fbuf[64];
    snprintf(fbuf, sizeof fbuf, "%.2f|%d|%g", 1.5, 7, 2.25);
    printf("CFD: snprintf floats -> \"%s\"\n", fbuf);
    check(strcmp(fbuf, "1.50|7|2.25") == 0, 32, "snprintf formats double arguments passed through varargs");

    /* 9. UNIX syntax names the same file. */
    f = fopen("/vda0/systmp/cfdout.txt", "r");
    check(f != NULL && fgets(got, sizeof got, f) && strncmp(got, "line 001", 8) == 0,
          22, "fopen(\"/vda0/systmp/cfdout.txt\") is VDA0:[SYSTMP]CFDOUT.TXT");
    if (f)
        fclose(f);

    /* 10. Missing file: NULL and ENOENT, never a silent success. */
    errno = 0;
    f = fopen(DIRSPEC "NO_SUCH_FILE.TXT", "r");
    check(f == NULL && errno == ENOENT, 23, "a missing file fails with ENOENT");

    /* 11. rename + unlink are RMS $RENAME / $ERASE. $RENAME re-links the
     *     directory entry (ACP MODIFY!M_MOVE), so the file keeps its File ID --
     *     not an erase plus a create of a new file. */
    struct stat rb, ra;
    int rbs = stat(DIRSPEC "CFDDUP.TXT", &rb);
    check(rename(DIRSPEC "CFDDUP.TXT", DIRSPEC "CFDREN.TXT") == 0 &&
              access(DIRSPEC "CFDREN.TXT", F_OK) == 0 &&
              access(DIRSPEC "CFDDUP.TXT", F_OK) != 0,
          24, "rename moves the directory entry");
    int ras = stat(DIRSPEC "CFDREN.TXT", &ra);
    printf("CFD: rename fid (%u,%u,%u) -> (%u,%u,%u)\n",
           (unsigned)rb.st_ino[0], (unsigned)rb.st_ino[1], (unsigned)rb.st_ino[2],
           (unsigned)ra.st_ino[0], (unsigned)ra.st_ino[1], (unsigned)ra.st_ino[2]);
    check(rbs == 0 && ras == 0 && rb.st_ino[0] != 0 && rb.st_ino[0] == ra.st_ino[0] &&
              rb.st_ino[1] == ra.st_ino[1] && rb.st_ino[2] == ra.st_ino[2],
          42, "the renamed file keeps its File ID");
    check(unlink(DIRSPEC "CFDREN.TXT") == 0 && access(DIRSPEC "CFDREN.TXT", F_OK) != 0, 25,
          "unlink erases it");

    /* 12. The kernel's namespaces stay the kernel's. */
    int nfd = open("/dev/null", O_WRONLY);
    check(nfd >= 0 && write(nfd, "x", 1) == 1, 26, "/dev/null is still the kernel device");
    if (nfd >= 0)
        close(nfd);

    /* 13. A directory reads as its entries (a $PARSE/$SEARCH wildcard
     *     context): readdir lists CFDOUT.TXT;n with the File ID stat gave it,
     *     not the erased name; rewinddir starts over; a missing directory
     *     fails honestly; the UNIX name of the directory is the same one. */
    struct stat ds;
    check(stat(DIRSPEC, &ds) == 0 && S_ISDIR(ds.st_mode), 33,
          "stat(\"" DIRSPEC "\") reports a directory");
    DIR *d = opendir(DIRSPEC);
    check(d != NULL, 34, "opendir(\"" DIRSPEC "\") opens the directory over RMS");
    if (d) {
        struct dirent *e;
        char first[256] = "";
        int n = 0, saw_out = 0, saw_ren = 0;
        unsigned long out_ino = 0;
        while ((e = readdir(d)) != NULL) {
            if (n++ == 0)
                snprintf(first, sizeof first, "%s", e->d_name);
            if (strncmp(e->d_name, "CFDOUT.TXT;", 11) == 0) {
                saw_out = 1;
                out_ino = (unsigned long)e->d_ino;
            }
            if (strncmp(e->d_name, "CFDREN.TXT;", 11) == 0)
                saw_ren = 1;
        }
        printf("CFD: readdir -> %d entries, first \"%s\", CFDOUT.TXT d_ino=%lu (stat %lu)\n",
               n, first, out_ino, out_fid);
        check(saw_out && out_ino == out_fid, 35,
              "readdir lists CFDOUT.TXT;n with its File ID as d_ino");
        check(!saw_ren, 36, "readdir does not list the erased CFDREN.TXT");
        rewinddir(d);
        e = readdir(d);
        check(e != NULL && strcmp(e->d_name, first) == 0, 37, "rewinddir starts the search over");
        long pos = telldir(d);
        char second[256] = "";
        e = readdir(d);
        if (e)
            snprintf(second, sizeof second, "%s", e->d_name);
        seekdir(d, pos);
        e = readdir(d);
        printf("CFD: telldir -> %ld, entry \"%s\", after seekdir \"%s\"\n", pos, second,
               e ? e->d_name : "(none)");
        check(n >= 2 && pos == 1 && second[0] && e != NULL && strcmp(e->d_name, second) == 0, 40,
              "telldir/seekdir return to the same entry");
        closedir(d);
    }
    errno = 0;
    d = opendir("VDA0:[NO_SUCH_DIR]");
    check(d == NULL && errno == ENOENT, 38, "opendir of a missing directory fails with ENOENT");
    if (d)
        closedir(d);
    d = opendir("/vda0/systmp");
    check(d != NULL && readdir(d) != NULL, 39, "opendir(\"/vda0/systmp\") is the same directory");
    if (d)
        closedir(d);

    /* 13b. A write far past one block-I/O transfer (126 blocks) lands whole:
     *      200000 bytes in uneven pieces, then size and content read back. */
    {
        static unsigned char big[200000], back[4096];
        for (int i = 0; i < (int)sizeof big; i++)
            big[i] = (unsigned char)(i * 7 + (i >> 9));
        int bfd = open(DIRSPEC "CFDBIG.DAT", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        long put = 0;
        if (bfd >= 0) {
            static const int piece[] = { 1, 511, 70000, 3, 65536, 64000 };
            for (int k = 0; put < (long)sizeof big; k = (k + 1) % 6) {
                long n = piece[k];
                if (put + n > (long)sizeof big)
                    n = (long)sizeof big - put;
                long w = write(bfd, big + put, (size_t)n);
                if (w != n)
                    break;
                put += w;
            }
            close(bfd);
        }
        struct stat bs;
        int bsr = stat(DIRSPEC "CFDBIG.DAT", &bs);
        long same = 0;
        int rfd = open(DIRSPEC "CFDBIG.DAT", O_RDONLY);
        if (rfd >= 0) {
            long got;
            while ((got = read(rfd, back, sizeof back)) > 0) {
                if (same + got > (long)sizeof big || memcmp(back, big + same, (size_t)got) != 0)
                    break;
                same += got;
            }
            close(rfd);
        }
        printf("CFD: big write put=%ld size=%ld readback=%ld\n", put,
               bsr == 0 ? (long)bs.st_size : -1L, same);
        check(put == (long)sizeof big && bsr == 0 && bs.st_size == (long)sizeof big &&
                  same == (long)sizeof big,
              43, "a 200000-byte write lands whole and reads back byte-exact");
        unlink(DIRSPEC "CFDBIG.DAT");
    }

    /* 14. DECC$FILE_SHARING changes how the C RTL opens a file: off (the
     *     default), a second open for write of a file already open for write
     *     meets the Files-11 file-access lock and is refused; on, the file is
     *     opened shared and the second open succeeds. */
    {
        const char *shf = DIRSPEC "CFDSHR.TXT";
        FILE *sf = fopen(shf, "w");
        if (sf)
            fclose(sf);
        int fi = decc$feature_get_index("DECC$FILE_SHARING");
        int a = open(shf, O_WRONLY), b = open(shf, O_WRONLY);
        int off_ok = sf != NULL && fi > 0 && decc$feature_get_value(fi, 1) == 0 && a >= 0 && b < 0;
        if (a >= 0) close(a);
        if (b >= 0) close(b);
        int set = fi > 0 ? decc$feature_set_value(fi, 1, 1) : -1;
        a = open(shf, O_WRONLY);
        b = open(shf, O_WRONLY);
        int on_ok = set == 0 && a >= 0 && b >= 0;
        if (a >= 0) close(a);
        if (b >= 0) close(b);
        if (fi > 0)
            decc$feature_set_value(fi, 1, 0);
        unlink(shf);
        printf("CFD: DECC$FILE_SHARING index=%d off_ok=%d on_ok=%d\n", fi, off_ok, on_ok);
        check(off_ok && on_ok, 41,
              "DECC$FILE_SHARING: refused second open for write when off, shared when on");
    }

    /* vms-45f: DEC C fopen/creat/open take RMS file-attribute keywords after
     * their standard arguments; they describe the file $CREATE makes. The DCL
     * reader prints F$FILE_ATTRIBUTES of the same files afterwards. */
    {
        struct stat us, fs, cs;
        static char blk[600];
        memset(blk, 'u', sizeof blk);
        FILE *uf = fopen(DIRSPEC "CFDUDF.DAT", "wb", "rfm=udf", "rat=none");
        size_t uw = uf ? fwrite(blk, 1, sizeof blk, uf) : 0;
        int uc = uf ? fclose(uf) : -1;
        int usr = stat(DIRSPEC "CFDUDF.DAT", &us);
        printf("CFD: udf w=%u close=%d stat=%d rfm=%d size=%ld\n", (unsigned)uw, uc, usr,
               usr == 0 ? (int)us.st_fab_rfm : -1, usr == 0 ? (long)us.st_size : -1L);
        check(uf != NULL && uw == sizeof blk && uc == 0 && usr == 0 && us.st_fab_rfm == 0 &&
              us.st_size == (long)sizeof blk, 42,
              "fopen(\"wb\", \"rfm=udf\", \"rat=none\") creates an undefined-record-format file");

        int cfd = creat(DIRSPEC "CFDFIX.DAT", 0666, "rfm=fix", "mrs=20");
        int cw = cfd >= 0 ? (int)write(cfd, blk, 40) : -1;
        int cc = cfd >= 0 ? close(cfd) : -1;
        int fsr = stat(DIRSPEC "CFDFIX.DAT", &fs);
        check(cfd >= 0 && cw == 40 && cc == 0 && fsr == 0 && fs.st_fab_rfm == 1 && fs.st_fab_mrs == 20, 43,
              "creat(..., \"rfm=fix\", \"mrs=20\") creates a fixed 20-byte-record file");

        int ofd = open(DIRSPEC "CFDSLF.DAT", O_WRONLY | O_CREAT | O_TRUNC, 0666, "rfm=stmlf", "rat=none");
        int ow = ofd >= 0 ? (int)write(ofd, "lf\n", 3) : -1;
        int oc = ofd >= 0 ? close(ofd) : -1;
        int csr = stat(DIRSPEC "CFDSLF.DAT", &cs);
        check(ofd >= 0 && ow == 3 && oc == 0 && csr == 0 && cs.st_fab_rfm == 5 && cs.st_fab_rat == 0, 44,
              "open(..., mode, \"rfm=stmlf\", \"rat=none\") creates a Stream_LF file with no carriage control");

        struct stat ns;
        errno = 0;
        FILE *bf = fopen(DIRSPEC "CFDBAD.DAT", "w", "rfm=bogus");
        int be = errno;
        check(bf == NULL && be == EINVAL && stat(DIRSPEC "CFDBAD.DAT", &ns) != 0, 45,
              "an unknown keyword value is EINVAL and creates nothing");
        errno = 0;
        FILE *af = fopen(DIRSPEC "CFDALQ.DAT", "w", "alq=10");
        int ae = errno;
        check(af == NULL && ae == EOPNOTSUPP && stat(DIRSPEC "CFDALQ.DAT", &ns) != 0, 46,
              "a keyword whose effect is not provided (alq) is refused, never ignored");

        FILE *rf = fopen(DIRSPEC "CFDUDF.DAT", "rb", "rfm=var");
        char rb[8] = "";
        size_t rr = rf ? fread(rb, 1, 4, rf) : 0;
        if (rf)
            fclose(rf);
        struct stat us2;
        check(rf != NULL && rr == 4 && memcmp(rb, "uuuu", 4) == 0 &&
              stat(DIRSPEC "CFDUDF.DAT", &us2) == 0 && us2.st_fab_rfm == 0, 47,
              "opening an existing file keeps its own record format (keywords describe a creation)");
        if (bf) fclose(bf);
        if (af) fclose(af);
    }

    if (fails) {
        printf("OVMX CRTL-FD test: %d check(s) FAILED (first %d)\n", fails, first_fail);
        return first_fail;
    }
    printf("OVMX CRTL-FD test: OK (stdio + descriptors over RMS on ODS-2) argc=%d\n", argc);
    return 7;
}
