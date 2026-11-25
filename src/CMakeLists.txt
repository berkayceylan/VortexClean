# ---------------------------------------------------------------------------
# vortex_bridge — thread-safe adapters between the C++ engine and Cocoa.
# ---------------------------------------------------------------------------
add_library(vortex_bridge STATIC
    bridge/VCCommandLine.cpp
    bridge/VCEngine.mm
    bridge/VCModels.mm
    bridge/VCSystemHooks.mm)
target_include_directories(vortex_bridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/bridge)
# ARC for every Objective-C++ translation unit (the plain C++ ones are unaffected).
target_compile_options(vortex_bridge PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_link_libraries(vortex_bridge PUBLIC
    vortex_core
    "-framework Foundation"
    "-framework AppKit")

# ---------------------------------------------------------------------------
# VortexClean.app — the programmatic AppKit interface. No .xib, no storyboard.
# ---------------------------------------------------------------------------
set(VORTEX_BUNDLE_IDENTIFIER "com.berkayceylan.VortexClean")
set(VORTEX_ICON_FILE "")

add_executable(VortexClean MACOSX_BUNDLE
    ui/AppDelegate.mm
    ui/DetailViewController.mm
    ui/MainWindowController.mm
    ui/SidebarViewController.mm
    ui/StorageBreakdownView.mm
    ui/VCDetailViews.mm
    ui/VCFormat.mm
    ui/main.mm)
target_include_directories(VortexClean PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ui)
target_compile_options(VortexClean PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_link_libraries(VortexClean PRIVATE
    vortex_bridge
    "-framework AppKit"
    "-framework ImageIO"
    "-framework Quartz"
    "-framework QuartzCore"
    "-framework UniformTypeIdentifiers")

# ---- Application icon: rendered with Core Graphics by a build-time tool, so
# ---- the repository carries no binary assets.
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/make_icon.mm)
    add_executable(vortex_make_icon ${CMAKE_SOURCE_DIR}/tools/make_icon.mm)
    target_compile_options(vortex_make_icon PRIVATE -fobjc-arc)
    target_link_libraries(vortex_make_icon PRIVATE
        "-framework AppKit" "-framework ImageIO" "-framework UniformTypeIdentifiers")
    set(VORTEX_ICON_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/VortexClean.icns)
    add_custom_command(
        OUTPUT ${VORTEX_ICON_OUTPUT}
        COMMAND vortex_make_icon ${VORTEX_ICON_OUTPUT}
        DEPENDS vortex_make_icon
        COMMENT "Rendering VortexClean.icns"
        VERBATIM)
    set_source_files_properties(${VORTEX_ICON_OUTPUT} PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources GENERATED TRUE)
    target_sources(VortexClean PRIVATE ${VORTEX_ICON_OUTPUT})
    set(VORTEX_ICON_FILE "VortexClean")
endif()

set_target_properties(VortexClean PROPERTIES
    OUTPUT_NAME "VortexClean"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/resources/Info.plist.in)

# ---- Ad-hoc signature. "-" needs no certificate, so the bundle builds and runs
# ---- on any Mac with only the Command Line Tools installed. The second command
# ---- fails the build if the result does not verify.
add_custom_command(TARGET VortexClean POST_BUILD
    COMMAND /usr/bin/codesign --force --sign - --timestamp=none
            --identifier ${VORTEX_BUNDLE_IDENTIFIER} $<TARGET_BUNDLE_DIR:VortexClean>
    COMMAND /usr/bin/codesign --verify --strict $<TARGET_BUNDLE_DIR:VortexClean>
    COMMENT "Ad-hoc signing VortexClean.app"
    VERBATIM)
