cmake_minimum_required(VERSION 3.24)

# Must be set before project(): it selects the SDK deployment target for every target.
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")

project(VortexClean
    VERSION 1.0.0
    DESCRIPTION "Native macOS deep application uninstaller"
    LANGUAGES CXX OBJCXX)

if(NOT APPLE)
    message(FATAL_ERROR "VortexClean is a macOS application and only builds with the macOS SDK.")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (Debug or Release)" FORCE)
endif()

option(ENABLE_SANITIZERS "Instrument everything with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(VORTEX_BUILD_APP "Build the VortexClean.app bundle" ON)
option(VORTEX_BUILD_TESTS "Build the CTest suite" ON)
option(VORTEX_WARNINGS_AS_ERRORS "Fail the build on any compiler warning" OFF)

# ---------------------------------------------------------------------------
# Language standards
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Build profiles
#   Debug   : -g -O0 -Wall -Wextra -Wpedantic   (+ ASan/UBSan with -DENABLE_SANITIZERS=ON)
#   Release : -O3 -flto
# The warning set is applied to every profile so Release is held to the same
# zero-warning standard.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
set(CMAKE_OBJCXX_FLAGS_DEBUG "-g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_OBJCXX_FLAGS_RELEASE "-O3 -DNDEBUG")

add_compile_options(-Wall -Wextra -Wpedantic)
if(VORTEX_WARNINGS_AS_ERRORS)
    add_compile_options(-Werror)
endif()

add_compile_options($<$<CONFIG:Release>:-flto>)
add_link_options($<$<CONFIG:Release>:-flto>)

if(ENABLE_SANITIZERS)
    # -fno-sanitize-recover turns every UBSan finding into a hard failure.
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# vortex_core — the pure C++20 engine (POSIX + CoreFoundation/Security C APIs).
# It never includes AppKit or any Objective-C header.
# ---------------------------------------------------------------------------
add_library(vortex_core STATIC
    src/core/AuditLogger.cpp
    src/core/BomParser.cpp
    src/core/BundleInspector.cpp
    src/core/CFUtil.cpp
    src/core/DeletionEngine.cpp
    src/core/DiskAnalyzer.cpp
    src/core/FileUtil.cpp
    src/core/Json.cpp
    src/core/LaunchControl.cpp
    src/core/MatchEngine.cpp
    src/core/PathPolicy.cpp
    src/core/PathUtil.cpp
    src/core/PrivilegedHelper.cpp
    src/core/ProcessInspector.cpp
    src/core/ReceiptStore.cpp
    src/core/Remnant.cpp
    src/core/RemnantScanner.cpp
    src/core/StringUtil.cpp
    src/core/ThreadPool.cpp)
target_include_directories(vortex_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
target_compile_definitions(vortex_core PUBLIC VORTEX_VERSION="${PROJECT_VERSION}")
# libobjc is only needed for the autorelease-pool entry points used by the
# thread pool; it is a system library that CoreFoundation loads anyway.
target_link_libraries(vortex_core PUBLIC
    "-framework CoreFoundation"
    "-framework CoreServices"
    "-framework Security"
    objc)

# ---------------------------------------------------------------------------
# VortexClean.app — Objective-C++ bridge and programmatic AppKit interface.
# ---------------------------------------------------------------------------
if(VORTEX_BUILD_APP)
    add_subdirectory(src)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(VORTEX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
