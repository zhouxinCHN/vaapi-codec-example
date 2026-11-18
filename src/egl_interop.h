/*
 * egl_interop.h - dmabuf 零拷贝导入 GL（EGL_EXT_image_dma_buf_import）
 *
 * 这是"解码帧零拷贝进 GL"链路的第二半：
 *
 *   AVDRMFrameDescriptor（dmabuf fd/offset/pitch）
 *     -> eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)
 *     -> glEGLImageTargetTexture2DOES
 *     -> 着色器直接采样 Y/UV 两个纹理，画面数据全程留在显存
 *
 * 依赖 EGL 的扩展：
 *   EGL_KHR_image_base、EGL_EXT_image_dma_buf_import，
 *   表面带 tiled modifier 时还需要 EGL_EXT_image_dma_buf_import_modifiers。
 *
 * 只在编译时找到 EGL 头/库（HAVE_EGL）才会编译本模块。
 */
#ifndef EGL_INTEROP_H
#define EGL_INTEROP_H

#include <libavutil/frame.h>
#include <libavutil/hwcontext_drm.h>

typedef struct EglInterop EglInterop;

/* 当前线程是否已有可用的 EGL 上下文（由 GLFW 以 EGL API 创建）。 */
int egl_interop_available(void);

/* 基于当前 EGL 上下文创建导入器；不可用时返回 NULL。 */
EglInterop *egl_interop_create(void);
void egl_interop_destroy(EglInterop *interop);

/* 把一帧 dmabuf 导入为 Y/UV 两张纹理，供当前帧渲染使用。 */
int egl_interop_upload(EglInterop *interop, const AVDRMFrameDescriptor *desc,
                       int width, int height);

/* 取导入后的纹理对象名（GL_TEXTURE_2D）。 */
void egl_interop_textures(EglInterop *interop, unsigned int *texture_y,
                          unsigned int *texture_uv);

#endif /* EGL_INTEROP_H */
