/*
 * renderer.c - EGL + Wayland 显示后端（不再使用 GLFW）
 *
 * 两种显示路径：
 *   1. CPU 上传：系统内存 NV12 -> glTexImage2D；
 *   2. 零拷贝：VAAPI 表面 -> dmabuf -> EGLImage -> GL 纹理（egl_interop）。
 *
 * 为什么不走 GLFW：GLFW 3.3 没有 Wayland 支持，只能走 XWayland，
 * 在部分桌面环境里会落到 llvmpipe 软件渲染，导致 dmabuf 无法跨设备导入。
 * 这里直接用 wayland-client + wayland-egl + EGL，窗口创建与渲染都走原生 Wayland。
 *
 * xdg-shell 协议在 libwayland-client 里没有现成声明（通常由 wayland-scanner 生成），
 * 为了不依赖 wayland-protocols 包，这里手工给出本模块需要的最小声明。
 */
#include "renderer.h"

#include "vaapi_common.h"

#ifdef HAVE_DISPLAY

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <glad/glad.h>

#include "egl_interop.h"
#include "util.h"

#ifndef EGL_PLATFORM_WAYLAND_EXT
#define EGL_PLATFORM_WAYLAND_EXT 0x31D8
#endif

/* ------------------------------------------------------------------------- */
/* xdg-shell 最小协议声明（仅用到 wm_base / surface / toplevel 的基本能力）    */
/* ------------------------------------------------------------------------- */

struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;

static const struct wl_interface xdg_wm_base_interface;
static const struct wl_interface xdg_surface_interface;
static const struct wl_interface xdg_toplevel_interface;

/*
 * wl_proxy_marshal_flags() 需要完整的方法签名表（通常由 wayland-scanner 生成），
 * 这里按 xdg-shell 协议手工给出，仅保留本模块用到的事件。
 */
static const struct wl_message xdg_wm_base_requests[] = {
    { "destroy",           "",   NULL },
    { "create_positioner", "n",  NULL },
    { "get_xdg_surface",   "no", (const struct wl_interface *[]) {
        &xdg_surface_interface, &wl_surface_interface } },
    { "pong",              "u",  NULL },
};

static const struct wl_message xdg_wm_base_events[] = {
    { "ping", "u", NULL },
};

static const struct wl_message xdg_surface_requests[] = {
    { "destroy",               "",   NULL },
    { "get_toplevel",          "n",  (const struct wl_interface *[]) {
        &xdg_toplevel_interface } },
    { "set_parent",            "?o", (const struct wl_interface *[]) {
        &xdg_surface_interface } },
    { "set_title",             "s",  NULL },
    { "set_app_id",            "s",  NULL },
    { "show_window_menu",      "osu", NULL },
    { "move",                  "ou", NULL },
    { "resize",                "ou", NULL },
    { "ack_configure",         "u",  NULL },
    { "get_window_geometry",   "",   NULL },
    { "set_maximized",         "",   NULL },
    { "unset_maximized",       "",   NULL },
    { "set_fullscreen",        "o",  NULL },
    { "unset_fullscreen",      "",   NULL },
    { "set_minimized",         "",   NULL },
};

static const struct wl_message xdg_surface_events[] = {
    { "configure", "u", NULL },
};

static const struct wl_message xdg_toplevel_requests[] = {
    { "destroy",            "",   NULL },
    { "set_parent",         "?o", NULL },
    { "set_title",          "s",  NULL },
    { "set_app_id",         "s",  NULL },
    { "show_window_menu",   "osu", NULL },
    { "move",               "ou", NULL },
    { "resize",             "ou", NULL },
    { "set_max_size",       "ii", NULL },
    { "set_min_size",       "ii", NULL },
    { "set_maximized",      "",   NULL },
    { "unset_maximized",    "",   NULL },
    { "set_fullscreen",     "o",  NULL },
    { "unset_fullscreen",   "",   NULL },
    { "set_minimized",      "",   NULL },
};

static const struct wl_message xdg_toplevel_events[] = {
    { "configure",        "iia", NULL },
    { "close",            "",    NULL },
    { "configure_bounds", "ii",  NULL },
    { "wm_capabilities",  "a",   NULL },
};

