/*
 * vms_analyze.c - ANALYZE.EXE: VMS ANALYZE utility
 *
 * Implements the following ANALYZE modes:
 *   /DISK_STRUCTURE  - Analyze VMSFS volume structure
 *   /SYSTEM          - System Dump Analyzer (SDA) interactive mode
 *   /IMAGE           - Analyze ELF executable image
 *   /OBJECT          - Object file analysis (stub)
 *
 * Usage: ANALYZE /qualifier [filename]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <elf.h>
#include <dirent.h>

#include "vmsfs_ondisk.h"

/* ================================================================
 * Utility helpers
 * ================================================================ */

/*
 * Format a Unix timestamp as a VMS-style date string.
 * Output: DD-MON-YYYY HH:MM:SS.00
 */
static void format_vms_date(uint64_t epoch, char *buf, size_t bufsz)
{
    static const char *months[] = {
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
    };
    time_t t = (time_t)epoch;
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(buf, bufsz, "%02d-%s-%04d %02d:%02d:%02d.00",
             tm.tm_mday, months[tm.tm_mon], tm.tm_year + 1900,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/*
 * Format current time as VMS date string.
 */
static void format_vms_now(char *buf, size_t bufsz)
{
    format_vms_date((uint64_t)time(NULL), buf, bufsz);
}

/*
 * Case-insensitive string comparison for qualifiers.
 */
static int strcasematch(const char *a, const char *b)
{
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

/* ================================================================
 * ANALYZE/DISK_STRUCTURE
 * ================================================================ */

static int analyze_disk_structure(const char *filename)
{
    if (!filename || filename[0] == '\0') {
        fprintf(stderr, "%%ANALYZE-E-NEEDFILE, device or file specification required\n");
        return 1;
    }

    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "%%ANALYZE-E-OPENIN, error opening %s\n", filename);
        return 1;
    }

    printf("%%ANALYZE-I-DISKSTR, analyzing disk structure\n\n");

    /* Try to read the VMSFS home block at LBN 1 */
    struct vmsfs_home_block hb;
    off_t home_off = (off_t)VMSFS_HOME_LBN * VMSFS_BLOCK_SIZE;

    ssize_t nr = pread(fd, &hb, sizeof(hb), home_off);
    if (nr == (ssize_t)sizeof(hb) && hb.hb_magic == VMSFS_HOME_MAGIC) {
        /* Valid VMSFS volume — report real structure */
        char volname[13];
        memcpy(volname, hb.hb_volname, 12);
        volname[12] = '\0';
        /* Trim trailing spaces */
        for (int i = 11; i >= 0 && volname[i] == ' '; i--)
            volname[i] = '\0';

        char datebuf[32];
        format_vms_date(hb.hb_created, datebuf, sizeof(datebuf));

        /* Count files in use by scanning index area */
        uint32_t files_in_use = 0;
        uint32_t max_scan = hb.hb_max_files;
        if (max_scan > 65535)
            max_scan = 65535;

        for (uint32_t fid = 1; fid <= max_scan; fid++) {
            struct vmsfs_file_header fh;
            off_t fh_off = (off_t)(hb.hb_index_lbn + fid - 1) * VMSFS_BLOCK_SIZE;
            ssize_t fh_nr = pread(fd, &fh, sizeof(fh), fh_off);
            if (fh_nr == (ssize_t)sizeof(fh) &&
                fh.fh_magic == VMSFS_FH_MAGIC &&
                (fh.fh_flags & VMSFS_FH_INUSE)) {
                files_in_use++;
            }
        }

        /* Verify bitmap consistency: count free bits and compare */
        const char *bitmap_status = "OK";
        uint32_t bitmap_free = 0;
        size_t bitmap_size = (size_t)hb.hb_bitmap_blocks * VMSFS_BLOCK_SIZE;
        uint8_t *bitmap = malloc(bitmap_size);
        if (bitmap) {
            off_t bm_off = (off_t)hb.hb_bitmap_lbn * VMSFS_BLOCK_SIZE;
            ssize_t bm_nr = pread(fd, bitmap, bitmap_size, bm_off);
            if (bm_nr == (ssize_t)bitmap_size) {
                for (uint32_t i = 0; i < hb.hb_total_blocks; i++) {
                    if (bitmap[i / 8] & (1u << (i % 8)))
                        bitmap_free++;
                }
                if (bitmap_free != hb.hb_free_blocks)
                    bitmap_status = "INCONSISTENT";
            } else {
                bitmap_status = "UNREADABLE";
            }
            free(bitmap);
        } else {
            bitmap_status = "UNREADABLE";
        }

        printf("Volume label:           %s\n", volname);
        printf("Volume creation date:   %s\n", datebuf);
        printf("Total blocks:           %u\n", hb.hb_total_blocks);
        printf("Free blocks:            %u\n", hb.hb_free_blocks);
        printf("Maximum files:          %u\n", hb.hb_max_files);
        printf("Files currently in use: %u\n", files_in_use);
        printf("Bitmap consistency:     %s\n", bitmap_status);
    } else {
        /* Not a VMSFS volume — provide simulated analysis from file stats */
        struct stat st;
        if (fstat(fd, &st) < 0) {
            fprintf(stderr, "%%ANALYZE-E-STATIN, cannot stat %s\n", filename);
            close(fd);
            return 1;
        }

        char datebuf[32];
        format_vms_date((uint64_t)st.st_mtime, datebuf, sizeof(datebuf));

        uint64_t total_blocks = (uint64_t)st.st_size / VMSFS_BLOCK_SIZE;
        if (total_blocks == 0)
            total_blocks = 1;

        printf("Volume label:           UNKNOWN\n");
        printf("Volume creation date:   %s\n", datebuf);
        printf("Total blocks:           %lu\n", (unsigned long)total_blocks);
        printf("Free blocks:            0\n");
        printf("Maximum files:          0\n");
        printf("Files currently in use: 0\n");
        printf("Bitmap consistency:     N/A (not VMSFS format)\n");
    }

    printf("\n%%ANALYZE-I-DISKDONE, disk structure analysis complete\n");
    close(fd);
    return 0;
}

/* ================================================================
 * ANALYZE/SYSTEM (SDA - System Dump Analyzer)
 * ================================================================ */

static void sda_show_summary(void)
{
    char datebuf[32];
    format_vms_now(datebuf, sizeof(datebuf));

    /* Read uptime from /proc/uptime */
    double uptime_sec = 0.0;
    FILE *f = fopen("/proc/uptime", "r");
    if (f) {
        if (fscanf(f, "%lf", &uptime_sec) != 1)
            uptime_sec = 0.0;
        fclose(f);
    }

    int up_days = (int)(uptime_sec / 86400);
    int up_hours = (int)((uptime_sec - up_days * 86400) / 3600);
    int up_mins = (int)((uptime_sec - up_days * 86400 - up_hours * 3600) / 60);
    int up_secs = (int)(uptime_sec - up_days * 86400 - up_hours * 3600 - up_mins * 60);

    /* Count VMS processes from /vms directory if available */
    int process_count = 0;
    DIR *d = opendir("/vms");
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            /* Look for PCB directories (numeric names or process-like entries) */
            if (ent->d_name[0] != '.' && ent->d_type == DT_DIR) {
                /* Check if it looks like a process directory */
                char path[512];
                snprintf(path, sizeof(path), "/vms/%s/pcb", ent->d_name);
                if (access(path, F_OK) == 0)
                    process_count++;
            }
        }
        closedir(d);
    }
    if (process_count == 0)
        process_count = 1;  /* At minimum, this process exists */

    /* Get available memory pages (4K pages) */
    struct sysinfo si;
    unsigned long avail_pages = 0;
    if (sysinfo(&si) == 0) {
        avail_pages = (si.freeram * si.mem_unit) / 4096;
    }

    printf("\nSystem Summary:\n");
    printf("  Node:           OVMX\n");
    printf("  System time:    %s\n", datebuf);
    printf("  Uptime:         %d %02d:%02d:%02d\n",
           up_days, up_hours, up_mins, up_secs);
    printf("  Processes:      %d\n", process_count);
    printf("  Available pages: %lu\n\n", avail_pages);
}

