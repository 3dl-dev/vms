#!/usr/bin/env python3
"""
gen_ssdef.py - VMS System Service Status Code Generator

Generates ssdef.h from a specification table. The status codes follow
the VMS condition value format:

  Bits 31-28: Control
  Bits 27-16: Facility (0 = SYSTEM)
  Bit  15:    Customer flag
  Bits 14-3:  Message number
  Bits 2-0:   Severity (0=W, 1=S, 2=E, 3=I, 4=F)

Usage:
    python gen_ssdef.py > src/libvms/include/ssdef.h

This script is the canonical source for all SS$ status codes.
"""

import sys
import datetime

# Severity codes
SEV_WARNING  = 0
SEV_SUCCESS  = 1
SEV_ERROR    = 2
SEV_INFO     = 3
SEV_FATAL    = 4

FACILITY_SYSTEM = 0

def make_code(facility, msg_no, severity):
    """Construct a VMS condition value."""
    return ((facility & 0xFFF) << 16) | ((msg_no & 0xFFF) << 3) | (severity & 0x7)


# ---------------------------------------------------------------
# Status code definitions: (name, message_number, severity, comment)
# ---------------------------------------------------------------
STATUS_CODES = [
    # Success codes
    ("SS$_NORMAL",      0x0000, SEV_SUCCESS,  "Normal successful completion"),
    ("SS$_WASCLR",      0x0001, SEV_SUCCESS,  "Flag was clear (success + info)"),
    ("SS$_WASSET",      0x0002, SEV_SUCCESS,  "Flag was set (success + info)"),
    ("SS$_BUFFEROVF",   0x0011, SEV_SUCCESS,  "Buffer overflow (partial success)"),
    ("SS$_SUPERSEDE",   0x0003, SEV_SUCCESS,  "Logical name superseded"),
    ("SS$_CONCEALED",   0x0004, SEV_SUCCESS,  "Concealed device"),
    ("SS$_CREATED",     0x0005, SEV_SUCCESS,  "Object created"),

    # Error codes
    ("SS$_ACCVIO",      0x0001, SEV_FATAL,    "Access violation"),
    ("SS$_BADPARAM",    0x0002, SEV_ERROR,    "Bad parameter value"),
    ("SS$_EXQUOTA",     0x0003, SEV_ERROR,    "Exceeded quota"),
    ("SS$_INSFMEM",     0x0004, SEV_ERROR,    "Insufficient memory"),
    ("SS$_NOPRIV",      0x0024, SEV_ERROR,    "No privilege"),
    ("SS$_NOSUCHDEV",   0x0005, SEV_ERROR,    "No such device"),
    ("SS$_NOSUCHFILE",  0x0006, SEV_ERROR,    "No such file"),
    ("SS$_BUGCHECK",    0x0007, SEV_FATAL,    "Internal consistency failure"),
    ("SS$_FILACCERR",   0x0008, SEV_ERROR,    "File access error"),
    ("SS$_DEVNOTMOUNT", 0x0009, SEV_ERROR,    "Device not mounted"),
    ("SS$_DEVALLOC",    0x000A, SEV_ERROR,    "Device already allocated"),
    ("SS$_IVDEVNAM",    0x000B, SEV_ERROR,    "Invalid device name"),
    ("SS$_IVLOGNAM",    0x000C, SEV_ERROR,    "Invalid logical name"),
    ("SS$_IVLOGTAB",    0x000D, SEV_ERROR,    "Invalid logical name table"),
    ("SS$_NOLOGNAM",    0x000E, SEV_ERROR,    "No such logical name"),
    ("SS$_NOLOGTAB",    0x000F, SEV_ERROR,    "No such logical name table"),
    ("SS$_RESULTOVF",   0x0010, SEV_ERROR,    "Result string overflow"),
    ("SS$_ABORT",       0x0025, SEV_ERROR,    "Abort"),
    ("SS$_CANCEL",      0x0012, SEV_ERROR,    "I/O operation cancelled"),
    ("SS$_DUPNAM",      0x0016, SEV_ERROR,    "Duplicate name"),
    ("SS$_IVCHAN",      0x0019, SEV_ERROR,    "Invalid I/O channel"),
    ("SS$_IVMODE",      0x001A, SEV_ERROR,    "Invalid access mode"),
    ("SS$_IVSSRQ",      0x001B, SEV_ERROR,    "Invalid system service request"),
    ("SS$_ILLIOFUNC",   0x001C, SEV_ERROR,    "Illegal I/O function"),
    ("SS$_TIMEOUT",     0x001D, SEV_ERROR,    "Device timeout"),
    ("SS$_UNASEFC",     0x001E, SEV_ERROR,    "Unassociated event flag cluster"),
    ("SS$_ILLEFC",      0x001F, SEV_ERROR,    "Illegal event flag cluster"),

    # Process-related
    ("SS$_NONEXPR",     0x0020, SEV_ERROR,    "Nonexistent process"),
    ("SS$_SUSPENDED",   0x0021, SEV_ERROR,    "Process suspended"),
    ("SS$_RESIGNAL",    0x0022, SEV_ERROR,    "Resignal condition"),
    ("SS$_CONTINUE",    0x0000, SEV_SUCCESS,  "Continue execution (same as SS$_NORMAL)"),
    ("SS$_UNWIND",      0x0023, SEV_ERROR,    "Unwind call stack"),

    # Timer
    ("SS$_NOTQUEUED",   0x0026, SEV_ERROR,    "Timer request not queued"),

    # Lock manager
    ("SS$_DEADLOCK",    0x0027, SEV_ERROR,    "Deadlock detected"),
    ("SS$_VALNOTVALID", 0x0028, SEV_ERROR,    "Lock value block not valid"),
    ("SS$_PARNOTGRANT", 0x0029, SEV_ERROR,    "Parent lock not granted"),

    # Quota / resource
    ("SS$_EXENQLM",     0x002A, SEV_ERROR,    "Exceeded enqueue limit"),
    ("SS$_EXASTLM",     0x002B, SEV_ERROR,    "Exceeded AST limit"),
    ("SS$_EXBYTLM",     0x002C, SEV_ERROR,    "Exceeded byte count limit"),

    # Feature / I/O
    ("SS$_UNSUPPORTED", 0x002D, SEV_ERROR,    "Unsupported operation"),
    ("SS$_NOREADER",    0x002E, SEV_ERROR,    "No reader on mailbox"),
    ("SS$_NOWRITER",    0x002F, SEV_ERROR,    "No writer on mailbox"),
    ("SS$_NOMSG",       0x0030, SEV_ERROR,    "No message"),
    ("SS$_ENDOFFILE",   0x0031, SEV_ERROR,    "End of file"),
    ("SS$_ENDOFTAPE",   0x0032, SEV_ERROR,    "End of tape"),
    ("SS$_DATACHECK",   0x0033, SEV_ERROR,    "Data check error"),
    ("SS$_PARITY",      0x0034, SEV_ERROR,    "Parity error"),
    ("SS$_MSGNOTFND",   0x0035, SEV_ERROR,    "Message not found"),

    # CLI
    ("SS$_IVVERB",      0x0036, SEV_ERROR,    "Invalid verb (unknown command)"),
    ("SS$_IVQUAL",      0x0037, SEV_ERROR,    "Invalid qualifier"),
    ("SS$_IVKEYW",      0x0038, SEV_ERROR,    "Invalid keyword"),
]