static const struct wl_interface xdg_wm_base_interface = {
    "xdg_wm_base", 1, 4, xdg_wm_base_requests, 1, xdg_wm_base_events
};
static const struct wl_interface xdg_surface_interface = {
    "xdg_surface", 1, 14, xdg_surface_requests, 1, xdg_surface_events
};
static const struct wl_interface xdg_toplevel_interface = {
    "xdg_toplevel", 1, 14, xdg_toplevel_requests, 4, xdg_toplevel_events
};

struct xdg_wm_base_listener {
    void (*ping)(void *data, struct xdg_wm_base *base, uint32_t serial);
};

struct xdg_surface_listener {
    void (*configure)(void *data, struct xdg_surface *surface, uint32_t serial);
};

struct xdg_toplevel_listener {
    void (*configure)(void *data, struct xdg_toplevel *toplevel,
                      int32_t width, int32_t height, struct wl_array *states);
    void (*close)(void *data, struct xdg_toplevel *toplevel);
    void (*configure_bounds)(void *data, struct xdg_toplevel *toplevel,
                             int32_t width, int32_t height);
    void (*wm_capabilities)(void *data, struct xdg_toplevel *toplevel,
                            struct wl_array *capabilities);
};

enum {
    XDG_WM_BASE_GET_XDG_SURFACE = 2,
    XDG_WM_BASE_PONG            = 3,
    XDG_SURFACE_GET_TOPLEVEL    = 1,
    XDG_TOPLEVEL_SET_PARENT     = 1,
    XDG_SURFACE_ACK_CONFIGURE   = 8,
    XDG_TOPLEVEL_SET_TITLE      = 2,
};

/* ------------------------------------------------------------------------- */
/* 渲染器                                                                     */
/* ------------------------------------------------------------------------- */

struct Renderer {
    /* Wayland */
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct xdg_wm_base *wm_base;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_egl_window *egl_window;

    /* EGL */
    EGLDisplay egl_display;
    EGLSurface egl_surface;
    EGLContext egl_context;
    EGLConfig egl_config;
    EglInterop *egl;

    /* GL 资源 */
    GLuint vao;
    GLuint vbo;
    GLuint program;
    GLuint texture_y;
    GLuint texture_uv;
    GLint uniform_y;
    GLint uniform_uv;
    GLint uniform_uv_scale;

    int width;
    int height;
    int window_width;
    int window_height;
    int pending_width;
    int pending_height;
    int should_close;
    int frame_presented;
};

static const char *kVertexShader =
    "#version 330 core\n"
    "layout (location = 0) in vec3 pos;\n"
    "layout (location = 1) in vec2 texCoord;\n"
    "out vec2 TexCoord;\n"
    "void main()\n"
    "{\n"
    "    gl_Position = vec4(pos, 1.0);\n"
    "    TexCoord = texCoord;\n"
    "}\n";

static const char *kFragmentShader =
    "#version 330 core\n"
    "layout(location = 0) out vec4 FragColor;\n"
    "in vec2 TexCoord;\n"
    "uniform sampler2D dataY;\n"
    "uniform sampler2D dataUV;\n"
    "uniform vec2 uvScale;\n"
    "const mat4 YUV_TO_RGB_MATRIX =\n"
    "    mat4(\n"
    "        1.1643835616, 0, 1.7927410714, -0.9729450750,\n"
    "        1.1643835616, -0.2132486143, -0.5329093286, 0.3014826655,\n"
    "        1.1643835616, 2.1124017857, 0, -1.1334022179,\n"
    "        0, 0, 0, 1);\n"
    "void main()\n"
    "{\n"
    "    vec2 uv = TexCoord * uvScale;\n"
    "    vec3 yuv;\n"
    "    yuv.x = texture(dataY, uv).r;\n"
    "    yuv.y = texture(dataUV, uv).r;\n"
    "    yuv.z = texture(dataUV, uv).g;\n"
    "    FragColor = vec4(yuv, 1) * YUV_TO_RGB_MATRIX;\n"
    "}\n";

