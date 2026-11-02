# Pre-registration: DCL file verbs take a comma list in P1 of any length (rd vms-ba2)

Written before the capture. Lab: a real OpenVMS node in ovmx-lab, a scratch
directory [.BA2] under SYS$LOGIN holding A.TXT..L.TXT (one line each, "A".."L").
Sources for the expectations: the OpenVMS DCL Dictionary (DELETE, COPY, TYPE,
PURGE, DIRECTORY, APPEND, RENAME: "parameter: filespec[,...]") and the DCL
User's Manual on parameter lists and temporary (sticky) file defaults.

| # | Command | Expected |
|---|---------|----------|
| 1 | DELETE A.TXT;*,B.TXT;*,...,J.TXT;* (10 elements) | all ten gone, $STATUS %X00000001 |
| 2 | DELETE K.TXT;* L.TXT;* (two parameters, blank-separated) | %DCL-W-MAXPARM ... \L.TXT;*\ ; nothing deleted |
| 3 | TYPE A.TXT,B.TXT,C.TXT | A, B, C printed in order |
| 4 | COPY A.TXT,B.TXT C2.TXT | one C2.TXT;1 holding A then B (concatenated) |
| 5 | COPY A.TXT,B.TXT [.SUB]*.* | [.SUB]A.TXT;1 and [.SUB]B.TXT;1 |
| 6 | APPEND A.TXT,B.TXT C.TXT | C.TXT;1 now C, A, B |
| 7 | DIRECTORY A.TXT,B.TXT,C.TXT | one header, three files, "Total of 3 files." |
| 8 | PURGE A.TXT,B.TXT (after making A.TXT;2, B.TXT;2) | only ;2 of each remain |
| 9 | RENAME A.TXT,B.TXT *.NEW | A.NEW;1, B.NEW;1 |
| 10 | DELETE [.SUB]A.TXT;*,B.TXT;* | B.TXT is taken from [.SUB] (directory is sticky) |
| 11 | DELETE A.TXT;*,NOPE.TXT;*,B.TXT;* | A and B deleted; a warning for NOPE; $STATUS a warning |
| 12 | DELETE/LOG X1.TXT;*,X2.TXT;* | one FILDEL line per file, then a TOTAL line for the command |
