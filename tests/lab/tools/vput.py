# vput.py -- runs INSIDE a lab pod: FTP text files to a real VAX node's
# SYS$LOGIN as SYSTEM (the lab test account). Used by stage_rmhold.sh.
#   python3 vput.py <vax-ip> <local>[=<remote>] ...
import ftplib, os, sys

host = sys.argv[1]
ftp = ftplib.FTP(host, timeout=60)
ftp.login('SYSTEM', 'system')
for p in sys.argv[2:]:
    loc, _, rem = p.partition('=')
    with open(loc, 'rb') as f:
        ftp.storlines('STOR ' + (rem or os.path.basename(loc)), f)
    print('put', loc, '->', rem or os.path.basename(loc))
ftp.quit()
