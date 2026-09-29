# ============================================================================
# hpulogc tools
# ============================================================================

# hpulogc_chk_conf: configuration validation CLI (spec rd_v0.6 v0.6.1 §11).
# Built only when the INI parser is compiled in (guarded by the parent).
add_executable(hpulogc_chk_conf chk_conf/hpulogc_chk_conf.c)
target_include_directories(hpulogc_chk_conf PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(hpulogc_chk_conf PRIVATE hpulogc)
target_compile_options(hpulogc_chk_conf PRIVATE ${HPULOGC_WARNING_FLAGS})
# Sanitized builds link an instrumented library: the CLI must be
# instrumented too (same rule as the test binaries).
if(HPU_SANITIZER_FLAGS)
    target_compile_options(hpulogc_chk_conf PRIVATE ${HPU_SANITIZER_FLAGS})
    target_link_options(hpulogc_chk_conf PRIVATE ${HPU_SANITIZER_FLAGS})
endif()

install(TARGETS hpulogc_chk_conf RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# CI regression gate (spec rd_v0.6 v0.6.1 §11): the example config must
# stay valid. conf_eg.ini references a syslog sink, which Windows builds
# do not compile in — limit the automated check to UNIX.
if(HPULOGC_BUILD_TESTS AND NOT WIN32)
    add_test(NAME chk_conf_example
        COMMAND hpulogc_chk_conf ${CMAKE_SOURCE_DIR}/examples/conf_eg.ini)
endif()

# 24h soak driver (spec rd_v0.6 §13.3, decision D-R6): release-gate
# harness, NOT part of ctest; built on demand via
# `cmake --build <dir> --target soak_driver`. Linux-only (pthreads +
# /proc RSS sampling); the driver itself links the INI parser, matching
# this subdirectory's guard in the parent.
if(UNIX AND NOT HPULOGC_SANITIZER MATCHES "address|undefined")
    add_executable(soak_driver EXCLUDE_FROM_ALL soak/soak_driver.c)
    target_include_directories(soak_driver PRIVATE
        ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(soak_driver PRIVATE hpulogc Threads::Threads)
    target_compile_options(soak_driver PRIVATE ${HPULOGC_WARNING_FLAGS})
    if(HPU_SANITIZER_FLAGS)
        target_compile_options(soak_driver PRIVATE ${HPU_SANITIZER_FLAGS})
        target_link_options(soak_driver PRIVATE ${HPU_SANITIZER_FLAGS})
    endif()
endif()
