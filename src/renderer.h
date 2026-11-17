/*
 * renderer.h - 可选模块：用 OpenGL(GLFW) 把解码得到的 NV12 画面显示出来。
 *              如果编译时没有找到 glfw3，renderer_available() 返回 0，
 *              其余功能（编码/解码/转码/自检）不受影响。
 */
#ifndef RENDERER_H
#define RENDERER_H

#include <stdint.h>

typedef struct Renderer Renderer;

int renderer_available(void);

Renderer *renderer_create(int width, int height, const char *title);
void renderer_destroy(Renderer *renderer);

/* 上传一帧 NV12 数据（Y 平面 + UV 交错平面），宽高由 renderer_create 指定。 */
int renderer_draw_nv12(Renderer *renderer, const uint8_t *data,
                       int linesize_y, int linesize_uv);

int renderer_should_close(Renderer *renderer);
void renderer_swap_buffers(Renderer *renderer);
void renderer_poll_events(Renderer *renderer);
double renderer_time(void);

#endif /* RENDERER_H */
