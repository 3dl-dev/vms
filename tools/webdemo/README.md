# tools/webdemo — auto-track the latest release on the in-browser demo

When an OVMX release is published, the demo at **openvmx.3dl.dev** should become
that release, with no hand-copying. This directory is the machinery that makes it
happen, driven by `.github/workflows/deploy-web-demo.yml`
(`on: release: published`).

## The pipeline

```
OVMX release published
  ├─ download the release's OWN web assets:
  │     vmlinuz · initramfs-ovmx-slim.cpio.gz · ovmx-distrib.img
  │     (published by tools/publish-release.sh — the release IS the source)
  ├─ download OUR qemu-wasm binary from 3dl-dev/qemu-wasm's latest Release
  │     (built by that fork's build-wasm workflow; not the borrowed upstream one)
  ├─ CAPTURE the boot snapshot in CI:
  │     headless Chromium boots ovmx-distrib.img under our qemu-wasm and
  │     `savevm`s the pre-booted 'ovmx' snapshot (tools/webdemo/capture.js).
  │     This is why the demo resumes in ~seconds instead of a ~70s cold boot.
  └─ push the rebuilt payload (wasm + kernel + slim initramfs + snapshot) to
        3dl-dev/openvmx-site/boot/, cache-busted so clients fetch the new set.
```

The site's *page* source (index.html, module.js, coi-serviceworker.js, xterm
assets, load-rom.\*) stays in openvmx-site; the workflow only regenerates the
**binary** payload, so the two never drift.

## Files

| File | Role |
|---|---|
| `harness/index.html`, `harness/module.js` | minimal page that boots the guest FRESH and exposes `__ovmxTerm`/`__ovmxMod` so the snapshot can be captured. Not the shipped page. |
| `coi-server.js` | static server that sets COOP/COEP so qemu-wasm gets SharedArrayBuffer under headless Chromium. |
| `capture.js` | drives the boot → `savevm ovmx` → exports the snapshot qcow2. |

**The harness `module.js` machine shape (`-M pc -m 256M`, qcow2 `if=virtio`) MUST
match the deployed page's `module.js`**, or the captured snapshot won't resume in
the shipped demo.

## One-time setup

1. **`3dl-dev/qemu-wasm` publishes a Release** carrying
   `qemu-system-x86_64.wasm`, `out.js`, `qemu-system-x86_64.worker.js` (flip the
   fork's `build-wasm` workflow to `softprops/action-gh-release` instead of
   upload-artifact).
2. **Repo secret `DEPLOY_TOKEN`** (Settings → Secrets → Actions) — a fine-grained
   PAT with:
   - `contents: read` on `3dl-dev/qemu-wasm` (download our binary)
   - `contents: write` on `3dl-dev/openvmx-site` (push the rebuilt demo)

That's the entire manual surface. After it, every published release re-deploys
the demo automatically. Re-run on demand with the `workflow_dispatch` (pass the
release tag).

## Gate

Do **not** enable this until our qemu-wasm binary is confirmed to fix the
intermittent MTTCG halt (vms-e33). Automating a halting demo just ships the halt
faster. Fix first, then let releases drive it.
