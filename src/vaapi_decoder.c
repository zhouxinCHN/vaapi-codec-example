#include "vaapi_decoder.h"

#include <stdlib.h>
#include <string.h>

#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

#include "vaapi_vpp.h"

struct VaapiDecoder {
    AVFormatContext *format_ctx;
    AVCodecContext *avctx;
    AVBufferRef *device_ctx;
    AVStream *stream;
    int stream_index;

    enum AVPixelFormat hw_pix_fmt;
    enum AVPixelFormat sw_format;
    int width;
    int height;
    int out_width;
    int out_height;
    double fps;
    int64_t frame_count;
    double duration;
    char codec_name[32];

    int want_hw_frames;
    enum AVPixelFormat out_format;
    int max_frames;
    int frames_decoded;

    VaapiVpp *vpp;
    AVFrame *sw_frame;
    struct SwsContext *sws;
    AVFrame *convert_frame;
    enum AVPixelFormat sws_src_format;
    int sws_width;
    int sws_height;
};

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                        const enum AVPixelFormat *pix_fmts)
{
    VaapiDecoder *dec = ctx->opaque;
    const enum AVPixelFormat *p;

    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == dec->hw_pix_fmt)
            return *p;
    }

    LOG_ERROR("解码器不支持 VAAPI 硬件帧格式 (期望 %s)",
              av_get_pix_fmt_name(dec->hw_pix_fmt));
    return AV_PIX_FMT_NONE;
}

