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
pkg_check_modules(DRM REQUIRED IMPORTED_TARGET libdrm)

# EGL：dmabuf -> EGLImage -> GL 纹理，以及 surfaceless 导入自检
pkg_check_modules(EGL QUIET IMPORTED_TARGET egl)

# Wayland：原生窗口显示（不使用 GLFW，避免 XWayland 落到软件渲染）
pkg_check_modules(WAYLAND QUIET IMPORTED_TARGET wayland-client wayland-egl)

# X11：供纯 X 环境使用（可用 XWayland 验证）
pkg_check_modules(X11 QUIET IMPORTED_TARGET x11)

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
    src/vaapi_dmabuf.c
    src/vaapi_vpp.c
    src/renderer.c
    third_party/glad/glad.c
)

if(EGL_FOUND)
    list(APPEND VAAPI_DEMO_SOURCES src/egl_interop.c src/egl_headless.c)
endif()

set_source_files_properties(third_party/glad/glad.c PROPERTIES COMPILE_OPTIONS "-w")

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
    PkgConfig::DRM
    m
    pthread
    ${CMAKE_DL_LIBS}
)

if(EGL_FOUND)
    target_compile_definitions(vaapi_demo PRIVATE HAVE_EGL=1)
    target_link_libraries(vaapi_demo PRIVATE PkgConfig::EGL)
    message(STATUS "找到 EGL: 已启用 dmabuf 导入与 surfaceless 自检")
else()
    message(STATUS "未找到 EGL: 不编译 dmabuf 导入 (需要时安装 libegl-dev 后重新编译)")
endif()

if(EGL_FOUND AND WAYLAND_FOUND)
    target_compile_definitions(vaapi_demo PRIVATE HAVE_WAYLAND=1)
    target_link_libraries(vaapi_demo PRIVATE PkgConfig::WAYLAND)
    message(STATUS "找到 wayland-client/wayland-egl: 启用 Wayland 窗口后端")
endif()

if(EGL_FOUND AND X11_FOUND)
    target_compile_definitions(vaapi_demo PRIVATE HAVE_X11=1)
    target_link_libraries(vaapi_demo PRIVATE PkgConfig::X11)
    message(STATUS "找到 X11: 启用 X11 窗口后端")
endif()

if(EGL_FOUND AND (WAYLAND_FOUND OR X11_FOUND))
    target_compile_definitions(vaapi_demo PRIVATE HAVE_DISPLAY=1)
else()
    message(STATUS "缺少 Wayland/X11 开发库: 不编译窗口播放 "
                   "(需要时安装 libwayland-dev 或 libx11-dev + libegl-dev 后重新编译)")
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
