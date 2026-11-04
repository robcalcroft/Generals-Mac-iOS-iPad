#!/usr/bin/env bash
# GeneralsX @build Claude 04/11/2026 Assemble a self-contained, relocatable macOS .app.
#
# Usage: package-app-zh.sh [--game-data DIR] [--build-dir DIR] [--out DIR] [--name NAME]
#
# Layout produced (nothing outside the bundle is needed at runtime):
#   Contents/MacOS/GeneralsXZH        real binary, rpath @executable_path/../Frameworks
#   Contents/Frameworks/*.dylib       every non-system dependency, @rpath install names
#   Contents/Resources/               icon, dxvk.conf, MoltenVK ICD, fontconfig
#   Contents/Resources/GameData/      retail data (APFS-cloned from --game-data)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"

GAME_DATA="${GX_GAME_DATA:-${HOME}/GeneralsX/GeneralsZH}"
BUILD_DIR="${PROJECT_ROOT}/build/macos-vulkan"
OUT_DIR="${PROJECT_ROOT}/dist"
APP_NAME="Generals Zero Hour"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --game-data) GAME_DATA="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --out)       OUT_DIR="$2"; shift 2 ;;
        --name)      APP_NAME="$2"; shift 2 ;;
        -h|--help)   sed -n 2,12p "$0"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done

die() { echo "ERROR: $*" >&2; exit 1; }

[[ "$(uname -s)" == "Darwin" ]] || die "macOS only"
[[ "$(uname -m)" == "arm64" ]] || die "Apple Silicon only (arm64)"

BINARY="${BUILD_DIR}/GeneralsMD/GeneralsXZH"
[[ -x "${BINARY}" ]] || die "binary not found: ${BINARY} (build first)"
[[ -d "${GAME_DATA}" ]] || die "game data folder not found: ${GAME_DATA} (use --game-data)"
for req in INIZH.big W3DZH.big TexturesZH.big; do
    [[ -f "${GAME_DATA}/${req}" ]] || die "${GAME_DATA}/${req} missing - is this a Zero Hour install?"
done
[[ -d "${GAME_DATA}/ZH_Generals" ]] || die "${GAME_DATA}/ZH_Generals missing - Zero Hour needs the base-game data"

APP="${OUT_DIR}/${APP_NAME}.app"
STAGE="${APP}.partial"
CONTENTS="${STAGE}/Contents"
MACOS_DIR="${CONTENTS}/MacOS"
FW_DIR="${CONTENTS}/Frameworks"
RES_DIR="${CONTENTS}/Resources"

rm -rf "${STAGE}"
mkdir -p "${MACOS_DIR}" "${FW_DIR}" "${RES_DIR}"

# ---------------------------------------------------------------------------
# Mach-O helpers
# ---------------------------------------------------------------------------
# Keep only the arm64 slice of universal (fat) libraries such as the Vulkan SDK's.
thin_arm64() {
    if lipo -info "$1" 2>/dev/null | grep -q '^Architectures in the fat file'; then
        lipo "$1" -thin arm64 -output "$1.thin" && mv "$1.thin" "$1"
    fi
}

