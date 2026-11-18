#include "vaapi_dmabuf.h"

#include <inttypes.h>
#include <string.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
#include <libavutil/hwcontext_vaapi.h>

static const char *fourcc_name(uint32_t fourcc)
{
    switch (fourcc) {
    case DRM_FORMAT_NV12: return "NV12";
    case DRM_FORMAT_NV21: return "NV21";
    case DRM_FORMAT_P010: return "P010";
    case DRM_FORMAT_R8:   return "R8";
    case DRM_FORMAT_GR88: return "GR88";
    case DRM_FORMAT_RG88: return "RG88";
    default:              return "?";
    }
}

static void close_objects(VaapiDmabuf *dmabuf)
{
    for (int i = 0; i < dmabuf->desc.nb_objects; i++) {
        if (dmabuf->desc.objects[i].fd >= 0)
            close(dmabuf->desc.objects[i].fd);
        dmabuf->desc.objects[i].fd = -1;
    }
    dmabuf->desc.nb_objects = 0;
    dmabuf->exported = 0;
}

int vaapi_dmabuf_export(AVFrame *hw_frame, VaapiDmabuf *dmabuf)
{
    AVHWFramesContext *frames;
    AVHWDeviceContext *device;
    AVVAAPIDeviceContext *vaapi;
    VADRMPRIMESurfaceDescriptor va_desc;
    VADisplay display;
    VASurfaceID surface;
    VAStatus status;
    int ret = 0;

    if (!hw_frame || !dmabuf)
        return AVERROR(EINVAL);
    memset(dmabuf, 0, sizeof(*dmabuf));

    if (hw_frame->format != AV_PIX_FMT_VAAPI || !hw_frame->hw_frames_ctx) {
        LOG_ERROR("dmabuf: 输入不是 VAAPI 硬件帧");
        return AVERROR(EINVAL);
    }

    /* 从帧的帧池一路取到 VADisplay 与 VASurfaceID */
    frames = (AVHWFramesContext *)hw_frame->hw_frames_ctx->data;
    device = frames->device_ctx;
    vaapi  = (AVVAAPIDeviceContext *)device->hwctx;
    display = vaapi->display;
    surface = (VASurfaceID)(uintptr_t)hw_frame->data[3];

    /* 导出前必须等这一帧真正解码完成 */
    status = vaSyncSurface(display, surface);
    if (status != VA_STATUS_SUCCESS) {
        LOG_ERROR("dmabuf: vaSyncSurface 失败: %s", vaErrorStr(status));
        return AVERROR(EIO);
    }

    memset(&va_desc, 0, sizeof(va_desc));
    status = vaExportSurfaceHandle(display, surface,
                                   VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                   VA_EXPORT_SURFACE_READ_ONLY |
                                   VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                                   &va_desc);
    if (status != VA_STATUS_SUCCESS) {
        if (status == VA_STATUS_ERROR_UNIMPLEMENTED) {
            LOG_ERROR("dmabuf: 当前驱动不支持 VAAPI PRIME 2 导出");
            return AVERROR(ENOSYS);
        }
        LOG_ERROR("dmabuf: vaExportSurfaceHandle 失败: %s", vaErrorStr(status));
        return AVERROR(EIO);
    }

    if (va_desc.num_objects > AV_DRM_MAX_PLANES ||
        va_desc.num_layers > AV_DRM_MAX_PLANES) {
        LOG_ERROR("dmabuf: 描述符层数超出 FFmpeg 上限");
        ret = AVERROR(ENOSYS);
        goto fail;
    }

    /* 转成 FFmpeg 的 AVDRMFrameDescriptor（EGL 导入层使用这套结构） */
    dmabuf->desc.nb_objects = (int)va_desc.num_objects;
    for (int i = 0; i < dmabuf->desc.nb_objects; i++) {
        dmabuf->desc.objects[i].fd   = va_desc.objects[i].fd;
        dmabuf->desc.objects[i].size = va_desc.objects[i].size;
        dmabuf->desc.objects[i].format_modifier =
            va_desc.objects[i].drm_format_modifier;
    }
    dmabuf->desc.nb_layers = (int)va_desc.num_layers;
    for (int i = 0; i < dmabuf->desc.nb_layers; i++) {
        AVDRMLayerDescriptor *layer = &dmabuf->desc.layers[i];

        layer->format    = va_desc.layers[i].drm_format;
        layer->nb_planes = (int)va_desc.layers[i].num_planes;
        for (int j = 0; j < layer->nb_planes; j++) {
            layer->planes[j].object_index = (int)va_desc.layers[i].object_index[j];
            layer->planes[j].offset       = (ptrdiff_t)va_desc.layers[i].offset[j];
            layer->planes[j].pitch        = (ptrdiff_t)va_desc.layers[i].pitch[j];
        }
    }

    dmabuf->fourcc   = va_desc.fourcc;
    dmabuf->width    = (int)va_desc.width;
    dmabuf->height   = (int)va_desc.height;
    dmabuf->exported = 1;
    return 0;

fail:
    /* fd 已经通过 vaExportSurfaceHandle 交给调用方，失败路径要自己关掉 */
    for (int i = 0; i < (int)va_desc.num_objects; i++) {
        if (va_desc.objects[i].fd >= 0)
            close(va_desc.objects[i].fd);
    }
    memset(dmabuf, 0, sizeof(*dmabuf));
    return ret;
}

void vaapi_dmabuf_release(VaapiDmabuf *dmabuf)
{
    if (!dmabuf)
        return;
    if (dmabuf->exported)
        close_objects(dmabuf);
    memset(dmabuf, 0, sizeof(*dmabuf));
}

void vaapi_dmabuf_print(const VaapiDmabuf *dmabuf)
{
    const AVDRMFrameDescriptor *desc;

    if (!dmabuf || !dmabuf->exported)
        return;
    desc = &dmabuf->desc;

    LOG_INFO("导出成功: %dx%d fourcc=0x%08x, %d 个对象, %d 个层",
             dmabuf->width, dmabuf->height, dmabuf->fourcc,
             desc->nb_objects, desc->nb_layers);
    for (int i = 0; i < desc->nb_objects; i++) {
        LOG_INFO("  object[%d]: fd=%d size=%" PRId64 " modifier=0x%016" PRIx64,
                 i, desc->objects[i].fd, desc->objects[i].size,
                 desc->objects[i].format_modifier);
    }
    for (int i = 0; i < desc->nb_layers; i++) {
        const AVDRMLayerDescriptor *layer = &desc->layers[i];
        uint32_t fourcc = layer->format;

        LOG_INFO("  layer[%d] : fourcc=%c%c%c%c (%s) 平面数=%d",
                 i, (char)(fourcc), (char)(fourcc >> 8), (char)(fourcc >> 16),
                 (char)(fourcc >> 24), fourcc_name(fourcc), layer->nb_planes);
        for (int j = 0; j < layer->nb_planes; j++) {
            LOG_INFO("    plane[%d]: object=%d offset=%td pitch=%td",
                     j, layer->planes[j].object_index,
                     layer->planes[j].offset, layer->planes[j].pitch);
        }
    }
}

int vaapi_dmabuf_is_nv12_like(const VaapiDmabuf *dmabuf)
{
    int planes = 0;

    if (!dmabuf || !dmabuf->exported)
        return 0;

    for (int i = 0; i < dmabuf->desc.nb_layers; i++)
        planes += dmabuf->desc.layers[i].nb_planes;

    /* NV12 表现为两个平面：Y(R8) + UV(GR88)，可以分别导入成两张纹理 */
    return planes == 2;
}
