#!/usr/bin/env bash
# deploy-vax-demo.sh — AWS-free deploy of a built VAX demo disk to the live browser demo.
#
# This is the DEPLOY half of the per-release VAX browser-demo refresh. It removes the
# manual gzip → commit → push → repoint → verify toil I did by hand for V0.5-7.
#
#   Pipeline overview (two halves, different owners):
#     BUILD half  (VAX lane): tools/k3s/run-on-rail.sh runs sysboot-single at the tag on
#                 k3s-worker (needs 32Gi + the emulator + the ovmx-vax-buildcache PVC —
#                 it will NOT fit a GitHub cloud runner, so a track-release.yml-style
#                 cloud mirror is impossible). It drops the raw disk at:
#                     .boot-cache/lab-vax/demo-swap/ovmx-vax-<tag>.img
#     DEPLOY half (this script): everything below. Same-origin GitHub Pages, ZERO AWS.
#
#   A full auto-refresh = a trigger (workshop cron or a self-hosted runner with kubectl +
#   internet + git creds) that runs the BUILD half then this. That scheduler/creds piece
#   is infra/operator-scope; this script is the deterministic deploy core it would call.
#
# Usage:  deploy-vax-demo.sh <tag> [raw-disk-path]
#   <tag>            e.g. V0.5-8  (case-insensitive; the gz/URL use the lowercased form)
#   [raw-disk-path]  default: .boot-cache/lab-vax/demo-swap/ovmx-vax-<tag>.img
#
# Requires: node + playwright-core (the $-gate verifier), git push creds for both repos,
#           a checkout of pcjs (baron-3dl/pcjs) and openvmx-site (3dl-dev/openvmx-site).
#
# It is fail-fast and GATED: it will not push a disk that does not drive to a real DCL $
# in a real headless browser (local gate before the pcjs push, live gate after deploy).

set -euo pipefail

TAG="${1:?usage: deploy-vax-demo.sh <tag> [raw-disk-path]}"
TAGLC="${TAG,,}"                                   # V0.5-8 -> v0.5-8
VMS="${OVMX_VMS_DIR:-/home/baron/projects/vms}"
PCJS="${OVMX_PCJS_DIR:-/home/baron/projects/pcjs}"
SITE="${OVMX_SITE_DIR:-/home/baron/projects/openvmx-site}"
DISK="${2:-$VMS/.boot-cache/lab-vax/demo-swap/ovmx-vax-${TAG}.img}"
PW="${OVMX_PW_DIR:-/home/baron/.npm/_npx/db89d7302a373f10/node_modules/playwright-core}"
VERIFY="${OVMX_VERIFY_MJS:-$VMS/tests/lab-vax/verify-login-demo.mjs}"
BROWSERDIR="machines/dec/vax/browser"
GZNAME="ovmx-vax-${TAGLC}.img.gz"
PAGES_GZ="https://vax.3dl.network/$BROWSERDIR/$GZNAME"
PORT="${OVMX_DEPLOY_PORT:-8207}"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"; jobs -p | xargs -r kill 2>/dev/null || true' EXIT
say(){ echo "[deploy-vax-demo $(date +%H:%M:%S)] $*"; }
die(){ echo "[deploy-vax-demo] FATAL: $*" >&2; exit 1; }

# ---- 1. verify the raw disk -------------------------------------------------
[[ -f "$DISK" ]] || die "raw disk not found: $DISK (has the k3s build dropped it?)"
SZ=$(stat -c%s "$DISK")
(( SZ % 512 == 0 )) || die "disk is not a whole multiple of 512 bytes ($SZ)"
say "raw disk: $DISK ($SZ bytes = $((SZ/512)) sectors)  sha256=$(sha256sum "$DISK" | cut -c1-16)…"

# ---- 2. gzip (must fit GitHub Pages' 100MB file limit) ----------------------
GZ="$WORK/$GZNAME"
say "gzipping → $GZNAME"
gzip -9 -c "$DISK" > "$GZ"
GZSZ=$(stat -c%s "$GZ")
(( GZSZ < 100*1024*1024 )) || die "gz is ${GZSZ}B (>100MB Pages limit) — split into parts + concat in-browser"
say "gz: $GZSZ bytes ($((GZSZ/1024/1024)) MB) — under the 100MB Pages limit"

# ---- 3. LOCAL gate: does it drive to $ ? (before touching production) -------
say "local \$-gate: serving pcjs master + the new gz, driving login to \$ in headless Chrome…"
git -C "$PCJS" fetch origin master -q
git -C "$PCJS" worktree add --detach "$WORK/pcjs" origin/master >/dev/null 2>&1
grep -q WANT_LOGIN "$WORK/pcjs/$BROWSERDIR/ovmx.html" || die "master's ovmx.html lacks the login self-drive (?drive=login)"
cp "$GZ" "$WORK/pcjs/$BROWSERDIR/$GZNAME"
( cd "$WORK/pcjs" && python3 -m http.server "$PORT" --bind 127.0.0.1 >/dev/null 2>&1 ) &
SRV=$!; sleep 2
B="http://localhost:$PORT"
PW="$PW" SITE_BASE="$B" \
  ROM_URL="$B/$BROWSERDIR/ka655x.bin" \
  DISK_GZ_URL="$B/$BROWSERDIR/$GZNAME" \
  node "$VERIFY" || die "LOCAL \$-gate FAILED — the $TAG disk does not drive to \$ (wrong disk/creds). NOT deploying."
