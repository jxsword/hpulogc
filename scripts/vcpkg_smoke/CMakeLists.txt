# Minimal consumer project for the vcpkg smoke CI job (D-R9): resolves
# the installed package via the vcpkg toolchain's manifest mode (the
# repo-root vcpkg.json + vcpkg-configuration.json overlay) and exercises
# the exported CMake config package.
cmake_minimum_required(VERSION 3.16)
project(hpulogc_vcpkg_smoke C)

find_package(hpulogc CONFIG REQUIRED)

add_executable(vcpkg_smoke main.c)
target_link_libraries(vcpkg_smoke PRIVATE hpulogc::hpulogc)

enable_testing()
add_test(NAME vcpkg_smoke COMMAND vcpkg_smoke)
