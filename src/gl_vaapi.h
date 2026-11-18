/*
 * gl_vaapi.h - OpenGL 渲染结果零拷贝送入 VAAPI 编码器
 *
 * 链路（画面数据全程留在显存）：
 *
 *   GL 渲染到 Y/UV 两张纹理（FBO，MRT）
 *     -> eglCreateImageKHR(EGL_GL_TEXTURE_2D) + eglExportDMABUFImageMESA
 *     -> dmabuf fd / stride / modifier
 *     -> vaCreateSurfaces(VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) 得到 VASurface
 *     -> 包成 AVFrame(AV_PIX_FMT_VAAPI) 送编码器
 *
 * 输入可以是解码得到的硬件帧（经 egl_interop 导入成纹理后再合成），
 * 也可以是纯合成画面（无输入时生成动态图案），用于演示 GL 合成/OSD 后直接硬编。
 *
 * 依赖 EGL_MESA_image_dma_buf_export，编译期需要 HAVE_EGL。
 */
#ifndef GL_VAAPI_H
#define GL_VAAPI_H

#include <libavcodec/avcodec.h>

#include "vaapi_common.h"
#include "vaapi_encoder.h"

typedef struct GlVaapiEncoder GlVaapiEncoder;

typedef struct {
    const char *codec;        /* h264 / hevc */
    int width;
    int height;
    int fps;
    int qp;
    int bitrate_kbps;
    int gop;
    const char *device;
    const char *connection_type;
    AVBufferRef *device_ctx;  /* 外部 VAAPI 设备，可为 NULL 由内部创建 */
} GlVaapiConfig;

int gl_vaapi_open(GlVaapiEncoder **out, const GlVaapiConfig *config);
void gl_vaapi_close(GlVaapiEncoder *enc);

/*
 * 渲染一帧并编码。
 * @param hw_frame 可选的解码硬件帧（AV_PIX_FMT_VAAPI），为 NULL 时生成合成画面
 * @param pts      帧序号
 * @param writer   码流回调
 */
int gl_vaapi_encode_frame(GlVaapiEncoder *enc, AVFrame *hw_frame, int64_t pts,
                          VaapiPacketWriter writer, void *opaque);

int gl_vaapi_flush(GlVaapiEncoder *enc, VaapiPacketWriter writer, void *opaque);

const char *gl_vaapi_renderer_name(GlVaapiEncoder *enc);

#endif /* GL_VAAPI_H */
