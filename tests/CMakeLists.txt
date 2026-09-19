# ============================================================================
# hpulogc test suite
# ============================================================================

# Common helper library: just the runner implementation. Test binaries
# decide which flavor of the library they link (see below).
add_library(hpu_test_util STATIC test_main.c)
target_include_directories(hpu_test_util PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(hpu_test_util PUBLIC Threads::Threads)
if(HPU_SANITIZER_FLAGS)
    target_compile_options(hpu_test_util PRIVATE ${HPU_SANITIZER_FLAGS})
    target_link_options(hpu_test_util PRIVATE ${HPU_SANITIZER_FLAGS})
endif()

# Standard test binary: links the build-selected hpulogc library.
function(hpu_add_test_binary name)
    cmake_parse_arguments(ARG "" "" "SRCS;DEFS;LIBS" ${ARGN})
    add_executable(${name} ${ARG_SRCS})
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    if(ARG_DEFS)
        target_compile_definitions(${name} PRIVATE ${ARG_DEFS})
    endif()
    target_link_libraries(${name} PRIVATE hpu_test_util hpulogc
        hpulogc_build_defs ${ARG_LIBS})
    target_compile_options(${name} PRIVATE ${HPULOGC_WARNING_FLAGS})
    if(HPU_SANITIZER_FLAGS)
        target_compile_options(${name} PRIVATE ${HPU_SANITIZER_FLAGS})
        target_link_options(${name} PRIVATE ${HPU_SANITIZER_FLAGS})
    endif()
    add_test(NAME ${name} COMMAND ${name})
    if(HPULOGC_SANITIZER MATCHES "thread")
        set_tests_properties(${name} PROPERTIES
            ENVIRONMENT "TSAN_OPTIONS=die_after_fork=0")
    endif()
endfunction()

# Ring test binary: compiles a SPECIFIC ring implementation together with
# the platform object library, independent of HPULOGC_LOCKFREE.
#   impl_name: locked | lockfree (picks the source file)
#   impl_num / conc_num: numeric values baked into the test (0/1)
function(hpu_add_ring_test name impl_name impl_num conc_num)
    add_executable(${name} unit/test_ring.c)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(${name} PRIVATE
        HPU_RING_TEST_IMPL=${impl_num} HPU_RING_TEST_CONC=${conc_num})
    target_sources(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/src/ring/ringbuf_${impl_name}.c
        $<TARGET_OBJECTS:hpulogc_platform>)
    target_link_libraries(${name} PRIVATE hpu_test_util hpulogc_build_defs)
    target_compile_options(${name} PRIVATE ${HPULOGC_WARNING_FLAGS})
    if(HPU_SANITIZER_FLAGS)
        target_compile_options(${name} PRIVATE ${HPU_SANITIZER_FLAGS})
        target_link_options(${name} PRIVATE ${HPU_SANITIZER_FLAGS})
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# ---------------------------------------------------------------------------
# Unit tests
# ---------------------------------------------------------------------------

# Ring buffer: all 4 build combinations (impl x concurrency). Runtime tests
# cover the policies available for each combination.
hpu_add_ring_test(test_ring_locked_spsc locked 0 0)
hpu_add_ring_test(test_ring_locked_mpsc locked 0 1)
hpu_add_ring_test(test_ring_lockfree_spsc lockfree 1 0)
hpu_add_ring_test(test_ring_lockfree_mpsc lockfree 1 1)

# Atomic backends: same source compiled once per forced backend. Windows
# has exactly one backend (Interlocked*, spec 4.3); POSIX tests the
# stdatomic + GCC paths.
if(WIN32)
    set(HPU_ATOMIC_TEST_BACKENDS MSVC)
else()
    set(HPU_ATOMIC_TEST_BACKENDS stdatomic gcc gcc_sync)
endif()
foreach(backend ${HPU_ATOMIC_TEST_BACKENDS})
    set(tname test_atomic_backend_${backend})
    add_executable(${tname} unit/test_atomic.c)
    target_include_directories(${tname} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(${tname} PRIVATE
        HPULOGC_ATOMIC_BACKEND_${backend})
    target_link_libraries(${tname} PRIVATE hpu_test_util Threads::Threads)
    target_compile_options(${tname} PRIVATE ${HPULOGC_WARNING_FLAGS})
    if(backend STREQUAL "stdatomic")
        set_target_properties(${tname} PROPERTIES C_STANDARD 11)
    endif()
    if(HPU_SANITIZER_FLAGS)
        target_compile_options(${tname} PRIVATE ${HPU_SANITIZER_FLAGS})
        target_link_options(${tname} PRIVATE ${HPU_SANITIZER_FLAGS})
    endif()
    add_test(NAME ${tname} COMMAND ${tname})
endforeach()


hpu_add_test_binary(test_ini SRCS unit/test_ini.c)
hpu_add_test_binary(test_format SRCS unit/test_format.c)
hpu_add_test_binary(test_rotate SRCS unit/test_rotate.c)
hpu_add_test_binary(test_lifecycle SRCS unit/test_lifecycle.c)
hpu_add_test_binary(test_pipeline SRCS integration/test_pipeline.c)
if(NOT WIN32 AND NOT HPULOGC_SANITIZER MATCHES "thread"
   AND HPULOGC_ENABLE_INI)
    # fork() behaviors: POSIX only (Windows has no fork, spec 9); TSan
    # cannot model fork()+threads either; needs the INI path.
    hpu_add_test_binary(test_fork SRCS integration/test_fork.c)
endif()
hpu_add_test_binary(test_signal_safe SRCS integration/test_signal_safe.c)
hpu_add_test_binary(test_diskfail SRCS integration/test_diskfail.c)

# ---------------------------------------------------------------------------
# Benchmarks (spec 13.3): independent build targets, NOT in default ctest.
# ---------------------------------------------------------------------------
foreach(bench_name bench_log bench_memory)
    add_executable(${bench_name} EXCLUDE_FROM_ALL bench/${bench_name}.c)
    target_include_directories(${bench_name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(${bench_name} PRIVATE hpulogc Threads::Threads)
    target_compile_options(${bench_name} PRIVATE ${HPULOGC_WARNING_FLAGS})
    if(HPU_SANITIZER_FLAGS)
        target_compile_options(${bench_name} PRIVATE ${HPU_SANITIZER_FLAGS})
        target_link_options(${bench_name} PRIVATE ${HPU_SANITIZER_FLAGS})
    endif()
endforeach()

# Fuzz: deterministic driver in ctest; libFuzzer target built on demand.
if(HPULOGC_ENABLE_INI)
add_executable(fuzz_ini_deterministic fuzz/fuzz_ini.c fuzz/fuzz_main.c)
target_include_directories(fuzz_ini_deterministic PRIVATE
    ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(fuzz_ini_deterministic PRIVATE hpu_test_util hpulogc)
target_compile_options(fuzz_ini_deterministic PRIVATE
    ${HPULOGC_WARNING_FLAGS})
if(HPU_SANITIZER_FLAGS)
    target_compile_options(fuzz_ini_deterministic PRIVATE
        ${HPU_SANITIZER_FLAGS})
    target_link_options(fuzz_ini_deterministic PRIVATE
        ${HPU_SANITIZER_FLAGS})
endif()
add_test(NAME fuzz_ini_deterministic COMMAND fuzz_ini_deterministic)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    add_executable(fuzz_ini_libfuzzer EXCLUDE_FROM_ALL fuzz/fuzz_ini.c)
    target_include_directories(fuzz_ini_libfuzzer PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(fuzz_ini_libfuzzer PRIVATE
        -fsanitize=fuzzer,address -g)
    target_link_options(fuzz_ini_libfuzzer PRIVATE -fsanitize=fuzzer,address)
endif()
