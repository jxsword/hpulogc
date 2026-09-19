# ============================================================================
# hpulogc examples
# ============================================================================

function(hpu_add_example name)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE hpulogc)
    target_compile_options(${name} PRIVATE ${HPULOGC_WARNING_FLAGS})
    if(HPU_SANITIZER_FLAGS)
        target_compile_options(${name} PRIVATE ${HPU_SANITIZER_FLAGS})
        target_link_options(${name} PRIVATE ${HPU_SANITIZER_FLAGS})
    endif()
    add_test(NAME example_${name} COMMAND ${name})
    set_tests_properties(example_${name} PROPERTIES
        ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
endfunction()

hpu_add_example(minimal)
hpu_add_example(async)
hpu_add_example(multi_category)
hpu_add_example(custom_format)
