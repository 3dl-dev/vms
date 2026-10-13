import ftplib,sys,os
host=sys.argv[1]; ftp=ftplib.FTP(host,timeout=60); ftp.login('SYSTEM','system')
for p in sys.argv[2:]:
    loc,_,rem=p.partition('=')
    with open(loc,'rb') as f: ftp.storlines('STOR '+(rem or os.path.basename(loc)), f)
    print('put',loc,'->',rem or os.path.basename(loc))
ftp.quit()