int vaapi_decoder_open(VaapiDecoder **decoder, const char *path,
                       const VaapiDecoderConfig *config)
{
    VaapiDecoder *dec;
    const AVCodec *codec = NULL;
    const AVCodecHWConfig *hw_config;
    int ret;

    if (!decoder || !path)
        return AVERROR(EINVAL);
    *decoder = NULL;

    dec = calloc(1, sizeof(*dec));
    if (!dec)
        return AVERROR(ENOMEM);
    dec->want_hw_frames = config ? config->want_hw_frames : 0;
    dec->out_format     = (config && config->out_format != AV_PIX_FMT_NONE)
                        ? config->out_format : AV_PIX_FMT_NONE;
    dec->max_frames     = config ? config->max_frames : 0;

    ret = avformat_open_input(&dec->format_ctx, path, NULL, NULL);
    if (ret < 0) {
        LOG_ERROR("打开输入文件失败 %s: %s", path, vaapi_error_string(ret));
        goto fail;
    }
    ret = avformat_find_stream_info(dec->format_ctx, NULL);
    if (ret < 0) {
        LOG_ERROR("读取流信息失败: %s", vaapi_error_string(ret));
        goto fail;
    }

    ret = av_find_best_stream(dec->format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (ret < 0 || !codec) {
        LOG_ERROR("输入文件 %s 中没有找到视频流", path);
        goto fail;
    }
    dec->stream_index = ret;
    dec->stream = dec->format_ctx->streams[dec->stream_index];
    snprintf(dec->codec_name, sizeof(dec->codec_name), "%s", codec->name);

    /* 找到该解码器的 VAAPI 硬件配置，确认显卡能解这个编码格式 */
    dec->hw_pix_fmt = AV_PIX_FMT_NONE;
    for (int i = 0;; i++) {
        hw_config = avcodec_get_hw_config(codec, i);
        if (!hw_config) {
            LOG_ERROR("%s 解码器不支持 VAAPI 硬件解码", codec->name);
            ret = AVERROR_DECODER_NOT_FOUND;
            goto fail;
        }
        if (hw_config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
            hw_config->device_type == AV_HWDEVICE_TYPE_VAAPI) {
            dec->hw_pix_fmt = hw_config->pix_fmt;
            break;
        }
    }

    dec->device_ctx = vaapi_device_create(config ? config->device : NULL,
                                          config ? config->connection_type : NULL);
    if (!dec->device_ctx) {
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    dec->avctx = avcodec_alloc_context3(codec);
    if (!dec->avctx) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ret = avcodec_parameters_to_context(dec->avctx, dec->stream->codecpar);
    if (ret < 0) {
        LOG_ERROR("复制解码参数失败: %s", vaapi_error_string(ret));
        goto fail;
    }

    dec->avctx->hw_device_ctx = av_buffer_ref(dec->device_ctx);
    dec->avctx->get_format     = get_hw_format;
    dec->avctx->opaque         = dec;

    ret = avcodec_open2(dec->avctx, codec, NULL);
    if (ret < 0) {
        LOG_ERROR("打开 %s 解码器失败: %s", codec->name, vaapi_error_string(ret));
        goto fail;
    }

    dec->width  = dec->stream->codecpar->width;
    dec->height = dec->stream->codecpar->height;
    if (dec->avctx->width > 0 && dec->avctx->height > 0) {
        dec->width  = dec->avctx->width;
        dec->height = dec->avctx->height;
    }

    dec->fps = av_q2d(av_guess_frame_rate(dec->format_ctx, dec->stream, NULL));
    if (dec->fps <= 0.0 || dec->fps > 240.0)
        dec->fps = av_q2d(dec->stream->avg_frame_rate) > 0.0
                 ? av_q2d(dec->stream->avg_frame_rate) : 25.0;

    dec->frame_count = dec->stream->nb_frames;
    dec->duration    = dec->format_ctx->duration != AV_NOPTS_VALUE
                     ? dec->format_ctx->duration / (double)AV_TIME_BASE : 0.0;
    if (dec->frame_count <= 0 && dec->duration > 0)
        dec->frame_count = (int64_t)(dec->duration * dec->fps + 0.5);

    /* 解码器初始化后 hw_frames_ctx 即由 libavcodec 创建完毕，可交给编码器复用 */
    if (dec->avctx->hw_frames_ctx)
        dec->sw_format = ((AVHWFramesContext *)dec->avctx->hw_frames_ctx->data)->sw_format;
    else
        dec->sw_format = AV_PIX_FMT_NV12;

    dec->out_width  = dec->width;
    dec->out_height = dec->height;

    /* 解码后 VPP 缩放：接一层 scale_vaapi，帧数据不回到 CPU */
    if (config && config->scale_width > 0 && config->scale_height > 0) {
        dec->vpp = vaapi_vpp_create(dec->device_ctx, dec->avctx->hw_frames_ctx,
                                    dec->width, dec->height,
                                    config->scale_width, config->scale_height,
                                    (int)(dec->fps + 0.5));
        if (!dec->vpp) {
            LOG_ERROR("初始化解码后 VPP 缩放失败 (%dx%d -> %dx%d)",
                      dec->width, dec->height, config->scale_width, config->scale_height);
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
        dec->out_width  = config->scale_width;
        dec->out_height = config->scale_height;
    }

    *decoder = dec;
    return 0;

fail:
    vaapi_decoder_close(dec);
    return ret;
}

void vaapi_decoder_close(VaapiDecoder *decoder)
{
    if (!decoder)
        return;

    if (decoder->vpp)
        vaapi_vpp_free(decoder->vpp);
    if (decoder->sws)
        sws_freeContext(decoder->sws);
    if (decoder->sw_frame)
        av_frame_free(&decoder->sw_frame);
    if (decoder->convert_frame)
        av_frame_free(&decoder->convert_frame);
    if (decoder->avctx)
        avcodec_free_context(&decoder->avctx);
    if (decoder->format_ctx)
        avformat_close_input(&decoder->format_ctx);
    av_buffer_unref(&decoder->device_ctx);
    free(decoder);
}

/*
 * 处理一帧解码结果：
 *   1. 若启用 VPP，先在显存内完成缩放；
 *   2. 需要硬件帧（零拷贝转码）直接回调，否则取回内存并按需转换颜色格式。
 */
static int deliver_frame(VaapiDecoder *dec, AVFrame *hw_frame,
                         VaapiFrameHandler handler, void *opaque)
{
    AVFrame *scaled = NULL;
    AVFrame *source = hw_frame;
    int ret;

    if (dec->vpp) {
        ret = vaapi_vpp_process(dec->vpp, hw_frame, &scaled);
        if (ret < 0) {
            LOG_ERROR("VPP 缩放失败: %s", vaapi_error_string(ret));
            return ret;
        }
        source = scaled;
    }

    if (dec->want_hw_frames) {
        ret = handler(opaque, source);   /* 零拷贝：直接把硬件帧交给上层 */
        av_frame_free(&scaled);
        return ret;
    }

    if (!dec->sw_frame) {
        dec->sw_frame = av_frame_alloc();
        if (!dec->sw_frame) {
            av_frame_free(&scaled);
            return AVERROR(ENOMEM);
        }
    }
    av_frame_unref(dec->sw_frame);

    ret = av_hwframe_transfer_data(dec->sw_frame, source, 0);
    if (ret < 0) {
        LOG_ERROR("从显存取回帧数据失败: %s", vaapi_error_string(ret));
        av_frame_free(&scaled);
        return ret;
    }
    dec->sw_frame->pts = source->pts;
    av_frame_free(&scaled);

    if (dec->out_format == AV_PIX_FMT_NONE || dec->sw_frame->format == dec->out_format)
        return handler(opaque, dec->sw_frame);

    /* 需要做颜色格式转换（例如 NV12 -> I420，便于和原始 YUV 文件对比） */
    if (!dec->sws || dec->sws_src_format != dec->sw_frame->format ||
        dec->sws_width != dec->sw_frame->width || dec->sws_height != dec->sw_frame->height) {
        if (dec->sws)
            sws_freeContext(dec->sws);
        dec->sws = sws_getContext(dec->sw_frame->width, dec->sw_frame->height,
                                  (enum AVPixelFormat)dec->sw_frame->format,
                                  dec->sw_frame->width, dec->sw_frame->height,
                                  dec->out_format, SWS_BILINEAR, NULL, NULL, NULL);
        if (!dec->sws) {
            LOG_ERROR("创建颜色转换上下文失败");
            return AVERROR_EXTERNAL;
        }
        dec->sws_src_format = dec->sw_frame->format;
        dec->sws_width      = dec->sw_frame->width;
        dec->sws_height     = dec->sw_frame->height;
        av_frame_free(&dec->convert_frame);   /* 尺寸可能变化，重新分配 */
    }

    if (!dec->convert_frame) {
        dec->convert_frame = av_frame_alloc();
        if (!dec->convert_frame)
            return AVERROR(ENOMEM);
        dec->convert_frame->format = dec->out_format;
        dec->convert_frame->width  = dec->sw_frame->width;
        dec->convert_frame->height = dec->sw_frame->height;
        if (av_frame_get_buffer(dec->convert_frame, 32) < 0) {
            av_frame_free(&dec->convert_frame);
            return AVERROR(ENOMEM);
        }
    }

    sws_scale(dec->sws, (const uint8_t * const *)dec->sw_frame->data, dec->sw_frame->linesize,
              0, dec->sw_frame->height, dec->convert_frame->data, dec->convert_frame->linesize);
    dec->convert_frame->pts = dec->sw_frame->pts;

    return handler(opaque, dec->convert_frame);
}

static int receive_frames(VaapiDecoder *dec, VaapiFrameHandler handler, void *opaque)
{
    AVFrame *frame = av_frame_alloc();
    int ret = 0;

    if (!frame)
        return AVERROR(ENOMEM);

    for (;;) {
        ret = avcodec_receive_frame(dec->avctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            ret = 0;
            break;
        }
        if (ret < 0) {
            LOG_ERROR("解码失败: %s", vaapi_error_string(ret));
            break;
        }

        dec->frames_decoded++;
        ret = deliver_frame(dec, frame, handler, opaque);
        av_frame_unref(frame);
        if (ret < 0)
            break;
        if (dec->max_frames > 0 && dec->frames_decoded >= dec->max_frames) {
            ret = AVERROR_EOF;
            break;
        }
    }

    av_frame_free(&frame);
    return ret;
}

int vaapi_decoder_decode(VaapiDecoder *dec, VaapiFrameHandler handler, void *opaque)
{
    AVPacket *packet;
    int ret = 0;

    if (!dec || !handler)
        return AVERROR(EINVAL);

    packet = av_packet_alloc();
    if (!packet)
        return AVERROR(ENOMEM);

    while (av_read_frame(dec->format_ctx, packet) >= 0) {
        if (packet->stream_index == dec->stream_index) {
            ret = avcodec_send_packet(dec->avctx, packet);
            if (ret < 0) {
                LOG_ERROR("送入解码数据包失败: %s", vaapi_error_string(ret));
                av_packet_unref(packet);
                break;
            }
            ret = receive_frames(dec, handler, opaque);
            if (ret < 0)
                break;
        }
        av_packet_unref(packet);
        if (ret == AVERROR_EOF)
            break;
    }
    av_packet_unref(packet);
    av_packet_free(&packet);

    if (ret == AVERROR_EOF && dec->max_frames > 0)
        return 0;
    if (ret < 0)
        return ret;

    /* 冲刷解码器 */
    ret = avcodec_send_packet(dec->avctx, NULL);
    if (ret < 0 && ret != AVERROR_EOF) {
        LOG_ERROR("冲刷解码器失败: %s", vaapi_error_string(ret));
        return ret;
    }
    return receive_frames(dec, handler, opaque);
}

int vaapi_decoder_width(const VaapiDecoder *decoder)
{
    return decoder ? decoder->out_width : 0;
}

int vaapi_decoder_height(const VaapiDecoder *decoder)
{
    return decoder ? decoder->out_height : 0;
}

int vaapi_decoder_source_width(const VaapiDecoder *decoder)
{
    return decoder ? decoder->width : 0;
}

int vaapi_decoder_source_height(const VaapiDecoder *decoder)
{
    return decoder ? decoder->height : 0;
}

double vaapi_decoder_fps(const VaapiDecoder *decoder)
{
    return decoder ? decoder->fps : 0.0;
}

int64_t vaapi_decoder_frame_count(const VaapiDecoder *decoder)
{
    return decoder ? decoder->frame_count : 0;
}

double vaapi_decoder_duration(const VaapiDecoder *decoder)
{
    return decoder ? decoder->duration : 0.0;
}

const char *vaapi_decoder_codec_name(const VaapiDecoder *decoder)
{
    return decoder ? decoder->codec_name : "";
}

enum AVPixelFormat vaapi_decoder_sw_format(const VaapiDecoder *decoder)
{
    return decoder ? decoder->sw_format : AV_PIX_FMT_NONE;
}

int vaapi_decoder_frames_decoded(const VaapiDecoder *decoder)
{
    return decoder ? decoder->frames_decoded : 0;
}

AVBufferRef *vaapi_decoder_frames_ctx(VaapiDecoder *decoder)
{
    if (!decoder || !decoder->avctx)
        return NULL;
    return decoder->avctx->hw_frames_ctx;
}

AVBufferRef *vaapi_decoder_device_ctx(VaapiDecoder *decoder)
{
    return decoder ? decoder->device_ctx : NULL;
}
