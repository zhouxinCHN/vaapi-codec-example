/*
 * egl_headless.h - 无显示环境下建立 EGL 上下文（surfaceless）
 *
 * 用于在没有 X11/Wayland 会话的机器上验证：
 *   VAAPI 解码 -> dmabuf 导出 -> EGLImage 导入 -> GL 纹理
 * 创建成功后即可用 egl_interop 导入 dmabuf，并用 FBO 把纹理读回做像素校验。
 *
 * 优先走 EGL_EXT_platform_base 的 EGL_PLATFORM_SURFACELESS_MESA 平台，
 * 这样完全不需要窗口系统，只在有 /dev/dri 设备节点时即可工作。
 */
#ifndef EGL_HEADLESS_H
#define EGL_HEADLESS_H

#include <stdint.h>

typedef struct EglHeadless EglHeadless;

EglHeadless *egl_headless_create(void);
void egl_headless_destroy(EglHeadless *egl);

/*
 * 把一张 GL 纹理通过 FBO 读回内存。
 * @param planes 1 表示单平面(GL_RED)，2 表示双平面(GL_RG)
 * 返回 0 成功。
 */
int egl_headless_read_texture(EglHeadless *egl, unsigned int texture,
                              int width, int height, int planes, uint8_t *out);

#endif /* EGL_HEADLESS_H */
