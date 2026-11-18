#include "gl_vaapi.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <glad/glad.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
#include <libavutil/hwcontext_vaapi.h>

#include "egl_headless.h"
#include "egl_interop.h"
#include "vaapi_dmabuf.h"
#include "vaapi_encoder.h"

#ifndef EGL_GL_TEXTURE_2D_KHR
#define EGL_GL_TEXTURE_2D_KHR 0x30B1
#endif
#ifndef EGL_GL_TEXTURE_LEVEL_KHR
#define EGL_GL_TEXTURE_LEVEL_KHR 0x30BC
#endif

struct GlVaapiEncoder {
    EglHeadless *egl;
    EglInterop *input_import;
    EglInterop *output_import;
    VaapiEncoder *encoder;
    AVBufferRef *device_ctx;
    int owns_device;

    int width;
    int height;
    int fps;
    int frame_index;
    char renderer[128];

    GLuint fbo;
    GLuint vao;
    GLuint vbo;
    GLuint program;
    GLint u_tex_y;
    GLint u_tex_uv;
    GLint u_has_input;
    GLint u_box_center;
    GLint u_box_size;
    GLint u_time;
    GLint u_pass;
};

static const char *kVertexShader =
    "#version 330 core\n"
    "layout (location = 0) in vec3 pos;\n"
    "layout (location = 1) in vec2 texCoord;\n"
    "out vec2 TexCoord;\n"
    "void main()\n"
    "{\n"
    "    gl_Position = vec4(pos, 1.0);\n"
    "    TexCoord = texCoord;\n"
    "}\n";