static void sda_show_process(const char *id_str)
{
    if (!id_str || id_str[0] == '\0') {
        printf("%%SDA-E-NEEDPID, /ID= qualifier requires a process ID\n");
        return;
    }

    /* Try to find process info from /vms internal state */
    char pcb_path[512];
    snprintf(pcb_path, sizeof(pcb_path), "/vms/%s/pcb", id_str);

    if (access(pcb_path, F_OK) == 0) {
        /* Read actual PCB data */
        printf("\nProcess %s:\n", id_str);

        char field_path[576];
        char buf[256];
        FILE *f;

        snprintf(field_path, sizeof(field_path), "%s/name", pcb_path);
        f = fopen(field_path, "r");
        if (f) {
            if (fgets(buf, sizeof(buf), f)) {
                buf[strcspn(buf, "\n")] = '\0';
                printf("  Process name:   %s\n", buf);
            }
            fclose(f);
        }

        snprintf(field_path, sizeof(field_path), "%s/state", pcb_path);
        f = fopen(field_path, "r");
        if (f) {
            if (fgets(buf, sizeof(buf), f)) {
                buf[strcspn(buf, "\n")] = '\0';
                printf("  State:          %s\n", buf);
            }
            fclose(f);
        }

        snprintf(field_path, sizeof(field_path), "%s/priority", pcb_path);
        f = fopen(field_path, "r");
        if (f) {
            if (fgets(buf, sizeof(buf), f)) {
                buf[strcspn(buf, "\n")] = '\0';
                printf("  Priority:       %s\n", buf);
            }
            fclose(f);
        }
        printf("\n");
    } else {
        /* No VMS PCB data — show simulated output */
        printf("\nProcess %s:\n", id_str);
        printf("  Process name:   <unknown>\n");
        printf("  State:          CUR\n");
        printf("  Priority:       4\n\n");
    }
}

