/*
 * vaapi_decoder.h - 基于 VAAPI 的硬件解码封装
 *
 * 典型用法：
 *   vaapi_decoder_open()   打开媒体文件并创建 VAAPI 解码器；
 *   vaapi_decoder_decode() 循环解码，每解出一帧就回调一次：
 *       - want_hw_frames = 1 : 回调得到 AV_PIX_FMT_VAAPI 硬件帧，
 *                              可直接送进编码器，实现纯 GPU 零拷贝转码；
 *       - want_hw_frames = 0 : 回调得到系统内存帧（NV12 / I420 等），
 *                              用于保存文件、显示或做图像处理。
 */
#ifndef VAAPI_DECODER_H
#define VAAPI_DECODER_H

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include "vaapi_common.h"

typedef struct VaapiDecoder VaapiDecoder;

typedef struct {
    const char *device;              /* /dev/dri/renderD128，NULL 自动 */
    const char *connection_type;     /* "drm"(默认) / "x11" / NULL 自动 */
    int want_hw_frames;              /* 1: 输出 VAAPI 硬件帧（零拷贝） */
    enum AVPixelFormat out_format;   /* 软件帧格式：I420 / NV12；AV_PIX_FMT_NONE 保持解码器原生格式 */
    int max_frames;                  /* 最多解码多少帧，0 表示全部 */
    int scale_width;                 /* VPP 缩放目标宽，0 表示不缩放 */
    int scale_height;                /* VPP 缩放目标高，0 表示不缩放 */
} VaapiDecoderConfig;

/* 帧回调：返回 0 继续解码，返回负值中止。frame 的生命周期仅限回调内。 */
typedef int (*VaapiFrameHandler)(void *opaque, AVFrame *frame);

int vaapi_decoder_open(VaapiDecoder **decoder, const char *path,
                       const VaapiDecoderConfig *config);
int vaapi_decoder_decode(VaapiDecoder *decoder, VaapiFrameHandler handler, void *opaque);
void vaapi_decoder_close(VaapiDecoder *decoder);

/* 输出尺寸：启用 VPP 缩放时返回缩放后的尺寸 */
int vaapi_decoder_width(const VaapiDecoder *decoder);
int vaapi_decoder_height(const VaapiDecoder *decoder);
/* 解码器原始尺寸（缩放前） */
int vaapi_decoder_source_width(const VaapiDecoder *decoder);
int vaapi_decoder_source_height(const VaapiDecoder *decoder);
double vaapi_decoder_fps(const VaapiDecoder *decoder);
int64_t vaapi_decoder_frame_count(const VaapiDecoder *decoder);
double vaapi_decoder_duration(const VaapiDecoder *decoder);
const char *vaapi_decoder_codec_name(const VaapiDecoder *decoder);
enum AVPixelFormat vaapi_decoder_sw_format(const VaapiDecoder *decoder);
int vaapi_decoder_frames_decoded(const VaapiDecoder *decoder);

/* 零拷贝转码时把这两个上下文交给编码器复用。 */
AVBufferRef *vaapi_decoder_frames_ctx(VaapiDecoder *decoder);
AVBufferRef *vaapi_decoder_device_ctx(VaapiDecoder *decoder);

#endif /* VAAPI_DECODER_H */
