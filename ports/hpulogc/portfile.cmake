# hpulogc overlay port: builds the upstream sources through the project's
# CMake install/export rules (lib/cmake/hpulogc config package). REF is
# pinned to a main commit; bump REF and SHA512 together at each release
# (see docs/decision_log.md D-R9).
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO jxsword/hpulogc
    REF 4333e609b49a58a7f4cfdebc956db1457942c45a
    SHA512 5e3d9103c1bc3faddb40ad15f57fefd54169d406c17a3243c4f7ebe8781ad81407ff29a5cc311d1123460a9b29b755e629d7f5e5d359213aafd84f84ffba2b60
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
