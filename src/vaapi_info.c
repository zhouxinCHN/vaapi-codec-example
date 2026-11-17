#include "vaapi_info.h"

#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_vaapi.h>
#include <va/va.h>

#ifndef VAAPI_DEMO_VERSION
#define VAAPI_DEMO_VERSION "1.0.0"
#endif

static const char *profile_name(VAProfile profile)
{
    switch (profile) {
    case VAProfileMPEG2Simple:            return "MPEG-2 Simple";
    case VAProfileMPEG2Main:              return "MPEG-2 Main";
    case VAProfileH264ConstrainedBaseline:return "H.264 Constrained Baseline";
    case VAProfileH264Main:               return "H.264 Main";
    case VAProfileH264High:               return "H.264 High";
    case VAProfileHEVCMain:               return "H.265 Main";
    case VAProfileHEVCMain10:             return "H.265 Main10";
    case VAProfileVP8Version0_3:          return "VP8";
    case VAProfileVP9Profile0:            return "VP9 Profile0";
    case VAProfileVP9Profile2:            return "VP9 Profile2";
    case VAProfileAV1Profile0:            return "AV1 Profile0";
    case VAProfileJPEGBaseline:           return "JPEG Baseline";
    case VAProfileNone:                   return "VAProfileNone";
    case VAProfileVC1Simple:              return "VC-1 Simple";
    case VAProfileVC1Main:                return "VC-1 Main";
    case VAProfileVC1Advanced:            return "VC-1 Advanced";
    default:                              return NULL;
    }
}

static const char *entrypoint_name(VAEntrypoint entrypoint)
{
    switch (entrypoint) {
    case VAEntrypointVLD:        return "解码(VLD)";
    case VAEntrypointEncSlice:   return "编码(EncSlice)";
    case VAEntrypointEncSliceLP: return "编码(EncSliceLP 低功耗)";
    case VAEntrypointEncPicture: return "编码(EncPicture)";
    case VAEntrypointVideoProc:  return "图像处理(VideoProc)";
    default:                     return NULL;
    }
}

static void print_driver_capabilities(VADisplay display)
{
    VAProfile profiles[64];
    int profile_count = 0;
    const char *vendor = vaQueryVendorString(display); /* display 已由 FFmpeg 初始化 */

    LOG_INFO("驱动信息        : %s", vendor ? vendor : "(未知)");
    LOG_INFO("libva 版本      : %d.%d", VA_MAJOR_VERSION, VA_MINOR_VERSION);

    if (vaQueryConfigProfiles(display, profiles, &profile_count) != VA_STATUS_SUCCESS) {
        LOG_WARN("查询 VAAPI profile 失败");
        return;
    }

    LOG_INFO("显卡支持的 profile / entrypoint:");
    for (int i = 0; i < profile_count; i++) {
        VAEntrypoint entrypoints[16];
        int entrypoint_count = 0;
        char line[256];
        size_t used = 0;
        const char *name = profile_name(profiles[i]);

        if (vaQueryConfigEntrypoints(display, profiles[i], entrypoints, &entrypoint_count)
            != VA_STATUS_SUCCESS)
            continue;

        line[0] = '\0';
        for (int j = 0; j < entrypoint_count; j++) {
            const char *ep = entrypoint_name(entrypoints[j]);
            if (!ep)
                continue;
            if (used && used < sizeof(line) - 1)
                used += (size_t)snprintf(line + used, sizeof(line) - used, ", ");
            if (used < sizeof(line) - 1)
                used += (size_t)snprintf(line + used, sizeof(line) - used, "%s", ep);
        }
        if (!line[0])
            continue;

        if (name)
            LOG_INFO("  %-28s : %s", name, line);
        else
            LOG_INFO("  VAProfile #%-3d%18s : %s", profiles[i], "", line);
    }
}

static void print_ffmpeg_codecs(void)
{
    static const char *encoders[] = {
        "h264_vaapi", "hevc_vaapi", "vp9_vaapi", "av1_vaapi", "mjpeg_vaapi", NULL
    };
    static const struct {
        enum AVCodecID id;
        const char *name;
    } decoders[] = {
        { AV_CODEC_ID_H264, "H.264" },
        { AV_CODEC_ID_HEVC, "H.265" },
        { AV_CODEC_ID_VP9,  "VP9"   },
        { AV_CODEC_ID_AV1,  "AV1"   },
        { AV_CODEC_ID_MPEG2VIDEO, "MPEG-2" },
        { AV_CODEC_ID_VC1,  "VC-1"  },
        { AV_CODEC_ID_NONE, NULL    },
    };
    char line[256];
    size_t used;

    used = 0;
    line[0] = '\0';
    for (int i = 0; encoders[i]; i++) {
        if (!avcodec_find_encoder_by_name(encoders[i]))
            continue;
        used += (size_t)snprintf(line + used, sizeof(line) - used, "%s%s",
                                 used ? " " : "", encoders[i]);
    }
    LOG_INFO("FFmpeg 可用 VAAPI 编码器: %s", line[0] ? line : "(无)");

    used = 0;
    line[0] = '\0';
    for (int i = 0; decoders[i].id != AV_CODEC_ID_NONE; i++) {
        const AVCodec *codec = avcodec_find_decoder(decoders[i].id);
        int vaapi = 0;

        if (!codec)
            continue;
        for (int j = 0;; j++) {
            const AVCodecHWConfig *config = avcodec_get_hw_config(codec, j);
            if (!config)
                break;
            if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
                config->device_type == AV_HWDEVICE_TYPE_VAAPI) {
                vaapi = 1;
                break;
            }
        }
        if (vaapi)
            used += (size_t)snprintf(line + used, sizeof(line) - used, "%s%s",
                                     used ? " " : "", decoders[i].name);
    }
    LOG_INFO("FFmpeg 可用 VAAPI 解码器: %s", line[0] ? line : "(无)");
}

int vaapi_print_environment(const char *device)
{
    AVBufferRef *device_ctx;
    AVHWDeviceContext *hw_device;
    AVVAAPIDeviceContext *vaapi;
    int supported = 0;

    LOG_INFO("================ VAAPI 环境自检 ================");
    LOG_INFO("程序版本        : %s", VAAPI_DEMO_VERSION);
    LOG_INFO("FFmpeg 版本     : %u.%u.%u (%s)", LIBAVCODEC_VERSION_MAJOR,
             LIBAVCODEC_VERSION_MINOR, LIBAVCODEC_VERSION_MICRO, av_version_info());

    device_ctx = vaapi_device_create(device, "drm");
    if (!device_ctx) {
        LOG_INFO("VAAPI 设备      : 不可用");
        LOG_INFO("===============================================");
        return -1;
    }

    hw_device = (AVHWDeviceContext *)device_ctx->data;
    vaapi = (AVVAAPIDeviceContext *)hw_device->hwctx;
    LOG_INFO("VAAPI 设备      : 创建成功 (%s)", device ? device : "自动选择");

    if (vaapi && vaapi->display)
        print_driver_capabilities(vaapi->display);
    else
        LOG_WARN("无法获取 VADisplay，跳过驱动能力查询");

    print_ffmpeg_codecs();
    LOG_INFO("===============================================");

    supported = 1;
    av_buffer_unref(&device_ctx);
    return supported ? 0 : -1;
}
