#include "vaapi_encoder.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

struct VaapiEncoder {
    AVCodecContext *avctx;
    AVBufferRef *device_ctx;
    AVBufferRef *frames_ctx;
    int owns_device;
    int width;
    int height;
    char codec_name[32];
    VaapiEncoderStats stats;
};

static const char *normalize_codec_name(const char *name)
{
    if (!name || !*name)
        return "h264";
    if (!strcasecmp(name, "h264") || !strcasecmp(name, "avc") || !strcasecmp(name, "h264_vaapi"))
        return "h264";
    if (!strcasecmp(name, "hevc") || !strcasecmp(name, "h265") || !strcasecmp(name, "hevc_vaapi"))
        return "hevc";
    return NULL;
}

static AVBufferRef *create_frames_ctx(AVBufferRef *device_ctx, int width, int height,
                                      enum AVPixelFormat sw_format)
{
    AVBufferRef *frames_ref;
    AVHWFramesContext *frames;

    frames_ref = av_hwframe_ctx_alloc(device_ctx);
    if (!frames_ref) {
        LOG_ERROR("无法创建 VAAPI 帧上下文");
        return NULL;
    }

    frames = (AVHWFramesContext *)frames_ref->data;
    frames->format            = AV_PIX_FMT_VAAPI;
    frames->sw_format         = sw_format;
    frames->width             = width;
    frames->height            = height;
    frames->initial_pool_size = 20;

    if (av_hwframe_ctx_init(frames_ref) < 0) {
        LOG_ERROR("初始化 VAAPI 帧上下文失败 (sw_format=%s)",
                  av_get_pix_fmt_name(sw_format));
        av_buffer_unref(&frames_ref);
        return NULL;
    }
    return frames_ref;
}

