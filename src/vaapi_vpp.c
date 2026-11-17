#include "vaapi_vpp.h"

#include <stdio.h>
#include <stdlib.h>

#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>

struct VaapiVpp {
    AVFilterGraph *graph;
    AVFilterContext *src;
    AVFilterContext *scale;
    AVFilterContext *sink;
    AVBufferRef *input_frames;
    int owns_input_frames;
    int src_width;
    int src_height;
    int dst_width;
    int dst_height;
};

static AVBufferRef *create_input_pool(AVBufferRef *device_ctx, int width, int height)
{
    AVBufferRef *frames_ref;
    AVHWFramesContext *frames;

    frames_ref = av_hwframe_ctx_alloc(device_ctx);
    if (!frames_ref)
        return NULL;

    frames = (AVHWFramesContext *)frames_ref->data;
    frames->format            = AV_PIX_FMT_VAAPI;
    frames->sw_format         = AV_PIX_FMT_NV12;
    frames->width             = width;
    frames->height            = height;
    frames->initial_pool_size = 20;

    if (av_hwframe_ctx_init(frames_ref) < 0) {
        av_buffer_unref(&frames_ref);
        return NULL;
    }
    return frames_ref;
}

VaapiVpp *vaapi_vpp_create(AVBufferRef *device_ctx,
                           AVBufferRef *input_frames_ctx,
                           int src_width, int src_height,
                           int dst_width, int dst_height,
                           int fps)
{
    VaapiVpp *vpp;
    const AVFilter *buffersrc;
    const AVFilter *buffersink;
    const AVFilter *scale;
    AVBufferSrcParameters *params = NULL;
    AVHWFramesContext *frames;
    char args[128];
    int ret;

    if (!device_ctx || dst_width <= 0 || dst_height <= 0)
        return NULL;

    vpp = calloc(1, sizeof(*vpp));
    if (!vpp)
        return NULL;

    if (input_frames_ctx) {
        vpp->input_frames = av_buffer_ref(input_frames_ctx);
    } else {
        vpp->input_frames = create_input_pool(device_ctx, src_width, src_height);
        vpp->owns_input_frames = 1;
    }
    if (!vpp->input_frames) {
        LOG_ERROR("VPP: 创建输入帧池失败");
        goto fail;
    }
    frames = (AVHWFramesContext *)vpp->input_frames->data;
    vpp->src_width  = frames->width;
    vpp->src_height = frames->height;
    vpp->dst_width  = dst_width;
    vpp->dst_height = dst_height;

    buffersrc  = avfilter_get_by_name("buffer");
    buffersink = avfilter_get_by_name("buffersink");
    scale      = avfilter_get_by_name("scale_vaapi");
    if (!buffersrc || !buffersink || !scale) {
        LOG_ERROR("VPP: 当前 FFmpeg 未提供 scale_vaapi 过滤器");
        LOG_ERROR("请确认 FFmpeg 编译时启用了 --enable-vaapi 与 --enable-libavfilter");
        goto fail;
    }

    vpp->graph = avfilter_graph_alloc();
    if (!vpp->graph)
        goto fail;

    snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=1/%d:pixel_aspect=1/1",
             vpp->src_width, vpp->src_height, AV_PIX_FMT_VAAPI, fps > 0 ? fps : 25);
    if ((ret = avfilter_graph_create_filter(&vpp->src, buffersrc, "in", args, NULL,
                                            vpp->graph)) < 0) {
        LOG_ERROR("VPP: 创建输入滤镜失败: %s", vaapi_error_string(ret));
        goto fail;
    }

    /* 输入是 VAAPI 硬件帧，必须通过 AVBufferSrcParameters 指定设备帧池 */
    params = av_buffersrc_parameters_alloc();
    if (!params)
        goto fail;
    params->format        = AV_PIX_FMT_VAAPI;
    params->width         = vpp->src_width;
    params->height        = vpp->src_height;
    params->time_base     = (AVRational){ 1, fps > 0 ? fps : 25 };
    params->hw_frames_ctx = av_buffer_ref(vpp->input_frames);
    if (!params->hw_frames_ctx) {
        av_free(params);
        goto fail;
    }
    if ((ret = av_buffersrc_parameters_set(vpp->src, params)) < 0) {
        LOG_ERROR("VPP: 设置输入帧池失败: %s", vaapi_error_string(ret));
        av_buffer_unref(&params->hw_frames_ctx);
        av_free(params);
        goto fail;
    }
    /* 滤镜已持有自己的引用，这里释放参数结构里的临时引用 */
    av_buffer_unref(&params->hw_frames_ctx);
    av_free(params);

    snprintf(args, sizeof(args), "w=%d:h=%d:mode=hq", dst_width, dst_height);
    if ((ret = avfilter_graph_create_filter(&vpp->scale, scale, "scale", args, NULL,
                                            vpp->graph)) < 0) {
        LOG_ERROR("VPP: 创建 scale_vaapi 失败: %s", vaapi_error_string(ret));
        goto fail;
    }
    if ((ret = avfilter_graph_create_filter(&vpp->sink, buffersink, "out", NULL, NULL,
                                            vpp->graph)) < 0) {
        LOG_ERROR("VPP: 创建输出滤镜失败: %s", vaapi_error_string(ret));
        goto fail;
    }
    if ((ret = avfilter_link(vpp->src, 0, vpp->scale, 0)) < 0 ||
        (ret = avfilter_link(vpp->scale, 0, vpp->sink, 0)) < 0) {
        LOG_ERROR("VPP: 连接滤镜失败: %s", vaapi_error_string(ret));
        goto fail;
    }
    if ((ret = avfilter_graph_config(vpp->graph, NULL)) < 0) {
        LOG_ERROR("VPP: 初始化滤镜图失败: %s", vaapi_error_string(ret));
        goto fail;
    }

    return vpp;

fail:
    vaapi_vpp_free(vpp);
    return NULL;
}

void vaapi_vpp_free(VaapiVpp *vpp)
{
    if (!vpp)
        return;

    if (vpp->graph) {
        if (vpp->src) {
            int ret = av_buffersrc_add_frame_flags(vpp->src, NULL, 0);   /* 冲刷滤镜 */
            (void)ret;
        }
        avfilter_graph_free(&vpp->graph);
    }
    av_buffer_unref(&vpp->input_frames);
    free(vpp);
}

AVBufferRef *vaapi_vpp_input_frames(VaapiVpp *vpp)
{
    return vpp ? vpp->input_frames : NULL;
}

int vaapi_vpp_process(VaapiVpp *vpp, AVFrame *src, AVFrame **dst)
{
    AVFrame *out;
    int ret;

    if (!vpp || !src || !dst)
        return AVERROR(EINVAL);
    *dst = NULL;

    out = av_frame_alloc();
    if (!out)
        return AVERROR(ENOMEM);

    ret = av_buffersrc_add_frame_flags(vpp->src, src, AV_BUFFERSRC_FLAG_KEEP_REF);
    if (ret < 0) {
        LOG_ERROR("VPP: 送入输入帧失败: %s", vaapi_error_string(ret));
        av_frame_free(&out);
        return ret;
    }

    ret = av_buffersink_get_frame(vpp->sink, out);
    if (ret < 0) {
        LOG_ERROR("VPP: 取回缩放结果失败: %s", vaapi_error_string(ret));
        av_frame_free(&out);
        return ret;
    }

    *dst = out;
    return 0;
}
