/*
 * vaapi_encoder.h - 基于 VAAPI 的硬件编码封装
 *
 * 典型用法（客户可直接参考）：
 *   1. vaapi_encoder_open()        创建 AVHWDeviceContext + AVHWFramesContext，
 *                                  并打开 h264_vaapi / hevc_vaapi 编码器；
 *   2. vaapi_encoder_send_sw()     送一帧系统内存图像（自动上传到 GPU）；
 *      或 vaapi_encoder_send()     送一帧已经在显存里的 VAAPI 帧（零拷贝）；
 *   3. vaapi_encoder_flush()       冲刷编码器，取回剩余码流。
 *
 * 编码器只负责产出 AVPacket，码流写到哪里由 VaapiPacketWriter 回调决定，
 * 这样同一份封装既可写文件，也可推流，便于复用。
 */
#ifndef VAAPI_ENCODER_H
#define VAAPI_ENCODER_H

#include <libavcodec/avcodec.h>

#include "vaapi_common.h"

typedef struct VaapiEncoder VaapiEncoder;

/* 码流回调：返回 0 表示成功，负值表示失败并终止编码。 */
typedef int (*VaapiPacketWriter)(void *opaque, AVPacket *packet);

typedef struct {
    const char *codec;          /* "h264"(默认) 或 "hevc"/"h265" */
    int width;                  /* 必须为偶数 */
    int height;                 /* 必须为偶数 */
    int fps;                    /* 默认 25 */
    int bitrate_kbps;           /* >0 时使用码率控制(VBR)；=0 时使用 qp */
    int qp;                     /* 恒定质量模式下的 QP，默认 26；<0 表示不用 CQP */
    int gop;                    /* 关键帧间隔，0 表示等于 fps */
    int bframes;                /* 连续 B 帧数量，默认 3；<0 表示用默认值，0 表示关闭 */
    const char *device;         /* 例如 /dev/dri/renderD128，NULL 自动 */
    const char *connection_type;/* "drm"(默认) / "x11" / NULL 自动 */
    enum AVPixelFormat sw_format;/* 输入图像格式：NV12(默认) / I420 / YUV420P */

    /* 以下两项用于"解码->编码"零拷贝转码：外部设备与解码器帧池。传 NULL 则内部创建。 */
    AVBufferRef *hw_device_ctx;
    AVBufferRef *hw_frames_ctx;
} VaapiEncoderConfig;

typedef struct {
    int frames;          /* 已编码帧数 */
    int64_t bytes;       /* 已输出码流字节数 */
} VaapiEncoderStats;

int vaapi_encoder_open(VaapiEncoder **encoder, const VaapiEncoderConfig *config);
void vaapi_encoder_close(VaapiEncoder *encoder);

/* 送入一帧 VAAPI 硬件帧（AV_PIX_FMT_VAAPI）；frame 为 NULL 等价于 flush。 */
int vaapi_encoder_send(VaapiEncoder *encoder, AVFrame *frame,
                       VaapiPacketWriter writer, void *opaque);

/* 送入一帧系统内存图像，内部自动申请 GPU 表面并上传。 */
int vaapi_encoder_send_sw(VaapiEncoder *encoder, AVFrame *frame,
                          VaapiPacketWriter writer, void *opaque);

/* 冲刷编码器，取回全部剩余码流。 */
int vaapi_encoder_flush(VaapiEncoder *encoder, VaapiPacketWriter writer, void *opaque);

/* 申请一帧本编码器帧池中的硬件帧（AV_PIX_FMT_VAAPI），用完 av_frame_free。 */
AVFrame *vaapi_encoder_alloc_hw_frame(VaapiEncoder *encoder);

AVCodecContext *vaapi_encoder_context(VaapiEncoder *encoder);
AVBufferRef *vaapi_encoder_frames_ctx(VaapiEncoder *encoder);
AVBufferRef *vaapi_encoder_device_ctx(VaapiEncoder *encoder);
const char *vaapi_encoder_codec_name(VaapiEncoder *encoder);
void vaapi_encoder_get_stats(const VaapiEncoder *encoder, VaapiEncoderStats *stats);

#endif /* VAAPI_ENCODER_H */
