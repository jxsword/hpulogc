# ============================================================================
# hpulogc - pure C high-performance logging library
# ============================================================================
cmake_minimum_required(VERSION 3.16)

project(hpulogc
    VERSION 0.1.0
    DESCRIPTION "High Performance Log in C"
    LANGUAGES C)

# ---------------------------------------------------------------------------
# Build presets (spec 5). Preset values override user options with a warning.
# ---------------------------------------------------------------------------
set(HPULOGC_BUILD_PRESET "full" CACHE STRING
    "Build preset: full | min | sync_thread | async_single | custom")

function(hpulogc_force_option name type value)
    if(DEFINED CACHE{${name}} AND NOT "${${name}}" STREQUAL "${value}")
        message(WARNING
            "hpulogc preset '${HPULOGC_BUILD_PRESET}' overrides ${name}: "
            "'${${name}}' -> '${value}'")
    endif()
    set(${name} "${value}" CACHE "${type}" "" FORCE)
endfunction()

if(HPULOGC_BUILD_PRESET STREQUAL "min")
    hpulogc_force_option(HPULOGC_ENABLE_ASYNC BOOL OFF)
    hpulogc_force_option(HPULOGC_ENABLE_COLOR BOOL OFF)
    hpulogc_force_option(HPULOGC_ENABLE_ROTATE BOOL OFF)
    hpulogc_force_option(HPULOGC_ENABLE_CATEGORY BOOL OFF)
    hpulogc_force_option(HPULOGC_ENABLE_HOT_RELOAD BOOL OFF)
    hpulogc_force_option(HPULOGC_ENABLE_THROTTLE BOOL OFF)
    hpulogc_force_option(HPULOGC_ENABLE_INI BOOL OFF)
    hpulogc_force_option(HPULOGC_CONCURRENCY STRING SPSC)
    hpulogc_force_option(HPULOGC_LOCKFREE BOOL OFF)
    hpulogc_force_option(HPULOGC_MIN_SIZE_OPT BOOL ON)
elseif(HPULOGC_BUILD_PRESET STREQUAL "sync_thread")
    hpulogc_force_option(HPULOGC_ENABLE_ASYNC BOOL OFF)
    hpulogc_force_option(HPULOGC_LOCKFREE BOOL OFF)
    hpulogc_force_option(HPULOGC_MIN_SIZE_OPT BOOL OFF)
elseif(HPULOGC_BUILD_PRESET STREQUAL "async_single")
    hpulogc_force_option(HPULOGC_ENABLE_ASYNC BOOL ON)
    hpulogc_force_option(HPULOGC_LOCKFREE BOOL ON)
    hpulogc_force_option(HPULOGC_CONCURRENCY STRING SPSC)
    hpulogc_force_option(HPULOGC_MIN_SIZE_OPT BOOL OFF)
else()
    if(NOT HPULOGC_BUILD_PRESET STREQUAL "full" AND
       NOT HPULOGC_BUILD_PRESET STREQUAL "custom")
        message(WARNING
            "Unknown HPULOGC_BUILD_PRESET '${HPULOGC_BUILD_PRESET}', "
            "falling back to 'full'")
        set(HPULOGC_BUILD_PRESET "full" CACHE STRING "" FORCE)
    endif()
    hpulogc_force_option(HPULOGC_MIN_SIZE_OPT BOOL OFF)
endif()

# ---------------------------------------------------------------------------
# Feature options (spec 4.8)
# ---------------------------------------------------------------------------
option(HPULOGC_ENABLE_ASYNC       "Async writer thread + batching" ON)
option(HPULOGC_ENABLE_COLOR       "ANSI color support" ON)
option(HPULOGC_ENABLE_ROTATE      "Log rotation" ON)
option(HPULOGC_ENABLE_INI         "INI config parser" ON)
option(HPULOGC_ENABLE_HOT_RELOAD  "Config hot reload" ON)
option(HPULOGC_ENABLE_CATEGORY    "Category routing + per-category levels" ON)
option(HPULOGC_ENABLE_THROTTLE    "Rate limiting and sampling" OFF)
option(HPULOGC_ENABLE_SOURCE_LOC  "Capture __FILE__/__LINE__" ON)
option(HPULOGC_LOCKFREE           "Lock-free ring buffer (else locked)" OFF)
set(HPULOGC_CONCURRENCY "MPSC" CACHE STRING
    "Concurrency mode: SPSC | MPSC")