/* ------------------------------------------------------------------------- */
/* Wayland 事件                                                               */
/* ------------------------------------------------------------------------- */

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version)
{
    Renderer *r = data;

    if (strcmp(interface, "wl_compositor") == 0) {
        r->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
    } else if (strcmp(interface, "xdg_wm_base") == 0) {
        r->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface,
                                      version > 1 ? 1 : version);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

static void wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
    (void)data;
    wl_proxy_marshal_flags((struct wl_proxy *)base, XDG_WM_BASE_PONG, NULL, 0, 0, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
    Renderer *r = data;

    /* Mutter 在收到 xdg_toplevel.set_parent 后即接受该 surface，
     * 此处按常规仍尝试 ack；失败也不影响呈现。 */
    wl_proxy_marshal_flags((struct wl_proxy *)surface, XDG_SURFACE_ACK_CONFIGURE,
                           NULL, wl_proxy_get_version((struct wl_proxy *)surface), 0, serial);
    if (r->pending_width > 0 && r->pending_height > 0) {
        r->window_width  = r->pending_width;
        r->window_height = r->pending_height;
        if (r->egl_window)
            wl_egl_window_resize(r->egl_window, r->window_width, r->window_height, 0, 0);
        glViewport(0, 0, r->window_width, r->window_height);
        r->pending_width = r->pending_height = 0;
    }
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                               int32_t width, int32_t height, struct wl_array *states)
{
    Renderer *r = data;

    (void)toplevel;
    (void)states;
    if (width > 0 && height > 0) {
        r->pending_width  = width;
        r->pending_height = height;
    }
}

static void frame_done(void *data, struct wl_callback *callback, uint32_t time)
{
    Renderer *r = data;

    (void)time;
    r->frame_presented = 1;
    wl_callback_destroy(callback);
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    Renderer *r = data;

    (void)toplevel;
    r->should_close = 1;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure        = toplevel_configure,
    .close            = toplevel_close,
    .configure_bounds = NULL,
    .wm_capabilities  = NULL,
};

/* ------------------------------------------------------------------------- */
/* GL 初始化                                                                  */
/* ------------------------------------------------------------------------- */

static GLuint compile_shader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    GLint success = 0;
    char info[512];

    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(shader, sizeof(info), NULL, info);
        LOG_ERROR("着色器编译失败: %s", info);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static int renderer_init_gl(Renderer *r)
{
    static const float vertices[] = {
        1.0f,  1.0f, 0.0f,  1.0f, 0.0f,
        1.0f, -1.0f, 0.0f,  1.0f, 1.0f,
        -1.0f, -1.0f, 0.0f,  0.0f, 1.0f,
        -1.0f,  1.0f, 0.0f,  0.0f, 0.0f
    };
    GLuint vertex_shader, fragment_shader;
    GLint success = 0;
    char info[512];

    vertex_shader   = compile_shader(GL_VERTEX_SHADER, kVertexShader);
    fragment_shader = compile_shader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vertex_shader || !fragment_shader)
        return -1;

    r->program = glCreateProgram();
    glAttachShader(r->program, vertex_shader);
    glAttachShader(r->program, fragment_shader);
    glBindAttribLocation(r->program, 0, "pos");
    glBindAttribLocation(r->program, 1, "texCoord");
    glLinkProgram(r->program);
    glGetProgramiv(r->program, GL_LINK_STATUS, &success);
    if (!success) {
        glGetProgramInfoLog(r->program, sizeof(info), NULL, info);
        LOG_ERROR("着色器链接失败: %s", info);
        return -1;
    }
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);
    glUseProgram(r->program);

    glGenVertexArrays(1, &r->vao);
    glBindVertexArray(r->vao);
    glGenBuffers(1, &r->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    r->uniform_y        = glGetUniformLocation(r->program, "dataY");
    r->uniform_uv       = glGetUniformLocation(r->program, "dataUV");
    r->uniform_uv_scale = glGetUniformLocation(r->program, "uvScale");

    glGenTextures(1, &r->texture_y);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, r->texture_y);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glUniform1i(r->uniform_y, 0);

    glGenTextures(1, &r->texture_uv);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, r->texture_uv);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glUniform1i(r->uniform_uv, 1);

    glUseProgram(0);
    return 0;
}

