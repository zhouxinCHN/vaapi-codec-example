/*
 * vaapi_vpp.h - VAAPI VPP（Video Post-Processing，视频后处理）缩放
 *
 * 通过 VAAPI 的 VideoProc 通道在显存内完成缩放/格式转换，数据不回到 CPU。
 * 实现上使用 FFmpeg 的 scale_vaapi 硬件过滤器，输入输出都是 AV_PIX_FMT_VAAPI 帧，
 * 可直接接在"编码前"或"解码后"：
 *
 *   编码前缩放：YUV -> hwupload -> VPP(scale_vaapi) -> 编码器
 *   解码后缩放：解码器 -> VPP(scale_vaapi) -> 取回内存 / 送编码器 / 显示
 */
#ifndef VAAPI_VPP_H
#define VAAPI_VPP_H

#include <libavcodec/avcodec.h>

#include "vaapi_common.h"

typedef struct VaapiVpp VaapiVpp;

/*
 * 创建 VPP 缩放器。
 *
 * @param device_ctx        VAAPI 设备（必填）
 * @param input_frames_ctx  输入帧池；传 NULL 时内部按 src 尺寸创建 NV12 帧池
 *                          （编码前缩放用，配合 vaapi_vpp_input_frames 上传图像）。
 *                          解码后缩放时传解码器的 hw_frames_ctx。
 * @param src_width/height  输入尺寸（input_frames_ctx 非空时以帧池尺寸为准）
 * @param dst_width/height  缩放后的尺寸
 * @param fps               时基用，仅影响滤镜图参数
 */
VaapiVpp *vaapi_vpp_create(AVBufferRef *device_ctx,
                           AVBufferRef *input_frames_ctx,
                           int src_width, int src_height,
                           int dst_width, int dst_height,
                           int fps);

void vaapi_vpp_free(VaapiVpp *vpp);

/* 输入帧池（仅内部创建时有意义），用于把系统内存图像上传到 GPU。 */
AVBufferRef *vaapi_vpp_input_frames(VaapiVpp *vpp);

/* 输入一帧 VAAPI 帧，输出缩放后的 VAAPI 帧（*dst 由调用方 av_frame_free）。 */
int vaapi_vpp_process(VaapiVpp *vpp, AVFrame *src, AVFrame **dst);

#endif /* VAAPI_VPP_H */
