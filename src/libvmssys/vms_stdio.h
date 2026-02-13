/*
 * vms_stdio.h - Buffered I/O declarations (replaces stdio.h)
 */

#ifndef _VMS_STDIO_H
#define _VMS_STDIO_H

#include "vms_types.h"

#define VMS_STDIO_BUFSZ  4096
#define VMS_EOF          (-1)

/* Buffered file stream */
typedef struct vms_file {
    int   fd;
    int   flags;          /* VMS_FILE_* flags */
    int   error;
    int   eof;
    char  buf[VMS_STDIO_BUFSZ];
    int   buf_pos;        /* current read/write position in buffer */
    int   buf_len;        /* valid bytes in buffer (for reads) */
    int   buf_mode;       /* 0=fully buffered, 1=line buffered, 2=unbuffered */
} vms_file_t;

/* Internal flags */
#define VMS_FILE_READ    0x01
#define VMS_FILE_WRITE   0x02
#define VMS_FILE_APPEND  0x04
#define VMS_FILE_ALLOC   0x08  /* dynamically allocated */

/* Standard streams (initialized by runtime) */
extern vms_file_t vms_stdin_obj;
extern vms_file_t vms_stdout_obj;
extern vms_file_t vms_stderr_obj;

#define vms_stdin  (&vms_stdin_obj)
#define vms_stdout (&vms_stdout_obj)
#define vms_stderr (&vms_stderr_obj)

/* File operations */
vms_file_t *vms_fopen(const char *path, const char *mode);
int         vms_fclose(vms_file_t *f);
vms_size_t  vms_fread(void *ptr, vms_size_t size, vms_size_t nmemb, vms_file_t *f);
vms_size_t  vms_fwrite(const void *ptr, vms_size_t size, vms_size_t nmemb, vms_file_t *f);
char       *vms_fgets(char *s, int size, vms_file_t *f);
int         vms_fputc(int c, vms_file_t *f);
int         vms_fputs(const char *s, vms_file_t *f);
int         vms_fgetc(vms_file_t *f);
int         vms_fflush(vms_file_t *f);
int         vms_feof(vms_file_t *f);
int         vms_ferror(vms_file_t *f);
int         vms_fileno(vms_file_t *f);
int         vms_fseek(vms_file_t *f, long offset, int whence);
long        vms_ftell(vms_file_t *f);

/* Formatted output */
int vms_fprintf(vms_file_t *f, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
int vms_printf(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
int vms_vfprintf(vms_file_t *f, const char *fmt, va_list ap);

/* Initialize standard streams (called by runtime init) */
void vms_stdio_init(void);

#endif /* _VMS_STDIO_H */
