# The launcher: one exe a player runs to build and play a game from their own disc (Windows), and launcher_cli, its
# steps from a terminal (any platform, for testing).
#   cmake -S sdk/launcher -B build/launcher -DLAUNCHER_GAME=<the game's launcher folder, with game.json>
cmake_minimum_required(VERSION 3.25)
project(xbe_launcher CXX C)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LAUNCHER_GAME "" CACHE PATH "the game's launcher folder: game.json, toolchain.json, icon.ico")
if(NOT EXISTS "${LAUNCHER_GAME}/game.json")
    message(FATAL_ERROR "LAUNCHER_GAME must name a folder holding game.json")
endif()
get_filename_component(SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

include(FetchContent)
# json, zip and xz, each pinned by hash (MIT, MIT, 0BSD; listed in THIRD_PARTY.txt)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp
    URL_HASH SHA256=aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63
    DOWNLOAD_NO_EXTRACT TRUE)
FetchContent_Declare(miniz
    URL https://github.com/richgel999/miniz/releases/download/3.1.2/miniz-3.1.2.zip
    URL_HASH SHA256=f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a)
FetchContent_Declare(xz_embedded
    URL https://github.com/tukaani-project/xz-embedded/archive/refs/tags/v2024-12-30.tar.gz
    URL_HASH SHA256=ee12fa8c49c9c0ef4a144af4234d2530d786c1ce14247a7d5fc92a946628977d)
FetchContent_GetProperties(nlohmann_json)
if(NOT nlohmann_json_POPULATED)
    FetchContent_Populate(nlohmann_json)
endif()
FetchContent_GetProperties(miniz)
if(NOT miniz_POPULATED)
    FetchContent_Populate(miniz)
endif()
FetchContent_GetProperties(xz_embedded)
if(NOT xz_embedded_POPULATED)
    FetchContent_Populate(xz_embedded)
endif()
# json.hpp as nlohmann/json.hpp
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/include/nlohmann)
configure_file(${nlohmann_json_SOURCE_DIR}/json.hpp ${CMAKE_BINARY_DIR}/include/nlohmann/json.hpp COPYONLY)

set(XZ ${xz_embedded_SOURCE_DIR}/linux/lib/xz)
add_library(launcher_core STATIC
    core/archive.cpp core/builder.cpp core/game.cpp core/hash.cpp core/music.cpp core/net.cpp core/plan.cpp
    ${SDK_ROOT}/runtime/kernel/disc.cpp
    ${miniz_SOURCE_DIR}/miniz.c
    ${XZ}/xz_crc32.c ${XZ}/xz_crc64.c ${XZ}/xz_dec_lzma2.c ${XZ}/xz_dec_stream.c ${XZ}/xz_dec_bcj.c)
target_include_directories(launcher_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR} ${SDK_ROOT}/runtime ${CMAKE_BINARY_DIR}/include ${miniz_SOURCE_DIR}
    ${xz_embedded_SOURCE_DIR}/linux/include/linux ${xz_embedded_SOURCE_DIR}/userspace)
target_compile_definitions(launcher_core PUBLIC XZ_USE_CRC64 XZ_DEC_X86 XZ_DEC_CONCATENATED MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
set_source_files_properties(${XZ}/xz_crc32.c ${XZ}/xz_crc64.c ${XZ}/xz_dec_lzma2.c ${XZ}/xz_dec_stream.c
    ${XZ}/xz_dec_bcj.c PROPERTIES COMPILE_OPTIONS "-include;${xz_embedded_SOURCE_DIR}/userspace/xz_config.h")

if(WIN32)
    # the launcher itself: a window (Dear ImGui on Direct3D 11), WinHTTP, the game's json built in
    FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.0 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(imgui)
    file(READ ${LAUNCHER_GAME}/game.json GAME_JSON)
    file(READ ${LAUNCHER_GAME}/toolchain.json TOOLCHAIN_JSON)
    configure_file(win/embedded.h.in ${CMAKE_BINARY_DIR}/include/embedded.h @ONLY)
    set(ICON "${LAUNCHER_GAME}/icon.ico")
    if(EXISTS ${ICON})
        file(WRITE ${CMAKE_BINARY_DIR}/launcher.rc "1 ICON \"${ICON}\"\n")
        set(RC ${CMAKE_BINARY_DIR}/launcher.rc)
    endif()
    file(READ ${LAUNCHER_GAME}/game.json GJ)
    string(JSON SHORT GET "${GJ}" short)
    add_executable(launcher WIN32 win/main.cpp win/platform_win.cpp ${RC}
        ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
    set_target_properties(launcher PROPERTIES OUTPUT_NAME "${SHORT}Launcher")
    target_include_directories(launcher PRIVATE ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_compile_definitions(launcher PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(launcher PRIVATE launcher_core winhttp d3d11 dxgi shell32 ole32 comdlg32 dwmapi)
    if(MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # one exe, nothing beside it: the C++ runtime linked in
        set_property(TARGET launcher launcher_core PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    endif()
else()
    add_executable(launcher_cli test/cli.cpp test/posix.cpp)
    target_link_libraries(launcher_cli PRIVATE launcher_core)
endif()