static int init_egl(Renderer *r)
{
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display;
    EGLint major = 0, minor = 0, count = 0;
    EGLint config_attrs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RED_SIZE,   8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE,  8,
        EGL_ALPHA_SIZE, 0,
        EGL_NONE
    };

    get_platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
        eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get_platform_display) {
        r->egl_display = get_platform_display(EGL_PLATFORM_WAYLAND_EXT, r->display, NULL);
    }
    if (!r->egl_display)
        r->egl_display = eglGetDisplay((EGLNativeDisplayType)r->display);
    if (r->egl_display == EGL_NO_DISPLAY) {
        LOG_ERROR("EGL: 获取 Wayland 显示设备失败");
        return -1;
    }

    if (!eglInitialize(r->egl_display, &major, &minor)) {
        LOG_ERROR("EGL: 初始化失败");
        return -1;
    }
    if (!eglBindAPI(EGL_OPENGL_API)) {
        LOG_ERROR("EGL: 绑定 OpenGL 失败");
        return -1;
    }
    if (!eglChooseConfig(r->egl_display, config_attrs, &r->egl_config, 1, &count) || count == 0) {
        r->egl_config = EGL_NO_CONFIG_KHR;   /* 允许无配置上下文 */
    }

    r->egl_surface = eglCreateWindowSurface(r->egl_display, r->egl_config,
                                            (EGLNativeWindowType)r->egl_window, NULL);
    if (r->egl_surface == EGL_NO_SURFACE) {
        LOG_ERROR("EGL: 创建窗口表面失败");
        return -1;
    }
    r->egl_context = eglCreateContext(r->egl_display, r->egl_config, EGL_NO_CONTEXT, NULL);
    if (r->egl_context == EGL_NO_CONTEXT) {
        LOG_ERROR("EGL: 创建上下文失败");
        return -1;
    }
    if (!eglMakeCurrent(r->egl_display, r->egl_surface, r->egl_surface, r->egl_context)) {
        LOG_ERROR("EGL: makeCurrent 失败");
        return -1;
    }
    if (!gladLoadGLLoader((GLADloadproc)eglGetProcAddress)) {
        LOG_ERROR("加载 OpenGL 函数失败");
        return -1;
    }

    LOG_INFO("GL: %s", glGetString(GL_RENDERER));
    return 0;
}

int renderer_available(void)
{
    return 1;
}

Renderer *renderer_create(int width, int height, const char *title)
{
    Renderer *r;

    r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->width  = width;
    r->height = height;
    r->window_width  = width  > 1280 ? width / 2  : width;
    r->window_height = height > 720  ? height / 2 : height;

    /* 1. 连接 Wayland，拿到 compositor 与 xdg_wm_base */
    r->display = wl_display_connect(NULL);
    if (!r->display) {
        LOG_ERROR("无法连接 Wayland 显示服务器（WAYLAND_DISPLAY 未设置？）");
        free(r);
        return NULL;
    }
    r->registry = wl_display_get_registry(r->display);
    wl_registry_add_listener(r->registry, &registry_listener, r);
    wl_display_roundtrip(r->display);
    if (!r->compositor || !r->wm_base) {
        LOG_ERROR("Wayland 合成器不支持 wl_compositor / xdg_wm_base");
        renderer_destroy(r);
        return NULL;
    }
    wl_proxy_add_listener((struct wl_proxy *)r->wm_base,
                          (void (**)(void))&wm_base_listener, r);

    /* 2. 创建 xdg 窗口（第一次 commit 必须在挂 EGL 窗口之前） */
    r->surface = wl_compositor_create_surface(r->compositor);
    r->xdg_surface = (struct xdg_surface *)wl_proxy_marshal_flags(
        (struct wl_proxy *)r->wm_base, XDG_WM_BASE_GET_XDG_SURFACE,
        &xdg_surface_interface, 1, 0, NULL, r->surface);
    r->toplevel = (struct xdg_toplevel *)wl_proxy_marshal_flags(
        (struct wl_proxy *)r->xdg_surface, XDG_SURFACE_GET_TOPLEVEL,
        &xdg_toplevel_interface, 1, 0, NULL);
    if (!r->surface || !r->xdg_surface || !r->toplevel) {
        LOG_ERROR("创建 xdg 窗口失败");
        renderer_destroy(r);
        return NULL;
    }
    wl_proxy_add_listener((struct wl_proxy *)r->xdg_surface,
                          (void (**)(void))&xdg_surface_listener, r);
    wl_proxy_add_listener((struct wl_proxy *)r->toplevel,
                          (void (**)(void))&toplevel_listener, r);
    wl_proxy_marshal_flags((struct wl_proxy *)r->toplevel, 1,
                           NULL, 0, 0, NULL);
    /* Mutter 需要先收到 set_parent（可为空）才会接受后续的 configure/ack */
    wl_proxy_marshal_flags((struct wl_proxy *)r->toplevel, XDG_TOPLEVEL_SET_PARENT,
                           NULL, wl_proxy_get_version((struct wl_proxy *)r->toplevel), 0, NULL);
    wl_proxy_marshal_flags((struct wl_proxy *)r->toplevel, XDG_TOPLEVEL_SET_TITLE,
                           NULL, wl_proxy_get_version((struct wl_proxy *)r->toplevel),
                           0, title ? title : "VAAPI Player");
    /* set_app_id：与常规 Wayland 客户端保持一致 */
    wl_proxy_marshal_flags((struct wl_proxy *)r->toplevel, 3,
                           NULL, 0, 0, "vaapi_demo");
    wl_surface_commit(r->surface);

    r->egl_window = wl_egl_window_create(r->surface, r->window_width, r->window_height);
    if (!r->egl_window) {
        LOG_ERROR("创建 wl_egl_window 失败");
        renderer_destroy(r);
        return NULL;
    }

    /* 3. EGL + GL 初始化 */
    if (init_egl(r) < 0) {
        renderer_destroy(r);
        return NULL;
    }
    glViewport(0, 0, r->window_width, r->window_height);
    if (renderer_init_gl(r) < 0) {
        renderer_destroy(r);
        return NULL;
    }

    /* 4. 创建 dmabuf 导入器。
     * 注意：configure 事件放到帧循环里处理，ack_configure 必须等第一帧
     * 缓冲提交之后再发，否则合成器会以 invalid method 拒绝。
     */
    wl_display_flush(r->display);
    wl_display_roundtrip(r->display);   /* 处理首个 configure 并完成 ack */
    r->egl = egl_interop_create();

    return r;
}

