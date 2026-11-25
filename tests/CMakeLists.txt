# Standalone C++20 test-suite for the engine. No third-party framework: the
# harness in TestSupport.{hpp,cpp} is ~200 lines.

add_library(vortex_test_support STATIC TestSupport.cpp)
target_include_directories(vortex_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vortex_test_support PUBLIC vortex_core)

# Sanitizer runtime options used whenever the suite is built with -DENABLE_SANITIZERS=ON.
# (Apple's AddressSanitizer has no LeakSanitizer; leaks are checked with leaks(1),
# see the "leaks" custom target below.)
set(VORTEX_TEST_ENVIRONMENT
    "ASAN_OPTIONS=abort_on_error=1:detect_stack_use_after_return=1:strict_string_checks=1:check_initialization_order=1"
    "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")

set(VORTEX_TEST_TARGETS "")

function(vortex_add_test name)
    add_executable(${name} ${name}.cpp ${ARGN})
    target_link_libraries(${name} PRIVATE vortex_test_support)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 900 ENVIRONMENT "${VORTEX_TEST_ENVIRONMENT}")
    set(VORTEX_TEST_TARGETS ${VORTEX_TEST_TARGETS} ${name} PARENT_SCOPE)
endfunction()

vortex_add_test(test_bom_parser)

vortex_add_test(test_disk_analyzer)
vortex_add_test(test_heuristics)
vortex_add_test(test_deletion_pipeline)
vortex_add_test(test_foundation)
vortex_add_test(test_bundle_inspector)

# The process tests launch a real child from inside a synthetic bundle.
add_executable(process_child process_child.cpp)
vortex_add_test(test_process_inspector)
add_dependencies(test_process_inspector process_child)
target_compile_definitions(test_process_inspector PRIVATE
    VORTEX_TEST_CHILD_PATH="$<TARGET_FILE:process_child>")

# Leak check with Apple's leaks(1): AddressSanitizer on macOS has no leak
# detector, so every suite is re-run under MallocStackLogging and inspected at exit.
#   cmake --build <dir> --target leaks
set(VORTEX_LEAK_COMMANDS "")
foreach(test_target IN LISTS VORTEX_TEST_TARGETS)
    list(APPEND VORTEX_LEAK_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E echo "== leaks: ${test_target}"
        COMMAND ${CMAKE_COMMAND} -E env MallocStackLogging=1
                /usr/bin/leaks --atExit --quiet -- $<TARGET_FILE:${test_target}>)
endforeach()
add_custom_target(leaks ${VORTEX_LEAK_COMMANDS} DEPENDS ${VORTEX_TEST_TARGETS} VERBATIM)

# Manual end-to-end check of the bridge against a disposable fixture application
# (it really removes the fixture's files, so it is not registered with CTest):
#   bridge_fixture_check <Fixture.app> trash|permanent
if(TARGET vortex_bridge)
    add_executable(bridge_fixture_check bridge_fixture_check.mm)
    target_compile_options(bridge_fixture_check PRIVATE -fobjc-arc)
    target_link_libraries(bridge_fixture_check PRIVATE vortex_bridge)
endif()

# A synthetic disk of fictional applications, for `VortexClean --root` and for the
# published screenshots (see tools/capture_screenshots.sh).
add_executable(vortex_make_demo_disk ${CMAKE_SOURCE_DIR}/tools/make_demo_disk.mm)
target_compile_options(vortex_make_demo_disk PRIVATE -fobjc-arc)
target_include_directories(vortex_make_demo_disk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vortex_make_demo_disk PRIVATE vortex_core
    "-framework Foundation" "-framework CoreGraphics" "-framework ImageIO")