static void sda_show_device(void)
{
    printf("\nDevice Summary:\n");
    printf("  %-12s  %-10s  %-10s  %s\n", "Device", "Type", "Status", "Volume");
    printf("  %-12s  %-10s  %-10s  %s\n", "------", "----", "------", "------");

    /* List block devices from /sys/block */
    DIR *d = opendir("/sys/block");
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.')
                continue;

            /* Skip virtual devices like loop* and ram* */
            if (strncmp(ent->d_name, "loop", 4) == 0 ||
                strncmp(ent->d_name, "ram", 3) == 0)
                continue;

            char devname[32];
            /* Convert to VMS-style device name */
            snprintf(devname, sizeof(devname), "%.8s:", ent->d_name);
            for (int i = 0; devname[i] && devname[i] != ':'; i++)
                devname[i] = (char)toupper((unsigned char)devname[i]);

            printf("  %-12s  %-10s  %-10s  %s\n",
                   devname, "Disk", "Online", "");
        }
        closedir(d);
    } else {
        printf("  (no block devices found)\n");
    }
    printf("\n");
}

static int analyze_system(void)
{
    printf("OpenVMS System Dump Analyzer\n\n");

    char line[256];
    for (;;) {
        printf("SDA> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin))
            break;

        /* Strip newline and leading/trailing whitespace */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[--len] = '\0';

        char *p = line;
        while (*p && isspace((unsigned char)*p))
            p++;
        if (*p == '\0')
            continue;

        /* Upcase for comparison */
        char upper[256];
        size_t i;
        for (i = 0; p[i] && i < sizeof(upper) - 1; i++)
            upper[i] = (char)toupper((unsigned char)p[i]);
        upper[i] = '\0';

        if (strcmp(upper, "EXIT") == 0 || strcmp(upper, "QUIT") == 0)
            break;

        if (strncmp(upper, "SHOW SUMMARY", 12) == 0) {
            sda_show_summary();
        } else if (strncmp(upper, "SHOW PROCESS", 12) == 0) {
            /* Look for /ID=xxx */
            char *id_pos = strstr(upper, "/ID=");
            if (id_pos) {
                sda_show_process(id_pos + 4);
            } else {
                /* Show current process */
                char pid_str[32];
                snprintf(pid_str, sizeof(pid_str), "%d", (int)getpid());
                sda_show_process(pid_str);
            }
        } else if (strncmp(upper, "SHOW DEVICE", 11) == 0) {
            sda_show_device();
        } else if (strncmp(upper, "HELP", 4) == 0) {
            printf("\nSDA Commands:\n");
            printf("  SHOW SUMMARY          - Display system summary\n");
            printf("  SHOW PROCESS /ID=pid  - Display process information\n");
            printf("  SHOW DEVICE           - List block devices\n");
            printf("  EXIT                  - Exit SDA\n\n");
        } else {
            printf("%%SDA-E-INVCOM, unrecognized command: %s\n", p);
        }
    }

    return 0;
}

/* ================================================================
 * ANALYZE/IMAGE
 * ================================================================ */

static const char *elf_section_flags_str(uint64_t flags, char *buf, size_t bufsz)
{
    buf[0] = '\0';
    if (flags & SHF_ALLOC) {
        if (flags & SHF_EXECINSTR)
            snprintf(buf, bufsz, "RE");
        else if (flags & SHF_WRITE)
            snprintf(buf, bufsz, "RW");
        else
            snprintf(buf, bufsz, "R");
    } else {
        snprintf(buf, bufsz, "--");
    }
    return buf;
}

static int analyze_image(const char *filename)
{
    if (!filename || filename[0] == '\0') {
        fprintf(stderr, "%%ANALYZE-E-NEEDFILE, image file specification required\n");
        return 1;
    }

    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "%%ANALYZE-E-OPENIN, error opening %s\n", filename);
        return 1;
    }

    /* Read ELF header */
    Elf64_Ehdr ehdr;
    ssize_t nr = read(fd, &ehdr, sizeof(ehdr));
    if (nr != (ssize_t)sizeof(ehdr)) {
        fprintf(stderr, "%%ANALYZE-E-READERR, error reading ELF header from %s\n", filename);
        close(fd);
        return 1;
    }

    /* Verify ELF magic */
    if (ehdr.e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr.e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr.e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr.e_ident[EI_MAG3] != ELFMAG3) {
        fprintf(stderr, "%%ANALYZE-E-NOTELF, %s is not a valid ELF image\n", filename);
        close(fd);
        return 1;
    }

    /* Check for 64-bit ELF */
    if (ehdr.e_ident[EI_CLASS] != ELFCLASS64) {
        fprintf(stderr, "%%ANALYZE-E-BADCLASS, only 64-bit ELF images are supported\n");
        close(fd);
        return 1;
    }

    /* Extract base filename for display */
    const char *basename_ptr = strrchr(filename, '/');
    const char *display_name = basename_ptr ? basename_ptr + 1 : filename;

    /* Determine image type */
    const char *image_type;
    switch (ehdr.e_type) {
    case ET_EXEC: image_type = "Executable"; break;
    case ET_DYN:  image_type = "Shareable image"; break;
    case ET_REL:  image_type = "Relocatable"; break;
    case ET_CORE: image_type = "Core dump"; break;
    default:      image_type = "Unknown"; break;
    }

    /* Get link date from file modification time */
    struct stat st;
    fstat(fd, &st);
    char datebuf[32];
    format_vms_date((uint64_t)st.st_mtime, datebuf, sizeof(datebuf));

    printf("%%ANALYZE-I-IMAGE, analyzing image %s\n\n", display_name);
    printf("Image name:            %s\n", display_name);
    printf("Image type:            %s\n", image_type);
    printf("Link date/time:        %s\n", datebuf);
    printf("Entry point:           0x%016lX\n", (unsigned long)ehdr.e_entry);

    /* Read section headers */
    if (ehdr.e_shnum > 0 && ehdr.e_shoff > 0) {
        size_t sh_size = (size_t)ehdr.e_shnum * ehdr.e_shentsize;
        Elf64_Shdr *shdrs = malloc(sh_size);
        if (!shdrs) {
            fprintf(stderr, "%%ANALYZE-W-NOMEM, cannot allocate memory for section headers\n");
            close(fd);
            printf("\n%%ANALYZE-I-IMAGEDONE, image analysis complete\n");
            return 0;
        }

        if (pread(fd, shdrs, sh_size, (off_t)ehdr.e_shoff) != (ssize_t)sh_size) {
            fprintf(stderr, "%%ANALYZE-W-READERR, error reading section headers\n");
            free(shdrs);
            close(fd);
            printf("\n%%ANALYZE-I-IMAGEDONE, image analysis complete\n");
            return 0;
        }

        /* Read section name string table */
        char *shstrtab = NULL;
        if (ehdr.e_shstrndx < ehdr.e_shnum) {
            Elf64_Shdr *strhdr = &shdrs[ehdr.e_shstrndx];
            shstrtab = malloc(strhdr->sh_size);
            if (shstrtab) {
                if (pread(fd, shstrtab, strhdr->sh_size, (off_t)strhdr->sh_offset)
                    != (ssize_t)strhdr->sh_size) {
                    free(shstrtab);
                    shstrtab = NULL;
                }
            }
        }

        printf("Image sections:\n");

        for (int i = 0; i < ehdr.e_shnum; i++) {
            Elf64_Shdr *sh = &shdrs[i];

            /* Only show allocated sections (loaded into memory) */
            if (!(sh->sh_flags & SHF_ALLOC))
                continue;
            if (sh->sh_size == 0)
                continue;

            const char *name = "???";
            if (shstrtab && sh->sh_name < shdrs[ehdr.e_shstrndx].sh_size)
                name = shstrtab + sh->sh_name;

            char flagbuf[8];
            elf_section_flags_str(sh->sh_flags, flagbuf, sizeof(flagbuf));

            printf("    %-8s Virtual addr: 0x%08lX  Size: 0x%08lX  Flags: %s\n",
                   name,
                   (unsigned long)sh->sh_addr,
                   (unsigned long)sh->sh_size,
                   flagbuf);
        }

        free(shstrtab);
        free(shdrs);
    }

    printf("\n%%ANALYZE-I-IMAGEDONE, image analysis complete\n");
    close(fd);
    return 0;
}