void renderer_destroy(Renderer *r)
{
    if (!r)
        return;

    if (r->egl) {
        egl_interop_destroy(r->egl);
        r->egl = NULL;
    }
    if (r->egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(r->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (r->program)
            glDeleteProgram(r->program);
        if (r->vbo)
            glDeleteBuffers(1, &r->vbo);
        if (r->vao)
            glDeleteVertexArrays(1, &r->vao);
        if (r->texture_y)
            glDeleteTextures(1, &r->texture_y);
        if (r->texture_uv)
            glDeleteTextures(1, &r->texture_uv);
        if (r->egl_surface != EGL_NO_SURFACE)
            eglDestroySurface(r->egl_display, r->egl_surface);
        if (r->egl_context != EGL_NO_CONTEXT)
            eglDestroyContext(r->egl_display, r->egl_context);
        eglTerminate(r->egl_display);
    }
    if (r->egl_window)
        wl_egl_window_destroy(r->egl_window);
    if (r->toplevel)
        wl_proxy_destroy((struct wl_proxy *)r->toplevel);
    if (r->xdg_surface)
        wl_proxy_destroy((struct wl_proxy *)r->xdg_surface);
    if (r->surface)
        wl_surface_destroy(r->surface);
    if (r->wm_base)
        wl_proxy_destroy((struct wl_proxy *)r->wm_base);
    if (r->registry)
        wl_registry_destroy(r->registry);
    if (r->display)
        wl_display_disconnect(r->display);

    free(r);
}

/* ------------------------------------------------------------------------- */
/* 绘制                                                                       */
/* ------------------------------------------------------------------------- */

static void draw_quad(Renderer *r, GLuint texture_y, GLuint texture_uv,
                      float uv_scale_x, float uv_scale_y)
{
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindVertexArray(r->vao);
    glUseProgram(r->program);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_y);
    glUniform1i(r->uniform_y, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, texture_uv);
    glUniform1i(r->uniform_uv, 1);

    glUniform2f(r->uniform_uv_scale, uv_scale_x, uv_scale_y);

    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glUseProgram(0);
    glBindVertexArray(0);
}

