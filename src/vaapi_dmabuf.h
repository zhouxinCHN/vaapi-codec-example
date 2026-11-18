/*
 * vaapi_dmabuf.h - 把 VAAPI 表面导出为 DRM PRIME（dmabuf）描述符
 *
 * 这是"解码帧零拷贝进 GL"链路的第一半，直接调用 libva 的导出接口：
 *
 *   VASurfaceID
 *     -> vaExportSurfaceHandle(VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
 *                              VA_EXPORT_SURFACE_SEPARATE_LAYERS)
 *     -> VADRMPRIMESurfaceDescriptor（每个 object 一个 dmabuf fd）
 *     -> 转换成 FFmpeg 的 AVDRMFrameDescriptor，交给 EGL 导入
 *
 * 本模块不依赖 EGL，可在没有图形环境的机器上验证导出是否可用
 * （见 `vaapi_demo dmabuf` 与 selftest 第 7 步）。
 */
#ifndef VAAPI_DMABUF_H
#define VAAPI_DMABUF_H

#include <stdint.h>

#include <libavutil/frame.h>
#include <libavutil/hwcontext_drm.h>

#include "vaapi_common.h"

typedef struct {
    AVDRMFrameDescriptor desc;   /* 转换后的描述符，fd 归本结构所有 */
    uint32_t fourcc;             /* VA 表面 fourcc，如 VA_FOURCC_NV12 */
    int width;
    int height;
    int exported;                /* 是否已成功导出（决定 release 时是否 close fd） */
} VaapiDmabuf;

/*
 * 导出一帧 VAAPI 硬件帧（AV_PIX_FMT_VAAPI）为 dmabuf 描述符。
 * 成功返回 0，失败返回负错误码（驱动不支持 PRIME 2 时为 AVERROR(ENOSYS)）。
 */
int vaapi_dmabuf_export(AVFrame *hw_frame, VaapiDmabuf *dmabuf);

/* 释放导出结果并关闭所有 dmabuf fd。 */
void vaapi_dmabuf_release(VaapiDmabuf *dmabuf);

/* 打印描述符：fourcc、对象数、各平面的 fd / offset / pitch / modifier。 */
void vaapi_dmabuf_print(const VaapiDmabuf *dmabuf);

/* 是否为 NV12 形式的 Y/UV 双平面（可直接作为两纹理导入 GL）。 */
int vaapi_dmabuf_is_nv12_like(const VaapiDmabuf *dmabuf);

#endif /* VAAPI_DMABUF_H */
