/*
 * main.c - VAAPI 硬件编解码演示程序（命令行入口）
 *
 * 子命令:
 *   info       环境自检
 *   encode     硬件编码  YUV  -> H.264 / H.265
 *   decode     硬件解码  MP4  -> YUV
 *   play       硬件解码 + 实时播放（需要 glfw3）
 *   transcode  解码 + 编码，默认零拷贝（帧数据全程留在显存）
 *   selftest   一键全流程校验（编码->解码->PSNR）
 */
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <strings.h>
#include <sys/types.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>

#include "renderer.h"
#include "util.h"
#include "vaapi_common.h"
#include "vaapi_decoder.h"
#include "vaapi_encoder.h"
#include "vaapi_info.h"
#include "vaapi_vpp.h"

#ifndef VAAPI_DEMO_VERSION
#define VAAPI_DEMO_VERSION "1.0.0"
#endif

#define SAMPLE_YUV "1920x1080_24_i420.yuv"
#define SAMPLE_MP4 "video1080.mp4"

typedef struct {
    int frames;
    int64_t bytes;
    double seconds;
} RunStats;

/* ------------------------------------------------------------------------- */
/* 通用小工具                                                                 */
/* ------------------------------------------------------------------------- */

static void print_version(void)
{
    LOG_INFO("vaapi_demo %s (FFmpeg %s)", VAAPI_DEMO_VERSION, av_version_info());
}

static void ensure_parent_dir(const char *path)
{
    char buffer[4096];
    size_t length;

    if (!path)
        return;
    length = strlen(path);
    if (length == 0 || length >= sizeof(buffer))
        return;
    memcpy(buffer, path, length + 1);

    for (char *p = buffer + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buffer, 0755);
            *p = '/';
        }
    }
}

static int parse_pix_fmt(const char *name, enum AVPixelFormat *format)
{
    if (!name)
        return -1;
    if (!strcasecmp(name, "i420") || !strcasecmp(name, "yuv420p") ||
        !strcasecmp(name, "yuv")) {
        *format = AV_PIX_FMT_YUV420P;
        return 0;
    }
    if (!strcasecmp(name, "nv12")) {
        *format = AV_PIX_FMT_NV12;
        return 0;
    }
    return -1;
}

static int64_t frame_size_for(enum AVPixelFormat format, int width, int height)
{
    if (format == AV_PIX_FMT_NV12 || format == AV_PIX_FMT_YUV420P)
        return (int64_t)width * height * 3 / 2;
    return (int64_t)width * height * 3 / 2;
}

/* 计数用回调：只统计字节数，不写文件 */
typedef struct {
    FILE *file;
    int64_t bytes;
} PacketSink;

