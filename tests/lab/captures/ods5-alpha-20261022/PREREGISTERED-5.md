# DEC C RTL file-name translation on ODS-5: pre-registered (written before session 5)

rd vms-da9b (epic vms-810). Written 2026-10-22 before the lab-Alpha session that
produced `CONSOLE-5.txt`. Predictions come from the public HP C Run-Time Library
Reference Manual (feature logical names DECC$EFS_CHARSET, DECC$EFS_CASE_PRESERVE,
DECC$FILENAME_UNIX_REPORT; decc$to_vms, decc$translate_vms). The probe is our own
MACRO-32 program (`TOVMS.MAR`, in the console) calling the CRTL entry points
directly, run once per feature setting.

| # | prediction |
|---|---|
| E1 | Default (no feature logicals): decc$to_vms("bits/c++config.h") does NOT produce an escaped `c^+^+config.h`; the '+' is either rejected or replaced. |
| E2 | Default: a Unix name with several dots ("a.b.c.d") keeps only the last dot as the type delimiter; the others become underscores (ODS-2 rules). |
| E3 | DECC$EFS_CHARSET=ENABLE: "bits/c++config.h" -> `[.bits]c^+^+config.h` (escaped, case kept); "a.b.c.d" -> `a^.b^.c.d`. |
| E4 | decc$translate_vms("[.bits]c^+^+config.h") gives "bits/c++config.h" once EFS_CHARSET is enabled (escapes removed). |
| E5 | DECC$EFS_CASE_PRESERVE has no visible effect on decc$to_vms output (case is already kept); it governs what the RTL reports for names RMS returns. |
