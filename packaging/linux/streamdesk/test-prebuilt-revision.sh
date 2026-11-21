#!/usr/bin/env bash
set -euo pipefail

files=$(cd "$(dirname "$0")" && pwd)
module="$files/../../../cmake/prep/build_version.cmake"
scratch=$(mktemp -d)
trap 'python3 -c "import shutil,sys; shutil.rmtree(sys.argv[1])" "$scratch"' EXIT

for repo in source caller; do
    mkdir "$scratch/$repo"
    git -C "$scratch/$repo" init -q -b "$repo"
    printf '%s\n' "$repo" > "$scratch/$repo/marker"
    git -C "$scratch/$repo" add marker
    git -C "$scratch/$repo" -c user.name='StreamDesk test' -c user.email='test@example.invalid' commit -qm "$repo"
done

cat > "$scratch/source/CMakeLists.txt" <<CMAKE
cmake_minimum_required(VERSION 3.25)
project(VersionCheck VERSION 1.2.3 LANGUAGES NONE)
include("$module")
file(WRITE "\${CMAKE_BINARY_DIR}/revision" "\${GITHUB_COMMIT}\n\${PROJECT_VERSION}\n")
CMAKE
source_commit=$(git -C "$scratch/source" rev-parse HEAD)
source_short=$(git -C "$scratch/source" rev-parse --short HEAD)
cd "$scratch/caller"
configure_check() {
    env -u BRANCH -u BUILD_VERSION -u COMMIT cmake -S "$scratch/source" -B "$scratch/build" \
        > "$scratch/cmake.log" 2>&1 || { cat "$scratch/cmake.log" >&2; return 1; }
    diff -u <(printf '%s\n' "$source_commit" "1.2.3-$source_short$1") "$scratch/build/revision"
}
configure_check ''
printf '%s\n' changed > "$scratch/source/marker"
git -C "$scratch/source" add marker
configure_check '-dirty'

commit=$(git -C "$files" rev-parse HEAD)
stage="$scratch/stage"
archive="$scratch/StreamDesk.tar.gz"
mkdir -p "$stage/bin" "$stage/share/streamdesk/web"
touch "$stage/share/streamdesk/web/index.html"
write_version() {
    printf '#!/bin/sh\ntest -f share/streamdesk/web/index.html || exit 1\nprintf "%%s\\n" "Sunshine version: %s commit: %s"\n' \
        "$1" "$2" > "$stage/bin/sunshine"
    chmod +x "$stage/bin/sunshine"
}
expect_rejected() {
    if bash "$files/pack-prebuilt.sh" "$stage" "$archive" > "$scratch/pack.log" 2>&1; then
        printf '%s\n' 'Packager accepted a mismatched or dirty binary' >&2
        return 1
    fi
    [[ ! -e "$archive" ]]
}

write_version '1.2.3' "$(git rev-parse HEAD)"
expect_rejected
write_version '1.2.3-dirty' "$commit"
expect_rejected
write_version '1.2.3' "$commit"
bash "$files/pack-prebuilt.sh" "$stage" "$archive"
diff -u <(printf '%s\n' "${commit:0:16}") <(tar -xOf "$archive" StreamDesk-Host/build-revision)
