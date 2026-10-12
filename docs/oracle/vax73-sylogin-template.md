# VAX V7.3 SYS$MANAGER:SYLOGIN.TEMPLATE -- terminal inquiry (rd vms-bd71)

Read on 2026-10-11 from the lab VAX (kslab-f8c, node VAX1) through `ovmx-lab-console`,
with `SEARCH/WINDOW=(12,4) SYS$MANAGER:SYLOGIN.TEMPLATE INQUIRE`. These are observed
facts about the shipped template; OVMX's SYLOGIN.COM implements the same behaviour in
its own words.

- The template enables `SET TERMINAL/INQUIRE` for interactive logins. It is not commented
  out.
- It skips terminals whose physical device name, `F$GETDVI("TT","TT_PHYDEVNAM")`,
  characters 1-2, is one of `TW RT WT TK WS PY FT TN`. These are network and virtual
  terminals: DECnet CTERM `RT`, telnet `TN`, and so on.
- It keeps the user's display size: `/PAGE=` is taken from `F$GETDVI("TT","TT_PAGE")` and
  `/WIDTH=` from `F$GETDVI("TT","DEVBUFSIZ")`.
- The node's SYLOGIN.COM;1 and ;3 carry the same line.

`DIRECTORY SYS$MANAGER:SYLOGIN*.*` on the node lists SYLOGIN.COM;3, ;2, ;1 and
SYLOGIN.TEMPLATE;1, all in SYS$COMMON:[SYSMGR].

On a terminal that does not answer, such as a serial console with nothing behind it, the
inquiry waits about 4 s for each of its three identification requests. Keystroke goldens
LOGIN.BANNER P and TA.LOGIN W show this timing. Anything typed during the wait is used up
by the inquiry. Test harnesses therefore wait for the first DCL prompt after login before
typing.