set(HPULOGC_COMPILE_TIME_LEVEL "TRACE" CACHE STRING
    "Compile-time trim threshold: TRACE|DEBUG|INFO|WARN|ERROR|FATAL|OFF")
set(HPULOGC_C_STANDARD "99" CACHE STRING "C standard: 99 | 11")
set(HPULOGC_ATOMIC_BACKEND "auto" CACHE STRING
    "Atomic backend: auto | stdatomic | gcc-atomic | gcc-sync | msvc-interlocked")
set(HPULOGC_SANITIZER "none" CACHE STRING
    "Sanitizer: none | address | thread | undefined | address,undefined")
option(HPULOGC_WERROR "Treat warnings as errors" OFF)
option(HPULOGC_BUILD_TESTS "Build tests" ON)
option(HPULOGC_BUILD_EXAMPLES "Build examples" ON)
option(HPULOGC_BUILD_SHARED "Also build the shared library" ON)
option(HPULOGC_ENABLE_LINT "Add clang-tidy/cppcheck helper targets" OFF)

# ---------------------------------------------------------------------------
# Language standard
# ---------------------------------------------------------------------------
if(NOT HPULOGC_C_STANDARD MATCHES "^(99|11)$")
    message(FATAL_ERROR "HPULOGC_C_STANDARD must be 99 or 11")
endif()
set(CMAKE_C_STANDARD ${HPULOGC_C_STANDARD})
set(CMAKE_C_STANDARD_REQUIRED ON)
# GNU extensions are required for `fmt, ##__VA_ARGS__` on C99 (spec 7.3).
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Concurrency mode
# ---------------------------------------------------------------------------
string(TOUPPER "${HPULOGC_CONCURRENCY}" HPULOGC_CONCURRENCY_UC)
if(NOT HPULOGC_CONCURRENCY_UC MATCHES "^(SPSC|MPSC)$")
    message(FATAL_ERROR "HPULOGC_CONCURRENCY must be SPSC or MPSC")
endif()

# ---------------------------------------------------------------------------
# Compile-time level (name -> numeric macro value)
# ---------------------------------------------------------------------------
set(HPULOGC_COMPILE_TIME_LEVEL_NUMERIC 0)
if(HPULOGC_COMPILE_TIME_LEVEL STREQUAL "DEBUG")
    set(HPULOGC_COMPILE_TIME_LEVEL_NUMERIC 1)
elseif(HPULOGC_COMPILE_TIME_LEVEL STREQUAL "INFO")
    set(HPULOGC_COMPILE_TIME_LEVEL_NUMERIC 2)
elseif(HPULOGC_COMPILE_TIME_LEVEL STREQUAL "WARN")
    set(HPULOGC_COMPILE_TIME_LEVEL_NUMERIC 3)
elseif(HPULOGC_COMPILE_TIME_LEVEL STREQUAL "ERROR")
    set(HPULOGC_COMPILE_TIME_LEVEL_NUMERIC 4)
elseif(HPULOGC_COMPILE_TIME_LEVEL STREQUAL "FATAL")
    set(HPULOGC_COMPILE_TIME_LEVEL_NUMERIC 5)
elseif(HPULOGC_COMPILE_TIME_LEVEL STREQUAL "OFF")
    set(HPULOGC_COMPILE_TIME_LEVEL_NUMERIC 6)
elseif(NOT HPULOGC_COMPILE_TIME_LEVEL STREQUAL "TRACE")
    if(HPULOGC_COMPILE_TIME_LEVEL MATCHES "^[0-6]$")
        set(HPULOGC_COMPILE_TIME_LEVEL_NUMERIC ${HPULOGC_COMPILE_TIME_LEVEL})
    else()
        message(FATAL_ERROR
            "HPULOGC_COMPILE_TIME_LEVEL must be a level name or 0..6")
    endif()
endif()

# ---------------------------------------------------------------------------
# Platform detection
# ---------------------------------------------------------------------------
if(WIN32)
    set(HPU_PLATFORM "windows")
elseif(APPLE)
    set(HPU_PLATFORM "darwin")
else()
    set(HPU_PLATFORM "linux")
    execute_process(COMMAND uname -s OUTPUT_VARIABLE _hpu_uname
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT _hpu_uname MATCHES "Linux")
        set(HPU_PLATFORM "posix")
    endif()
endif()

