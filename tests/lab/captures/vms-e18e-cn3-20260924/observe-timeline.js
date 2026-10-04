// cn3 timeline observation (rd vms-e18e). Polls from t=0 -- the earlier pass
// only started counting after every node had been clicked, so it could not say
// WHEN a node stopped emitting. Records, every 15 s: per-port SCA counts at the
// in-page hub, each pcjs machine's own nicTx counter (a machine that has
// stopped running stops incrementing it), and every console.
const { chromium } = require('playwright');
const fs = require('fs');
const PORT=8110, NB=process.env.NODE_B, NC=process.env.NODE_C, OUT=process.env.OUT_DIR||'/out/obs3';
const RUN_MS=+(process.env.RUN_MS||1500000), STAGGER=+(process.env.STAGGER_MS||150000);
const ORDER=(process.env.BOOT_ORDER||'C,B,A').split(',').map(s=>s.trim());
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
const log=(...a)=>console.log(`[${new Date().toISOString().slice(11,19)}]`,...a);
function pcjs(p,s){return p.frames().find(f=>f!==p.mainFrame()&&f.url().split('?')[0].endsWith('/ovmx-cluster.html')&&f.url().includes(s));}
function nodeA(p){return p.frames().find(f=>f!==p.mainFrame()&&f.url().split('?')[0].endsWith('/node.html'));}
async function con(p,w){
  if(w==='OVMXA'){const f=nodeA(p);return f?f.evaluate(()=>(window.__nodeState&&window.__nodeState.consoleText)||'').catch(()=>''):'';}
  const f=pcjs(p,w==='OVMXB'?'0B':'0C');return f?f.evaluate(()=>document.getElementById('screen').textContent).catch(()=>''):'';
}
async function machineTx(p,w){
  if(w==='OVMXA'){const f=nodeA(p);return f?f.evaluate(()=>(window.__nodeState||{}).nicTxCount??null).catch(()=>null):null;}
  const f=pcjs(p,w==='OVMXB'?'0B':'0C');
  return f?f.evaluate(()=>(window.__clusterNode||{}).nicTxCount??null).catch(()=>null):null;
}
(async()=>{
  fs.mkdirSync(OUT,{recursive:true});
  const b=await chromium.launch({headless:true,args:['--no-sandbox']});
  const p=await b.newPage();
  p.on('pageerror',e=>log('[pageerr]',e.message));
  const u=`http://localhost:${PORT}/index.html?mac=52%3A54%3A00%3A00%3A00%3A0A&initramfs=initramfs-ovmx-nodeA.cpio.gz&sysdisk=sysdisk-nodeA.qcow2.gz&nodeB=${encodeURIComponent(NB)}&nodeC=${encodeURIComponent(NC)}`;
  await p.goto(u,{waitUntil:'load'});
  await p.waitForFunction(()=>window.__demoReady===true,{timeout:60000});
  const btn={A:'#bootbtn-a',B:'#bootbtn-b',C:'#bootbtn-c'};
  const t0=Date.now(); let next=0;
  const rows=[];
  while(Date.now()-t0<RUN_MS){
    const el=Date.now()-t0;
    while(next<ORDER.length && el>=next*STAGGER){ const n=ORDER[next++]; log('boot',n); await p.click(btn[n]).catch(e=>log('click fail',e.message)); }
    const hub=await p.evaluate(()=>{const h=window.__hubframes||[];const s={};for(const f of h)if(f.ethertype===0x6007)s[f.port]=(s[f.port]||0)+1;return s;}).catch(()=>({}));
    const tx={}; for(const w of ['OVMXA','OVMXB','VAXC']) tx[w]=await machineTx(p,w);
    const row={t:Math.round(el/1000),hub,tx};
    rows.push(row); log(`t=${row.t}s hub=${JSON.stringify(hub)} machineTx=${JSON.stringify(tx)}`);
    for(const w of ['OVMXA','OVMXB','VAXC']){try{fs.writeFileSync(`${OUT}/${w}.console.log`,await con(p,w));}catch(e){}}
    fs.writeFileSync(`${OUT}/timeline.json`,JSON.stringify(rows,null,1));
    await sleep(15000);
  }
  await p.screenshot({path:`${OUT}/final.png`,fullPage:true}).catch(()=>{});
  await b.close();
})();
