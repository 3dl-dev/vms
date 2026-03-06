/*
 * terminal.h - VMS Terminal Characteristics Model
 *
 * Provides a structured representation of VMS terminal characteristics
 * that can be queried and modified via SET TERMINAL / SHOW TERMINAL.
 * Characteristics that map to real termios settings are applied;
 * others are stored and reported but have no physical effect.
 */

#ifndef __DCL_TERMINAL_H
#define __DCL_TERMINAL_H

#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

/* Terminal characteristic bits (matching VMS TT$ constants) */
#define TT_ECHO          (1U << 0)
#define TT_WRAP          (1U << 1)
#define TT_BROADCAST     (1U << 2)
#define TT_TYPEAHEAD     (1U << 3)
#define TT_HOSTSYNC      (1U << 4)
#define TT_TTSYNC        (1U << 5)
#define TT_LINE_EDITING  (1U << 6)
#define TT_INSERT        (1U << 7)
#define TT_SCOPE         (1U << 8)
#define TT_LOWERCASE     (1U << 9)
#define TT_TAB           (1U << 10)
#define TT_MECHTAB       (1U << 11)
#define TT_WRAP_AT_EOL   (1U << 12)
#define TT_HOLDSCREEN    (1U << 13)
#define TT_EIGHTBIT      (1U << 14)
#define TT_NOBRDCST      (1U << 15)   /* inverse of BROADCAST for display */
#define TT_READSYNC      (1U << 16)
#define TT_PASTHRU       (1U << 17)
#define TT_NOECHO        (1U << 18)   /* inverse of ECHO for display */
#define TT_ESCAPE        (1U << 19)
#define TT_NOTYPEAHEAD   (1U << 20)   /* inverse of TYPEAHEAD for display */
#define TT_FORM          (1U << 21)
#define TT_FULLDUP       (1U << 22)
#define TT_MODEM         (1U << 23)
#define TT_OPER          (1U << 24)
#define TT_PAGE          (1U << 25)
#define TT_ALTYPEAHD     (1U << 26)
#define TT_RUNOUT        (1U << 27)   /* physical carriages */
#define TT_FALLBACK      (1U << 28)
#define TT_DIALUP        (1U << 29)
#define TT_SECURE        (1U << 30)

/* Default characteristics for a VT100-class terminal */
#define TT_DEFAULT_CHARS ( \
    TT_ECHO | TT_WRAP | TT_BROADCAST | TT_TYPEAHEAD | \
    TT_HOSTSYNC | TT_TTSYNC | TT_LINE_EDITING | TT_INSERT | \
    TT_SCOPE | TT_LOWERCASE | TT_TAB | TT_FULLDUP | TT_PAGE)

/* VMS terminal state */
struct vms_terminal {
    char device_name[16];      /* _FTA0: */
    char device_type[16];      /* VT100 */
    char owner[64];
    uint32_t characteristics;  /* bit field of TT_ flags */
    int width;
    int page;
    int speed;                 /* baud rate (display only) */
    int parity;                /* 0=none, 1=even, 2=odd */
};

/* Initialize terminal to VMS defaults, probing real terminal */
void vms_terminal_init(struct vms_terminal *term);

/* Set or clear a characteristic bit */
void vms_terminal_set_char(struct vms_terminal *term, uint32_t bit, int on);

/* Query a characteristic bit (returns nonzero if set) */
int vms_terminal_get_char(const struct vms_terminal *term, uint32_t bit);

/* Apply characteristics that map to real termios (echo, etc.) */
void vms_terminal_apply(const struct vms_terminal *term);

/* Format SHOW TERMINAL output to FILE stream */
void vms_terminal_show(const struct vms_terminal *term, FILE *out);

/* Terminal device allocation pool */
#define VMS_TERM_MAX_DEVICES 100
#define VMS_TERM_TABLE_PATH "/vms/SYS0/SYSCOMMON/SYSEXE/TERMINAL_TABLE.DAT"

struct terminal_device {
    char name[16];            /* _FTA0: */
    pid_t owner_pid;          /* Linux PID of owning process */
    char owner_name[64];      /* VMS process name */
    uint32_t characteristics; /* from terminal model */
    int allocated;            /* 0=free, 1=in use */
};

/* Allocate a terminal device name. Returns device name or NULL. */
const char *vms_term_allocate(const char *prefix, pid_t pid, const char *owner);

/* Deallocate a terminal device. */
void vms_term_deallocate(const char *device_name);

/* Look up a terminal device by name. Returns NULL if not found. */
struct terminal_device *vms_term_lookup(const char *device_name);

/* List all allocated terminal devices. Returns count. */
int vms_term_list(struct terminal_device *out, int max, int *count);

#endif /* __DCL_TERMINAL_H */