kill "$SRV" 2>/dev/null || true
say "local \$-gate PASSED ✓"

# ---- 4. deploy the gz to pcjs master (GitHub Pages, same-origin) ------------
say "committing $GZNAME to pcjs master + pushing"
git -C "$WORK/pcjs" add "$BROWSERDIR/$GZNAME"
git -C "$WORK/pcjs" commit -q -m "vax demo: $TAG disk (ovmx-vax-${TAGLC}.img.gz, AWS-free Pages)

Built fresh from the $TAG tag; boots + logs in to a real DCL \$ (local \$-gate passed).
sha256 of raw disk: $(sha256sum "$DISK" | cut -d' ' -f1)"
git -C "$WORK/pcjs" push origin HEAD:master
say "waiting for Pages to serve $GZNAME…"
for i in $(seq 1 30); do
  [[ "$(curl -s -o /dev/null -w '%{http_code}' "$PAGES_GZ?cb=$RANDOM")" == 200 ]] && { say "gz live on Pages"; break; }
  (( i == 30 )) && die "Pages did not serve $GZNAME after ~6min"
  sleep 12
done

# ---- 5. repoint the homepage iframe to the new gz --------------------------
say "repointing homepage ?diskgz= → $GZNAME (drive=login already present)"
git -C "$SITE" fetch origin main -q
git -C "$SITE" worktree add --detach "$WORK/site" origin/main >/dev/null 2>&1
IDX="$WORK/site/index.html"
grep -q 'ovmx-vax-v0\.[0-9]' "$IDX" || die "could not find the VAX iframe diskgz line in index.html"
sed -i -E "s#ovmx-vax-v0\.[0-9]+(-[0-9]+)?\.img\.gz#$GZNAME#g" "$IDX"
grep -q "$GZNAME" "$IDX" || die "sed did not repoint the iframe to $GZNAME"
git -C "$WORK/site" add index.html
git -C "$WORK/site" commit -q -m "vax pane: track $TAG — repoint ?diskgz= to $GZNAME"
git -C "$WORK/site" push origin HEAD:main
say "waiting for the homepage deploy…"
for i in $(seq 1 25); do
  [[ "$(curl -s "https://openvmx.3dl.dev/?cb=$RANDOM" | grep -c "$GZNAME")" -ge 1 ]] && { say "homepage repointed live"; break; }
  (( i == 25 )) && die "homepage did not pick up $GZNAME after ~5min"
  sleep 12
done

# ---- 6. LIVE gate: confirm the pane drives to $ on the real site ------------
say "live \$-gate: driving the live VAX pane to \$ (AWS-free)…"
cat > "$WORK/live.mjs" <<'MJS'
const pkg = (await import(process.env.PW_INDEX)).default; const { chromium } = pkg;
const b=await chromium.launch({headless:true}); const p=await (await b.newContext()).newPage();
const cf=[]; p.on('request',r=>{const u=r.url(); if(u.includes('cloudfront')||u.includes('amazonaws'))cf.push(u);});
for(let i=0;i<3;i++){await p.goto('https://openvmx.3dl.dev/',{waitUntil:'load'});await p.waitForTimeout(2500);}
await p.evaluate(()=>document.querySelector('#bootbtn-vax')?.click());
let ok=false; for(let i=0;i<160;i++){await p.waitForTimeout(2000);
  const fr=p.frames().find(f=>f.url().includes('vax.3dl.network')); if(!fr)continue;
  try{if(await fr.evaluate(()=>window.ovmxReachedDollar?.()||false)){ok=true;break;}}catch{}}
console.log(ok&&cf.length===0?'LIVE-PASS':'LIVE-FAIL cf='+cf.length);
await b.close(); process.exit(ok&&cf.length===0?0:1);
MJS
PW_INDEX="file://$PW/index.js" node "$WORK/live.mjs" || die "LIVE \$-gate FAILED — the deployed pane did not drive to \$ (or hit CloudFront)."
say "live \$-gate PASSED ✓ — VAX pane boots + logs in to \$ on $TAG, AWS-free"

git -C "$PCJS" worktree remove "$WORK/pcjs" --force 2>/dev/null || true
git -C "$SITE" worktree remove "$WORK/site" --force 2>/dev/null || true
say "DONE. VAX browser demo is live on $TAG (login → \$), served from GitHub Pages, zero AWS."
