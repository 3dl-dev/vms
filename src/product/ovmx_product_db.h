/*
 * ovmx_product_db.h - the OVMX product (PCSI-equivalent) database format,
 * bead vms-df9.
 *
 * ----------------------------------------------------------------------------
 * CLEAN-ROOM LABELING (Rule 8) -- read this before touching a byte offset.
 * ----------------------------------------------------------------------------
 *
 * OpenVMS's PCSI utility keeps a product database on every system disk
 * recording what has been installed, so `PRODUCT SHOW PRODUCT` can list it
 * (behavior observed on real media, docs/design-vms-faithful-install.md
 * sec 2-3; docs/design-ovmx-kit-format.md sec 1). VSI has never published
 * that database's byte layout -- only its behavior (a per-system record of
 * name/producer/version/install-date/state that PRODUCT SHOW PRODUCT and
 * PRODUCT SHOW HISTORY read back). This header defines OVMX's OWN
 * representation of that behavior and labels it as such: a flat,
 * fixed-size, whole-file-struct database (the same idiom
 * src/imgact/known_images.h's KFE format and SYSGEN's sysgen_params.h
 * already use for their own OVMX-invented on-disk formats) -- never
 * presented as VSI's PCSI database layout.
 *
 * Location: SYS$SYSTEM:VMS$PRODUCT_DATABASE.DAT on the installed system's
 * OWN root (i.e. under the /DESTINATION device's [SYSEXE], not the
 * currently-booted system's SYS$SYSTEM: -- PRODUCT INSTALL installs onto a
 * possibly-different target volume than the one running it). "VMS$" is
 * the same OVMX-internal-database naming convention
 * SYS$SYSTEM:VMS$KNOWN_IMAGES.DAT already established.
 *
 * Writer + reader: src/product/product.c (PRODUCT.EXE) only. Plain
 * buffered fread()/fwrite() of the whole struct, matching known_images.h's
 * own writer/reader split rationale -- this database is small (at most
 * OVMX_PRODDB_MAX_PRODUCTS records) and never needs mmap.
 */
#ifndef OVMX_PRODUCT_DB_H
#define OVMX_PRODUCT_DB_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 8 raw bytes, no NUL terminator -- compare with memcmp(), not strcmp(). */
#define OVMX_PRODDB_MAGIC       "OVMXPDB1"
#define OVMX_PRODDB_MAGIC_LEN   8

#define OVMX_PRODDB_FORMAT_VERSION 1
#define OVMX_PRODDB_MAX_PRODUCTS   64

#define OVMX_PRODDB_NAME_MAX     40
#define OVMX_PRODDB_PRODUCER_MAX 16
#define OVMX_PRODDB_VERSION_MAX  16

/* ovmx_product_record.state */
#define OVMX_PRODUCT_STATE_INSTALLED 1u

struct ovmx_product_record {
    char     pr_name[OVMX_PRODDB_NAME_MAX];        /* e.g. "OVMX X86VMS VMS" */
    char     pr_producer[OVMX_PRODDB_PRODUCER_MAX]; /* e.g. "OVMX" */
    char     pr_version[OVMX_PRODDB_VERSION_MAX];   /* copied from the kit header verbatim -- never a literal here (INV-1) */
    uint64_t pr_install_time;   /* seconds since epoch, when PRODUCT INSTALL ran */
    uint32_t pr_file_count;     /* files the kit placed (from kh_file_count) */
    uint32_t pr_state;          /* OVMX_PRODUCT_STATE_* */
};

struct ovmx_product_db {
    char     db_magic[OVMX_PRODDB_MAGIC_LEN];  /* OVMX_PRODDB_MAGIC */
    uint32_t db_format_version;                /* OVMX_PRODDB_FORMAT_VERSION */
    uint32_t db_record_count;                  /* valid entries in db_records */
    struct ovmx_product_record db_records[OVMX_PRODDB_MAX_PRODUCTS];
};

#ifdef __cplusplus
}
#endif

#endif /* OVMX_PRODUCT_DB_H */
