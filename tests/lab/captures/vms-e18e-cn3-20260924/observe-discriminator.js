// Discriminator run (rd vms-e18e): is the real VAX's stall caused by the CPU
// cost of a third emulator, or by a third CLUSTER participant? The third node
// is booted with a group-1 CLUSTER_AUTHORIZE instead of group 257 -- identical
// image, identical CPU cost, identical NIC traffic volume, but every frame it
// sends is addressed to another cluster's multicast address and carries another
// cluster's number at abs 22, so VAXC and OVMXB must ignore all of it.
// Also records each console's LENGTH every 15 s, so a machine that has stopped
// emulating can be told apart from one whose PEDRIVER has wedged.
const { chromium } = require('playwright');
const fs = require('fs');
const PORT=8110, NB=process.env.NODE_B, NC=process.env.NODE_C, OUT=process.env.OUT_DIR||'/out/obs4';
const INITRAMFS=process.env.INITRAMFS||'initramfs-ovmx-nodeA.cpio.gz';
const RUN_MS=+(process.env.RUN_MS||1500000), STAGGER=+(process.env.STAGGER_MS||180000);
const ORDER=(process.env.BOOT_ORDER||'C,B,A').split(',').map(s=>s.trim());
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
const log=(...a)=>console.log(`[${new Date().toISOString().slice(11,19)}]`,...a);
function pcjs(p,s){return p.frames().find(f=>f!==p.mainFrame()&&f.url().split('?')[0].endsWith('/ovmx-cluster.html')&&f.url().includes(s));}
function nodeA(p){return p.frames().find(f=>f!==p.mainFrame()&&f.url().split('?')[0].endsWith('/node.html'));}
async function con(p,w){
  if(w==='OVMXA'){const f=nodeA(p);return f?f.evaluate(()=>(window.__nodeState&&window.__nodeState.consoleText)||'').catch(()=>''):'';}
  const f=pcjs(p,w==='OVMXB'?'0B':'0C');return f?f.evaluate(()=>document.getElementById('screen').textContent).catch(()=>''):'';
}
async function st(p,w){
  if(w==='OVMXA'){const f=nodeA(p);return f?f.evaluate(()=>{const s=window.__nodeState||{};return{tx:s.nicTxCount,rx:s.nicRxCount,err:s.workerError||null,halt:s.halt||null};}).catch(()=>null):null;}
  const f=pcjs(p,w==='OVMXB'?'0B':'0C');
  return f?f.evaluate(()=>{const s=window.__clusterNode||{};return{tx:s.nicTxCount,rx:s.nicRxCount,err:s.err||null,booted:s.booted};}).catch(()=>null):null;
}
(async()=>{
  fs.mkdirSync(OUT,{recursive:true});
  const b=await chromium.launch({headless:true,args:['--no-sandbox']});
  const p=await b.newPage();
  p.on('pageerror',e=>log('[pageerr]',e.message));
  const u=`http://localhost:${PORT}/index.html?mac=52%3A54%3A00%3A00%3A00%3A0A&initramfs=${encodeURIComponent(INITRAMFS)}&sysdisk=sysdisk-nodeA.qcow2.gz&nodeB=${encodeURIComponent(NB)}&nodeC=${encodeURIComponent(NC)}`;
  log('goto',u);
  await p.goto(u,{waitUntil:'load'});
  await p.waitForFunction(()=>window.__demoReady===true,{timeout:60000});
  const btn={A:'#bootbtn-a',B:'#bootbtn-b',C:'#bootbtn-c'};
  const t0=Date.now(); let next=0; const rows=[];
  while(Date.now()-t0<RUN_MS){
    const el=Date.now()-t0;
    while(next<ORDER.length && el>=next*STAGGER){ const n=ORDER[next++]; log('boot',n); await p.click(btn[n]).catch(e=>log('click fail',e.message)); }
    const hub=await p.evaluate(()=>{const h=window.__hubframes||[];const s={};for(const f of h)if(f.ethertype===0x6007)s[f.port]=(s[f.port]||0)+1;return s;}).catch(()=>({}));
    const S={}, L={};
    for(const w of ['OVMXA','OVMXB','VAXC']){ S[w]=await st(p,w); const c=await con(p,w); L[w]=c.length; try{fs.writeFileSync(`${OUT}/${w}.console.log`,c);}catch(e){} }
    const row={t:Math.round(el/1000),hub,st:S,conlen:L};
    rows.push(row); log(`t=${row.t}s hub=${JSON.stringify(hub)} st=${JSON.stringify(S)} conlen=${JSON.stringify(L)}`);
    fs.writeFileSync(`${OUT}/timeline.json`,JSON.stringify(rows,null,1));
    await sleep(15000);
  }
  await p.screenshot({path:`${OUT}/final.png`,fullPage:true}).catch(()=>{});
  await b.close();
})();
