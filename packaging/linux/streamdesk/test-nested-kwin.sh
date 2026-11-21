#!/usr/bin/env bash
set -euo pipefail

usage='Usage: test-nested-kwin.sh /absolute/path/to/install-root'
[[ $# == 1 && -x "$1/bin/sunshine" ]] || { printf '%s\n' "$usage" >&2; exit 2; }
prefix=$(cd "$1" && pwd)
if [[ ${STREAMDESK_NESTED_TEST:-0} != 1 ]]; then
    exec timeout --kill-after=4s 55s bwrap --die-with-parent --unshare-net --dev-bind / / \
        env STREAMDESK_NESTED_TEST=1 dbus-run-session -- "$0" "$prefix"
fi

scratch=$(mktemp -d)
pids=()
cleanup() {
    for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
    wait || true
    if [[ ! -f "$scratch/success" ]]; then
        for log in "$scratch/"*.log; do
            if [[ -f "$log" ]]; then printf '\n%s\n' "$log"; tail -20 "$log"; fi
        done
    fi
    python3 -c 'import shutil,sys; shutil.rmtree(sys.argv[1])' "$scratch"
}
trap cleanup EXIT
trap 'exit 1' TERM INT

export XDG_RUNTIME_DIR="$scratch/run" XDG_CONFIG_HOME="$scratch/config"
export XDG_CACHE_HOME="$scratch/cache" XDG_DATA_HOME="$scratch/data"
export PIPEWIRE_RUNTIME_DIR="$XDG_RUNTIME_DIR"
unset DISPLAY WAYLAND_DISPLAY PIPEWIRE_REMOTE
install -d -m700 "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME/sunshine" "$XDG_DATA_HOME/applications"
cat > "$XDG_DATA_HOME/applications/dev.lizardbyte.app.Sunshine.kwin.test.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Nested test capture permission
Exec="$prefix/bin/sunshine"
X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1
NoDisplay=true
DESKTOP

pipewire >"$scratch/pipewire.log" 2>&1 &
pids+=("$!")
QT_QPA_PLATFORM=offscreen kwin_wayland --virtual --socket wayland-streamdesk-test \
    --width 1280 --height 960 --no-lockscreen --no-global-shortcuts >"$scratch/kwin.log" 2>&1 &
pids+=("$!")
export WAYLAND_DISPLAY=wayland-streamdesk-test

for ((n=0; n<40; n++)); do
    if [[ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" && -S "$XDG_RUNTIME_DIR/pipewire-0" ]]; then break; fi
    sleep .2
done
wayland-info >"$scratch/wayland.log"
display=$(awk '/^[[:space:]]+name: / { if ($2 ~ /Virtual/) {print $2; exit} }' "$scratch/wayland.log")
[[ -n "$display" ]]

cat > "$scratch/base.conf" <<CONF
bind_address = 127.0.0.1
upnp = disabled
port = 48289
capture = kwin
kwin_require_output = enabled
stream_audio = disabled
controller = disabled
hevc_mode = 1
av1_mode = 1
CONF

cd "$prefix"
cp "$scratch/base.conf" "$scratch/fail.conf"
printf 'output_name = Expected-absent-output\n' >> "$scratch/fail.conf"
./bin/sunshine "$scratch/fail.conf" >"$scratch/absent.log" 2>&1 &
absent=$!
pids+=("$absent")
for ((n=0; n<40; n++)); do
    if grep -q 'Requested output is unavailable.*Expected-absent-output' "$scratch/absent.log"; then break; fi
    if ! kill -0 "$absent" 2>/dev/null; then break; fi
    sleep .25
done
kill "$absent" 2>/dev/null || true
wait "$absent" || true
pids=("${pids[@]:0:2}")
grep -q 'Requested output is unavailable.*Expected-absent-output' "$scratch/absent.log"
! grep -q 'Screencasting output name' "$scratch/absent.log"
printf 'Verified refusal to capture another screen when the configured output is missing\n'

cp "$scratch/base.conf" "$scratch/pass.conf"
printf 'output_name = %s\n' "$display" >> "$scratch/pass.conf"
./bin/sunshine "$scratch/pass.conf" >"$scratch/found.log" 2>&1 &
pids+=("$!")
for ((n=0; n<50; n++)); do
    if curl --noproxy '*' --max-time 1 -fsS http://127.0.0.1:48289/serverinfo >"$scratch/serverinfo.xml" 2>/dev/null; then break; fi
    sleep .25
done
grep -Fq "Screencasting output name $display" "$scratch/found.log"
grep -Fq 'Found H.264 encoder' "$scratch/found.log"
grep -q 'status_code="200"' "$scratch/serverinfo.xml"
printf 'Verified named KWin capture, H.264 and an isolated GameStream endpoint\n'
touch "$scratch/success"
