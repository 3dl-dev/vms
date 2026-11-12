#!/bin/sh
# vms-fb0b: test_keystroke_oracle.sh must remove every container it starts on
# every exit path. A fake `docker` keeps a live-container list: `run --name N`
# adds N and idles like a booting guest; `rm -f N` removes it; `ps` lists the
# live ones. The test kills the docker client (what ksplay does at the end of
# a run, and at each guest restart: the container survives that), TERMs the
# runner as a timeout would, and then
# asserts that no container with the runner's prefix is left.
set -u
ROOT=${1:-$(cd "$(dirname "$0")/../.." && pwd)}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir "$T/bin"
cat > "$T/bin/docker" <<'D'
#!/bin/sh
case "$1" in
run) n=$(echo "$*" | sed -n 's/.*--name \([^ ]*\) .*/\1/p')
     echo "$n" >> "$KS_LIVE"; echo "$n" >> "$KS_STARTED"
     echo $$ > "$KS_FAKE_PID"; exec sleep 30;;
ps) cat "$KS_LIVE";;
rm) for a; do [ "$a" = rm ] || [ "$a" = -f ] || sed -i "\\|^$a\$|d" "$KS_LIVE"; done;;
esac
exit 0
D
chmod +x "$T/bin/docker"
: > "$T/live"; : > "$T/started"
export KS_LIVE="$T/live" KS_STARTED="$T/started" KS_FAKE_PID="$T/pid"
PATH="$T/bin:$PATH" KS_CONTAINER_PREFIX=ovmx-kstest OVMX_BOOT_IMAGE=fake:none \
    setsid sh "$ROOT/tests/qemu/test_keystroke_oracle.sh" "$T/out" >/dev/null 2>&1 &
pid=$!
i=0
while [ $i -lt 100 ] && [ ! -s "$T/pid" ]; do sleep 0.1; i=$((i+1)); done
kill -KILL "$(cat "$T/pid")" 2>/dev/null
# a CI timeout / interrupt: TERM to the runner's whole process group
kill -TERM "-$pid" 2>/dev/null
wait $pid 2>/dev/null
[ -s "$T/started" ] || { echo "FAIL: runner never started a container"; exit 1; }
grep -qv '^ovmx-kstest-' "$T/started" && { echo "FAIL: container without the runner prefix"; exit 1; }
if [ -s "$T/live" ]; then echo "FAIL: containers left running: $(cat "$T/live")"; exit 1; fi
echo "PASS: no container with the oracle runner's prefix left behind"
