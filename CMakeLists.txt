cmake_minimum_required(VERSION 3.16)

project(vaapi_demo
    LANGUAGES C
    VERSION 1.0.0
    DESCRIPTION "VAAPI hardware encode/decode demo"
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---------------------------------------------------------------------------
# 依赖
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)

pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec
    libavformat
    libavutil
    libavfilter
    libswscale
)

pkg_check_modules(VA REQUIRED IMPORTED_TARGET libva)

# glfw3 只用于 play 播放窗口，没有也不影响编解码功能
pkg_check_modules(GLFW QUIET IMPORTED_TARGET glfw3)

# ---------------------------------------------------------------------------
# 目标
# ---------------------------------------------------------------------------
set(VAAPI_DEMO_SOURCES
    src/main.c
    src/util.c
    src/vaapi_common.c
    src/vaapi_encoder.c
    src/vaapi_decoder.c
    src/vaapi_info.c
    src/vaapi_vpp.c
    src/renderer.c
)

if(GLFW_FOUND)
    list(APPEND VAAPI_DEMO_SOURCES third_party/glad/glad.c)
    set_source_files_properties(third_party/glad/glad.c PROPERTIES COMPILE_OPTIONS "-w")
endif()

add_executable(vaapi_demo ${VAAPI_DEMO_SOURCES})

target_include_directories(vaapi_demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/glad
)

target_compile_options(vaapi_demo PRIVATE -Wall -Wextra -Wno-unused-parameter)

target_compile_definitions(vaapi_demo PRIVATE
    VAAPI_DEMO_VERSION="${PROJECT_VERSION}"
    VAAPI_DEMO_SHARE_DIR="${CMAKE_INSTALL_PREFIX}/share/vaapi_demo"
)

target_link_libraries(vaapi_demo PRIVATE
    PkgConfig::FFMPEG
    PkgConfig::VA
    m
    pthread
    ${CMAKE_DL_LIBS}
)

if(GLFW_FOUND)
    target_compile_definitions(vaapi_demo PRIVATE HAVE_GLFW=1)
    target_link_libraries(vaapi_demo PRIVATE PkgConfig::GLFW)
    message(STATUS "找到 glfw3: 已启用 play 播放功能")
else()
    message(STATUS "未找到 glfw3: 不编译播放功能 (需要时执行 sudo apt install libglfw3-dev 后重新编译)")
endif()

# ---------------------------------------------------------------------------
# 安装：make install 后得到一个可直接分发的独立目录
#   <prefix>/bin/vaapi_demo
#   <prefix>/samples/*.yuv|mp4
#   <prefix>/run
#   <prefix>/README.md
# ---------------------------------------------------------------------------
install(TARGETS vaapi_demo RUNTIME DESTINATION bin)
install(DIRECTORY samples/ DESTINATION samples)
install(FILES README.md DESTINATION .)
install(PROGRAMS run DESTINATION .)
