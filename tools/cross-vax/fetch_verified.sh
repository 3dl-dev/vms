#!/bin/sh
# fetch_verified.sh ALGO SUM OUTFILE URL [URL...]   (ALGO: sha256 | sha512)
#
# Download OUTFILE from the first mirror that yields bytes matching the PINNED
# checksum. Origins (ftp.gnu.org, archive.netbsd.org) intermittently time out or
# truncate long transfers -- sometimes with a clean EOF, which only the checksum
# can see -- so a single `wget || wget' is a flake source. Each round walks every
# URL, RESUMING any partial file (curl -C -, which never truncates it on an error
# status) and verifying; a full walk without a verified file discards it, backs
# off, and starts a new round. Integrity is enforced in every case; exhausting the
# rounds is a hard failure, never a silent pass.
set -u
algo=$1 sum=$2 out=$3; shift 3
rounds=${FETCH_ROUNDS:-6}
size() { wc -c < "$out" 2>/dev/null || echo 0; }
r=1
while [ "$r" -le "$rounds" ]; do
  before=$(size)
  for url in "$@"; do
    curl -fsSL -C - --retry 2 --connect-timeout 30 --max-time 900 -o "$out" "$url" || true
    if [ -s "$out" ] && echo "$sum  $out" | "${algo}sum" -c - >/dev/null 2>&1; then
      echo "fetch_verified: $out OK from $url (round $r)"
      exit 0
    fi
    echo "fetch_verified: $url did not yield a verified $out (round $r)" >&2
  done
  # Keep a partial that GREW this round (truncation: the next round resumes it);
  # drop one that did not (corrupt/oversized: resuming cannot repair it).
  [ "$(size)" -gt "$before" ] || rm -f "$out"
  sleep "${FETCH_BACKOFF:-$((r * 10))}"
  r=$((r + 1))
done
rm -f "$out"
echo "fetch_verified: FAILED to obtain a verified $out after $rounds rounds" >&2
exit 1