static int sink_write_packet(void *opaque, AVPacket *packet)
{
    PacketSink *sink = opaque;

    sink->bytes += packet->size;
    if (!sink->file)
        return 0;
    if (fwrite(packet->data, 1, (size_t)packet->size, sink->file) != (size_t)packet->size) {
        LOG_ERROR("写入码流失败: %s", strerror(errno));
        return AVERROR(EIO);
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* 编码                                                                       */
/* ------------------------------------------------------------------------- */

typedef struct {
    const char *input;
    const char *output;
    const char *codec;
    const char *device;
    const char *connection_type;
    int width;
    int height;
    int fps;
    int bitrate_kbps;
    int qp;
    int gop;
    int max_frames;
    int bframes;
    int scale_width;
    int scale_height;
    int quiet;
    enum AVPixelFormat pix_fmt;
} EncodeParams;

static int run_encode(const EncodeParams *params, RunStats *stats)
{
    VaapiEncoderConfig config;
    VaapiEncoder *encoder = NULL;
    VaapiVpp *vpp = NULL;
    AVBufferRef *device_ctx = NULL;
    FILE *input_file = NULL;
    FILE *output_file = NULL;
    AVFrame *frame = NULL;
    PacketSink sink = { 0 };
    Progress progress;
    char codec_name[32];
    int scaling    = params->scale_width > 0 && params->scale_height > 0;
    int out_width  = scaling ? params->scale_width  : params->width;
    int out_height = scaling ? params->scale_height : params->height;
    int ret = -1;
    int frames = 0;
    int64_t start_us;

    snprintf(codec_name, sizeof(codec_name), "%s_vaapi",
             (params->codec && (!strcasecmp(params->codec, "hevc") ||
                                !strcasecmp(params->codec, "h265"))) ? "hevc" : "h264");

    memset(&config, 0, sizeof(config));
    config.codec           = params->codec;
    config.width           = out_width;
    config.height          = out_height;
    config.fps             = params->fps;
    config.bitrate_kbps    = params->bitrate_kbps;
    config.qp              = params->qp;
    config.gop             = params->gop;
    config.bframes         = params->bframes;
    config.device          = params->device;
    config.connection_type = params->connection_type;
    config.sw_format       = AV_PIX_FMT_NV12;   /* 编码器输入统一为 NV12，CPU 侧自动转换 */

    input_file = fopen(params->input, "rb");
    if (!input_file) {
        LOG_ERROR("无法打开输入文件 %s: %s", params->input, strerror(errno));
        return -1;
    }

    ensure_parent_dir(params->output);
    output_file = fopen(params->output, "wb");
    if (!output_file) {
        LOG_ERROR("无法创建输出文件 %s: %s", params->output, strerror(errno));
        fclose(input_file);
        return -1;
    }

    if (scaling) {
        /*
         * 编码前 VPP 缩放：
         *   YUV -> (上传) -> VPP scale_vaapi -> 编码器，全程在显存内完成。
         * 编码器的帧池要等第一帧缩放结果出来后才能确定，因此延迟到循环里打开。
         */
        device_ctx = vaapi_device_create(params->device, params->connection_type);
        if (!device_ctx)
            goto cleanup;

        vpp = vaapi_vpp_create(device_ctx, NULL, params->width, params->height,
                               out_width, out_height, params->fps);
        if (!vpp) {
            LOG_ERROR("初始化编码前 VPP 缩放失败");
            goto cleanup;
        }
    } else if (vaapi_encoder_open(&encoder, &config) < 0) {
        LOG_ERROR("初始化硬件编码器失败");
        goto cleanup;
    }

    if (!params->quiet) {
        LOG_INFO("输入文件 : %s", params->input);
        LOG_INFO("输出文件 : %s", params->output);
        LOG_INFO("编码器   : %s | %dx%d @ %d fps | %s", codec_name,
                 out_width, out_height, params->fps,
                 params->bitrate_kbps > 0
                     ? util_format_bitrate(params->bitrate_kbps * 1000.0)
                     : "恒定质量模式");
        if (scaling)
            LOG_INFO("VPP 缩放 : %dx%d -> %dx%d (GPU VideoProc)",
                     params->width, params->height, out_width, out_height);
    }

    frame = av_frame_alloc();
    if (!frame)
        goto cleanup;
    frame->format = params->pix_fmt;
    frame->width  = params->width;
    frame->height = params->height;
    if (av_frame_get_buffer(frame, 32) < 0) {
        LOG_ERROR("分配输入帧失败");
        goto cleanup;
    }

    sink.file = output_file;
    progress_init(&progress, params->quiet);
    start_us = util_now_us();

    while (params->max_frames <= 0 || frames < params->max_frames) {
        int64_t bytes_before = sink.bytes;

        if (!util_read_yuv_frame(input_file, frame))
            break;
        frame->pts = frames;

        if (scaling) {
            AVFrame *hw_in  = NULL;
            AVFrame *hw_out = NULL;
            int send_ret;

            hw_in = vaapi_frame_upload(vaapi_vpp_input_frames(vpp), frame);
            if (!hw_in) {
                LOG_ERROR("上传输入图像失败");
                goto cleanup;
            }
            send_ret = vaapi_vpp_process(vpp, hw_in, &hw_out);
            av_frame_free(&hw_in);
            if (send_ret < 0)
                goto cleanup;

            if (!encoder) {
                config.hw_device_ctx = device_ctx;
                config.hw_frames_ctx = hw_out->hw_frames_ctx;
                if (vaapi_encoder_open(&encoder, &config) < 0) {
                    LOG_ERROR("初始化硬件编码器失败");
                    av_frame_free(&hw_out);
                    goto cleanup;
                }
            }

            send_ret = vaapi_encoder_send(encoder, hw_out, sink_write_packet, &sink);
            av_frame_free(&hw_out);
            if (send_ret < 0)
                goto cleanup;
        } else if (vaapi_encoder_send_sw(encoder, frame, sink_write_packet, &sink) < 0) {
            goto cleanup;
        }

        frames++;
        progress_tick(&progress, sink.bytes - bytes_before);
    }

    if (frames == 0) {
        LOG_ERROR("没有读到任何画面，请检查 -s 分辨率 / -p 像素格式是否与输入文件匹配");
        LOG_ERROR("当前设置: %dx%d %s", params->width, params->height,
                  av_get_pix_fmt_name(params->pix_fmt));
        goto cleanup;
    }

    {
        int64_t bytes_before = sink.bytes;
        if (vaapi_encoder_flush(encoder, sink_write_packet, &sink) < 0)
            goto cleanup;
        progress.bytes += sink.bytes - bytes_before;
    }

    stats->frames  = frames;
    stats->bytes   = sink.bytes;
    stats->seconds = (util_now_us() - start_us) / 1000000.0;

    if (!params->quiet)
        progress_finish(&progress);

    LOG_INFO("----------------------------------------------");
    LOG_INFO("编码完成: %d 帧, 输出 %s, %.1f fps, 平均码率 %s", frames,
             util_format_size(sink.bytes),
             stats->seconds > 0 ? frames / stats->seconds : 0.0,
             util_format_bitrate(params->fps > 0
                                 ? sink.bytes * 8.0 * params->fps / frames : 0.0));
    ret = 0;

cleanup:
    if (frame)
        av_frame_free(&frame);
    if (encoder)
        vaapi_encoder_close(encoder);
    if (vpp)
        vaapi_vpp_free(vpp);
    av_buffer_unref(&device_ctx);
    if (output_file)
        fclose(output_file);
    if (input_file)
        fclose(input_file);

    if (ret < 0) {
        LOG_ERROR("编码失败");
        if (output_file)
            unlink(params->output);
    }
    return ret;
}

static void encode_usage(void)
{
    fprintf(stdout,
        "用法: vaapi_demo encode [选项]\n"
        "\n"
        "  -i, --input <文件>        输入原始 YUV（默认 samples/%s）\n"
        "  -o, --output <文件>       输出码流（默认 output/encoded.h264|h265）\n"
        "  -s, --size <宽x高>        输入分辨率，默认 1920x1080\n"
        "  -S, --scale <宽x高>       编码前用 VPP 缩放，例如 1280x720（默认不缩放）\n"
        "  -p, --pix-fmt <i420|nv12> 输入像素格式，默认 i420\n"
        "  -c, --codec <h264|hevc>   编码格式，默认 h264\n"
        "  -r, --fps <帧率>          帧率，默认 25\n"
        "  -b, --bitrate <kbps>      目标码率，指定后使用 VBR 码率控制\n"
        "      --qp <0-51>           恒定质量 QP，默认 26（未指定 -b 时生效）\n"
        "  -g, --gop <帧数>          关键帧间隔，默认等于帧率\n"
        "      --bframes <N>         连续 B 帧数量，默认 3，0 表示关闭\n"
        "      --max-frames <N>      只编码前 N 帧\n"
        "  -d, --device <节点>       DRM 设备，例如 /dev/dri/renderD128\n"
        "      --connection-type <t> drm(默认) / x11\n"
        "  -q, --quiet               精简输出\n"
        "  -h, --help                显示帮助\n",
        SAMPLE_YUV);
}

static int command_encode(int argc, char **argv)
{
    EncodeParams params;
    RunStats stats;
    char sample_path[4096];
    char default_output[64];
    static const struct option options[] = {
        { "input",           required_argument, NULL, 'i' },
        { "output",          required_argument, NULL, 'o' },
        { "size",            required_argument, NULL, 's' },
        { "scale",           required_argument, NULL, 'S' },
        { "pix-fmt",         required_argument, NULL, 'p' },
        { "codec",           required_argument, NULL, 'c' },
        { "fps",             required_argument, NULL, 'r' },
        { "bitrate",         required_argument, NULL, 'b' },
        { "gop",             required_argument, NULL, 'g' },
        { "qp",              required_argument, NULL, 1000 },
        { "max-frames",      required_argument, NULL, 1001 },
        { "bframes",         required_argument, NULL, 1003 },
        { "device",          required_argument, NULL, 'd' },
        { "connection-type", required_argument, NULL, 1002 },
        { "quiet",           no_argument,       NULL, 'q' },
        { "help",            no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    int c;

    memset(&params, 0, sizeof(params));
    params.width           = 1920;
    params.height          = 1080;
    params.fps             = 25;
    params.qp              = 26;
    params.bframes         = 3;
    params.codec           = "h264";
    params.connection_type = "drm";
    params.pix_fmt         = AV_PIX_FMT_YUV420P;

    while ((c = getopt_long(argc, argv, "i:o:s:S:p:c:r:b:g:d:qh", options, NULL)) != -1) {
        switch (c) {
        case 'i': params.input = optarg; break;
        case 'o': params.output = optarg; break;
        case 's':
            if (vaapi_parse_size(optarg, &params.width, &params.height) < 0) {
                LOG_ERROR("分辨率格式错误: %s (应形如 1920x1080)", optarg);
                return 1;
            }
            break;
        case 'S':
            if (vaapi_parse_size(optarg, &params.scale_width, &params.scale_height) < 0 ||
                (params.scale_width & 1) || (params.scale_height & 1)) {
                LOG_ERROR("缩放尺寸格式错误: %s (应形如 1280x720，且宽高为偶数)", optarg);
                return 1;
            }
            break;
        case 'p':
            if (parse_pix_fmt(optarg, &params.pix_fmt) < 0) {
                LOG_ERROR("不支持的像素格式: %s (可选 i420 / nv12)", optarg);
                return 1;
            }
            break;
        case 'c': params.codec = optarg; break;
        case 'r': params.fps = atoi(optarg); break;
        case 'b': params.bitrate_kbps = atoi(optarg); break;
        case 'g': params.gop = atoi(optarg); break;
        case 1000: params.qp = atoi(optarg); break;
        case 1001: params.max_frames = atoi(optarg); break;
        case 1003: params.bframes = atoi(optarg); break;
        case 'd': params.device = optarg; break;
        case 1002: params.connection_type = optarg; break;
        case 'q': params.quiet = 1; break;
        case 'h':
        default:
            encode_usage();
            return c == 'h' ? 0 : 1;
        }
    }

    if (params.fps <= 0) {
        LOG_ERROR("帧率必须大于 0");
        return 1;
    }

    const char *family = "h264";
    if (params.codec && (!strcasecmp(params.codec, "hevc") || !strcasecmp(params.codec, "h265")))
        family = "hevc";

    if (!params.input) {
        if (!util_find_sample(SAMPLE_YUV, sample_path, sizeof(sample_path))) {
            LOG_ERROR("未指定 -i，且找不到示例文件 samples/%s", SAMPLE_YUV);
            return 1;
        }
        params.input = sample_path;
        LOG_INFO("未指定输入文件，使用内置示例: %s", params.input);
    }
    if (!params.output) {
        snprintf(default_output, sizeof(default_output), "output/encoded.%s",
                 strcmp(family, "hevc") == 0 ? "h265" : "h264");
        params.output = default_output;
    }

    return run_encode(&params, &stats) < 0 ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/* 解码                                                                       */
/* ------------------------------------------------------------------------- */

typedef struct {
    const char *input;
    const char *output;
    const char *device;
    const char *connection_type;
    const char *snapshot;
    enum AVPixelFormat out_format;
    int max_frames;
    int scale_width;
    int scale_height;
    int quiet;
} DecodeParams;

typedef struct {
    FILE *file;
    const char *snapshot_path;
    int snapshot_done;
    int quiet;
    Progress progress;
    int width;
    int height;
} DecodeSink;

static int decode_frame_handler(void *opaque, AVFrame *frame)
{
    DecodeSink *sink = opaque;

    if (sink->file && util_write_yuv_frame(sink->file, frame) < 0) {
        LOG_ERROR("写入 YUV 文件失败: %s", strerror(errno));
        return AVERROR(EIO);
    }
    if (sink->snapshot_path && !sink->snapshot_done) {
        if (util_write_ppm(sink->snapshot_path, frame) == 0)
            LOG_INFO("已保存首帧截图: %s", sink->snapshot_path);
        else
            LOG_WARN("保存截图失败: %s", sink->snapshot_path);
        sink->snapshot_done = 1;
    }

    progress_tick(&sink->progress, frame_size_for(frame->format, frame->width, frame->height));
    return 0;
}

static int run_decode(const DecodeParams *params, RunStats *stats)
{
    VaapiDecoderConfig config;
    VaapiDecoder *decoder = NULL;
    FILE *output_file = NULL;
    DecodeSink sink;
    int ret;
    int64_t start_us;

    memset(&config, 0, sizeof(config));
    config.device          = params->device;
    config.connection_type = params->connection_type;
    config.want_hw_frames  = 0;
    config.out_format      = params->out_format;
    config.max_frames      = params->max_frames;
    config.scale_width     = params->scale_width;
    config.scale_height    = params->scale_height;

    if (vaapi_decoder_open(&decoder, params->input, &config) < 0)
        return -1;

    if (params->output) {
        ensure_parent_dir(params->output);
        output_file = fopen(params->output, "wb");
        if (!output_file) {
            LOG_ERROR("无法创建输出文件 %s: %s", params->output, strerror(errno));
            vaapi_decoder_close(decoder);
            return -1;
        }
    }

    if (!params->quiet) {
        LOG_INFO("输入文件 : %s", params->input);
        LOG_INFO("解码器   : %s (VAAPI) | %dx%d | %.3f fps | 时长 %.2f 秒",
                 vaapi_decoder_codec_name(decoder), vaapi_decoder_source_width(decoder),
                 vaapi_decoder_source_height(decoder), vaapi_decoder_fps(decoder),
                 vaapi_decoder_duration(decoder));
        if (params->scale_width > 0)
            LOG_INFO("VPP 缩放 : %dx%d -> %dx%d (GPU VideoProc)",
                     vaapi_decoder_source_width(decoder), vaapi_decoder_source_height(decoder),
                     vaapi_decoder_width(decoder), vaapi_decoder_height(decoder));
        if (params->output)
            LOG_INFO("输出文件 : %s (%s)", params->output,
                     av_get_pix_fmt_name(params->out_format));
    }

    memset(&sink, 0, sizeof(sink));
    sink.file          = output_file;
    sink.snapshot_path = params->snapshot;
    sink.quiet         = params->quiet;
    sink.width         = vaapi_decoder_width(decoder);
    sink.height        = vaapi_decoder_height(decoder);
    progress_init(&sink.progress, params->quiet);

    start_us = util_now_us();
    ret = vaapi_decoder_decode(decoder, decode_frame_handler, &sink);
    stats->seconds = (util_now_us() - start_us) / 1000000.0;
    stats->frames  = vaapi_decoder_frames_decoded(decoder);
    stats->bytes   = frame_size_for(params->out_format,
                                    vaapi_decoder_width(decoder),
                                    vaapi_decoder_height(decoder)) * stats->frames;

    if (!params->quiet)
        progress_finish(&sink.progress);

    if (ret == 0) {
        LOG_INFO("----------------------------------------------");
        LOG_INFO("解码完成: %d 帧, 耗时 %.2f 秒 (%.1f fps)", stats->frames,
                 stats->seconds, stats->seconds > 0 ? stats->frames / stats->seconds : 0.0);
    }

    if (output_file)
        fclose(output_file);
    vaapi_decoder_close(decoder);

    if (ret < 0) {
        LOG_ERROR("解码失败");
        if (params->output)
            unlink(params->output);
        return -1;
    }
    return 0;
}

static void decode_usage(void)
{
    fprintf(stdout,
        "用法: vaapi_demo decode [选项]\n"
        "\n"
        "  -i, --input <文件>        输入视频（默认 samples/%s）\n"
        "  -o, --output <文件>       输出原始 YUV（默认 output/decoded_<fmt>.yuv）\n"
        "  -p, --pix-fmt <i420|nv12> 输出像素格式，默认 i420\n"
        "  -S, --scale <宽x高>       解码后用 VPP 缩放，例如 1280x720（默认不缩放）\n"
        "      --max-frames <N>      只解码前 N 帧\n"
        "      --snapshot <文件.ppm> 保存首帧为图片，便于无显示环境查看画面\n"
        "  -d, --device <节点>       DRM 设备，例如 /dev/dri/renderD128\n"
        "      --connection-type <t> drm(默认) / x11\n"
        "  -q, --quiet               精简输出\n"
        "  -h, --help                显示帮助\n",
        SAMPLE_MP4);
}

static int command_decode(int argc, char **argv)
{
    DecodeParams params;
    RunStats stats;
    char sample_path[4096];
    char default_output[64];
    static const struct option options[] = {
        { "input",           required_argument, NULL, 'i' },
        { "output",          required_argument, NULL, 'o' },
        { "pix-fmt",         required_argument, NULL, 'p' },
        { "scale",           required_argument, NULL, 'S' },
        { "max-frames",      required_argument, NULL, 1000 },
        { "snapshot",        required_argument, NULL, 1001 },
        { "device",          required_argument, NULL, 'd' },
        { "connection-type", required_argument, NULL, 1002 },
        { "quiet",           no_argument,       NULL, 'q' },
        { "help",            no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    int c;

    memset(&params, 0, sizeof(params));
    params.connection_type = "drm";
    params.out_format      = AV_PIX_FMT_YUV420P;

    while ((c = getopt_long(argc, argv, "i:o:p:S:d:qh", options, NULL)) != -1) {
        switch (c) {
        case 'i': params.input = optarg; break;
        case 'o': params.output = optarg; break;
        case 'p':
            if (parse_pix_fmt(optarg, &params.out_format) < 0) {
                LOG_ERROR("不支持的像素格式: %s (可选 i420 / nv12)", optarg);
                return 1;
            }
            break;
        case 'S':
            if (vaapi_parse_size(optarg, &params.scale_width, &params.scale_height) < 0 ||
                (params.scale_width & 1) || (params.scale_height & 1)) {
                LOG_ERROR("缩放尺寸格式错误: %s (应形如 1280x720，且宽高为偶数)", optarg);
                return 1;
            }
            break;
        case 1000: params.max_frames = atoi(optarg); break;
        case 1001: params.snapshot = optarg; break;
        case 'd': params.device = optarg; break;
        case 1002: params.connection_type = optarg; break;
        case 'q': params.quiet = 1; break;
        case 'h':
        default:
            decode_usage();
            return c == 'h' ? 0 : 1;
        }
    }

    if (!params.input) {
        if (!util_find_sample(SAMPLE_MP4, sample_path, sizeof(sample_path))) {
            LOG_ERROR("未指定 -i，且找不到示例文件 samples/%s", SAMPLE_MP4);
            return 1;
        }
        params.input = sample_path;
        LOG_INFO("未指定输入文件，使用内置示例: %s", params.input);
    }
    if (!params.output) {
        snprintf(default_output, sizeof(default_output), "output/decoded_%s.yuv",
                 params.out_format == AV_PIX_FMT_NV12 ? "nv12" : "i420");
        params.output = default_output;
    }

    return run_decode(&params, &stats) < 0 ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/* 播放                                                                       */
/* ------------------------------------------------------------------------- */

typedef struct {
    Renderer *renderer;
    double start_time;
    double fps;
    int frames;
    int width;
    int height;
} PlayState;

static int play_frame_handler(void *opaque, AVFrame *frame)
{
    PlayState *state = opaque;
    double target;

    if (renderer_should_close(state->renderer))
        return AVERROR_EXIT;

    if (renderer_draw_nv12(state->renderer, frame->data[0],
                           frame->linesize[0], frame->linesize[1]) < 0)
        return AVERROR_EXTERNAL;
    renderer_swap_buffers(state->renderer);
    renderer_poll_events(state->renderer);

    state->frames++;
    target = state->start_time + state->frames / (state->fps > 0 ? state->fps : 25.0);
    double now = renderer_time();
    if (target > now)
        usleep((useconds_t)((target - now) * 1000000.0));
    return 0;
}

static void play_usage(void)
{
    fprintf(stdout,
        "用法: vaapi_demo play [选项]\n"
        "\n"
        "  -i, --input <文件>        输入视频（默认 samples/%s）\n"
        "  -S, --scale <宽x高>       解码后用 VPP 缩放显示，例如 1280x720\n"
        "      --loop                循环播放\n"
        "  -d, --device <节点>       DRM 设备\n"
        "      --connection-type <t> drm(默认) / x11\n"
        "  -h, --help                显示帮助\n",
        SAMPLE_MP4);
}

static int command_play(int argc, char **argv)
{
    VaapiDecoderConfig config;
    VaapiDecoder *decoder = NULL;
    Renderer *renderer = NULL;
    PlayState state;
    const char *input = NULL;
    const char *device = NULL;
    const char *connection_type = "drm";
    char sample_path[4096];
    int scale_width = 0;
    int scale_height = 0;
    int loop = 0;
    int c;
    static const struct option options[] = {
        { "input",           required_argument, NULL, 'i' },
        { "scale",           required_argument, NULL, 'S' },
        { "loop",            no_argument,       NULL, 1000 },
        { "device",          required_argument, NULL, 'd' },
        { "connection-type", required_argument, NULL, 1001 },
        { "help",            no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    while ((c = getopt_long(argc, argv, "i:S:d:h", options, NULL)) != -1) {
        switch (c) {
        case 'i': input = optarg; break;
        case 'S':
            if (vaapi_parse_size(optarg, &scale_width, &scale_height) < 0 ||
                (scale_width & 1) || (scale_height & 1)) {
                LOG_ERROR("缩放尺寸格式错误: %s (应形如 1280x720，且宽高为偶数)", optarg);
                return 1;
            }
            break;
        case 1000: loop = 1; break;
        case 'd': device = optarg; break;
        case 1001: connection_type = optarg; break;
        case 'h':
        default:
            play_usage();
            return c == 'h' ? 0 : 1;
        }
    }

    if (!renderer_available()) {
        LOG_ERROR("当前程序编译时没有启用播放功能（缺少 glfw3）");
        LOG_ERROR("安装依赖后重新编译即可: sudo apt install libglfw3-dev && make clean && make");
        return 1;
    }

    if (!input) {
        if (!util_find_sample(SAMPLE_MP4, sample_path, sizeof(sample_path))) {
            LOG_ERROR("未指定 -i，且找不到示例文件 samples/%s", SAMPLE_MP4);
            return 1;
        }
        input = sample_path;
    }

    memset(&config, 0, sizeof(config));
    config.device          = device;
    config.connection_type = connection_type;
    config.want_hw_frames  = 0;
    config.out_format      = AV_PIX_FMT_NV12;   /* 显示模块按 NV12 上传纹理 */
    config.scale_width     = scale_width;
    config.scale_height    = scale_height;

    if (vaapi_decoder_open(&decoder, input, &config) < 0)
        return 1;

    renderer = renderer_create(vaapi_decoder_width(decoder), vaapi_decoder_height(decoder),
                               "VAAPI 硬件解码播放器");
    if (!renderer) {
        vaapi_decoder_close(decoder);
        return 1;
    }

    memset(&state, 0, sizeof(state));
    state.renderer   = renderer;
    state.fps        = vaapi_decoder_fps(decoder);
    state.width      = vaapi_decoder_width(decoder);
    state.height     = vaapi_decoder_height(decoder);
    state.start_time = renderer_time();

    LOG_INFO("播放中: %s (%dx%d, %.2f fps)，关闭窗口或按 ESC 退出",
             input, state.width, state.height, state.fps);

    do {
        state.frames     = 0;
        state.start_time = renderer_time();
        if (vaapi_decoder_decode(decoder, play_frame_handler, &state) < 0)
            break;
    } while (loop && !renderer_should_close(renderer));

    renderer_destroy(renderer);
    vaapi_decoder_close(decoder);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* 转码（解码 -> 编码，默认零拷贝）                                            */
/* ------------------------------------------------------------------------- */

typedef struct {
    VaapiDecoder *decoder;
    VaapiEncoder *encoder;
    FILE *output_file;
    PacketSink sink;
    const char *codec;
    int qp;
    int bitrate_kbps;
    int gop;
    int bframes;
    int scale_width;
    int scale_height;
    int want_zero_copy;
    int zero_copy_used;
    int frames;
    int width;
    int height;
    int fps;
    Progress progress;
} TranscodeState;

static int transcode_frame_handler(void *opaque, AVFrame *frame)
{
    TranscodeState *state = opaque;
    int64_t bytes_before = state->sink.bytes;

    if (!state->encoder) {
        VaapiEncoderConfig config;

        memset(&config, 0, sizeof(config));
        config.codec        = state->codec;
        config.width        = state->width;
        config.height       = state->height;
        config.fps          = state->fps;
        config.qp           = state->qp;
        config.bitrate_kbps = state->bitrate_kbps;
        config.gop          = state->gop;
        config.bframes      = state->bframes;
        config.sw_format    = AV_PIX_FMT_NV12;

        state->zero_copy_used = state->want_zero_copy && frame->hw_frames_ctx != NULL;
        if (state->zero_copy_used) {
            config.hw_device_ctx = vaapi_decoder_device_ctx(state->decoder);
            config.hw_frames_ctx = frame->hw_frames_ctx;
        }

        if (vaapi_encoder_open(&state->encoder, &config) < 0) {
            LOG_ERROR("初始化编码器失败");
            return AVERROR_EXTERNAL;
        }
        LOG_INFO("转码方式: %s", state->zero_copy_used ? "零拷贝（解码帧直接送编码器，数据不出显存）"
                                                       : "CPU 中转（显存 -> 内存 -> 显存）");
    }

    if (frame->format == AV_PIX_FMT_VAAPI) {
        if (vaapi_encoder_send(state->encoder, frame, sink_write_packet, &state->sink) < 0)
            return AVERROR_EXTERNAL;
    } else {
        if (vaapi_encoder_send_sw(state->encoder, frame, sink_write_packet, &state->sink) < 0)
            return AVERROR_EXTERNAL;
    }

    state->frames++;
    progress_tick(&state->progress, state->sink.bytes - bytes_before);
    return 0;
}

static void transcode_usage(void)
{
    fprintf(stdout,
        "用法: vaapi_demo transcode [选项]\n"
        "\n"
        "  -i, --input <文件>        输入视频（默认 samples/%s）\n"
        "  -o, --output <文件>       输出码流（默认 output/transcoded.h264|h265）\n"
        "  -c, --codec <h264|hevc>   输出编码格式，默认 h264\n"
        "  -b, --bitrate <kbps>      目标码率（默认使用恒定质量）\n"
        "      --qp <0-51>           恒定质量 QP，默认 26\n"
        "  -g, --gop <帧数>          关键帧间隔，默认等于帧率\n"
        "  -S, --scale <宽x高>       解码后用 VPP 缩放再编码，例如 1280x720\n"
        "      --max-frames <N>      只转码前 N 帧\n"
        "      --no-zero-copy        强制采用 CPU 中转（用于对比零拷贝开销）\n"
        "  -d, --device <节点>       DRM 设备\n"
        "      --connection-type <t> drm(默认) / x11\n"
        "  -q, --quiet               精简输出\n"
        "  -h, --help                显示帮助\n",
        SAMPLE_MP4);
}

static int command_transcode(int argc, char **argv)
{
    TranscodeState state;
    VaapiDecoderConfig config;
    const char *input = NULL;
    const char *output = NULL;
    const char *device = NULL;
    const char *connection_type = "drm";
    char sample_path[4096];
    char default_output[64];
    int quiet = 0;
    int max_frames = 0;
    int scale_width = 0;
    int scale_height = 0;
    int c;
    int ret;
    int64_t start_us;
    static const struct option options[] = {
        { "input",           required_argument, NULL, 'i' },
        { "output",          required_argument, NULL, 'o' },
        { "codec",           required_argument, NULL, 'c' },
        { "bitrate",         required_argument, NULL, 'b' },
        { "qp",              required_argument, NULL, 1000 },
        { "gop",             required_argument, NULL, 'g' },
        { "scale",           required_argument, NULL, 'S' },
        { "max-frames",      required_argument, NULL, 1001 },
        { "no-zero-copy",    no_argument,       NULL, 1002 },
        { "device",          required_argument, NULL, 'd' },
        { "connection-type", required_argument, NULL, 1003 },
        { "quiet",           no_argument,       NULL, 'q' },
        { "help",            no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    memset(&state, 0, sizeof(state));
    state.codec          = "h264";
    state.qp             = 26;
    state.bframes        = 3;
    state.want_zero_copy = 1;

    while ((c = getopt_long(argc, argv, "i:o:c:b:g:S:d:qh", options, NULL)) != -1) {
        switch (c) {
        case 'i': input = optarg; break;
        case 'o': output = optarg; break;
        case 'c': state.codec = optarg; break;
        case 'b': state.bitrate_kbps = atoi(optarg); break;
        case 1000: state.qp = atoi(optarg); break;
        case 'g': state.gop = atoi(optarg); break;
        case 'S':
            if (vaapi_parse_size(optarg, &scale_width, &scale_height) < 0 ||
                (scale_width & 1) || (scale_height & 1)) {
                LOG_ERROR("缩放尺寸格式错误: %s (应形如 1280x720，且宽高为偶数)", optarg);
                return 1;
            }
            break;
        case 1001: max_frames = atoi(optarg); break;
        case 1002: state.want_zero_copy = 0; break;
        case 'd': device = optarg; break;
        case 1003: connection_type = optarg; break;
        case 'q': quiet = 1; break;
        case 'h':
        default:
            transcode_usage();
            return c == 'h' ? 0 : 1;
        }
    }

    if (!input) {
        if (!util_find_sample(SAMPLE_MP4, sample_path, sizeof(sample_path))) {
            LOG_ERROR("未指定 -i，且找不到示例文件 samples/%s", SAMPLE_MP4);
            return 1;
        }
        input = sample_path;
        LOG_INFO("未指定输入文件，使用内置示例: %s", input);
    }
    if (!output) {
        snprintf(default_output, sizeof(default_output), "output/transcoded.%s",
                 (state.codec && (!strcasecmp(state.codec, "hevc") ||
                                  !strcasecmp(state.codec, "h265"))) ? "h265" : "h264");
        output = default_output;
    }

    memset(&config, 0, sizeof(config));
    config.device          = device;
    config.connection_type = connection_type;
    config.want_hw_frames  = state.want_zero_copy;
    config.out_format      = AV_PIX_FMT_NV12;
    config.max_frames      = max_frames;
    config.scale_width     = scale_width;
    config.scale_height    = scale_height;

    if (vaapi_decoder_open(&state.decoder, input, &config) < 0)
        return 1;

    state.width  = vaapi_decoder_width(state.decoder);
    state.height = vaapi_decoder_height(state.decoder);
    state.fps    = (int)(vaapi_decoder_fps(state.decoder) + 0.5);
    if (state.fps <= 0)
        state.fps = 25;

    ensure_parent_dir(output);
    state.output_file = fopen(output, "wb");
    if (!state.output_file) {
        LOG_ERROR("无法创建输出文件 %s: %s", output, strerror(errno));
        vaapi_decoder_close(state.decoder);
        return 1;
    }

    state.sink.file = state.output_file;
    progress_init(&state.progress, quiet);

    if (!quiet) {
        LOG_INFO("输入文件 : %s", input);
        LOG_INFO("输出文件 : %s", output);
        LOG_INFO("解码器   : %s (VAAPI) | %dx%d | %.2f fps", vaapi_decoder_codec_name(state.decoder),
                 vaapi_decoder_source_width(state.decoder), vaapi_decoder_source_height(state.decoder),
                 vaapi_decoder_fps(state.decoder));
        if (scale_width > 0)
            LOG_INFO("VPP 缩放 : %dx%d -> %dx%d (GPU VideoProc)",
                     vaapi_decoder_source_width(state.decoder),
                     vaapi_decoder_source_height(state.decoder),
                     state.width, state.height);
    }

    start_us = util_now_us();
    ret = vaapi_decoder_decode(state.decoder, transcode_frame_handler, &state);

    if (state.encoder) {
        int64_t bytes_before = state.sink.bytes;
        if (vaapi_encoder_flush(state.encoder, sink_write_packet, &state.sink) < 0)
            ret = AVERROR_EXTERNAL;
        state.progress.bytes += state.sink.bytes - bytes_before;
    }

    {
        double seconds = (util_now_us() - start_us) / 1000000.0;

        if (!quiet)
            progress_finish(&state.progress);
        if (ret == 0) {
            LOG_INFO("----------------------------------------------");
            LOG_INFO("转码完成: %d 帧, 输出 %s, %.1f fps, 平均码率 %s", state.frames,
                     util_format_size(state.sink.bytes),
                     seconds > 0 ? state.frames / seconds : 0.0,
                     util_format_bitrate(state.fps > 0
                                         ? state.sink.bytes * 8.0 * state.fps / state.frames : 0.0));
        }
    }

    if (state.encoder)
        vaapi_encoder_close(state.encoder);
    fclose(state.output_file);
    vaapi_decoder_close(state.decoder);

    if (ret < 0) {
        LOG_ERROR("转码失败");
        unlink(output);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* 自检                                                                       */
/* ------------------------------------------------------------------------- */

static double compare_yuv_files(const char *reference_path, const char *test_path,
                                int width, int height, int max_frames,
                                int *compared_frames)
{
    FILE *reference = NULL;
    FILE *test = NULL;
    AVFrame *a = NULL;
    AVFrame *b = NULL;
    PsnrAccum accum = { 0.0, 0 };
    int frames = 0;
    int chroma_w = (width + 1) / 2;
    int chroma_h = (height + 1) / 2;

    reference = fopen(reference_path, "rb");
    test = fopen(test_path, "rb");
    if (!reference || !test)
        goto end;

    a = av_frame_alloc();
    b = av_frame_alloc();
    if (!a || !b)
        goto end;
    a->format = b->format = AV_PIX_FMT_YUV420P;
    a->width  = b->width  = width;
    a->height = b->height = height;
    if (av_frame_get_buffer(a, 32) < 0 || av_frame_get_buffer(b, 32) < 0)
        goto end;

    while (max_frames <= 0 || frames < max_frames) {
        if (!util_read_yuv_frame(reference, a) || !util_read_yuv_frame(test, b))
            break;

        psnr_accumulate(&accum, a->data[0], a->linesize[0], b->data[0], b->linesize[0],
                        width, height);
        psnr_accumulate(&accum, a->data[1], a->linesize[1], b->data[1], b->linesize[1],
                        chroma_w, chroma_h);
        psnr_accumulate(&accum, a->data[2], a->linesize[2], b->data[2], b->linesize[2],
                        chroma_w, chroma_h);
        frames++;
    }

end:
    if (compared_frames)
        *compared_frames = frames;
    if (a)
        av_frame_free(&a);
    if (b)
        av_frame_free(&b);
    if (reference)
        fclose(reference);
    if (test)
        fclose(test);

    return frames > 0 ? psnr_value(&accum) : 0.0;
}

static int command_selftest(int argc, char **argv)
{
    EncodeParams encode;
    DecodeParams decode;
    RunStats encode_stats, decode_stats;
    char yuv_path[4096];
    char mp4_path[4096];
    const char *device = NULL;
    const char *connection_type = "drm";
    int quiet = 0;
    int compared = 0;
    double psnr;
    int c;
    static const struct option options[] = {
        { "device",          required_argument, NULL, 'd' },
        { "connection-type", required_argument, NULL, 1000 },
        { "quiet",           no_argument,       NULL, 'q' },
        { "help",            no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    while ((c = getopt_long(argc, argv, "d:qh", options, NULL)) != -1) {
        switch (c) {
        case 'd': device = optarg; break;
        case 1000: connection_type = optarg; break;
        case 'q': quiet = 1; break;
        case 'h':
        default:
            fprintf(stdout,
                "用法: vaapi_demo selftest [选项]\n"
                "\n"
                "  -d, --device <节点>       DRM 设备\n"
                "      --connection-type <t> drm(默认) / x11\n"
                "  -q, --quiet               精简输出\n"
                "  -h, --help                显示帮助\n");
            return c == 'h' ? 0 : 1;
        }
    }

    if (!util_find_sample(SAMPLE_YUV, yuv_path, sizeof(yuv_path))) {
        LOG_ERROR("找不到示例文件 samples/%s", SAMPLE_YUV);
        return 1;
    }

    LOG_INFO("================ VAAPI 全流程自检 ================");

    LOG_INFO("[1/6] 检查 VAAPI 设备 ...");
    {
        AVBufferRef *device_ctx = vaapi_device_create(device, connection_type);
        if (!device_ctx) {
            LOG_ERROR("[1/6] 失败: VAAPI 设备不可用");
            return 1;
        }
        av_buffer_unref(&device_ctx);
        LOG_INFO("[1/6] 通过");
    }

    memset(&encode, 0, sizeof(encode));
    encode.input           = yuv_path;
    encode.output          = "output/selftest.h264";
    encode.codec           = "h264";
    encode.width           = 1920;
    encode.height          = 1080;
    encode.fps             = 25;
    encode.qp              = 26;
    encode.bframes         = 3;
    encode.device          = device;
    encode.connection_type = connection_type;
    encode.pix_fmt         = AV_PIX_FMT_YUV420P;
    encode.quiet           = quiet;

    LOG_INFO("[2/6] 硬件编码 %s -> %s ...", encode.input, encode.output);
    if (run_encode(&encode, &encode_stats) < 0) {
        LOG_ERROR("[2/6] 失败: 硬件编码失败");
        return 1;
    }
    LOG_INFO("[2/6] 通过 (%d 帧, %.1f fps)", encode_stats.frames,
             encode_stats.seconds > 0 ? encode_stats.frames / encode_stats.seconds : 0.0);

    memset(&decode, 0, sizeof(decode));
    decode.input           = encode.output;
    decode.output          = "output/selftest_decoded.yuv";
    decode.device          = device;
    decode.connection_type = connection_type;
    decode.out_format      = AV_PIX_FMT_YUV420P;
    decode.quiet           = quiet;

    LOG_INFO("[3/6] 硬件解码 %s -> %s ...", decode.input, decode.output);
    if (run_decode(&decode, &decode_stats) < 0) {
        LOG_ERROR("[3/6] 失败: 硬件解码失败");
        return 1;
    }
    LOG_INFO("[3/6] 通过 (%d 帧, %.1f fps)", decode_stats.frames,
             decode_stats.seconds > 0 ? decode_stats.frames / decode_stats.seconds : 0.0);

    LOG_INFO("[4/6] 画质校验（编码前 vs 解码后 PSNR）...");
    psnr = compare_yuv_files(encode.input, decode.output, encode.width, encode.height,
                             encode_stats.frames, &compared);
    if (compared <= 0) {
        LOG_ERROR("[4/6] 失败: 无法比较 YUV 数据");
        return 1;
    }
    LOG_INFO("[4/6] 对比 %d 帧, PSNR = %.2f dB %s", compared, psnr,
             psnr >= 30.0 ? "(通过)" : "(偏低，请检查显卡状态)");
    if (psnr < 30.0) {
        LOG_ERROR("[4/6] 失败: PSNR 过低");
        return 1;
    }

    LOG_INFO("[5/6] 解码示例视频 %s ...", SAMPLE_MP4);
    if (!util_find_sample(SAMPLE_MP4, mp4_path, sizeof(mp4_path))) {
        LOG_WARN("[5/6] 跳过: 找不到 samples/%s", SAMPLE_MP4);
    } else {
        memset(&decode, 0, sizeof(decode));
        decode.input           = mp4_path;
        decode.output          = "output/selftest_mp4.yuv";
        decode.device          = device;
        decode.connection_type = connection_type;
        decode.out_format      = AV_PIX_FMT_YUV420P;
        decode.max_frames      = 30;
        decode.quiet           = quiet;
        if (run_decode(&decode, &decode_stats) < 0) {
            LOG_ERROR("[5/6] 失败: 示例视频解码失败");
            return 1;
        }
        LOG_INFO("[5/6] 通过 (%d 帧, %.1f fps)", decode_stats.frames,
                 decode_stats.seconds > 0 ? decode_stats.frames / decode_stats.seconds : 0.0);
    }

    LOG_INFO("[6/6] VPP 缩放：编码 1920x1080 -> 1280x720，再解码 1280x720 -> 640x360 ...");
    {
        int64_t expected;
        int64_t actual;

        encode.output      = "output/selftest_scaled.h264";
        encode.scale_width = 1280;
        encode.scale_height = 720;
        if (run_encode(&encode, &encode_stats) < 0) {
            LOG_ERROR("[6/6] 失败: 编码前 VPP 缩放失败");
            return 1;
        }

        memset(&decode, 0, sizeof(decode));
        decode.input           = encode.output;
        decode.output          = "output/selftest_scaled.yuv";
        decode.device          = device;
        decode.connection_type = connection_type;
        decode.out_format      = AV_PIX_FMT_YUV420P;
        decode.scale_width     = 640;
        decode.scale_height    = 360;
        decode.quiet           = quiet;
        if (run_decode(&decode, &decode_stats) < 0) {
            LOG_ERROR("[6/6] 失败: 解码后 VPP 缩放失败");
            return 1;
        }

        expected = (int64_t)decode.scale_width * decode.scale_height * 3 / 2 * decode_stats.frames;
        actual   = util_file_size(decode.output);
        if (decode_stats.frames <= 0 || actual != expected) {
            LOG_ERROR("[6/6] 失败: 缩放输出尺寸不符 (期望 %lld 字节, 实际 %lld 字节)",
                      (long long)expected, (long long)actual);
            return 1;
        }
        LOG_INFO("[6/6] 通过 (%d 帧, 输出 %dx%d)", decode_stats.frames,
                 decode.scale_width, decode.scale_height);
    }

    LOG_INFO("==================================================");
    LOG_INFO("全部自检通过：VAAPI 硬件编解码工作正常");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* 入口                                                                       */
/* ------------------------------------------------------------------------- */

static void print_usage(const char *program)
{
    fprintf(stdout,
        "vaapi_demo %s - VAAPI 硬件编解码演示程序\n"
        "\n"
        "用法: %s <命令> [选项]\n"
        "\n"
        "命令:\n"
        "  info        查看 VAAPI 环境、驱动能力、FFmpeg 硬件编解码器支持\n"
        "  encode      硬件编码: YUV 原始数据 -> H.264 / H.265 码流\n"
        "  decode      硬件解码: MP4/H.264/H.265 -> YUV 原始数据\n"
        "  play        硬件解码并实时显示（需要 glfw3）\n"
        "  transcode   解码 + 编码转码，默认零拷贝（帧数据不出显存）\n"
        "  selftest    一键全流程自检（编码 -> 解码 -> PSNR 校验）\n"
        "\n"
        "常用示例:\n"
        "  %s info\n"
        "  %s encode                          # 用内置示例 YUV 编码 H.264\n"
        "  %s encode -c hevc -s 1920x1080     # 编码 H.265\n"
        "  %s decode                          # 解码内置示例 MP4\n"
        "  %s decode --snapshot first.ppm     # 解码并保存首帧图片\n"
        "  %s play                            # 播放示例视频\n"
        "  %s transcode -c hevc               # 转码为 H.265\n"
        "  %s selftest                        # 全流程自检\n"
        "\n"
        "各命令的详细选项: %s <命令> --help\n",
        VAAPI_DEMO_VERSION, program, program, program, program, program, program,
        program, program, program, program);
}

int main(int argc, char **argv)
{
    const char *command;

    av_log_set_level(AV_LOG_WARNING);

    if (argc < 2) {
        print_usage(argv[0]);
        return 0;
    }

    command = argv[1];

    if (!strcmp(command, "encode"))
        return command_encode(argc - 1, argv + 1);
    if (!strcmp(command, "decode"))
        return command_decode(argc - 1, argv + 1);
    if (!strcmp(command, "play"))
        return command_play(argc - 1, argv + 1);
    if (!strcmp(command, "transcode"))
        return command_transcode(argc - 1, argv + 1);
    if (!strcmp(command, "selftest"))
        return command_selftest(argc - 1, argv + 1);
    if (!strcmp(command, "info")) {
        int ret = vaapi_print_environment(NULL);
        return ret == 0 ? 0 : 1;
    }
    if (!strcmp(command, "help") || !strcmp(command, "-h") || !strcmp(command, "--help")) {
        print_usage(argv[0]);
        return 0;
    }
    if (!strcmp(command, "version") || !strcmp(command, "-v") || !strcmp(command, "--version")) {
        print_version();
        return 0;
    }

    LOG_ERROR("未知命令: %s", command);
    print_usage(argv[0]);
    return 1;
}
