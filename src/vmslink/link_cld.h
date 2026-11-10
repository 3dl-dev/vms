/*
 * link_cld.h - the LINK command definition (CDU source), compiled into LINK.EXE.
 *
 * LINK.EXE takes its command the VMS way (rd vms-3cb1): the command line DCL
 * hands it is parsed by CLI$DCL_PARSE against this table, and the linker reads
 * every qualifier and file through CLI$PRESENT / CLI$GET_VALUE (link_cli.c).
 * On OpenVMS the LINK verb lives in DCLTABLES and LINK.EXE asks the CLI for the
 * same entities; a utility that is its own foreign command carries its CLD
 * object in the image, as here. The text is Command Definition Utility source;
 * cli$compile_cld (src/libvms/rtl/lib_cli.c) is OVMX's SET COMMAND.
 *
 * PROVENANCE (Rule 8): the verb, its parameter and the qualifier set, with each
 * qualifier's placement (command qualifier, file qualifier, or both), value and
 * negatability, are taken from the public OpenVMS Linker Utility Manual (LINK
 * Command Reference) and the DCL Dictionary LINK entry. Nothing is copied from
 * VSI/HPE material. Which of them this linker acts on, and the honest refusal
 * of the rest, is in link_cli.c.
 *
 * Placement, per the manual:
 *   - file qualifiers   (PLACEMENT=LOCAL)      /INCLUDE /LIBRARY /OPTIONS
 *                                              /SELECTIVE_SEARCH
 *   - both              (PLACEMENT=POSITIONAL) /SHAREABLE: after the verb it
 *     asks for a shareable image; after an input file it marks that file as a
 *     shareable image to link against (that use is legal only in an options
 *     file on OpenVMS; this linker also takes it on the command line, as it
 *     always has).
 *   - everything else: command qualifiers (PLACEMENT=GLOBAL, the default).
 */
#ifndef OVMX_LINK_CLD_H
#define OVMX_LINK_CLD_H

static const char link_cld_source[] =
    "MODULE LINK_CLD\n"
    "DEFINE VERB LINK\n"
    "    PARAMETER P1, LABEL=FILE, PROMPT=\"File\", VALUE(REQUIRED, LIST, TYPE=$INFILE)\n"
    /* image kind and output */
    "    QUALIFIER EXECUTABLE, NEGATABLE, VALUE(TYPE=$OUTFILE)\n"
    "    QUALIFIER SHAREABLE, NEGATABLE, PLACEMENT=POSITIONAL, VALUE(TYPE=$OUTFILE)\n"
    "    QUALIFIER SYSTEM, NEGATABLE, VALUE\n"
    "    QUALIFIER SYMBOL_TABLE, NEGATABLE, VALUE(TYPE=$OUTFILE)\n"
    "    QUALIFIER DSF, NEGATABLE, VALUE(TYPE=$OUTFILE)\n"
    /* map */
    "    QUALIFIER MAP, NEGATABLE, VALUE(TYPE=$OUTFILE)\n"
    "    QUALIFIER BRIEF, NONNEGATABLE\n"
    "    QUALIFIER FULL, NEGATABLE, VALUE(LIST)\n"
    "    QUALIFIER CROSS_REFERENCE, NEGATABLE\n"
    /* input files */
    "    QUALIFIER OPTIONS, NONNEGATABLE, PLACEMENT=LOCAL\n"
    "    QUALIFIER LIBRARY, NONNEGATABLE, PLACEMENT=LOCAL\n"
    "    QUALIFIER INCLUDE, NONNEGATABLE, PLACEMENT=LOCAL, VALUE(REQUIRED, LIST)\n"
    "    QUALIFIER SELECTIVE_SEARCH, NONNEGATABLE, PLACEMENT=LOCAL\n"
    /* library search */
    "    QUALIFIER SYSLIB, NEGATABLE\n"
    "    QUALIFIER SYSSHR, NEGATABLE\n"
    "    QUALIFIER SYSEXE, NEGATABLE, VALUE\n"
    "    QUALIFIER USERLIBRARY, NEGATABLE, VALUE(LIST)\n"
    /* debugging */
    "    QUALIFIER DEBUG, NEGATABLE, VALUE(TYPE=$INFILE)\n"
    "    QUALIFIER TRACEBACK, NEGATABLE\n"
    /* messages */
    "    QUALIFIER INFORMATIONALS, NEGATABLE\n"
    /* image layout and attributes */
    "    QUALIFIER BPAGE, NEGATABLE, VALUE\n"
    "    QUALIFIER CONTIGUOUS, NEGATABLE\n"
    "    QUALIFIER DEMAND_ZERO, NEGATABLE, VALUE\n"
    "    QUALIFIER DNI, NEGATABLE\n"
    "    QUALIFIER GST, NEGATABLE\n"
    "    QUALIFIER HEADER, NEGATABLE\n"
    "    QUALIFIER NATIVE_ONLY, NEGATABLE\n"
    "    QUALIFIER P0IMAGE, NEGATABLE\n"
    "    QUALIFIER PROTECT, NEGATABLE\n"
    "    QUALIFIER REPLACE, NEGATABLE\n"
    "    QUALIFIER SECTION_BINDING, NEGATABLE, VALUE(LIST)\n"
    "    QUALIFIER THREADS_ENABLE, NEGATABLE, VALUE(LIST)\n"
    "    QUALIFIER ALPHA, NONNEGATABLE\n"
    "    QUALIFIER VAX, NONNEGATABLE\n";

#endif