# ---------------------------------------------------------------------------
# Atomic backend selection (spec 4.3/11)
# ---------------------------------------------------------------------------
set(HPU_ATOMIC_BACKEND_DEFINE "")
if(HPULOGC_ATOMIC_BACKEND STREQUAL "auto")
    if(WIN32)
        set(HPU_ATOMIC_BACKEND_DEFINE HPULOGC_ATOMIC_BACKEND_MSVC)
    elseif(CMAKE_C_STANDARD EQUAL 11 AND NOT CMAKE_C_COMPILER_ID MATCHES
           ".*(MSVC).*")
        # Verify <stdatomic.h> really works in C11 mode.
        include(CheckCSourceCompiles)
        set(CMAKE_REQUIRED_QUIET ON)
        check_c_source_compiles(
            "#include <stdatomic.h>\nint main(void){_Atomic int x=0;return (int)atomic_load(&x);}"
            HPU_HAVE_STDATOMIC)
        if(HPU_HAVE_STDATOMIC)
            set(HPU_ATOMIC_BACKEND_DEFINE HPULOGC_ATOMIC_BACKEND_STDATOMIC)
        else()
            set(HPU_ATOMIC_BACKEND_DEFINE HPULOGC_ATOMIC_BACKEND_GCC)
        endif()
    elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        set(HPU_ATOMIC_BACKEND_DEFINE HPULOGC_ATOMIC_BACKEND_GCC)
    else()
        message(FATAL_ERROR
            "hpulogc: no atomic backend for compiler ${CMAKE_C_COMPILER_ID}")
    endif()
elseif(HPULOGC_ATOMIC_BACKEND STREQUAL "stdatomic")
    if(NOT CMAKE_C_STANDARD EQUAL 11)
        message(FATAL_ERROR
            "atomic backend 'stdatomic' requires HPULOGC_C_STANDARD=11")
    endif()
    if(WIN32)
        message(FATAL_ERROR
            "atomic backend 'stdatomic' is not available on Windows")
    endif()
    set(HPU_ATOMIC_BACKEND_DEFINE HPULOGC_ATOMIC_BACKEND_STDATOMIC)
elseif(HPULOGC_ATOMIC_BACKEND STREQUAL "gcc-atomic")
    if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        message(FATAL_ERROR
            "atomic backend 'gcc-atomic' requires GCC or Clang")
    endif()
    set(HPU_ATOMIC_BACKEND_DEFINE HPULOGC_ATOMIC_BACKEND_GCC)
elseif(HPULOGC_ATOMIC_BACKEND STREQUAL "gcc-sync")
    if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        message(FATAL_ERROR
            "atomic backend 'gcc-sync' requires GCC or Clang")
    endif()
    set(HPU_ATOMIC_BACKEND_DEFINE HPULOGC_ATOMIC_BACKEND_GCC_SYNC)
elseif(HPULOGC_ATOMIC_BACKEND STREQUAL "msvc-interlocked")
    if(NOT WIN32)
        message(FATAL_ERROR
            "atomic backend 'msvc-interlocked' requires Windows")
    endif()
    set(HPU_ATOMIC_BACKEND_DEFINE HPULOGC_ATOMIC_BACKEND_MSVC)
else()
    message(FATAL_ERROR "Unknown HPULOGC_ATOMIC_BACKEND "
        "'${HPULOGC_ATOMIC_BACKEND}'")
endif()

# ---------------------------------------------------------------------------
# Sanitizers (spec 11)
# ---------------------------------------------------------------------------
set(HPU_SANITIZER_FLAGS "")
if(NOT HPULOGC_SANITIZER STREQUAL "none")
    if(MSVC)
        if(HPULOGC_SANITIZER MATCHES "address")
            # /fsanitize=address requires VS2019 16.9+ (spec 16.2 DoD).
            list(APPEND HPU_SANITIZER_FLAGS /fsanitize=address)
        endif()
        if(HPULOGC_SANITIZER MATCHES "thread")
            message(WARNING
                "hpulogc: ThreadSanitizer is not available with MSVC; "
                "HPULOGC_SANITIZER='${HPULOGC_SANITIZER}' downgraded")
        endif()
        if(HPULOGC_SANITIZER MATCHES "undefined")
            message(WARNING
                "hpulogc: UBSan is not available with MSVC; "
                "HPULOGC_SANITIZER='${HPULOGC_SANITIZER}' downgraded")
        endif()
    else()
        if(HPULOGC_SANITIZER MATCHES "thread")
            if(NOT HPULOGC_SANITIZER STREQUAL "thread")
                message(FATAL_ERROR
                    "HPULOGC_SANITIZER: thread is mutually exclusive with "
                    "others")
            endif()
            list(APPEND HPU_SANITIZER_FLAGS -fsanitize=thread)
        endif()
        if(HPULOGC_SANITIZER MATCHES "address")
            list(APPEND HPU_SANITIZER_FLAGS -fsanitize=address)
        endif()
        if(HPULOGC_SANITIZER MATCHES "undefined")
            list(APPEND HPU_SANITIZER_FLAGS -fsanitize=undefined
                 -fno-sanitize-recover=all)
        endif()
    endif()
    if(HPU_SANITIZER_FLAGS STREQUAL "")
        message(WARNING
            "hpulogc: no sanitizer of '${HPULOGC_SANITIZER}' applies to "
            "this compiler; building without")
    endif()
