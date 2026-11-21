#!/usr/bin/env bash
set -euo pipefail

[[ $# == 2 ]] || { printf '%s\n' 'Usage: pack-prebuilt.sh CMAKE_INSTALLED_ROOT BUNDLE.tar.gz' >&2; exit 2; }
files=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$1" && pwd)
archive=$(realpath -m "$2")
commit=$(git -C "$files" rev-parse HEAD)
revision=${commit:0:16}
[[ -x "$root/bin/sunshine" && -f "$root/share/streamdesk/web/index.html" ]]
scratch=$(mktemp -d)
trap 'python3 -c "import shutil,sys; shutil.rmtree(sys.argv[1])" "$scratch"' EXIT
if ! build_info=$(XDG_CONFIG_HOME="$scratch/config" timeout 5s "$root/bin/sunshine" --version 2>&1) ||
    [[ "$build_info"$'\n' != *" commit: $commit"$'\n'* || "$build_info" == *"-dirty commit: $commit"* ]]; then
    printf 'Expected a clean Sunshine build from commit %s. Reconfigure and rebuild before packaging.\n' "$commit" >&2
    exit 1
fi
bundle="$scratch/StreamDesk-Host"
install -d "$bundle/payload/bin" "$bundle/payload/share"
install -m755 "$root/bin/sunshine" "$bundle/payload/bin/"
cp -a "$root/share/streamdesk" "$bundle/payload/share/"
install -m755 "$files/install.sh" "$files/streamdesk-host" "$bundle/"
install -m644 "$files/"*.service "$files/sunshine.conf" "$files/apps.json" "$files/README.md" "$bundle/"
install -m644 "$files/../../../LICENSE" "$bundle/LICENSE"
printf '%s\n' "$revision" > "$bundle/build-revision"
uname -m > "$bundle/build-architecture"
(
    cd "$bundle"
    find payload -type f -print0 | sort -z | xargs -0 sha256sum > payload.sha256
)
tar -czf "$archive" -C "$scratch" StreamDesk-Host
printf 'Created %s\n' "$archive"