int renderer_draw_nv12(Renderer *r, const uint8_t *data,
                       int linesize_y, int linesize_uv)
{
    if (!r || !data)
        return -1;

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, r->texture_y);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, linesize_y);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, r->width, r->height, 0,
                 GL_RED, GL_UNSIGNED_BYTE, data);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, r->texture_uv);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, linesize_uv / 2);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG, r->width / 2, r->height / 2, 0,
                 GL_RG, GL_UNSIGNED_BYTE, data + (size_t)linesize_y * r->height);

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    draw_quad(r, r->texture_y, r->texture_uv, 1.0f, 1.0f);
    return 0;
}

int renderer_supports_dmabuf(Renderer *r)
{
    return r && r->egl != NULL;
}

int renderer_draw_dmabuf(Renderer *r, const AVDRMFrameDescriptor *desc,
                         int buffer_width, int buffer_height,
                         int visible_width, int visible_height)
{
    GLuint texture_y = 0;
    GLuint texture_uv = 0;
    float scale_x = 1.0f;
    float scale_y = 1.0f;

    if (!r || !r->egl || !desc)
        return -1;

    /* dmabuf -> EGLImage -> GL 纹理，画面数据不再经过 CPU */
    if (egl_interop_upload(r->egl, desc, buffer_width, buffer_height) < 0)
        return -1;

    /* 表面可能按 16 像素对齐（如 1080 -> 1088），采样时裁掉底部 padding */
    if (buffer_width > 0 && buffer_height > 0) {
        scale_x = visible_width  > 0 && visible_width  < buffer_width
                ? (float)visible_width / buffer_width : 1.0f;
        scale_y = visible_height > 0 && visible_height < buffer_height
                ? (float)visible_height / buffer_height : 1.0f;
    }

    egl_interop_textures(r->egl, &texture_y, &texture_uv);
    draw_quad(r, texture_y, texture_uv, scale_x, scale_y);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* 事件与交换                                                                 */
/* ------------------------------------------------------------------------- */

int renderer_should_close(Renderer *r)
{
    return !r || r->should_close;
}

void renderer_swap_buffers(Renderer *r)
{
    if (r && r->egl_surface != EGL_NO_SURFACE)
        eglSwapBuffers(r->egl_display, r->egl_surface);

    /* 第一帧交换后请求 frame 回调，用于确认合成器确实在呈现 */
    if (r && !r->frame_presented) {
        struct wl_callback *cb = wl_surface_frame(r->surface);
        if (cb)
            wl_callback_add_listener(cb, &frame_listener, r);
    }
}

void renderer_poll_events(Renderer *r)
{
    if (!r || !r->display)
        return;

    wl_display_flush(r->display);
    wl_display_dispatch_pending(r->display);
}

double renderer_time(void)
{
    return util_now_us() / 1000000.0;
}

#else /* !HAVE_DISPLAY */

struct Renderer {
    int unused;
};

int renderer_available(void)
{
    return 0;
}

Renderer *renderer_create(int width, int height, const char *title)
{
    (void)width;
    (void)height;
    (void)title;
    LOG_WARN("本版本编译时未找到 wayland-client/wayland-egl/EGL，无法显示视频窗口");
    LOG_WARN("如需播放功能请先安装: sudo apt install libwayland-dev libegl-dev 然后重新 make");
    return NULL;
}

void renderer_destroy(Renderer *renderer)
{
    (void)renderer;
}

int renderer_draw_nv12(Renderer *renderer, const uint8_t *data,
                       int linesize_y, int linesize_uv)
{
    (void)renderer;
    (void)data;
    (void)linesize_y;
    (void)linesize_uv;
    return -1;
}

int renderer_supports_dmabuf(Renderer *renderer)
{
    (void)renderer;
    return 0;
}

int renderer_draw_dmabuf(Renderer *renderer, const AVDRMFrameDescriptor *desc,
                         int buffer_width, int buffer_height,
                         int visible_width, int visible_height)
{
    (void)renderer;
    (void)desc;
    (void)buffer_width;
    (void)buffer_height;
    (void)visible_width;
    (void)visible_height;
    return -1;
}

int renderer_should_close(Renderer *renderer)
{
    (void)renderer;
    return 1;
}

void renderer_swap_buffers(Renderer *renderer)
{
    (void)renderer;
}

void renderer_poll_events(Renderer *renderer)
{
    (void)renderer;
}

double renderer_time(void)
{
    return 0.0;
}

#endif /* HAVE_DISPLAY */
