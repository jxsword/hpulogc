# hpulogc overlay port: builds the upstream sources through the project's
# CMake install/export rules (lib/cmake/hpulogc config package). REF is
# pinned to a main commit; bump REF and SHA512 together at each release
# (see docs/decision_log.md D-R9).
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO jxsword/hpulogc
    REF 0330de683bab685973477789eaffa5834a760fdc
    SHA512 ecc1d992bcde33dc2b1a8a621913aecd8defd4b00df9cdce919cf40db669cdc098bd1f220f83ef3a17284313f150e8dfb4da7b495ea8d7af9806a822fbfa0d72
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        # vcpkg consumes the static library (default linkage); the shared
        # build would leave an unhandled DLL in bin/.
        "-DHPULOGC_BUILD_SHARED=OFF"
        "-DHPULOGC_BUILD_TESTS=OFF"
        "-DHPULOGC_BUILD_EXAMPLES=OFF"
)

vcpkg_cmake_install()

# CMake installs the header set once per config pass, so the debug pass
# duplicates include/ under debug/; vcpkg requires that tree to go.
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage"
     DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")

vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/hpulogc")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
