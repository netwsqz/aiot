# PC LVGL 测试工程 — 用 SDL2 在 Windows 主机上跑项目 UI 源码，免实机烧录。
#
# 不走 ESP-IDF：用本机 gcc (D:\mingw64) + CMake + Ninja。
# LVGL 取自主仓 managed_components/lvgl__lvgl（与固件同版本 v9.5.0，由
# `idf.py build` 生成）。SDL2 取自本机解压的 MinGW devel 包。
#
# 用法（从主仓根运行）：
#   cmake -S tools/pc_sim -B tools/pc_sim/build -G Ninja \
#         -DSDL2_ROOT=<SDL2-2.30.12>/x86_64-w64-mingw32
#   cmake --build tools/pc_sim/build
#   tools/pc_sim/build/aiot_sim.exe   (SDL2.dll 已 post-copy 到同目录)

cmake_minimum_required(VERSION 3.16)
project(aiot_sim C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# ---- SDL2 (本机 MinGW devel 包，非系统安装) ----
# SDL2_ROOT 指向 devel 包的 x86_64-w64-mingw32 目录（含 include/SDL2, lib, bin）。
# 调用时通过 -DSDL2_ROOT=<path> 传入，例如
#   cmake -S tools/pc_sim -B tools/pc_sim/build -G Ninja \
#         -DSDL2_ROOT=E:/SDL2-2.30.12/x86_64-w64-mingw32
# 落到 $CLAUDE_JOB_DIR 缓存的默认值仅本机有效，别人必须自行指定 -DSDL2_ROOT。
if(DEFINED ENV{CLAUDE_JOB_DIR} AND NOT SDL2_ROOT)
    set(SDL2_ROOT "$ENV{CLAUDE_JOB_DIR}/tmp/sdl2dl/SDL2-2.30.12/x86_64-w64-mingw32"
        CACHE PATH "SDL2-devel MinGW root")
endif()

if(NOT EXISTS "${SDL2_ROOT}/include/SDL2/SDL.h")
    message(FATAL_ERROR
        "SDL2 headers not found under SDL2_ROOT='${SDL2_ROOT}'.\n"
        "Set -DSDL2_ROOT=<path-to-SDL2-2.30.12>/x86_64-w64-mingw32")
endif()

# SDL2 自带 cmake config（lib/cmake/SDL2/SDL2Config.cmake）
find_package(SDL2 CONFIG REQUIRED HINTS "${SDL2_ROOT}/lib/cmake/SDL2")
message(STATUS "SDL2 found: ${SDL2_LIBRARIES}")

# ---- LVGL (项目 managed_components，子目录编译) ----
# lv_conf.h 放在 test_pc/ 根目录（LVGL desktop cmake 默认从这里找）。
set(LV_BUILD_CONF_PATH  "" CACHE PATH "" FORCE)   # 不用 LV_CONF_PATH 宏
set(LV_BUILD_LVGL_H_SIMPLE_INCLUDE ON CACHE BOOL "" FORCE)
set(LV_BUILD_USE_KCONFIG OFF CACHE BOOL "" FORCE)
set(LV_BUILD_SET_CONFIG_OPTS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)

# LVGL 源码路径：直接相对主仓根解析。tools/pc_sim 在主仓内，
# 主仓根 = CMAKE_SOURCE_DIR/../.. ，managed_components 由 idf.py 生成（gitignored）。
# 先跑一次 `idf.py build` 让 LVGL 被 fetch 下来，本工程才能编。
get_filename_component(_LVGL_SRC
    "${CMAKE_SOURCE_DIR}/../../managed_components/lvgl__lvgl"
    ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
if(NOT EXISTS "${_LVGL_SRC}/CMakeLists.txt")
    message(FATAL_ERROR
        "LVGL source not found at ${_LVGL_SRC}.\n"
        "Run `idf.py build` in the repo root once to fetch managed_components, "
        "or point LVGL_SRC manually.")
endif()
message(STATUS "LVGL source: ${_LVGL_SRC}")

# 源码/头文件相对路径解析。tools/pc_sim 在主仓内，
# 主仓根 = CMAKE_SOURCE_DIR/../.. ，hint 相对该根解析。
function(pc_resolve out_var hint)
    get_filename_component(_p "${hint}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}/../..")
    if(EXISTS "${_p}")
        set(${out_var} "${_p}" PARENT_SCOPE)
        return()
    endif()
    message(FATAL_ERROR "Path not found: '${hint}'\n  tried: ${_p}")
endfunction()

add_subdirectory(${_LVGL_SRC} ${CMAKE_BINARY_DIR}/lvgl_build)

# LVGL 的 SDL driver 源码 include "SDL2/SDL.h"，需要 SDL2 的 include 目录。
# find_package(SDL2) 给的是 imported target，不会自动渗透到子目录的 lvgl target，
# 这里显式把 include 目录加到 lvgl 上。
target_include_directories(lvgl PUBLIC ${SDL2_ROOT}/include)

# LVGL 的 ASM glob 把 ARM Helium .S 也收进来了，x86-64 上编不过且不需要。
# 直接从 lvgl target 的 SOURCES 里过滤掉这条 .S（HEADER_FILE_ONLY 对 ASM 不可靠）。
get_target_property(_lv_srcs lvgl SOURCES)
list(FILTER _lv_srcs EXCLUDE REGEX "lv_blend_helium\\.S$")
set_target_properties(lvgl PROPERTIES SOURCES "${_lv_srcs}")

# ---- 本工程源码 ----
# ESP-IDF shim 头目录（esp_*.h 空实现）+ mock stubs + main
# WIN32 让链接器走 /SUBSYSTEM:WINDOWS（用 SDL2main 的 WinMain→main 桥）。
add_executable(aiot_sim
    src/main.c
    src/mocks.c
)

set(_COMPONENT_DIRS
    ui pet_ui draw_ui snake_ui app_list_ui todo_ui todo_manager timer_ui timer_engine
    weather_ui weather_fetch wifi_ui lvgl_port font_cn port_rtc port_battery)
set(_INC_DIRS "")
foreach(c ${_COMPONENT_DIRS})
    pc_resolve(_d components/${c})
    list(APPEND _INC_DIRS ${_d})
endforeach()
target_include_directories(aiot_sim PRIVATE
    ${CMAKE_SOURCE_DIR}/include          # esp shim 头
    ${CMAKE_SOURCE_DIR}                 # lv_conf.h（LVGL desktop cmake 默认从源码目录找）
    ${_INC_DIRS}
)

# 直接编译项目 UI 源码（不改一行）。
# draw_ui.c 用 fopen/fread 读写 /spiffs/draw.bmp —— PC 无 SPIFFS：
# fopen("/spiffs/...") 在 MinGW 下落到当前盘根目录（如 C:\spiffs\...），
# 无该目录时 wb 失败、rb 失败，DrawUi_HasSavedImage/RenderThumbnail 安全返回 false，
# dashboard 右下角显示 "Image" 文字占位。sim 只验证"能进画板、画线、刷新"，
# 笔迹保存/缩略图降级，不崩。
pc_resolve(_UI_SCREEN  components/ui/ui_screen.c)
pc_resolve(_PET_UI     components/pet_ui/pet_ui.c)
pc_resolve(_FONT_CN    components/font_cn/lv_font_chinese_14.c)
pc_resolve(_DRAW_UI    components/draw_ui/draw_ui.c)
pc_resolve(_SNAKE_UI    components/snake_ui/snake_ui.c)
pc_resolve(_APP_LIST_UI components/app_list_ui/app_list_ui.c)
target_sources(aiot_sim PRIVATE
    ${_UI_SCREEN}
    ${_PET_UI}
    ${_FONT_CN}
    ${_DRAW_UI}
    ${_SNAKE_UI}
    ${_APP_LIST_UI}
)

target_link_libraries(aiot_sim PRIVATE lvgl::lvgl SDL2::SDL2)

# MinGW + SDL2 main 链接惯例
target_compile_definitions(aiot_sim PRIVATE
    SDL_MAIN_HANDLED
    LV_CONF_INCLUDE_SIMPLE
    LV_LVGL_H_INCLUDE_SIMPLE
)

# 用 SDL_MAIN_HANDLED：我们直接用 console 的 main，不接 SDL2main 的 WinMain 桥。
# 因此不链 SDL2main，只链 SDL2（DLL）。
if(WIN32)
    set_target_properties(aiot_sim PROPERTIES LINK_FLAGS "-mconsole")
endif()

# 把 SDL2.dll 拷到 exe 同目录，否则运行时找不到
add_custom_command(TARGET aiot_sim POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SDL2_ROOT}/bin/SDL2.dll"
        $<TARGET_FILE_DIR:aiot_sim>/SDL2.dll)
