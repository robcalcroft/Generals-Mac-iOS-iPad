# GeneralsX @build Claude 04/11/2026 Dynamic arm64 macOS triplet used ONLY by
# scripts/build/macos/bootstrap-toolchain.sh for build-time tools and the Vulkan
# loader (libvulkan must be a dylib; the main game triplet arm64-osx is static).
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET "15.0")
