#!/usr/bin/env bash
# GeneralsX @build Claude 04/11/2026 Install every build-time dependency into
# <repo>/.toolchain so the only prerequisite on the Mac is Xcode Command Line Tools.
# Nothing is written to Homebrew, ~/VulkanSDK or ~/vcpkg; `rm -rf .toolchain` undoes it.
#
#   .toolchain/venv     cmake, ninja, meson (pinned, from PyPI)
#   .toolchain/vcpkg    vcpkg at the manifest's builtin-baseline + bootstrapped
#   .toolchain/vulkan   a minimal Vulkan "SDK": headers, libvulkan (vcpkg loader),
#                       libMoltenVK (pinned release), glslangValidator
#   .toolchain/bin      pkg-config (vcpkg's pkgconf)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
TOOL="${ROOT}/.toolchain"
DL="${TOOL}/downloads"

MVK_VERSION="v1.4.1"
MVK_SHA256="5ea0c259df7ded9a275444820f09cced54d6e5a7c7a31d262de62a5cdb7e15cf"

die() { echo "ERROR: $*" >&2; exit 1; }
step() { echo; echo "==> $*"; }

[[ "$(uname -s)" == "Darwin" && "$(uname -m)" == "arm64" ]] || die "Apple Silicon macOS only"
xcode-select -p >/dev/null 2>&1 || die "Xcode Command Line Tools missing - run: xcode-select --install"
for t in git curl tar shasum make clang; do
    command -v "$t" >/dev/null || die "'$t' not found (Xcode Command Line Tools incomplete?)"
done

PATH="/usr/bin:/bin:/usr/sbin:/sbin"   # deliberately ignore Homebrew & friends
mkdir -p "${TOOL}" "${DL}"

# ---------------------------------------------------------------- Python tools
step "Build tools (cmake, ninja, meson)"
REQ="${SCRIPT_DIR}/toolchain-requirements.txt"
REQ_HASH="$(shasum -a 256 "${REQ}" | cut -c1-16)"
if [[ -f "${TOOL}/.stamp-venv" && "$(cat "${TOOL}/.stamp-venv")" == "${REQ_HASH}" ]]; then
    echo "    up to date"
else
    [[ -x /usr/bin/python3 ]] || die "/usr/bin/python3 missing (Xcode Command Line Tools)"
    rm -rf "${TOOL}/venv"
    /usr/bin/python3 -m venv "${TOOL}/venv"
    "${TOOL}/venv/bin/python" -m pip install --quiet --upgrade pip
    "${TOOL}/venv/bin/python" -m pip install --quiet -r "${REQ}"
    echo "${REQ_HASH}" > "${TOOL}/.stamp-venv"
fi

# ----------------------------------------------------------------------- vcpkg
step "vcpkg"
# vcpkg checkout used for the whole build. vcpkg.json's builtin-baseline is older than the
# ffmpeg 8.1.1#2 override it pins, and vcpkg reads the version database from the checkout
# itself, so we pin a newer commit (the one this port was validated with). It must be a
# FULL clone: vcpkg pulls older port trees out of git history.
VCPKG_COMMIT="ddf0518c31a259dcc359fd4c7bcb1ce8a27e9855"
if [[ ! -x "${TOOL}/vcpkg/vcpkg" || "$(git -C "${TOOL}/vcpkg" rev-parse HEAD 2>/dev/null || true)" != "${VCPKG_COMMIT}" ]]; then
    rm -rf "${TOOL}/vcpkg"
    git clone -q https://github.com/microsoft/vcpkg.git "${TOOL}/vcpkg"
    git -C "${TOOL}/vcpkg" checkout -q "${VCPKG_COMMIT}"
    "${TOOL}/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
else
    echo "    up to date (${VCPKG_COMMIT:0:10})"
fi
export VCPKG_ROOT="${TOOL}/vcpkg"
export VCPKG_DISABLE_METRICS=1

# --------------------------------------------------- Vulkan loader + glslang + pkgconf
step "Vulkan loader, glslang, pkg-config (vcpkg, dynamic triplet)"
# Run from a directory OUTSIDE the repo: vcpkg searches upward for vcpkg.json and would
# switch to manifest mode (which rejects package names). This is a classic-mode install.
(cd "$(mktemp -d)" && "${TOOL}/vcpkg/vcpkg" install vulkan-loader "glslang[tools]" pkgconf \
    --triplet arm64-osx-dynamic \
    --overlay-triplets="${ROOT}/cmake/triplets" \
    --x-install-root="${TOOL}/vcpkg-tools" \
    --recurse --clean-after-build)