is_system_dep() { [[ "$1" == /System/* || "$1" == /usr/lib/* ]]; }

# Non-system dependency install names of a Mach-O (skips the file's own id).
deps_of() {
    local self=""
    [[ "$1" == *.dylib ]] && self="$(otool -D "$1" 2>/dev/null | tail -1)"
    otool -L "$1" 2>/dev/null | grep -E '^[[:space:]]' | sed -E 's/^[[:space:]]+//; s/ \(compatibility version.*$//' | while IFS= read -r d; do
        [[ "$d" == "$self" ]] && continue
        is_system_dep "$d" || echo "$d"
    done
}

rpaths_of() {
    otool -l "$1" 2>/dev/null | awk '
        $1 == "cmd" && $2 == "LC_RPATH" { in_rp = 1; next }
        in_rp && $1 == "path" { sub(/^ *path /, ""); sub(/ \(offset [0-9]+\)$/, ""); print; in_rp = 0 }'
}

# Directories searched when a dependency is an @rpath/... reference.
SEARCH_DIRS=("${BUILD_DIR}" "${BUILD_DIR}/GeneralsMD")
for d in "${BUILD_DIR}"/_deps/*-build "${BUILD_DIR}"/_deps/dxvk-build-macos/src/d3d8 \
         "${BUILD_DIR}"/_deps/dxvk-build-macos/src/d3d9; do
    [[ -d "$d" ]] && SEARCH_DIRS+=("$d")
done
for v in "${VULKAN_SDK:-}" "${VULKAN_SDK_ROOT:-}" "${PROJECT_ROOT}/.toolchain/vulkan"; do
    [[ -n "$v" && -d "$v/lib" ]] && SEARCH_DIRS+=("$v/lib")
done
for d in "${PROJECT_ROOT}"/.toolchain/vcpkg/installed/arm64-osx/lib "${BUILD_DIR}"/vcpkg_installed/arm64-osx/lib; do
    [[ -d "$d" ]] && SEARCH_DIRS+=("$d")
done

# Resolve a dependency reference to a real file on disk, or print nothing.
resolve_dep() {
    local from="$1" dep="$2" base cand rp
    base="$(basename "$dep")"
    case "$dep" in
        /*) [[ -f "$dep" ]] && { echo "$dep"; return 0; } ;;
        @loader_path/*)
            cand="$(dirname "$from")/${dep#@loader_path/}"
            [[ -f "$cand" ]] && { echo "$cand"; return 0; } ;;
    esac
    # @rpath/@executable_path/missing absolute: try the file's own rpaths, then search dirs
    while IFS= read -r rp; do
        [[ -z "$rp" ]] && continue
        rp="${rp/@loader_path/$(dirname "$from")}"
        rp="${rp/@executable_path/$(dirname "$BINARY")}"
        [[ -f "${rp}/${base}" ]] && { echo "${rp}/${base}"; return 0; }
    done < <(rpaths_of "$from")
    for cand in "${SEARCH_DIRS[@]}"; do
        [[ -f "${cand}/${base}" ]] && { echo "${cand}/${base}"; return 0; }
    done
    return 1
}

COLLECTED=()   # basenames already copied into Frameworks
in_collected() { local x; for x in ${COLLECTED[@]+"${COLLECTED[@]}"}; do [[ "$x" == "$1" ]] && return 0; done; return 1; }

# Copy a library into Frameworks under the basename its dependents ask for,
# then recurse into its own dependencies.
collect() {
    local from="$1" dep real base
    while IFS= read -r dep; do
        [[ -z "$dep" ]] && continue
        base="$(basename "$dep")"
        in_collected "$base" && continue
        real="$(resolve_dep "$from" "$dep" || true)"
        [[ -n "$real" ]] || die "cannot resolve dependency '${dep}' of ${from}"
        cp -X "$real" "${FW_DIR}/${base}"
        chmod u+w "${FW_DIR}/${base}"
        thin_arm64 "${FW_DIR}/${base}"
        COLLECTED+=("$base")
        echo "    + ${base}  (${real})"
        collect "${FW_DIR}/${base}"
    done < <(deps_of "$from")
}

# ---------------------------------------------------------------------------
echo "==> Executable"
cp -X "${BINARY}" "${MACOS_DIR}/GeneralsXZH"
chmod u+w "${MACOS_DIR}/GeneralsXZH"

echo "==> Collecting libraries"
collect "${MACOS_DIR}/GeneralsXZH"

# Libraries the game dlopen()s at runtime rather than links against.
add_runtime_lib() {  # <basename-to-install-as> <candidate paths...>
    local name="$1"; shift
    local cand
    in_collected "$name" && return 0
    for cand in "$@"; do
        if [[ -f "$cand" ]]; then
            cp -X "$cand" "${FW_DIR}/${name}"; chmod u+w "${FW_DIR}/${name}"; thin_arm64 "${FW_DIR}/${name}"
            COLLECTED+=("$name"); echo "    + ${name}  (${cand})  [runtime]"
            collect "${FW_DIR}/${name}"
            return 0
        fi
    done
    die "runtime library ${name} not found"
}
D8_DIR="${BUILD_DIR}/_deps/dxvk-build-macos/src/d3d8"
D9_DIR="${BUILD_DIR}/_deps/dxvk-build-macos/src/d3d9"
add_runtime_lib libdxvk_d3d8.0.dylib "${BUILD_DIR}/libdxvk_d3d8.0.dylib" "${D8_DIR}/libdxvk_d3d8.0.dylib"
add_runtime_lib libdxvk_d3d9.0.dylib "${BUILD_DIR}/libdxvk_d3d9.0.dylib" "${D9_DIR}/libdxvk_d3d9.0.dylib"
MVK_CANDIDATES=()
for v in "${VULKAN_SDK:-}" "${VULKAN_SDK_ROOT:-}" "${PROJECT_ROOT}/.toolchain/vulkan"; do
    [[ -n "$v" ]] && MVK_CANDIDATES+=("$v/lib/libMoltenVK.dylib")
done
for v in "${HOME}"/VulkanSDK/*/macOS/lib/libMoltenVK.dylib; do MVK_CANDIDATES+=("$v"); done
add_runtime_lib libMoltenVK.dylib ${MVK_CANDIDATES[@]+"${MVK_CANDIDATES[@]}"}
# DXVK dlopens the loader by bare name; having it already loaded (the game links
# it) satisfies that, but make sure it ships either way.
add_runtime_lib libvulkan.1.dylib "${FW_DIR}/libvulkan.1.dylib" ${MVK_CANDIDATES[@]+"${MVK_CANDIDATES[@]//libMoltenVK/libvulkan.1}"}

# ---------------------------------------------------------------------------
echo "==> Making the bundle relocatable"
# install_name_tool warns that it invalidates signatures; everything is re-signed below.
install_name_tool() { command install_name_tool "$@" 2> >(grep -v 'invalidate the code signature' >&2); }

set_rpaths() {  # <file> <new rpath>
    local f="$1" new="$2" rp
    while IFS= read -r rp; do
        [[ -z "$rp" ]] && continue
        install_name_tool -delete_rpath "$rp" "$f" 2>/dev/null || true
    done < <(rpaths_of "$f")
    install_name_tool -add_rpath "$new" "$f"
}

rewrite_deps() {  # <file>
    local f="$1" dep base
    while IFS= read -r dep; do
        [[ -z "$dep" ]] && continue
        base="$(basename "$dep")"
        if in_collected "$base"; then
            [[ "$dep" == "@rpath/${base}" ]] || install_name_tool -change "$dep" "@rpath/${base}" "$f"
        fi
    done < <(deps_of "$f")
}

set_rpaths "${MACOS_DIR}/GeneralsXZH" "@executable_path/../Frameworks"
rewrite_deps "${MACOS_DIR}/GeneralsXZH"
for lib in "${FW_DIR}"/*.dylib; do
    install_name_tool -id "@rpath/$(basename "$lib")" "$lib"
    set_rpaths "$lib" "@loader_path"
    rewrite_deps "$lib"
done

# ---------------------------------------------------------------------------
echo "==> Resources"
DXVK_CONF="${PROJECT_ROOT}/resources/dxvk/dxvk.conf"
[[ -f "${DXVK_CONF}" ]] || die "missing ${DXVK_CONF}"
cp -X "${DXVK_CONF}" "${RES_DIR}/dxvk.conf"

cat > "${RES_DIR}/MoltenVK_icd.json" <<'JSON'
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "../Frameworks/libMoltenVK.dylib",
        "api_version": "1.4.0",
        "is_portability_driver": true
    }
}
JSON

FONTCONFIG_ETC=""
for d in "${GX_FONTCONFIG_DIR:-}" "${BUILD_DIR}/vcpkg_installed/arm64-osx/etc/fonts" \
         "${PROJECT_ROOT}/.toolchain/vcpkg/installed/arm64-osx/etc/fonts" \
         "${PROJECT_ROOT}/vcpkg_installed/arm64-osx/etc/fonts"; do
    [[ -n "$d" && -f "$d/fonts.conf" ]] && { FONTCONFIG_ETC="$d"; break; }
done
[[ -n "${FONTCONFIG_ETC}" ]] || die "fontconfig config not found (set GX_FONTCONFIG_DIR)"
mkdir -p "${RES_DIR}/fontconfig"
cp -X "${FONTCONFIG_ETC}/fonts.conf" "${RES_DIR}/fontconfig/fonts.conf"
[[ -d "${FONTCONFIG_ETC}/conf.d" ]] && cp -RX "${FONTCONFIG_ETC}/conf.d" "${RES_DIR}/fontconfig/conf.d"
# fontconfig resolves its default cache dir relative to the executable (Contents/var/cache),
# which would write into - and break the signature of - the bundle. Point it at the user's
# Caches folder instead (fontconfig expands a leading ~).
python3 -I - "${RES_DIR}/fontconfig/fonts.conf" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p).read()
s = re.sub(r'[ \t]*<cachedir[^>]*>[^<]*</cachedir>[ \t]*\n?', '', s)
s = s.replace('</fontconfig>', '\t<cachedir>~/Library/Caches/GeneralsX/fontconfig</cachedir>\n</fontconfig>')
open(p, 'w').write(s)
PY

ICON_SRC="${PROJECT_ROOT}/assets/generalsx-zh_icon.png"
if [[ -f "${ICON_SRC}" ]]; then
    ICONSET="$(mktemp -d)/AppIcon.iconset"; mkdir -p "${ICONSET}"
    for s in 16 32 128 256 512; do
        sips -z $s $s "${ICON_SRC}" --out "${ICONSET}/icon_${s}x${s}.png" >/dev/null
        sips -z $((s*2)) $((s*2)) "${ICON_SRC}" --out "${ICONSET}/icon_${s}x${s}@2x.png" >/dev/null
    done
    iconutil -c icns "${ICONSET}" -o "${RES_DIR}/AppIcon.icns"
fi

COMMIT="$(git -C "${PROJECT_ROOT}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
git -C "${PROJECT_ROOT}" diff --quiet 2>/dev/null || COMMIT="${COMMIT}+dirty"
MINOS="$(otool -l "${MACOS_DIR}/GeneralsXZH" | awk '/LC_BUILD_VERSION/ {f=1} f && $1=="minos" {print $2; exit}')"
sed -e "s|@APP_NAME@|${APP_NAME}|g" -e "s|@COMMIT@|${COMMIT}|g" -e "s|@MINOS@|${MINOS:-15.0}|g" \
    "${PROJECT_ROOT}/resources/macos/Info.plist.in" > "${CONTENTS}/Info.plist"
printf 'APPL????' > "${CONTENTS}/PkgInfo"
plutil -lint "${CONTENTS}/Info.plist" >/dev/null

# ---------------------------------------------------------------------------
echo "==> Game data (${GAME_DATA})"
GD="${RES_DIR}/GameData"
mkdir -p "${GD}"
# APFS clone if possible (zero extra disk); otherwise a normal copy.
CP_FLAGS="-R"
probe_src="$(find "${GAME_DATA}" -maxdepth 1 -name '*.big' -print -quit)"
if cp -c "${probe_src}" "${GD}/.clone-probe" 2>/dev/null; then
    CP_FLAGS="-cR"; rm -f "${GD}/.clone-probe"; echo "    using APFS clones (no extra disk space)"
else
    rm -f "${GD}/.clone-probe"
    need_kb=$(du -sk "${GAME_DATA}" | cut -f1)
    avail_kb=$(df -k "${OUT_DIR%/*}" 2>/dev/null | awk 'NR==2 {print $4}' || echo 0)
    echo "    clones unavailable on this volume - copying ($((need_kb/1024)) MB)"
    [[ "${avail_kb:-0}" -gt "${need_kb}" ]] || die "not enough free disk space for game data"
fi

EXCLUDES=()
while IFS= read -r line; do
    [[ -z "$line" || "$line" == \#* ]] || EXCLUDES+=("$line")
done < "${SCRIPT_DIR}/../common/gamedata-excludes.txt"

shopt -s nullglob dotglob
for entry in "${GAME_DATA}"/*; do
    name="$(basename "$entry")"
    skip=0
    for pat in "${EXCLUDES[@]}"; do
        # shellcheck disable=SC2254
        case "$name" in $pat) skip=1; break ;; esac
    done
    [[ $skip -eq 1 ]] && continue
    cp ${CP_FLAGS} "$entry" "${GD}/"
done
shopt -u nullglob dotglob

EXTRAS_WND="${PROJECT_ROOT}/GeneralsZH/Data/Window/Menus/ExtrasMenu.wnd"
if [[ -f "${EXTRAS_WND}" ]]; then
    mkdir -p "${GD}/Window/Menus"
    cp -X "${EXTRAS_WND}" "${GD}/Window/Menus/ExtrasMenu.wnd"
fi
echo "    bundled $(du -sh "${GD}" | cut -f1) (logical size)"

# ---------------------------------------------------------------------------
echo "==> Verifying relocatability"
bad=0
check_macho() {
    local f="$1" d rp
    while IFS= read -r d; do
        [[ -z "$d" ]] && continue
        case "$d" in
            @rpath/*|@executable_path/*|@loader_path/*) ;;
            *) echo "  BAD dep   ${f#${STAGE}/}: $d"; bad=1 ;;
        esac
    done < <(deps_of "$f")
    while IFS= read -r rp; do
        [[ -z "$rp" ]] && continue
        case "$rp" in
            @executable_path/*|@loader_path*) ;;
            *) echo "  BAD rpath ${f#${STAGE}/}: $rp"; bad=1 ;;
        esac
    done < <(rpaths_of "$f")
    # every @rpath dep must exist in Frameworks
    while IFS= read -r d; do
        case "$d" in @rpath/*) [[ -f "${FW_DIR}/$(basename "$d")" ]] || { echo "  MISSING   ${f#${STAGE}/} -> $d"; bad=1; } ;; esac
    done < <(deps_of "$f")
}
check_macho "${MACOS_DIR}/GeneralsXZH"
for lib in "${FW_DIR}"/*.dylib; do check_macho "$lib"; done
[[ $bad -eq 0 ]] || die "bundle still references locations outside itself"
echo "    ok: $(ls "${FW_DIR}" | wc -l | tr -d ' ') libraries, all self-contained"

# ---------------------------------------------------------------------------
echo "==> Signing (ad-hoc)"
find "${STAGE}" -name '._*' -delete 2>/dev/null || true
xattr -cr "${STAGE}" 2>/dev/null || true
for lib in "${FW_DIR}"/*.dylib; do codesign --force --sign - --timestamp=none "$lib" 2>&1 | grep -v "replacing existing signature" || true; done
codesign --force --sign - --timestamp=none "${MACOS_DIR}/GeneralsXZH"
codesign --force --sign - --timestamp=none "${STAGE}"
codesign --verify --deep --strict "${STAGE}"

mkdir -p "${OUT_DIR}"
rm -rf "${APP}"
mv "${STAGE}" "${APP}"
echo
echo "Done: ${APP}"
