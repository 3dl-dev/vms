# $CREATE_UID on OpenVMS Alpha V8.4 (observation)

MACRO-32 program (assembled with the lab's own `MACRO`, linked with `LINK`) that calls
`SYS$CREATE_UID` twice and prints the four longwords of each uid with `$FAO !XL`:

    AD7CC146 11F1C09C 4C41058D 31414850
    AD7CC147 11F1C09C 4C41058D 31414850

Read as little-endian bytes: time_low = AD7CC146 / AD7CC147 (+1 between back-to-back calls),
time_mid = C09C, time_hi_and_version = 11F1 (version nibble 1, so the 60-bit timestamp is
0x1F1C09CAD7CC146 = 100 ns since 15-OCT-1582), clock_seq_hi_and_reserved = 8D (variant 10xx),
clock_seq_low = 05, node = 41 4C 50 48 41 31 = "ALPHA1" (the node's SCSNODE name). Captured
2026-10-05 on lab pod corpusalpha-1 (ALPHA1). Nothing disassembled (Rule 8): this is the
service's observable output.
