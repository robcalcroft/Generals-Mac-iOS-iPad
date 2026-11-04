#!/usr/bin/env bash
# GeneralsX @build Claude 04/11/2026 One command: bootstrap toolchain -> build -> .app.
#
# Usage: make-app-zh.sh [--game-data DIR] [--out DIR] [--name NAME] [--skip-build]
#   --game-data   your retail Zero Hour folder (default: ~/GeneralsX/GeneralsZH)
#   --out         output folder for the .app (default: <repo>/dist)
#   --skip-build  repackage the existing build without recompiling
# Needs only Xcode Command Line Tools; everything else lands in <repo>/.toolchain.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
TOOL="${ROOT}/.toolchain"
PKG_ARGS=()
SKIP_BUILD=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --game-data|--out|--name) PKG_ARGS+=("$1" "$2"); shift 2 ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        -h|--help) sed -n 2,9p "$0"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done

mkdir -p "${ROOT}/logs"

"${SCRIPT_DIR}/bootstrap-toolchain.sh"

# Hermetic environment: our tools first, Homebrew deliberately NOT on PATH.
# (GX_KEEP_PATH=1 appends the caller's PATH for debugging.)
ORIG_PATH="${PATH}"
export VCPKG_ROOT="${TOOL}/vcpkg"
export VCPKG_DISABLE_METRICS=1
export VULKAN_SDK="${TOOL}/vulkan"
unset VULKAN_SDK_ROOT PKG_CONFIG_PATH
PATH="${TOOL}/venv/bin:${TOOL}/bin:${VULKAN_SDK}/bin:/usr/bin:/bin:/usr/sbin:/sbin"
[[ "${GX_KEEP_PATH:-0}" == "1" ]] && PATH="${PATH}:${ORIG_PATH:-}"
export PATH

cd "${ROOT}"
if [[ $SKIP_BUILD -eq 0 ]]; then
    echo; echo "==> Configure (macos-vulkan)"
    # DEBUG off: debug options break replay determinism (AGENTS.md).
    cmake --preset macos-vulkan -DRTS_BUILD_OPTION_DEBUG=OFF 2>&1 | tee logs/make_app_configure.log

    echo; echo "==> Build (z_generals)"
    cmake --build build/macos-vulkan --target z_generals -j "$(sysctl -n hw.logicalcpu)" 2>&1 \
        | tee logs/make_app_build.log
fi

"${SCRIPT_DIR}/package-app-zh.sh" ${PKG_ARGS[@]+"${PKG_ARGS[@]}"}
