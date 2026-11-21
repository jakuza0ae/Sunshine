#!/usr/bin/env bash
set -euo pipefail
umask 022

[[ "$(uname -s)" == Linux && $EUID != 0 ]] || {
    printf '%s\n' 'Run as your regular desktop user on Linux' >&2
    exit 1
}
files=$(cd "$(dirname "$0")" && pwd)
data="${XDG_DATA_HOME:-$HOME/.local/share}/streamdesk-host"
config="${XDG_CONFIG_HOME:-$HOME/.config}"
jobs=${STREAMDESK_BUILD_JOBS:-2}
prebuilt=0
programs=(bwrap krfb-virtualmonitor kscreen-doctor jq systemctl flock python3)
if [[ -d "$files/payload" ]]; then
    prebuilt=1
    read -r revision < "$files/build-revision"
    [[ "$revision" =~ ^[0-9a-f]{16}$ ]]
    [[ "$(uname -m)" == "$(cat "$files/build-architecture")" ]] || {
        printf 'This package requires %s\n' "$(cat "$files/build-architecture")" >&2
        exit 1
    }
    programs+=(sha256sum ldd)
else
    src=$(cd "$files/../../.." && pwd)
    revision=$(git -C "$src" rev-parse --short=16 HEAD)
    build=${STREAMDESK_BUILD_DIR:-"$src/cmake-build-streamdesk"}
    programs+=(cmake git ninja npm pkg-config)
fi

for program in "${programs[@]}"; do
    command -v "$program" >/dev/null || { printf 'Required command missing: %s\n' "$program" >&2; exit 1; }
done

install -d "$data/releases"
exec 9>"$data/install.lock"
flock -n 9 || { printf '%s\n' 'StreamDesk installation is already running' >&2; exit 1; }

staging=$(mktemp -d "$data/staging.XXXXXXXX")
managed=()
publishing=0
cleanup() {
    local status=$? index destination backup
    trap - EXIT
    trap '' INT TERM
    if (( publishing )); then
        printf '%s\n' 'Restoring the previous StreamDesk installation' >&2
        for index in "${!managed[@]}"; do
            destination=${managed[index]}
            backup="$staging/backup/$index"
            if [[ -e "$backup" || -L "$backup" ]]; then
                cp -aT --remove-destination "$backup" "$destination" ||
                    printf 'Could not restore %s\n' "$destination" >&2
            elif [[ -e "$destination" || -L "$destination" ]]; then
                unlink "$destination" || printf 'Could not remove %s\n' "$destination" >&2
            fi
        done
        systemctl --user daemon-reload ||
            printf '%s\n' 'Could not reload systemd after restoring the files' >&2
    fi
    python3 -c 'import shutil,sys; shutil.rmtree(sys.argv[1])' "$staging" ||
        printf 'Could not clean temporary installation files in %s\n' "$staging" >&2
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# Keep any running StreamDesk process and its assets on its original release.
target="$data/releases/$revision"
if [[ ! -x "$target/bin/sunshine" ]]; then
    if (( prebuilt )); then
        (cd "$files" && sha256sum --quiet --strict -c payload.sha256)
        if ! dependencies=$(LC_ALL=C ldd "$files/payload/bin/sunshine" 2>&1) ||
            [[ "$dependencies" == *"not found"* ]]; then
            printf '%s\n' 'The binary needs Linux libraries unavailable on this system. Use the source installer.' >&2
            printf '%s\n' "$dependencies" | awk '/not found/' >&2
            exit 1
        fi
        cp -a "$files/payload" "$staging/usr"
    else
        git -C "$src" submodule update --init --depth 1 \
            third-party/build-deps third-party/glad third-party/libdisplaydevice third-party/libvirtualhid \
            third-party/lizardbyte-common third-party/moonlight-common-c third-party/Simple-Web-Server \
            third-party/plasma-wayland-protocols third-party/wayland-protocols third-party/wlr-protocols
        git -C "$src" submodule update --init --recursive --depth 1 third-party/libdisplaydevice third-party/libvirtualhid third-party/lizardbyte-common third-party/moonlight-common-c

        nice -n 15 cmake -S "$src" -B "$build" -G Ninja \
            -DCMAKE_CXX_SCAN_FOR_MODULES=OFF -DBUILD_DOCS=OFF -DBUILD_TESTS=OFF -DSUNSHINE_ENABLE_TRAY=OFF \
            -DSUNSHINE_ENABLE_CUDA=OFF -DSUNSHINE_ENABLE_VULKAN=OFF \
            -DSUNSHINE_SYSTEM_VULKAN_HEADERS=ON -DOPUS_USE_STATIC=OFF \
            -DCMAKE_INSTALL_PREFIX=/usr -DSUNSHINE_ASSETS_DIR=share/streamdesk \
            -DSUNSHINE_ASSETS_DIR_DEF=share/streamdesk -DSUNSHINE_PUBLISHER_NAME=StreamDesk \
            -DSUNSHINE_PUBLISHER_WEBSITE=https://github.com/jakuza0ae/Sunshine/tree/linux-streamdesk
        nice -n 15 cmake --build "$build" --parallel "$jobs"
        DESTDIR="$staging" cmake --install "$build"
        install -m755 "$files/streamdesk-usb-relay.py" "$staging/usr/bin/"
    fi
    [[ -x "$staging/usr/bin/sunshine" && -f "$staging/usr/share/streamdesk/web/index.html" ]]
    mv -T "$staging/usr" "$target"
fi

install -d "$HOME/.local/bin" "$config/systemd/user" \
    "$config/streamdesk-host/sunshine" "${XDG_DATA_HOME:-$HOME/.local/share}/applications"
for file in sunshine.conf apps.json; do
    if [[ ! -e "$config/streamdesk-host/sunshine/$file" ]]; then
        install -m600 "$files/$file" "$config/streamdesk-host/sunshine/$file"
    fi
done

permissions="${XDG_DATA_HOME:-$HOME/.local/share}/applications/dev.lizardbyte.app.Sunshine.kwin.streamdesk-$revision.desktop"
cat > "$staging/capture.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=StreamDesk capture permission
Exec="$target/bin/sunshine"
X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1
NoDisplay=true
DESKTOP
if command -v desktop-file-validate >/dev/null; then
    desktop-file-validate "$staging/capture.desktop"
fi

launcher="$HOME/.local/bin/streamdesk-host"
units=("$files/"*.service)
managed=("$data/current" "$launcher" "$permissions")
for unit in "${units[@]}"; do
    managed+=("$config/systemd/user/${unit##*/}")
done
install -d "$staging/backup"
for index in "${!managed[@]}"; do
    destination=${managed[index]}
    if [[ -d "$destination" && ! -L "$destination" ]]; then
        printf 'Expected a file or symlink at %s\n' "$destination" >&2
        exit 1
    fi
    if [[ -e "$destination" || -L "$destination" ]]; then
        cp -a "$destination" "$staging/backup/$index"
    fi
done

ln -s "$target" "$staging/current.next"
publishing=1
install -m755 -T "$files/streamdesk-host" "$launcher"
for unit in "${units[@]}"; do
    install -m644 -T "$unit" "$config/systemd/user/${unit##*/}"
done
install -m644 -T "$staging/capture.desktop" "$permissions"
systemctl --user daemon-reload
mv -Tf "$staging/current.next" "$data/current"
publishing=0
printf 'Installed %s\nStart when ready with %s/.local/bin/streamdesk-host start\n' "$revision" "$HOME"
