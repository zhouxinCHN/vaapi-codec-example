#include "egl_headless.h"

#include <stdlib.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <glad/glad.h>

#include "vaapi_common.h"

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

struct EglHeadless {
    EGLDisplay display;
    EGLContext context;
    int initialized;
};

EglHeadless *egl_headless_create(void)
{
    EglHeadless *egl;
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display;
    EGLint major = 0, minor = 0, count = 0;
    EGLint config_attrs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_NONE
    };
    EGLConfig config;

    egl = calloc(1, sizeof(*egl));
    if (!egl)
        return NULL;

    /* 优先 surfaceless 平台，避免依赖 X11/Wayland */
    get_platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
        eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get_platform_display) {
        egl->display = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA,
                                            EGL_DEFAULT_DISPLAY, NULL);
    }
    if (!egl->display)
        egl->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl->display == EGL_NO_DISPLAY) {
        LOG_ERROR("EGL: 获取显示设备失败");
        goto fail;
    }

    if (!eglInitialize(egl->display, &major, &minor)) {
        LOG_ERROR("EGL: eglInitialize 失败");
        goto fail;
    }
    egl->initialized = 1;
    LOG_INFO("EGL %d.%d 初始化成功（surfaceless）", major, minor);

    if (!eglBindAPI(EGL_OPENGL_API)) {
        LOG_ERROR("EGL: eglBindAPI 失败");
        goto fail;
    }
    if (!eglChooseConfig(egl->display, config_attrs, &config, 1, &count) || count == 0) {
        LOG_ERROR("EGL: 找不到可用配置");
        goto fail;
    }

    egl->context = eglCreateContext(egl->display, config, EGL_NO_CONTEXT, NULL);
    if (egl->context == EGL_NO_CONTEXT) {
        LOG_ERROR("EGL: 创建上下文失败");
        goto fail;
    }
    if (!eglMakeCurrent(egl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, egl->context)) {
        LOG_ERROR("EGL: makeCurrent 失败");
        goto fail;
    }
    if (!gladLoadGLLoader((GLADloadproc)eglGetProcAddress)) {
        LOG_ERROR("EGL: 加载 OpenGL 函数失败");
        goto fail;
    }

    return egl;

fail:
    egl_headless_destroy(egl);
    return NULL;
}

void egl_headless_destroy(EglHeadless *egl)
{
    if (!egl)
        return;
    if (egl->display != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (egl->context != EGL_NO_CONTEXT)
            eglDestroyContext(egl->display, egl->context);
        if (egl->initialized)
            eglTerminate(egl->display);
    }
    free(egl);
}

int egl_headless_read_texture(EglHeadless *egl, unsigned int texture,
                              int width, int height, int planes, uint8_t *out)
{
    GLuint framebuffer = 0;
    GLenum status;

    if (!egl || !texture || !out || width <= 0 || height <= 0)
        return -1;

    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("FBO 不完整 (0x%x)，无法读回纹理", status);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &framebuffer);
        return -1;
    }

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    (void)glGetError();   /* 清除历史错误标志，避免误判本次读回 */
    glReadPixels(0, 0, width, height,
                 planes == 2 ? GL_RG : GL_RED, GL_UNSIGNED_BYTE, out);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &framebuffer);

    {
        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            LOG_ERROR("glReadPixels 失败: 0x%x", err);
            return -1;
        }
    }
    return 0;
}