/* ================================================================
 * ANALYZE/OBJECT (stub)
 * ================================================================ */

static int analyze_object(void)
{
    printf("%%ANALYZE-I-NOTIMPL, object file analysis not available\n");
    return 0;
}

/* ================================================================
 * Main
 * ================================================================ */

static void usage(void)
{
    fprintf(stderr, "Usage: ANALYZE qualifier [filename]\n");
    fprintf(stderr, "  /DISK_STRUCTURE device  Analyze VMSFS volume structure\n");
    fprintf(stderr, "  /SYSTEM                 System Dump Analyzer (interactive)\n");
    fprintf(stderr, "  /IMAGE filename         Analyze executable image (ELF)\n");
    fprintf(stderr, "  /OBJECT filename        Analyze object file (stub)\n");
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage();
        return 1;
    }

    /* Parse qualifier from argv[1] — may be /DISK_STRUCTURE, /SYSTEM, etc. */
    const char *qual = argv[1];
    const char *filename = (argc >= 3) ? argv[2] : NULL;

    /* Strip leading / or - */
    if (qual[0] == '/' || qual[0] == '-')
        qual++;

    /* Allow minimum unique abbreviation */
    if (strncasecmp(qual, "DISK_STRUCTURE", 4) == 0 ||
        strcasematch(qual, "DISK")) {
        return analyze_disk_structure(filename);
    } else if (strncasecmp(qual, "SYSTEM", 3) == 0) {
        return analyze_system();
    } else if (strncasecmp(qual, "IMAGE", 3) == 0) {
        return analyze_image(filename);
    } else if (strncasecmp(qual, "OBJECT", 3) == 0) {
        return analyze_object();
    } else {
        fprintf(stderr, "%%ANALYZE-E-INVQUAL, unrecognized qualifier: /%s\n", qual);
        usage();
        return 1;
    }
}
