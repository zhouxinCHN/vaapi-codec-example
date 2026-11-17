#include "vaapi_common.h"

#include <stdlib.h>
#include <string.h>

#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

static _Thread_local char error_buffer[AV_ERROR_MAX_STRING_SIZE];

const char *vaapi_error_string(int errnum)
{
    if (errnum >= 0) {
        snprintf(error_buffer, sizeof(error_buffer), "成功");
        return error_buffer;
    }
    if (av_strerror(errnum, error_buffer, sizeof(error_buffer)) < 0)
        snprintf(error_buffer, sizeof(error_buffer), "未知错误码 %d", errnum);
    return error_buffer;
}

AVBufferRef *vaapi_device_create(const char *device, const char *connection_type)
{
    AVBufferRef *device_ctx = NULL;
    AVDictionary *options = NULL;
    const char *type = (connection_type && *connection_type) ? connection_type : NULL;
    int err;

    if (type)
        av_dict_set(&options, "connection_type", type, 0);

    err = av_hwdevice_ctx_create(&device_ctx, AV_HWDEVICE_TYPE_VAAPI, device, options, 0);
    av_dict_free(&options);

    if (err < 0) {
        /* 指定了连接方式但失败时，退回 libva 的自动探测再试一次。 */
        if (type) {
            LOG_WARN("使用 connection_type=%s 创建设备失败(%s)，改为自动探测重试",
                     type, vaapi_error_string(err));
            err = av_hwdevice_ctx_create(&device_ctx, AV_HWDEVICE_TYPE_VAAPI, device, NULL, 0);
        }
        if (err < 0) {
            LOG_ERROR("创建 VAAPI 硬件设备失败: %s", vaapi_error_string(err));
            LOG_ERROR("请确认: 1) 已安装显卡驱动 (vainfo 可正常输出); "
                      "2) 当前用户对 %s 有读写权限; 3) 设备节点是否正确 (-d 参数)",
                      device ? device : "/dev/dri/renderD*");
            return NULL;
        }
    }

    return device_ctx;
}

int vaapi_parse_size(const char *text, int *width, int *height)
{
    char *end = NULL;
    long w, h;

    if (!text)
        return -1;

    w = strtol(text, &end, 10);
    if (end == text || (*end != 'x' && *end != 'X' && *end != '*'))
        return -1;
    h = strtol(end + 1, &end, 10);
    if (end == text || w <= 0 || h <= 0)
        return -1;

    *width  = (int)w;
    *height = (int)h;
    return 0;
}

AVFrame *vaapi_frame_alloc_hw(AVBufferRef *hw_frames_ctx)
{
    AVFrame *frame;

    if (!hw_frames_ctx)
        return NULL;

    frame = av_frame_alloc();
    if (!frame)
        return NULL;
    if (av_hwframe_get_buffer(hw_frames_ctx, frame, 0) < 0) {
        av_frame_free(&frame);
        return NULL;
    }
    return frame;
}

/* 转换成帧池要求的格式与尺寸（I420 -> NV12、分辨率不一致等）。 */
static AVFrame *convert_frame(AVFrame *src, enum AVPixelFormat format, int width, int height)
{
    struct SwsContext *sws;
    AVFrame *dst;

    dst = av_frame_alloc();
    if (!dst)
        return NULL;
    dst->format = format;
    dst->width  = width;
    dst->height = height;
    if (av_frame_get_buffer(dst, 32) < 0) {
        av_frame_free(&dst);
        return NULL;
    }

    sws = sws_getContext(src->width, src->height, (enum AVPixelFormat)src->format,
                         width, height, format, SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) {
        av_frame_free(&dst);
        return NULL;
    }
    sws_scale(sws, (const uint8_t * const *)src->data, src->linesize, 0, src->height,
              dst->data, dst->linesize);
    sws_freeContext(sws);
    return dst;
}

AVFrame *vaapi_frame_upload(AVBufferRef *hw_frames_ctx, AVFrame *sw_frame)
{
    AVHWFramesContext *frames;
    AVFrame *hw_frame = NULL;
    AVFrame *converted = NULL;
    AVFrame *input = sw_frame;

    if (!hw_frames_ctx || !sw_frame)
        return NULL;

    frames = (AVHWFramesContext *)hw_frames_ctx->data;

    if (sw_frame->width != frames->width || sw_frame->height != frames->height ||
        sw_frame->format != frames->sw_format) {
        converted = convert_frame(sw_frame, frames->sw_format,
                                  frames->width, frames->height);
        if (!converted) {
            LOG_ERROR("转换输入帧失败 (%s %dx%d -> %s %dx%d)",
                      av_get_pix_fmt_name(sw_frame->format), sw_frame->width, sw_frame->height,
                      av_get_pix_fmt_name(frames->sw_format), frames->width, frames->height);
            return NULL;
        }
        input = converted;
    }

    hw_frame = vaapi_frame_alloc_hw(hw_frames_ctx);
    if (!hw_frame)
        goto fail;

    if (av_hwframe_transfer_data(hw_frame, input, 0) < 0) {
        LOG_ERROR("上传图像到显卡失败");
        av_frame_free(&hw_frame);
        goto fail;
    }
    hw_frame->pts      = input->pts;
    hw_frame->duration = input->duration;

    av_frame_free(&converted);
    return hw_frame;

fail:
    av_frame_free(&converted);
    return NULL;
}
