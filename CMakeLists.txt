cmake_minimum_required(VERSION 3.22)
project(RTSEngine VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Platform detection
# ---------------------------------------------------------------------------
if(EMSCRIPTEN)
    set(RTS_PLATFORM web)
elseif(ANDROID)
    set(RTS_PLATFORM android)
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(RTS_PLATFORM ios)
elseif(APPLE)
    set(RTS_PLATFORM macos)
elseif(WIN32)
    set(RTS_PLATFORM windows)
else()
    set(RTS_PLATFORM linux)
endif()
message(STATUS "RTSEngine platform: ${RTS_PLATFORM}")

set(RTS_DESKTOP OFF)
if(RTS_PLATFORM MATCHES "^(macos|windows|linux)$")
    set(RTS_DESKTOP ON)
endif()

option(RTS_BUILD_TESTS "Build unit tests" ${RTS_DESKTOP})
option(RTS_BUILD_HEADLESS "Build headless runner" ${RTS_DESKTOP})
option(RTS_BUILD_CLIENT "Build the game client" ON)
option(RTS_BUILD_EDITOR "Build the map editor" ${RTS_DESKTOP})
option(RTS_BUILD_TOOLS "Build rtsbuild packager" ${RTS_DESKTOP})

# Game being built (rtsbuild overrides these).
set(RTS_CONTENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/games/skirmish" CACHE PATH "Game content folder (contains game.json)")
set(RTS_ENGINE_ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/engine/assets")
set(RTS_GAME_NAME "RTSEngine Game" CACHE STRING "Display name")
set(RTS_GAME_ID "com.rtsengine.game" CACHE STRING "Bundle / application id")
set(RTS_GAME_VERSION "0.1.0" CACHE STRING "Version string")
set(RTS_GAME_BUILD "1" CACHE STRING "Build number")
set(RTS_GAME_EXECUTABLE "RTSGame" CACHE STRING "Executable / bundle file name")
set(RTS_APPLE_TEAM_ID "" CACHE STRING "Apple development team id (iOS signing)")
set(RTS_GEN_DIR "" CACHE PATH "Generated packaging assets from rtsbuild (icons)")

if(MSVC)
    add_compile_options(/W4 /permissive- /fp:precise /utf-8 /wd4100 /wd4505)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -fno-fast-math)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>)
    if(WIN32)
        add_compile_options(-Wno-cast-function-type) # GetProcAddress casts are intended
    endif()
endif()

# ---------------------------------------------------------------------------
# Core: deterministic simulation (no platform code, no float in gameplay)
# ---------------------------------------------------------------------------
add_library(rts_engine STATIC
    engine/core/Fixed.cpp
    engine/data/Json.cpp
    engine/sim/Simulation.cpp
    engine/sim/Replay.cpp)
target_include_directories(rts_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Generic data-driven RTS gameplay (units, orders, economy, combat, garrisons, AI)
add_library(rts_game STATIC
    modules/rts/RtsData.cpp
    modules/rts/RtsMap.cpp
    modules/rts/RtsModule.cpp
    modules/rts/RtsEconomy.cpp
    modules/rts/RtsUnits.cpp
    modules/rts/RtsCombat.cpp
    modules/rts/RtsWorld.cpp
    modules/rts/RtsTriggers.cpp
    modules/rts/RtsAI.cpp)
target_link_libraries(rts_game PUBLIC rts_engine)

# Localization: string tables, plurals, language choice (presentation only, no sim dependency).
# RtsLocalize maps RTS data (types, resources, maps...) to string keys.
add_library(rts_i18n STATIC
    engine/i18n/Localization.cpp
    engine/i18n/GameLanguage.cpp
    engine/i18n/SystemLanguage.cpp
    modules/rts/RtsLocalize.cpp)
target_link_libraries(rts_i18n PUBLIC rts_game)
if(APPLE)
    target_link_libraries(rts_i18n PUBLIC "-framework CoreFoundation")
endif()

# ---------------------------------------------------------------------------
# Runtime: platform layer + RHI backend + renderer + Dear ImGui
# ---------------------------------------------------------------------------
add_library(rts_imgui STATIC
    third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_demo.cpp)
target_include_directories(rts_imgui PUBLIC third_party/imgui)
if(NOT MSVC)
    target_compile_options(rts_imgui PRIVATE -w)
endif()

set(RTS_RUNTIME_SRC
    engine/platform/PlatformCommon.cpp
    engine/render/Renderer3D.cpp
    engine/render/ImGuiLayer.cpp)

if(RTS_PLATFORM STREQUAL "macos")
    list(APPEND RTS_RUNTIME_SRC engine/platform/macos/PlatformMac.mm engine/gfx/metal/GfxMetal.mm)
elseif(RTS_PLATFORM STREQUAL "ios")
    list(APPEND RTS_RUNTIME_SRC engine/platform/ios/PlatformIOS.mm engine/gfx/metal/GfxMetal.mm)
elseif(RTS_PLATFORM STREQUAL "android")
    list(APPEND RTS_RUNTIME_SRC engine/platform/android/PlatformAndroid.cpp engine/gfx/gl/GfxGL.cpp)
elseif(RTS_PLATFORM STREQUAL "web")
    list(APPEND RTS_RUNTIME_SRC engine/platform/web/PlatformWeb.cpp engine/gfx/gl/GfxGL.cpp)
elseif(RTS_PLATFORM STREQUAL "windows")
    list(APPEND RTS_RUNTIME_SRC engine/platform/windows/PlatformWin32.cpp engine/gfx/gl/GfxGL.cpp)
else()
    list(APPEND RTS_RUNTIME_SRC engine/platform/linux/PlatformLinux.cpp engine/gfx/gl/GfxGL.cpp)
endif()

# The platform files define main(); they are compiled into each executable through an OBJECT
# library so that the entry point lands in the final binary on every toolchain.
add_library(rts_runtime OBJECT ${RTS_RUNTIME_SRC})
target_link_libraries(rts_runtime PUBLIC rts_engine rts_imgui)
if(APPLE)
    set_source_files_properties(engine/platform/macos/PlatformMac.mm engine/platform/ios/PlatformIOS.mm
        engine/gfx/metal/GfxMetal.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_link_libraries(rts_runtime PUBLIC "-framework Metal" "-framework QuartzCore" "-framework Foundation")
    if(RTS_PLATFORM STREQUAL "macos")
        target_link_libraries(rts_runtime PUBLIC "-framework Cocoa")
    else()
        target_link_libraries(rts_runtime PUBLIC "-framework UIKit")
    endif()
elseif(RTS_PLATFORM STREQUAL "android")
    set(GLUE ${ANDROID_NDK}/sources/android/native_app_glue)
    add_library(native_app_glue STATIC ${GLUE}/android_native_app_glue.c)
    target_include_directories(native_app_glue PUBLIC ${GLUE})
    target_link_libraries(rts_runtime PUBLIC native_app_glue android EGL GLESv3 log)
elseif(RTS_PLATFORM STREQUAL "web")
    target_link_options(rts_runtime PUBLIC -sUSE_WEBGL2=1 -sMAX_WEBGL_VERSION=2 -sMIN_WEBGL_VERSION=2 -sALLOW_MEMORY_GROWTH=1)
elseif(RTS_PLATFORM STREQUAL "windows")
    target_link_libraries(rts_runtime PUBLIC opengl32 user32 gdi32 shell32)
else()
    target_link_libraries(rts_runtime PUBLIC X11 EGL GLESv2)
endif()

add_library(rts_game_view STATIC modules/rts/view/RtsView.cpp)
target_link_libraries(rts_game_view PUBLIC rts_game rts_imgui)

# Helper: package content + platform metadata around an executable.
function(rts_package_app target)
    if(RTS_PLATFORM STREQUAL "macos" OR RTS_PLATFORM STREQUAL "ios")
        set_target_properties(${target} PROPERTIES
            MACOSX_BUNDLE TRUE
            OUTPUT_NAME "${RTS_GAME_EXECUTABLE}"
            MACOSX_BUNDLE_BUNDLE_NAME "${RTS_GAME_NAME}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${RTS_GAME_ID}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${RTS_GAME_BUILD}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${RTS_GAME_VERSION}"
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${RTS_GAME_ID}")
        if(RTS_PLATFORM STREQUAL "ios" AND RTS_GEN_DIR AND EXISTS "${RTS_GEN_DIR}/Assets.xcassets")
            target_sources(${target} PRIVATE "${RTS_GEN_DIR}/Assets.xcassets")
            set_source_files_properties("${RTS_GEN_DIR}/Assets.xcassets" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
            set_target_properties(${target} PROPERTIES XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon")
        endif()
        if(RTS_PLATFORM STREQUAL "macos" AND RTS_GEN_DIR AND EXISTS "${RTS_GEN_DIR}/AppIcon.icns")
            target_sources(${target} PRIVATE "${RTS_GEN_DIR}/AppIcon.icns")
            set_source_files_properties("${RTS_GEN_DIR}/AppIcon.icns" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        endif()
        if(RTS_PLATFORM STREQUAL "ios")
            set_target_properties(${target} PROPERTIES
                MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/platform/ios/Info.plist.in"
                XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
                XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "15.0"
                XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
                XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${RTS_APPLE_TEAM_ID}")
            set(res_dir "$<TARGET_BUNDLE_DIR:${target}>")
        else()
            set_target_properties(${target} PROPERTIES
                MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/Info.plist.in")
            set(res_dir "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Resources")
        endif()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${res_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${RTS_CONTENT_DIR}" "${res_dir}/content"
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${RTS_ENGINE_ASSETS}" "${res_dir}/content/engine"
            COMMENT "Bundling content")
    elseif(RTS_PLATFORM STREQUAL "web")
        set_target_properties(${target} PROPERTIES OUTPUT_NAME "index" SUFFIX ".html")
        target_link_options(${target} PRIVATE
            "--preload-file=${RTS_CONTENT_DIR}@/content"
            "--preload-file=${RTS_ENGINE_ASSETS}@/content/engine"
            "--shell-file=${CMAKE_CURRENT_SOURCE_DIR}/platform/web/shell.html")
    elseif(RTS_PLATFORM STREQUAL "windows" OR RTS_PLATFORM STREQUAL "linux")
        set_target_properties(${target} PROPERTIES OUTPUT_NAME "${RTS_GAME_EXECUTABLE}")
        if(WIN32)
            set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE TRUE)
        endif()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${RTS_CONTENT_DIR}" "$<TARGET_FILE_DIR:${target}>/content"
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${RTS_ENGINE_ASSETS}" "$<TARGET_FILE_DIR:${target}>/content/engine"
            COMMENT "Copying content next to the executable")
    endif()
endfunction()

if(RTS_BUILD_CLIENT)
    if(RTS_PLATFORM STREQUAL "android")
        add_library(main SHARED apps/client/main.cpp $<TARGET_OBJECTS:rts_runtime>)
        target_link_libraries(main PRIVATE rts_runtime rts_game_view rts_i18n)
        # Keep ANativeActivity_onCreate from the glue library.
        set_target_properties(main PROPERTIES LINK_FLAGS "-u ANativeActivity_onCreate")
    else()
        add_executable(rts_client apps/client/main.cpp $<TARGET_OBJECTS:rts_runtime>)
        target_link_libraries(rts_client PRIVATE rts_runtime rts_game_view rts_i18n)
        rts_package_app(rts_client)
    endif()
endif()

if(RTS_BUILD_EDITOR)
    add_executable(rts_editor apps/editor/main.cpp apps/editor/MapDoc.cpp apps/editor/Editor.cpp $<TARGET_OBJECTS:rts_runtime>)
    target_link_libraries(rts_editor PRIVATE rts_runtime rts_game_view)
    target_compile_definitions(rts_editor PRIVATE RTS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    if(APPLE)
        set_target_properties(rts_editor PROPERTIES MACOSX_BUNDLE TRUE OUTPUT_NAME "RTSEditor"
            MACOSX_BUNDLE_BUNDLE_NAME "RTS Editor" MACOSX_BUNDLE_GUI_IDENTIFIER "com.rtsengine.editor"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/Info.plist.in")
        # engine assets (UI font) inside the bundle, so the editor works when moved elsewhere
        add_custom_command(TARGET rts_editor POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${RTS_ENGINE_ASSETS}" "$<TARGET_BUNDLE_CONTENT_DIR:rts_editor>/Resources/content/engine")
    elseif(WIN32)
        set_target_properties(rts_editor PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
endif()

if(RTS_BUILD_TOOLS)
    add_executable(rtsbuild tools/rtsbuild/main.cpp)
    target_link_libraries(rtsbuild PRIVATE rts_engine)
    target_compile_definitions(rtsbuild PRIVATE RTS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
endif()

if(RTS_BUILD_TOOLS)
    add_executable(rts_i18n_tool tools/i18n/main.cpp)
    set_target_properties(rts_i18n_tool PROPERTIES OUTPUT_NAME rts_i18n)
    target_link_libraries(rts_i18n_tool PRIVATE rts_i18n)
    target_compile_definitions(rts_i18n_tool PRIVATE RTS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
endif()

if(RTS_BUILD_HEADLESS)
    add_executable(rts_headless apps/headless/main.cpp)
    target_link_libraries(rts_headless PRIVATE rts_game)
endif()

if(RTS_BUILD_TESTS)
    enable_testing()
    add_executable(rts_tests tests/tests.cpp)
    target_link_libraries(rts_tests PRIVATE rts_game rts_i18n)
    target_compile_definitions(rts_tests PRIVATE RTS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME rts_tests COMMAND rts_tests)
endif()
