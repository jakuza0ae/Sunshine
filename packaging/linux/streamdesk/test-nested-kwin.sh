#!/usr/bin/env bash
set -euo pipefail

usage='Usage: test-nested-kwin.sh /absolute/path/to/install-root'
[[ $# == 1 && -x "$1/bin/sunshine" ]] || { printf '%s\n' "$usage" >&2; exit 2; }
prefix=$(cd "$1" && pwd)
script_dir=$(cd "$(dirname "$0")" && pwd)
client=${STREAMDESK_PROBE_CLIENT:-}
animated=${STREAMDESK_TEST_ANIMATED:-0}
desktop_pairing=${STREAMDESK_TEST_KDE_PAIRING:-0}
command -v wireplumber >/dev/null || { echo "The isolated test requires WirePlumber 0.5 or newer" >&2; exit 2; }
if [[ "$animated" == 1 ]]; then
    [[ -n "$client" ]] || { echo "The animated test requires STREAMDESK_PROBE_CLIENT" >&2; exit 2; }
    command -v ffmpeg >/dev/null
fi
if [[ "$desktop_pairing" == 1 ]]; then
    [[ -n "$client" ]] || { echo "The KDE pairing test requires STREAMDESK_PROBE_CLIENT" >&2; exit 2; }
    python3 -c 'from gi.repository import Gio' || exit 2
fi
test_timeout=55
if [[ -n "$client" ]]; then
    test_timeout=170
    export STREAMDESK_PROBE_CLIENT="$(realpath -e "$client")"
    client=$STREAMDESK_PROBE_CLIENT
    [[ -x "$client" ]]
    command -v ffprobe >/dev/null
fi
if [[ ${STREAMDESK_NESTED_TEST:-0} != 1 ]]; then
    runtime="${XDG_RUNTIME_DIR:-/run/user/$UID}"
    exec timeout --kill-after=4s "${test_timeout}s" bwrap --die-with-parent --unshare-net --dev-bind / / \
        --tmpfs "$runtime" env STREAMDESK_NESTED_TEST=1 dbus-run-session -- "$0" "$prefix"
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

export NO_PROXY=127.0.0.1,localhost no_proxy=127.0.0.1,localhost
export XDG_RUNTIME_DIR="$scratch/run" XDG_CONFIG_HOME="$scratch/config"
export XDG_CACHE_HOME="$scratch/cache" XDG_DATA_HOME="$scratch/data"
export PIPEWIRE_RUNTIME_DIR="$XDG_RUNTIME_DIR"
export XDG_STATE_HOME="$scratch/state"
export DBUS_SYSTEM_BUS_ADDRESS="unix:path=$scratch/no-system-bus"
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
wireplumber --profile=policy >"$scratch/wireplumber.log" 2>&1 &
pids+=("$!")
QT_QPA_PLATFORM=offscreen kwin_wayland --virtual --socket wayland-streamdesk-test \
    --width 1280 --height 960 --no-lockscreen --no-global-shortcuts >"$scratch/kwin.log" 2>&1 &
pids+=("$!")
export WAYLAND_DISPLAY=wayland-streamdesk-test

for ((n=0; n<40; n++)); do
    if [[ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" && -S "$XDG_RUNTIME_DIR/pipewire-0" ]]; then break; fi
    sleep .2
done
QT_QPA_PLATFORM=wayland krfb-virtualmonitor --desktopfile org.kde.krfb.virtualmonitor \
    --name StreamDesk --resolution 1440x1080 --scale 1 --port 45901 --password isolated-test >"$scratch/krfb.log" 2>&1 &
pids+=("$!")
display=Virtual-StreamDesk
for ((n=0; n<40; n++)); do
    wayland-info >"$scratch/wayland.log"
    if grep -Eq "^[[:space:]]+name: $display$" "$scratch/wayland.log"; then break; fi
    sleep .2
done
grep -Eq "^[[:space:]]+name: $display$" "$scratch/wayland.log"
grep -Eq '^[[:space:]]+name: Virtual-0$' "$scratch/wayland.log"
if [[ "$animated" == 1 ]]; then
    GDK_BACKEND=wayland GTK_A11Y=none LD_PRELOAD=libgtk4-layer-shell.so.0 nice -n 12 \
        python3 "$script_dir/test-motion-pattern.py" >"$scratch/pattern.log" 2>&1 &
    pids+=("$!")
    for ((n=0; n<35; n++)); do
        if grep -q 'Pattern ready Virtual-StreamDesk' "$scratch/pattern.log"; then break; fi
        sleep .2
    done
    grep -q 'Pattern ready Virtual-StreamDesk' "$scratch/pattern.log"
fi

cat > "$scratch/base.conf" <<CONF
bind_address = 127.0.0.1
upnp = disabled
port = 48289
sunshine_name = StreamDesk nested probe
capture = kwin
encoder = vaapi
kwin_require_output = enabled
stream_audio = disabled
fec_percentage = 20
min_log_level = debug
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
unset 'pids[-1]'
grep -q 'Requested output is unavailable.*Expected-absent-output' "$scratch/absent.log"
! grep -q 'Screencasting output name' "$scratch/absent.log"
printf 'Verified refusal to capture another screen when the configured output is missing\n'

cp "$scratch/base.conf" "$scratch/pass.conf"
printf 'output_name = %s\n' "$display" >> "$scratch/pass.conf"
if [[ -n "$client" ]]; then
    cp "$script_dir/apps.json" "$scratch/apps.json"
    printf 'file_apps = %s\n' "$scratch/apps.json" >> "$scratch/pass.conf"
    ./bin/sunshine "$scratch/pass.conf" --creds streamdesk-test isolated-test-password >"$scratch/setup.log" 2>&1
fi
if [[ "$desktop_pairing" == 1 ]]; then
    python3 "$script_dir/test-notification-pairing.py" "$scratch" >"$scratch/notification.log" 2>&1 &
    pids+=("$!")
    for ((n=0; n<35; n++)); do
        [[ -f "$scratch/notification-ready" ]] && break
        sleep .1
    done
    [[ -f "$scratch/notification-ready" ]]
fi
STREAMDESK_KDE_PAIRING="$desktop_pairing" ./bin/sunshine "$scratch/pass.conf" >"$scratch/found.log" 2>&1 &
pids+=("$!")
for ((n=0; n<115; n++)); do
    if grep -q "Found H.264 encoder" "$scratch/found.log" && curl --noproxy '*' --max-time 1 -fsS http://127.0.0.1:48289/serverinfo >"$scratch/serverinfo.xml" 2>/dev/null; then break; fi
    sleep .25
done
grep -Fq "Screencasting output name $display" "$scratch/found.log"
grep -Fq 'Found H.264 encoder' "$scratch/found.log"
grep -Fq 'PipeWire stream state: paused -> streaming' "$scratch/found.log"
grep -q 'status_code="200"' "$scratch/serverinfo.xml"
printf 'Verified named KWin capture, H.264 and an isolated GameStream endpoint\n'
if [[ -n "$client" ]]; then
    keydir="$scratch/moonlight-keys"
    mkdir "$keydir" "$scratch/client-home"
    HOME="$scratch/client-home" "$client" pair -pin 4321 -keydir "$keydir" -port 48289 127.0.0.1 >"$scratch/pair.log" 2>&1 &
    pair=$!
    pids+=("$pair")
    if [[ "$desktop_pairing" == 1 ]]; then
        for ((n=0; n<50; n++)); do
            [[ -f "$scratch/notification-accepted" ]] && break
            sleep .2
        done
        [[ -f "$scratch/notification-accepted" ]]
        printf 'Verified inline pairing notification, ignored wrong ID, and retried invalid PIN input\n'
    else
        api=(curl --noproxy '*' -kfsS --max-time 2 -u streamdesk-test:isolated-test-password)
        pairid=""
        for ((n=0; n<35; n++)); do
            pairid=$("${api[@]}" https://127.0.0.1:48290/api/pin | jq -r '.pairings[0].id // empty')
            [[ -n "$pairid" ]] && break
            sleep .2
        done
        [[ "$pairid" =~ ^[0-9a-f]{32}$ ]]
        jq -nc --arg id "$pairid" '{pairing_id:$id,pin:"4321",name:"Headless regression"}' |
            "${api[@]}" -H 'Content-Type: application/json' --data-binary @- https://127.0.0.1:48290/api/pin | jq -e '.status' >/dev/null
    fi
    wait "$pair"
    unset 'pids[-1]'

    for size in 1024x768 1280x960 1440x1080; do
        case "$size" in
            1024x768) bitrate=4000; video_bitrate=2880 ;;
            1280x960) bitrate=6000; video_bitrate=4320 ;;
            1440x1080) bitrate=8000; video_bitrate=5900 ;;
        esac
        outfile="$scratch/video-$size.h264"
        status=0
        HOME="$scratch/client-home" STREAMDESK_BENCHMARK=1 STREAMDESK_VIDEO_PROBE="$outfile" \
            timeout --signal=INT --kill-after=3s 12s \
            stdbuf -oL -eL "$client" stream -verbose -width "${size%x*}" -height "${size#*x}" -fps 30 -bitrate "$bitrate" \
            -app "StreamDesk Display" -codec h264 -platform fake -viewonly -nosops -quitappafter \
            -keydir "$keydir" -port 48289 127.0.0.1 >"$scratch/stream-$size.log" 2>&1 || status=$?
        [[ "$status" == 0 || "$status" == 124 ]]
        if ! grep -Fq "Final adjusted video encoding bitrate is $video_bitrate Kbps" "$scratch/found.log"; then
            printf 'Unexpected video bitrate with audio disabled for %s\n' "$size" >&2
            exit 1
        fi
        ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=codec_name,width,height,nb_read_frames \
            -of json "$outfile" |
            jq -e --arg size "$size" '.streams[0] | select(.codec_name=="h264" and "\(.width)x\(.height)"==$size and (.nb_read_frames|tonumber)>=30)'
        if [[ "$animated" == 1 ]]; then
            ffmpeg -v error -threads 1 -i "$outfile" -threads 1 -f framemd5 - |
                awk -F, '!/^#/ && NF >= 6 {hashes[$NF]=1}
                    END {print "Distinct decoded frame hashes:", length(hashes); exit !(length(hashes)>30)}'
            awk '/^PROFILE_SAMPLE / && $3>0 {count++}
                END {print "Frames with host processing time:", count+0; exit !(count>=30)}' "$scratch/stream-$size.log"
        fi
    done
fi
touch "$scratch/success"