VI="${TOOL}/vcpkg-tools/arm64-osx-dynamic"
mkdir -p "${TOOL}/bin"
PKGCONF="$(find "${VI}/tools/pkgconf" -maxdepth 1 -type f -name 'pkgconf*' ! -name '*.exe' | head -1)"
[[ -x "${PKGCONF}" ]] || die "pkgconf tool not found under ${VI}/tools/pkgconf"
ln -sf "${PKGCONF}" "${TOOL}/bin/pkg-config"
ln -sf "${PKGCONF}" "${TOOL}/bin/pkgconf"

# ---------------------------------------------------------------- MoltenVK
step "MoltenVK ${MVK_VERSION}"
TAR="${DL}/MoltenVK-macos-${MVK_VERSION}.tar"
if [[ ! -f "${TAR}" ]]; then
    curl -fsSL --retry 3 -o "${TAR}.part" \
        "https://github.com/KhronosGroup/MoltenVK/releases/download/${MVK_VERSION}/MoltenVK-macos.tar"
    mv "${TAR}.part" "${TAR}"
fi
echo "${MVK_SHA256}  ${TAR}" | shasum -a 256 -c - >/dev/null || { rm -f "${TAR}"; die "MoltenVK checksum mismatch"; }
MVK_X="${DL}/MoltenVK-${MVK_VERSION}"
if [[ ! -d "${MVK_X}" ]]; then
    mkdir -p "${MVK_X}.part"
    tar -xf "${TAR}" -C "${MVK_X}.part"
    mv "${MVK_X}.part" "${MVK_X}"
fi
MVK_ROOT="${MVK_X}/MoltenVK/MoltenVK"
[[ -f "${MVK_ROOT}/dynamic/dylib/macOS/libMoltenVK.dylib" ]] || die "libMoltenVK.dylib missing in release tar"

# ------------------------------------------ assemble the minimal Vulkan "SDK" tree
step "Assembling ${TOOL}/vulkan"
VK="${TOOL}/vulkan"
rm -rf "${VK}"
mkdir -p "${VK}/lib" "${VK}/bin" "${VK}/include" "${VK}/share/vulkan/icd.d"
cp -R "${MVK_ROOT}/include/." "${VK}/include/"
cp "${MVK_ROOT}/dynamic/dylib/macOS/libMoltenVK.dylib" "${VK}/lib/libMoltenVK.dylib"
# vcpkg's loader: copy the real file under the SONAME-style names consumers ask for
LOADER="$(find "${VI}/lib" -maxdepth 1 -name 'libvulkan.1*.dylib' -type f | head -1)"
if [[ -z "${LOADER}" ]]; then   # symlink-only layouts
    LOADER="$(find -L "${VI}/lib" -maxdepth 1 -name 'libvulkan*.dylib' -type f | head -1)"
fi
[[ -n "${LOADER}" ]] || die "libvulkan dylib not produced by vcpkg vulkan-loader"
cp "${LOADER}" "${VK}/lib/libvulkan.1.dylib"
# The game links @rpath/libvulkan.1.dylib; the vcpkg build names itself libvulkan.1.<patch>.dylib.
install_name_tool -id "@rpath/libvulkan.1.dylib" "${VK}/lib/libvulkan.1.dylib" 2>/dev/null
codesign --force --sign - "${VK}/lib/libvulkan.1.dylib" 2>/dev/null
ln -sf libvulkan.1.dylib "${VK}/lib/libvulkan.dylib"
# vcpkg's loader headers (newer than MoltenVK's tar are fine to overlay if present)
[[ -d "${VI}/include/vulkan" ]] && cp -R "${VI}/include/vulkan/." "${VK}/include/vulkan/"
GLSLANG="$(find "${VI}/tools" -name glslangValidator | head -1)"
[[ -x "${GLSLANG}" ]] || die "glslangValidator not found under ${VI}/tools"
cp -L "${GLSLANG}" "${VK}/bin/glslangValidator"
GLSLANG2="$(find "${VI}/tools" -name glslang -type f | head -1 || true)"
if [[ -n "${GLSLANG2}" ]]; then cp -L "${GLSLANG2}" "${VK}/bin/glslang"; fi
cat > "${VK}/share/vulkan/icd.d/MoltenVK_icd.json" <<'JSON'
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "../../../lib/libMoltenVK.dylib",
        "api_version": "1.4.0",
        "is_portability_driver": true
    }
}
JSON

step "Done"
echo "    tools:  ${TOOL}/venv/bin  ${TOOL}/bin"
echo "    vcpkg:  ${TOOL}/vcpkg"
echo "    vulkan: ${VK}   (export VULKAN_SDK=${VK})"
