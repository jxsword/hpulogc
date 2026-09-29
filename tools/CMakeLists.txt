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
