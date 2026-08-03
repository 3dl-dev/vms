import sys,struct
sys.path.insert(0,'/home/baron/vax/cluster/work')
from pcap import frames, mac, is6007
path,node,peer=sys.argv[1],sys.argv[2].lower(),sys.argv[3].lower()
u16=lambda p,o: struct.unpack('<H',p[o:o+2])[0]
fs=frames(path)
out_last=None; in_last=None; peer_max_ss=0; my_max_ss=0
print(f'{"idx":>5} {"dir":3} {"mt":4} {"op":>3} {"declLen":>7} {"wire":>4} {"inner":>5} {"ss":>4} {"ra":>4}  note')
for i,(ts,p) in enumerate(fs):
    if not is6007(p) or len(p)<62: continue
    if p[30] not in (0x5b,0x4b): continue
    src=mac(p[6:12]); dst=mac(p[0:6])
    if not ((src==node and dst==peer) or (src==peer and dst==node)): continue
    d='OUT' if src==node else 'IN'
    ss=u16(p,34); ra=u16(p,32); op=u16(p,60)
    decl=u16(p,14); inner=u16(p,56)
    note=''
    if decl+2 != len(p)-14: note+=f' *LEN-MISMATCH decl+2={decl+2} payload={len(p)-14}*'
    if d=='OUT':
        if out_last is not None and ss!=out_last+1: note+=f' *SS-GAP prev={out_last}*'
        out_last=ss
        if ra!=peer_max_ss: note+=f' *RA={ra} but peer max ss={peer_max_ss}*'
    else:
        if in_last is not None and ss!=in_last+1: note+=f' *peer SS-GAP prev={in_last}*'
        in_last=ss; peer_max_ss=ss
    print(f'{i:5d} {d:3} 0x{p[30]:02x} {op:3d} {decl:7d} {len(p)-14:4d} {inner:5d} {ss:4d} {ra:4d} {note}')