def generate_header():
    """Generate the ssdef.h header file."""
    now = datetime.datetime.now()

    print(f"#ifndef __SSDEF_H")
    print(f"#define __SSDEF_H")
    print()
    print(f"#include \"stsdef.h\"")
    print()
    print(f"/*")
    print(f" * VMS System Service Status Codes (SS$_ symbols)")
    print(f" *")
    print(f" * Auto-generated by gen_ssdef.py on {now.strftime('%Y-%m-%d %H:%M:%S')}")
    print(f" *")
    print(f" * These are the condition values returned by VMS system services.")
    print(f" * Bit 0 indicates success (1) or failure (0).")
    print(f" * Code that tests \"if (status & 1)\" is the standard VMS idiom.")
    print(f" */")
    print()

    # Group by severity for readability
    severity_names = {
        SEV_SUCCESS: "Success codes",
        SEV_INFO:    "Informational codes",
        SEV_WARNING: "Warning codes",
        SEV_ERROR:   "Error codes",
        SEV_FATAL:   "Fatal error codes",
    }

    emitted = set()
    for sev_val, sev_label in sorted(severity_names.items()):
        codes = [(n, m, s, c) for n, m, s, c in STATUS_CODES if s == sev_val]
        if not codes:
            continue

        print(f"/* {sev_label} (severity = {sev_val}) */")
        for name, msg_no, severity, comment in codes:
            code = make_code(FACILITY_SYSTEM, msg_no, severity)
            if name not in emitted:
                print(f"#define {name:<20s} 0x{code:08X}  /* {comment} */")
                emitted.add(name)
        print()

    # Aliases
    print("/* Aliases */")
    print("#define SS$_DUPLNAM     SS$_DUPNAM      /* Duplicate logical name */")
    print("#define SS$_NOTRAN      SS$_NOLOGNAM    /* No translation (alias) */")
    print()
    print("#endif /* __SSDEF_H */")


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--verify":
        # Verification mode: check that all codes have unique values
        # within the same severity level
        seen = {}
        errors = 0
        for name, msg_no, severity, comment in STATUS_CODES:
            code = make_code(FACILITY_SYSTEM, msg_no, severity)
            key = f"0x{code:08X}"
            if key in seen:
                print(f"WARNING: {name} ({key}) collides with {seen[key]}",
                      file=sys.stderr)
                errors += 1
            seen[key] = name
        if errors:
            print(f"\n{errors} collision(s) found.", file=sys.stderr)
            return 1
        else:
            print("All codes are unique.", file=sys.stderr)
            return 0

    generate_header()
    return 0


if __name__ == "__main__":
    sys.exit(main())