/* 取出编码器所有可用的码流并交给回调。 */
static int drain_encoder(VaapiEncoder *enc, VaapiPacketWriter writer, void *opaque)
{
    AVPacket *packet = av_packet_alloc();
    int ret = 0;

    if (!packet)
        return AVERROR(ENOMEM);

    for (;;) {
        ret = avcodec_receive_packet(enc->avctx, packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            ret = 0;
            break;
        }
        if (ret < 0) {
            LOG_ERROR("接收编码码流失败: %s", vaapi_error_string(ret));
            break;
        }

        enc->stats.frames++;
        enc->stats.bytes += packet->size;

        if (writer && writer(opaque, packet) < 0) {
            av_packet_unref(packet);
            ret = AVERROR(EIO);
            break;
        }
        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    return ret;
}

static int setup_context(VaapiEncoder *enc, const AVCodec *codec,
                         const VaapiEncoderConfig *cfg, int bframes)
{
    int fps = cfg->fps > 0 ? cfg->fps : 25;

    avcodec_free_context(&enc->avctx);
    enc->avctx = avcodec_alloc_context3(codec);
    if (!enc->avctx)
        return AVERROR(ENOMEM);

    enc->avctx->width      = cfg->width;
    enc->avctx->height     = cfg->height;
    enc->avctx->pix_fmt    = AV_PIX_FMT_VAAPI;
    enc->avctx->time_base  = (AVRational){ 1, fps };
    enc->avctx->framerate  = (AVRational){ fps, 1 };
    enc->avctx->gop_size   = cfg->gop > 0 ? cfg->gop : fps;
    enc->avctx->max_b_frames = bframes;
    enc->avctx->sample_aspect_ratio = (AVRational){ 1, 1 };

    enc->avctx->hw_frames_ctx = av_buffer_ref(enc->frames_ctx);
    if (!enc->avctx->hw_frames_ctx)
        return AVERROR(ENOMEM);
    return 0;
}

static int try_open(VaapiEncoder *enc, const AVCodec *codec, int use_qp,
                    int qp, int bitrate_kbps)
{
    AVDictionary *options = NULL;
    int ret;

    if (use_qp) {
        /* 恒定质量(CQP)：画面质量稳定，便于和软件编码对比。 */
        av_dict_set(&options, "rc_mode", "CQP", 0);
        av_dict_set_int(&options, "qp", qp, 0);
    } else {
        int64_t bit_rate = (int64_t)bitrate_kbps * 1000;

        if (bit_rate <= 0)
            bit_rate = 4000000;
        enc->avctx->bit_rate       = bit_rate;
        enc->avctx->rc_max_rate    = bit_rate;
        enc->avctx->rc_buffer_size = (int)bit_rate;
        av_dict_set(&options, "rc_mode", "VBR", 0);
    }

    ret = avcodec_open2(enc->avctx, codec, &options);
    av_dict_free(&options);
    return ret;
}

static int open_codec(VaapiEncoder *enc, const AVCodec *codec,
                      const VaapiEncoderConfig *cfg)
{
    int use_qp   = cfg->bitrate_kbps <= 0 && cfg->qp >= 0;
    int qp       = cfg->qp >= 0 ? cfg->qp : 26;
    int bframes  = cfg->bframes >= 0 ? cfg->bframes : 3;
    int ret;

    if ((ret = setup_context(enc, codec, cfg, bframes)) < 0)
        return ret;

    ret = try_open(enc, codec, use_qp, qp, cfg->bitrate_kbps);

    /* B 帧在一些驱动/低功耗模式下不可用，自动退化 */
    if (ret < 0 && bframes > 0) {
        LOG_WARN("编码器不支持 B 帧(%s)，改为不使用 B 帧重试", vaapi_error_string(ret));
        if ((ret = setup_context(enc, codec, cfg, 0)) < 0)
            return ret;
        ret = try_open(enc, codec, use_qp, qp, cfg->bitrate_kbps);
    }

    /* 个别驱动不支持恒定质量(CQP)，自动退回 VBR */
    if (ret < 0 && use_qp) {
        LOG_WARN("该驱动可能不支持 CQP(%s)，改用 VBR 4Mbps 重试", vaapi_error_string(ret));
        if ((ret = setup_context(enc, codec, cfg, bframes)) < 0)
            return ret;
        ret = try_open(enc, codec, 0, qp, 4000);
        if (ret < 0 && bframes > 0) {
            if ((ret = setup_context(enc, codec, cfg, 0)) < 0)
                return ret;
            ret = try_open(enc, codec, 0, qp, 4000);
        }
    }

    if (ret < 0)
        LOG_ERROR("打开编码器 %s 失败: %s", codec->name, vaapi_error_string(ret));
    return ret;
}

int vaapi_encoder_open(VaapiEncoder **encoder, const VaapiEncoderConfig *config)
{
    VaapiEncoder *enc;
    const AVCodec *codec;
    const char *codec_family;
    char codec_name[32];
    int ret;

    if (!encoder || !config)
        return AVERROR(EINVAL);
    *encoder = NULL;

    codec_family = normalize_codec_name(config->codec);
    if (!codec_family) {
        LOG_ERROR("不支持的编码格式: %s (可选 h264 / hevc)", config->codec);
        return AVERROR(EINVAL);
    }
    if (config->width <= 0 || config->height <= 0 ||
        (config->width & 1) || (config->height & 1)) {
        LOG_ERROR("分辨率必须是正偶数, 当前 %dx%d", config->width, config->height);
        return AVERROR(EINVAL);
    }

    snprintf(codec_name, sizeof(codec_name), "%s_vaapi", codec_family);
    codec = avcodec_find_encoder_by_name(codec_name);
    if (!codec) {
        LOG_ERROR("当前 FFmpeg 未提供 %s 编码器", codec_name);
        LOG_ERROR("请确认 FFmpeg 编译时启用了 --enable-vaapi，并已安装显卡驱动");
        return AVERROR_ENCODER_NOT_FOUND;
    }

    enc = calloc(1, sizeof(*enc));
    if (!enc)
        return AVERROR(ENOMEM);
    enc->width  = config->width;
    enc->height = config->height;
    snprintf(enc->codec_name, sizeof(enc->codec_name), "%s", codec_name);

    /* 1) 硬件设备上下文 */
    if (config->hw_device_ctx) {
        enc->device_ctx = av_buffer_ref(config->hw_device_ctx);
        enc->owns_device = 0;
    } else {
        enc->device_ctx = vaapi_device_create(config->device, config->connection_type);
        enc->owns_device = 1;
    }
    if (!enc->device_ctx) {
        free(enc);
        return AVERROR_EXTERNAL;
    }

    /* 2) 帧池：零拷贝转码时直接复用解码器的帧池 */
    if (config->hw_frames_ctx) {
        enc->frames_ctx = av_buffer_ref(config->hw_frames_ctx);
    } else {
        enum AVPixelFormat sw_format = config->sw_format != AV_PIX_FMT_NONE
                                     ? config->sw_format : AV_PIX_FMT_NV12;
        enc->frames_ctx = create_frames_ctx(enc->device_ctx, config->width,
                                            config->height, sw_format);
    }
    if (!enc->frames_ctx) {
        vaapi_encoder_close(enc);
        return AVERROR_EXTERNAL;
    }

    /* 3) 创建并打开编码器上下文 */
    ret = open_codec(enc, codec, config);
    if (ret < 0) {
        vaapi_encoder_close(enc);
        return ret;
    }

    *encoder = enc;
    return 0;
}

void vaapi_encoder_close(VaapiEncoder *encoder)
{
    if (!encoder)
        return;

    if (encoder->avctx)
        avcodec_free_context(&encoder->avctx);
    av_buffer_unref(&encoder->frames_ctx);
    av_buffer_unref(&encoder->device_ctx);
    free(encoder);
}

AVFrame *vaapi_encoder_alloc_hw_frame(VaapiEncoder *encoder)
{
    AVFrame *frame;

    if (!encoder || !encoder->frames_ctx)
        return NULL;

    frame = av_frame_alloc();
    if (!frame)
        return NULL;
    if (av_hwframe_get_buffer(encoder->frames_ctx, frame, 0) < 0) {
        av_frame_free(&frame);
        return NULL;
    }
    return frame;
}

int vaapi_encoder_send(VaapiEncoder *encoder, AVFrame *frame,
                       VaapiPacketWriter writer, void *opaque)
{
    int ret;

    if (!encoder)
        return AVERROR(EINVAL);

    ret = avcodec_send_frame(encoder->avctx, frame);
    if (ret == AVERROR(EAGAIN)) {
        ret = drain_encoder(encoder, writer, opaque);
        if (ret < 0)
            return ret;
        ret = avcodec_send_frame(encoder->avctx, frame);
    }
    if (ret < 0 && ret != AVERROR_EOF) {
        LOG_ERROR("送入编码帧失败: %s", vaapi_error_string(ret));
        return ret;
    }

    return drain_encoder(encoder, writer, opaque);
}

/* 当输入格式与帧池格式不一致时，先在 CPU 侧做一次颜色格式转换。 */
static AVFrame *convert_to_format(AVFrame *src, enum AVPixelFormat target)
{
    struct SwsContext *sws;
    AVFrame *dst;

    dst = av_frame_alloc();
    if (!dst)
        return NULL;
    dst->format = target;
    dst->width  = src->width;
    dst->height = src->height;
    if (av_frame_get_buffer(dst, 32) < 0) {
        av_frame_free(&dst);
        return NULL;
    }

    sws = sws_getContext(src->width, src->height, (enum AVPixelFormat)src->format,
                         dst->width, dst->height, target,
                         SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) {
        av_frame_free(&dst);
        return NULL;
    }
    sws_scale(sws, (const uint8_t * const *)src->data, src->linesize, 0, src->height,
              dst->data, dst->linesize);
    sws_freeContext(sws);
    return dst;
}

int vaapi_encoder_send_sw(VaapiEncoder *encoder, AVFrame *frame,
                          VaapiPacketWriter writer, void *opaque)
{
    AVHWFramesContext *frames;
    AVFrame *hw_frame;
    AVFrame *converted = NULL;
    AVFrame *input = frame;
    int ret;

    if (!encoder || !frame)
        return AVERROR(EINVAL);

    frames = (AVHWFramesContext *)encoder->frames_ctx->data;

    if (frame->width != frames->width || frame->height != frames->height ||
        frame->format != frames->sw_format) {
        converted = convert_to_format(frame, frames->sw_format);
        if (!converted) {
            LOG_ERROR("转换输入帧格式失败 (%s -> %s)",
                      av_get_pix_fmt_name(frame->format),
                      av_get_pix_fmt_name(frames->sw_format));
            return AVERROR_EXTERNAL;
        }
        input = converted;
    }

    hw_frame = vaapi_encoder_alloc_hw_frame(encoder);
    if (!hw_frame) {
        av_frame_free(&converted);
        return AVERROR(ENOMEM);
    }

    ret = av_hwframe_transfer_data(hw_frame, input, 0);
    if (ret < 0) {
        LOG_ERROR("上传图像到显卡失败: %s", vaapi_error_string(ret));
        av_frame_free(&hw_frame);
        av_frame_free(&converted);
        return ret;
    }
    hw_frame->pts = input->pts;
    hw_frame->duration = input->duration;

    ret = vaapi_encoder_send(encoder, hw_frame, writer, opaque);

    av_frame_free(&hw_frame);
    av_frame_free(&converted);
    return ret;
}

int vaapi_encoder_flush(VaapiEncoder *encoder, VaapiPacketWriter writer, void *opaque)
{
    return vaapi_encoder_send(encoder, NULL, writer, opaque);
}

AVCodecContext *vaapi_encoder_context(VaapiEncoder *encoder)
{
    return encoder ? encoder->avctx : NULL;
}

AVBufferRef *vaapi_encoder_frames_ctx(VaapiEncoder *encoder)
{
    return encoder ? encoder->frames_ctx : NULL;
}

AVBufferRef *vaapi_encoder_device_ctx(VaapiEncoder *encoder)
{
    return encoder ? encoder->device_ctx : NULL;
}

const char *vaapi_encoder_codec_name(VaapiEncoder *encoder)
{
    return encoder ? encoder->codec_name : "";
}

void vaapi_encoder_get_stats(const VaapiEncoder *encoder, VaapiEncoderStats *stats)
{
    if (stats)
        *stats = encoder ? encoder->stats : (VaapiEncoderStats){ 0, 0 };
}