/* 直接把 Y/UV 写进两个 RT：不经过 RGB，编码器拿到的就是 NV12 平面 */
static const char *kFragmentShader =
    "#version 330 core\n"
    "in vec2 TexCoord;\n"
    "uniform sampler2D texY;\n"
    "uniform sampler2D texUV;\n"
    "uniform int hasInput;\n"
    "uniform vec2 boxCenter;\n"
    "uniform float boxSize;\n"
    "uniform float time;\n"
    "uniform int pass;\n"
    "layout(location = 0) out vec4 outColor;\n"
    "void main()\n"
    "{\n"
    "    float y;\n"
    "    vec2 uv;\n"
    "    if (hasInput == 1) {\n"
    "        y = texture(texY, TexCoord).r;\n"
    "        uv = texture(texUV, TexCoord).rg;\n"
    "    } else {\n"
    "        y = 0.06 + 0.45 * (0.5 + 0.5 * sin(TexCoord.x * 18.0 + time));\n"
    "        uv = vec2(0.5, 0.5);\n"
    "    }\n"
    "    if (abs(TexCoord.x - boxCenter.x) < boxSize &&\n"
    "        abs(TexCoord.y - boxCenter.y) < boxSize) {\n"
    "        y = 0.92;\n"
    "        uv = vec2(0.5, 0.5);\n"
    "    }\n"
    "    outColor = pass == 0 ? vec4(y, 0.0, 0.0, 1.0)\n"
    "                         : vec4(uv, 0.0, 1.0);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    GLint success = 0;
    char info[512];

    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(shader, sizeof(info), NULL, info);
        LOG_ERROR("GL 着色器编译失败: %s", info);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static int init_gl(GlVaapiEncoder *e)
{
    /* 写入的是 VAAPI 表面（内存行序自上而下），V 方向与屏幕渲染相反 */
    static const float vertices[] = {
        1.0f,  1.0f, 0.0f,  1.0f, 1.0f,
        1.0f, -1.0f, 0.0f,  1.0f, 0.0f,
        -1.0f, -1.0f, 0.0f,  0.0f, 0.0f,
        -1.0f,  1.0f, 0.0f,  0.0f, 1.0f
    };
    GLuint vs, fs;
    GLint success = 0;
    char info[512];

    vs = compile_shader(GL_VERTEX_SHADER, kVertexShader);
    fs = compile_shader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vs || !fs)
        return -1;

    e->program = glCreateProgram();
    glAttachShader(e->program, vs);
    glAttachShader(e->program, fs);
    glBindAttribLocation(e->program, 0, "pos");
    glBindAttribLocation(e->program, 1, "texCoord");
    glLinkProgram(e->program);
    glGetProgramiv(e->program, GL_LINK_STATUS, &success);
    if (!success) {
        glGetProgramInfoLog(e->program, sizeof(info), NULL, info);
        LOG_ERROR("GL 着色器链接失败: %s", info);
        return -1;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    glUseProgram(e->program);

    e->u_tex_y      = glGetUniformLocation(e->program, "texY");
    e->u_tex_uv     = glGetUniformLocation(e->program, "texUV");
    e->u_has_input  = glGetUniformLocation(e->program, "hasInput");
    e->u_box_center = glGetUniformLocation(e->program, "boxCenter");
    e->u_box_size   = glGetUniformLocation(e->program, "boxSize");
    e->u_time       = glGetUniformLocation(e->program, "time");
    e->u_pass       = glGetUniformLocation(e->program, "pass");

    glGenVertexArrays(1, &e->vao);
    glBindVertexArray(e->vao);
    glGenBuffers(1, &e->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, e->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    /* FBO 的附件每帧绑定到编码器表面的两个平面纹理 */
    glGenFramebuffers(1, &e->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, e->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    snprintf(e->renderer, sizeof(e->renderer), "%s",
             glGetString(GL_RENDERER) ? (const char *)glGetString(GL_RENDERER) : "unknown");
    return 0;
}

int gl_vaapi_open(GlVaapiEncoder **out, const GlVaapiConfig *config)
{
    GlVaapiEncoder *e;
    VaapiEncoderConfig enc_config;

    if (!out || !config || config->width <= 0 || config->height <= 0)
        return AVERROR(EINVAL);
    *out = NULL;

    e = calloc(1, sizeof(*e));
    if (!e)
        return AVERROR(ENOMEM);
    e->width  = config->width & ~1;
    e->height = config->height & ~1;
    e->fps    = config->fps > 0 ? config->fps : 25;

    /* 1. EGL surfaceless + GL */
    e->egl = egl_headless_create();
    if (!e->egl) {
        LOG_ERROR("GL 编码：EGL surfaceless 初始化失败");
        goto fail;
    }
    if (init_gl(e) < 0)
        goto fail;
    LOG_INFO("GL 渲染器: %s", e->renderer);

    /* 2. 编码器（设备由编码器内部创建，避免与外部设备上下文不一致） */
    memset(&enc_config, 0, sizeof(enc_config));
    enc_config.codec        = config->codec ? config->codec : "h264";
    enc_config.width        = e->width;
    enc_config.height       = e->height;
    enc_config.fps          = e->fps;
    enc_config.qp           = config->qp;
    enc_config.bitrate_kbps = config->bitrate_kbps;
    enc_config.gop          = config->gop;
    enc_config.bframes      = 0;              /* 合成后直接编码，保持低延迟 */
    enc_config.sw_format    = AV_PIX_FMT_NV12;
    enc_config.device          = config->device;
    enc_config.connection_type = config->connection_type;
    if (vaapi_encoder_open(&e->encoder, &enc_config) < 0)
        goto fail;

    *out = e;
    return 0;

fail:
    gl_vaapi_close(e);
    return AVERROR_EXTERNAL;
}

void gl_vaapi_close(GlVaapiEncoder *e)
{
    if (!e)
        return;
    if (e->input_import)
        egl_interop_destroy(e->input_import);
    if (e->output_import)
        egl_interop_destroy(e->output_import);
    if (e->encoder)
        vaapi_encoder_close(e->encoder);
    if (e->egl) {
        if (e->program)
            glDeleteProgram(e->program);
        if (e->vbo)
            glDeleteBuffers(1, &e->vbo);
        if (e->vao)
            glDeleteVertexArrays(1, &e->vao);
        if (e->fbo)
            glDeleteFramebuffers(1, &e->fbo);
        egl_headless_destroy(e->egl);
    }
    av_buffer_unref(&e->device_ctx);
    free(e);
}

const char *gl_vaapi_renderer_name(GlVaapiEncoder *e)
{
    return e ? e->renderer : "";
}

/* ------------------------------------------------------------------------- */
/* 渲染 + 编码                                                                */
/* ------------------------------------------------------------------------- */

int gl_vaapi_encode_frame(GlVaapiEncoder *e, AVFrame *hw_frame, int64_t pts,
                          VaapiPacketWriter writer, void *opaque)
{
    VaapiDmabuf surface_dmabuf;
    AVFrame *out = NULL;
    GLuint in_tex_y = 0, in_tex_uv = 0;
    GLuint out_tex_y = 0, out_tex_uv = 0;
    GLenum one_buffer = GL_COLOR_ATTACHMENT0;
    int has_input = 0;
    int ret = -1;

    if (!e)
        return AVERROR(EINVAL);

    /* 1. 可选输入：解码硬件帧 -> dmabuf -> GL 纹理 */
    if (hw_frame) {
        VaapiDmabuf input_dmabuf;

        if (!e->input_import)
            e->input_import = egl_interop_create();
        if (!e->input_import) {
            LOG_ERROR("GL 编码：dmabuf 导入器不可用");
            return AVERROR_EXTERNAL;
        }
        if (vaapi_dmabuf_export(hw_frame, &input_dmabuf) < 0)
            return AVERROR_EXTERNAL;
        if (egl_interop_upload(e->input_import, &input_dmabuf.desc,
                               input_dmabuf.width, input_dmabuf.height) < 0) {
            vaapi_dmabuf_release(&input_dmabuf);
            return AVERROR_EXTERNAL;
        }
        egl_interop_textures(e->input_import, &in_tex_y, &in_tex_uv);
        vaapi_dmabuf_release(&input_dmabuf);
        has_input = 1;
    }

    /* 2. 从编码器帧池取一块 NV12 表面，导出 dmabuf 后导入成 GL 纹理，
     *    直接作为渲染目标：渲染结果天然就是编码器可用的表面，无需再导出。 */
    out = av_frame_alloc();
    if (!out) {
        LOG_ERROR("分配输出帧失败");
        return AVERROR(ENOMEM);
    }
    if (av_hwframe_get_buffer(vaapi_encoder_frames_ctx(e->encoder), out, 0) < 0) {
        LOG_ERROR("从编码器帧池申请表面失败");
        goto done;
    }
    if (vaapi_dmabuf_export_ex(out, &surface_dmabuf, 1) < 0)
        goto done;
    if (!e->output_import)
        e->output_import = egl_interop_create();
    if (!e->output_import) {
        LOG_ERROR("GL 编码：输出表面导入器不可用");
        vaapi_dmabuf_release(&surface_dmabuf);
        goto done;
    }
    if (egl_interop_upload(e->output_import, &surface_dmabuf.desc,
                           surface_dmabuf.width, surface_dmabuf.height) < 0) {
        vaapi_dmabuf_release(&surface_dmabuf);
        goto done;
    }
    egl_interop_textures(e->output_import, &out_tex_y, &out_tex_uv);
    vaapi_dmabuf_release(&surface_dmabuf);

    /* 3. 两次绘制：Y 全尺寸、UV 半尺寸。
     * 不能把不同尺寸的附件挂在同一个 FBO 上做 MRT，否则渲染区域会被裁剪。 */
    glBindFramebuffer(GL_FRAMEBUFFER, e->fbo);
    glDrawBuffers(1, &one_buffer);
    glUseProgram(e->program);
    glBindVertexArray(e->vao);
    if (has_input) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, in_tex_y);
        glUniform1i(e->u_tex_y, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, in_tex_uv);
        glUniform1i(e->u_tex_uv, 1);
    }
    glUniform1i(e->u_has_input, has_input);
    glUniform1f(e->u_time, (float)e->frame_index / (float)e->fps);
    glUniform2f(e->u_box_center,
                0.5f + 0.3f * sinf(e->frame_index * 0.15f),
                0.5f + 0.3f * cosf(e->frame_index * 0.15f));
    glUniform1f(e->u_box_size, 0.08f);

    /* pass 0: Y 平面 */
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, out_tex_y, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("GL 编码：Y 平面 FBO 不完整");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        goto done;
    }
    glViewport(0, 0, e->width, e->height);
    glUniform1i(e->u_pass, 0);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

    /* pass 1: UV 平面 */
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, out_tex_uv, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("GL 编码：UV 平面 FBO 不完整");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        goto done;
    }
    glViewport(0, 0, e->width / 2, e->height / 2);
    glUniform1i(e->u_pass, 1);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

    glBindVertexArray(0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glFinish();

    /* 4. 同一个表面直接送编码器（零拷贝） */
    out->pts = pts;
    ret = vaapi_encoder_send(e->encoder, out, writer, opaque);
    if (ret >= 0)
        e->frame_index++;
    else
        LOG_ERROR("GL 编码：送入编码器失败");

done:
    /* 表面由帧池引用计数管理，编码器仍持有时这里不会真正释放 */
    av_frame_free(&out);
    return ret;
}

int gl_vaapi_flush(GlVaapiEncoder *e, VaapiPacketWriter writer, void *opaque)
{
    if (!e)
        return AVERROR(EINVAL);
    return vaapi_encoder_flush(e->encoder, writer, opaque);
}
