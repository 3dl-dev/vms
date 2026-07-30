#!/bin/bash
# Clean-room evidence retention — snapshot this session's RE artifacts into the durable
# dated archive and refresh the SHA-256 chain-of-custody manifests.
# Run at the end of each VMScluster RE session (see PROVENANCE.md §6).
#
#   Usage: docs/clean-room/retain.sh [session-id]
#
# Retains: session transcripts (reasoning trail), derivation scripts (methodology),
# OVMX test captures + SCSD logs (what OVMX emitted), and re-hashes the reference
# captures (the legitimate observational source). Then updates the manifests in this
# directory so `git commit` carries the timestamped, tamper-evident record.
set -u
SID="${1:-$(date +%Y%m%d-%H%M%S)}"
DATE="$(date +%Y-%m-%d)"
REPO_CR="$(cd "$(dirname "$0")" && pwd)"
ARCH=~/vax/clean-room-archive/"$DATE-session-$SID"
JT_GLOB=/home/baron/.claude/jobs/*/tmp
TX=~/.claude/projects/-home-baron-projects-vms

mkdir -p "$ARCH"/{derivation-scripts,ovmx-test-captures,session-transcripts,scsd-logs}
echo "[retain] archiving to $ARCH"

cp $JT_GLOB/*.py            "$ARCH/derivation-scripts/"  2>/dev/null
cp $JT_GLOB/*.pcap          "$ARCH/ovmx-test-captures/"  2>/dev/null
cp $JT_GLOB/scsd*.log $JT_GLOB/*.status "$ARCH/scsd-logs/" 2>/dev/null
cp /tmp/clean-vax1-test/scsd*.log "$ARCH/scsd-logs/"     2>/dev/null
cp "$TX"/*.jsonl            "$ARCH/session-transcripts/"  2>/dev/null

# hash the legitimate source (reference captures) + the full archive
( cd ~/vax && sha256sum cluster/captures/*.pcap clean-cluster/captures/*.pcap 2>/dev/null ) > "$ARCH/reference-captures.sha256"
( cd "$ARCH" && find derivation-scripts ovmx-test-captures session-transcripts scsd-logs -type f 2>/dev/null | sort | xargs sha256sum ) > "$ARCH/MANIFEST.sha256"

# refresh the git-tracked manifests (the tamper-evident chain of custody)
cp "$ARCH/reference-captures.sha256" "$REPO_CR/reference-captures.sha256"
cp "$ARCH/MANIFEST.sha256"           "$REPO_CR/archive-manifest.sha256"

echo "[retain] reference captures: $(wc -l < "$ARCH/reference-captures.sha256")"
echo "[retain] archive files:      $(wc -l < "$ARCH/MANIFEST.sha256")"
echo "[retain] archive size:       $(du -sh "$ARCH" | cut -f1)"
echo "[retain] DONE. Review + commit docs/clean-room/*.sha256 to anchor the timestamp in git."
