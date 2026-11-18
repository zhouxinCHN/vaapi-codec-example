/*
 * renderer.h - 可选模块：用 OpenGL(GLFW) 把解码得到的 NV12 画面显示出来。
 *              如果编译时没有找到 glfw3，renderer_available() 返回 0，
 *              其余功能（编码/解码/转码/自检）不受影响。
 */
#ifndef RENDERER_H
#define RENDERER_H

#include <stdint.h>

#include <libavutil/frame.h>
#include <libavutil/hwcontext_drm.h>

typedef struct Renderer Renderer;

int renderer_available(void);

/* 实际使用的后端名（"wayland"/"x11"）与 GL 渲染器名，用于日志与能力判断。 */
const char *renderer_backend_name(Renderer *renderer);
const char *renderer_gl_renderer(Renderer *renderer);

/* backend: "wayland" / "x11" / NULL("auto") */
Renderer *renderer_create(int width, int height, const char *title, const char *backend);
void renderer_destroy(Renderer *renderer);

/* 上传一帧 NV12 数据（Y 平面 + UV 交错平面），宽高由 renderer_create 指定。 */
int renderer_draw_nv12(Renderer *renderer, const uint8_t *data,
                       int linesize_y, int linesize_uv);

/* 是否支持 dmabuf 零拷贝显示（编译期有 EGL + 运行时 EGL 上下文可用）。 */
int renderer_supports_dmabuf(Renderer *renderer);

/*
 * 直接渲染一帧 AV_PIX_FMT_DRM_PRIME 帧：
 * dmabuf -> EGLImage -> GL 纹理，画面数据全程留在显存。
 */
int renderer_draw_dmabuf(Renderer *renderer, const AVDRMFrameDescriptor *desc,
                         int buffer_width, int buffer_height,
                         int visible_width, int visible_height);

int renderer_should_close(Renderer *renderer);
void renderer_swap_buffers(Renderer *renderer);
void renderer_poll_events(Renderer *renderer);
double renderer_time(void);

#endif /* RENDERER_H */
