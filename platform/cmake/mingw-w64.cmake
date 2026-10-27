# Cross-compile for Windows x64 with MinGW-w64 (apt install mingw-w64 / brew install mingw-w64).
#   cmake -S . -B build/windows-mingw -G Ninja -DCMAKE_TOOLCHAIN_FILE=platform/cmake/mingw-w64.cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(RTS_MINGW_PREFIX x86_64-w64-mingw32 CACHE STRING "MinGW-w64 target triple")
set(CMAKE_C_COMPILER   ${RTS_MINGW_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${RTS_MINGW_PREFIX}-g++)
set(CMAKE_RC_COMPILER  ${RTS_MINGW_PREFIX}-windres)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# A self-contained .exe: no libstdc++/libgcc/winpthread DLLs next to it.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
# Tests and headless run under Wine in CI.
set(CMAKE_CROSSCOMPILING_EMULATOR wine CACHE STRING "")
