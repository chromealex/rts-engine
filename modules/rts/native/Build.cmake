# Native module authors call this from <project>/native/CMakeLists.txt.
function(rts_native_module)
    cmake_parse_arguments(M "" "ID;VERSION;ENTRY" "SOURCES" ${ARGN})
    if(M_UNPARSED_ARGUMENTS OR NOT M_ID MATCHES "^[a-z][a-z0-9_]*$" OR
       NOT M_ENTRY MATCHES "^[A-Za-z_][A-Za-z0-9_]*$" OR NOT M_SOURCES OR
       NOT M_VERSION MATCHES "^[1-9][0-9]*$" OR M_VERSION GREATER 4294967295)
        message(FATAL_ERROR "rts_native_module requires ID (lowercase identifier), VERSION (uint32), ENTRY (global factory), SOURCES")
    endif()
    get_property(ids GLOBAL PROPERTY RTS_NATIVE_IDS)
    get_property(entries GLOBAL PROPERTY RTS_NATIVE_ENTRIES)
    if(M_ID IN_LIST ids OR M_ENTRY IN_LIST entries)
        message(FATAL_ERROR "Duplicate native module ID or ENTRY: ${M_ID} / ${M_ENTRY}")
    endif()
    set_property(GLOBAL APPEND PROPERTY RTS_NATIVE_IDS "${M_ID}")
    set_property(GLOBAL APPEND PROPERTY RTS_NATIVE_ENTRIES "${M_ENTRY}")
    add_library(rts_native_${M_ID} OBJECT ${M_SOURCES})
    target_include_directories(rts_native_${M_ID} PRIVATE "${RTS_NATIVE_ENGINE_ROOT}")
    target_compile_features(rts_native_${M_ID} PRIVATE cxx_std_20)
    set_target_properties(rts_native_${M_ID} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_sources(rts_game PRIVATE $<TARGET_OBJECTS:rts_native_${M_ID}>)
    set_property(GLOBAL APPEND_STRING PROPERTY RTS_NATIVE_DECLS
        "std::unique_ptr<rts::game::NativeModule> ${M_ENTRY}();\n")
    set_property(GLOBAL APPEND_STRING PROPERTY RTS_NATIVE_DEFS
        "    result.push_back({\"${M_ID}\", ${M_VERSION}u, &::${M_ENTRY}});\n")
endfunction()
set(RTS_NATIVE_ENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(RTS_NATIVE_DIR "" CACHE PATH "Native module source directory; empty uses <content>/native")
set(_native_dir "${RTS_NATIVE_DIR}")
if(NOT _native_dir)
    set(_native_dir "${RTS_CONTENT_DIR}/native")
    # Exported builds keep sources beside the engine; Android content is in assets/.
    if(NOT EXISTS "${_native_dir}/CMakeLists.txt" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/native/CMakeLists.txt")
        set(_native_dir "${CMAKE_CURRENT_SOURCE_DIR}/native")
    endif()
elseif(NOT EXISTS "${_native_dir}/CMakeLists.txt")
    message(FATAL_ERROR "RTS_NATIVE_DIR has no CMakeLists.txt: ${_native_dir}")
endif()
if(EXISTS "${_native_dir}/CMakeLists.txt")
    add_subdirectory("${_native_dir}" "${CMAKE_CURRENT_BINARY_DIR}/native")
endif()
get_property(_decls GLOBAL PROPERTY RTS_NATIVE_DECLS)
get_property(_defs GLOBAL PROPERTY RTS_NATIVE_DEFS)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gen/RtsNativeRegistry.cpp" CONTENT
"#include \"modules/rts/RtsNative.h\"\n${_decls}
namespace rts::game {
std::vector<NativeDefinition> compiledNativeModules() {
    std::vector<NativeDefinition> result;
${_defs}    return result;
}
}")
target_sources(rts_game PRIVATE modules/rts/RtsNative.cpp "${CMAKE_CURRENT_BINARY_DIR}/gen/RtsNativeRegistry.cpp")