endif()

# ---------------------------------------------------------------------------
# Compile definitions / flags
# ---------------------------------------------------------------------------
set(HPULOGC_PRIVATE_DEFINES
    ${HPU_ATOMIC_BACKEND_DEFINE}
    HPULOGC_BUILD_VERSION_NAME="${HPULOGC_BUILD_PRESET}"
)
if(HPU_PLATFORM STREQUAL "windows")
    list(APPEND HPULOGC_PRIVATE_DEFINES HPU_PLATFORM_WIN32=1)
elseif(HPU_PLATFORM STREQUAL "darwin")
    # Phase 3: the darwin layer reuses the POSIX shared layer.
    list(APPEND HPULOGC_PRIVATE_DEFINES HPU_PLATFORM_POSIX=1)
else()
    list(APPEND HPULOGC_PRIVATE_DEFINES HPU_PLATFORM_POSIX=1)
endif()
if(HPU_PLATFORM STREQUAL "linux")
    list(APPEND HPULOGC_PRIVATE_DEFINES HPU_PLATFORM_LINUX=1)
endif()
if(HPU_PLATFORM STREQUAL "windows")
    # Keep the Win32 umbrella header from polluting library TUs.
    list(APPEND HPULOGC_PRIVATE_DEFINES WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
if(HPULOGC_CONCURRENCY_UC STREQUAL "SPSC")
    list(APPEND HPULOGC_PRIVATE_DEFINES HPULOGC_CONCURRENCY_SPSC=1)
else()
    list(APPEND HPULOGC_PRIVATE_DEFINES HPULOGC_CONCURRENCY_MSPC=1)
endif()
list(APPEND HPULOGC_PRIVATE_DEFINES
    HPULOGC_LOCKFREE=$<BOOL:${HPULOGC_LOCKFREE}>
    HPULOGC_ENABLE_ASYNC=$<BOOL:${HPULOGC_ENABLE_ASYNC}>
    HPULOGC_ENABLE_COLOR=$<BOOL:${HPULOGC_ENABLE_COLOR}>
    HPULOGC_ENABLE_ROTATE=$<BOOL:${HPULOGC_ENABLE_ROTATE}>
    HPULOGC_ENABLE_INI=$<BOOL:${HPULOGC_ENABLE_INI}>
    HPULOGC_ENABLE_HOT_RELOAD=$<BOOL:${HPULOGC_ENABLE_HOT_RELOAD}>
    HPULOGC_ENABLE_CATEGORY=$<BOOL:${HPULOGC_ENABLE_CATEGORY}>
    HPULOGC_ENABLE_THROTTLE=$<BOOL:${HPULOGC_ENABLE_THROTTLE}>
    HPULOGC_ENABLE_SOURCE_LOC=$<BOOL:${HPULOGC_ENABLE_SOURCE_LOC}>
)

# Internal build definitions shared by the library and the tests (tests
# include internal headers and need the same platform/feature macros).
add_library(hpulogc_build_defs INTERFACE)
target_compile_definitions(hpulogc_build_defs INTERFACE
    ${HPULOGC_PRIVATE_DEFINES})

if(MSVC)
    # CRT security deprecation warnings (fopen/strerror in frozen code)
    # are silenced project-wide: the library's string handling is bounded
    # already (spec 9); recorded in docs/implementation_notes.md.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    # /W4 zero-warning policy. /wd4210: frozen src/output/output.c uses
    # block-scope `extern` function declarations (legal C99, MSVC-only
    # style warning; recorded in docs/implementation_notes.md).
    set(HPULOGC_WARNING_FLAGS /W4 /utf-8 /wd4210)
    if(HPULOGC_WERROR)
        list(APPEND HPULOGC_WARNING_FLAGS /WX)
    endif()
else()
    set(HPULOGC_WARNING_FLAGS -Wall -Wextra)
    if(HPULOGC_WERROR)
        list(APPEND HPULOGC_WARNING_FLAGS -Werror)
    endif()
endif()

# ---------------------------------------------------------------------------
# Library sources
# ---------------------------------------------------------------------------
set(HPULOGC_PLATFORM_SOURCES)
if(HPU_PLATFORM STREQUAL "windows")
    list(APPEND HPULOGC_PLATFORM_SOURCES
        src/platform/win32/win32_sync.c
        src/platform/win32/win32_thread.c
        src/platform/win32/win32_time.c
        src/platform/win32/win32_fs.c
        src/platform/win32/win32_path.c
        src/platform/win32/win32_tid.c
        src/platform/win32/win32_tls.c
        src/platform/win32/win32_signal.c
        src/platform/win32/win32_watcher.c
    )
elseif(HPU_PLATFORM STREQUAL "darwin")
    # macOS (Phase 3): reuses the POSIX shared layer with two darwin
    # replacements - darwin_sync (macOS pthread_condattr_setclock only
    # supports CLOCK_REALTIME; relative timed waits are used instead)
    # and darwin_tid (pthread_threadid_np). The watcher stays on the
    # poll fallback backend (kqueue is a future enhancement).
    list(APPEND HPULOGC_PLATFORM_SOURCES
        src/platform/darwin/darwin_sync.c
        src/platform/darwin/darwin_tid.c
        src/platform/posix/posix_thread.c
        src/platform/posix/posix_time.c
        src/platform/posix/posix_fs.c
        src/platform/posix/posix_path.c
        src/platform/posix/posix_tls.c
        src/platform/posix/posix_signal.c
        src/platform/posix/posix_watcher_poll.c
        src/platform/posix/posix_watcher.c
    )
else()
    list(APPEND HPULOGC_PLATFORM_SOURCES
        src/platform/posix/posix_sync.c
        src/platform/posix/posix_thread.c
        src/platform/posix/posix_time.c
        src/platform/posix/posix_fs.c
        src/platform/posix/posix_path.c
        src/platform/posix/posix_tls.c
        src/platform/posix/posix_tid.c
        src/platform/posix/posix_signal.c
        src/platform/posix/posix_watcher_poll.c
    )
    if(HPU_PLATFORM STREQUAL "linux")
        list(APPEND HPULOGC_PLATFORM_SOURCES src/platform/linux/linux_watcher.c)
    else()
        list(APPEND HPULOGC_PLATFORM_SOURCES src/platform/posix/posix_watcher.c)
    endif()
endif()

set(HPULOGC_RING_SOURCE src/ring/ringbuf_locked.c)
if(HPULOGC_LOCKFREE)
    set(HPULOGC_RING_SOURCE src/ring/ringbuf_lockfree.c)
endif()

find_package(Threads REQUIRED)

# Platform layer object library: ring test binaries recombine it with a
# specific ring implementation (locked/lockfree) independent of the
# build-wide HPULOGC_LOCKFREE choice.
add_library(hpulogc_platform OBJECT ${HPULOGC_PLATFORM_SOURCES})
target_include_directories(hpulogc_platform PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hpulogc_platform PRIVATE
    hpulogc_build_defs Threads::Threads)
target_compile_options(hpulogc_platform PRIVATE ${HPULOGC_WARNING_FLAGS})
set_target_properties(hpulogc_platform PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
if(HPU_SANITIZER_FLAGS)
    target_compile_options(hpulogc_platform PRIVATE ${HPU_SANITIZER_FLAGS})
endif()

set(HPULOGC_CORE_SOURCES
    src/conf/conf_model.c
    src/conf/conf_file.c
    src/conf/ini.c
    src/core/api.c
    src/core/consumer.c
    src/core/core.c
    src/core/hotreload.c
    src/core/pipeline.c
    src/core/registry.c
    src/core/signal_safe.c
    src/format/format.c
    src/output/output.c
    src/output/output_console.c
    src/output/output_file.c
    src/output/rotate.c
)

if(NOT HPULOGC_ENABLE_INI)
    list(REMOVE_ITEM HPULOGC_CORE_SOURCES src/conf/ini.c)
    list(REMOVE_ITEM HPULOGC_CORE_SOURCES src/conf/conf_file.c)
endif()
if(NOT HPULOGC_ENABLE_ROTATE)
    list(REMOVE_ITEM HPULOGC_CORE_SOURCES src/output/rotate.c)
endif()

add_library(hpulogc
    ${HPULOGC_RING_SOURCE}
    ${HPULOGC_CORE_SOURCES}
    $<TARGET_OBJECTS:hpulogc_platform>
)
add_library(hpulogc::hpulogc ALIAS hpulogc)

target_include_directories(hpulogc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(hpulogc PRIVATE $<BUILD_INTERFACE:hpulogc_build_defs>
    PUBLIC Threads::Threads)
# The trim threshold is a public macro: propagate to consumers.
target_compile_definitions(hpulogc PUBLIC
    HPULOGC_COMPILE_TIME_LEVEL=${HPULOGC_COMPILE_TIME_LEVEL_NUMERIC})

target_compile_options(hpulogc PRIVATE ${HPULOGC_WARNING_FLAGS})
if(HPU_SANITIZER_FLAGS)
    target_compile_options(hpulogc PRIVATE ${HPU_SANITIZER_FLAGS})
    target_link_options(hpulogc PRIVATE ${HPU_SANITIZER_FLAGS})
endif()
if(HPULOGC_MIN_SIZE_OPT AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(hpulogc PRIVATE
        -Os -ffunction-sections -fdata-sections)
    if(APPLE)
        # Apple's linker has no --gc-sections; -dead_strip is its
        # section-GC equivalent (Phase 3).
        target_link_options(hpulogc PRIVATE -Wl,-dead_strip)
    else()
        target_link_options(hpulogc PRIVATE -Wl,--gc-sections)
    endif()
endif()
if(HPULOGC_MIN_SIZE_OPT AND MSVC)
    target_compile_options(hpulogc PRIVATE /Os /Gy /Gw /Zc:inline)
    target_link_options(hpulogc PRIVATE /OPT:REF /OPT:ICF)
endif()

set_target_properties(hpulogc PROPERTIES
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON)

if(HPULOGC_BUILD_SHARED)
    add_library(hpulogc_shared SHARED ${HPULOGC_RING_SOURCE}
                                      ${HPULOGC_CORE_SOURCES}
                                      $<TARGET_OBJECTS:hpulogc_platform>)
    target_include_directories(hpulogc_shared PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
    target_include_directories(hpulogc_shared PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(hpulogc_shared PRIVATE
        HPULOGC_BUILDING_DLL=1)
    target_link_libraries(hpulogc_shared PRIVATE $<BUILD_INTERFACE:hpulogc_build_defs>
        PUBLIC Threads::Threads)
    target_compile_definitions(hpulogc_shared PUBLIC
        HPULOGC_COMPILE_TIME_LEVEL=${HPULOGC_COMPILE_TIME_LEVEL_NUMERIC})
    target_compile_options(hpulogc_shared PRIVATE ${HPULOGC_WARNING_FLAGS})
    if(HPU_SANITIZER_FLAGS)
        target_compile_options(hpulogc_shared PRIVATE ${HPU_SANITIZER_FLAGS})
        target_link_options(hpulogc_shared PRIVATE ${HPU_SANITIZER_FLAGS})
    endif()
    set_target_properties(hpulogc_shared PROPERTIES
        C_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    if(WIN32)
        # The DLL import library would collide with the static library's
        # hpulogc.lib on Windows; give the DLL a distinct name.
        set_target_properties(hpulogc_shared PROPERTIES
            OUTPUT_NAME hpulogc_shared)
    else()
        set_target_properties(hpulogc_shared PROPERTIES
            OUTPUT_NAME hpulogc)
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests / examples / install
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(HPULOGC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(HPULOGC_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

install(TARGETS hpulogc
    EXPORT hpulogcTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(HPULOGC_BUILD_SHARED)
    install(TARGETS hpulogc_shared EXPORT hpulogcTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
install(FILES include/hpulogc.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

set(HPULOGC_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/hpulogc")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/hpulogcConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hpulogcConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/hpulogcConfig.cmake"
    INSTALL_DESTINATION ${HPULOGC_CMAKE_DIR})
install(EXPORT hpulogcTargets
    NAMESPACE hpulogc::
    DESTINATION ${HPULOGC_CMAKE_DIR})
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/hpulogcConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/hpulogcConfigVersion.cmake"
    DESTINATION ${HPULOGC_CMAKE_DIR})
