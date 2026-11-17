#include "renderer.h"

#include <stddef.h>
#include <stdlib.h>

#include "vaapi_common.h"

#ifdef HAVE_GLFW

#include <glad/glad.h>
#include <GLFW/glfw3.h>

struct Renderer {
    GLFWwindow *window;
    int width;
    int height;
    GLuint vao;
    GLuint vbo;
    GLuint program;
    GLuint texture_y;
    GLuint texture_uv;
    GLint uniform_y;
    GLint uniform_uv;
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

static const char *kFragmentShader =
    "#version 330 core\n"
    "layout(location = 0) out vec4 FragColor;\n"
    "in vec2 TexCoord;\n"
    "uniform sampler2D dataY;\n"
    "uniform sampler2D dataUV;\n"
    "const mat4 YUV_TO_RGB_MATRIX =\n"
    "    mat4(\n"
    "        1.1643835616, 0, 1.7927410714, -0.9729450750,\n"
    "        1.1643835616, -0.2132486143, -0.5329093286, 0.3014826655,\n"
    "        1.1643835616, 2.1124017857, 0, -1.1334022179,\n"
    "        0, 0, 0, 1);\n"
    "void main()\n"
    "{\n"
    "    vec3 yuv;\n"
    "    yuv.x = texture(dataY, TexCoord).r;\n"
    "    yuv.y = texture(dataUV, TexCoord).r;\n"
    "    yuv.z = texture(dataUV, TexCoord).g;\n"
    "    FragColor = vec4(yuv, 1) * YUV_TO_RGB_MATRIX;\n"
    "}\n";

int renderer_available(void)
{
    return 1;
}

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
        LOG_ERROR("着色器编译失败: %s", info);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static int renderer_init_gl(Renderer *r)
{
    static const float vertices[] = {
        1.0f,  1.0f, 0.0f,  1.0f, 0.0f,
        1.0f, -1.0f, 0.0f,  1.0f, 1.0f,
        -1.0f, -1.0f, 0.0f,  0.0f, 1.0f,
        -1.0f,  1.0f, 0.0f,  0.0f, 0.0f
    };
    GLuint vertex_shader, fragment_shader;
    GLint success = 0;
    char info[512];

    vertex_shader   = compile_shader(GL_VERTEX_SHADER, kVertexShader);
    fragment_shader = compile_shader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vertex_shader || !fragment_shader)
        return -1;

    r->program = glCreateProgram();
    glAttachShader(r->program, vertex_shader);
    glAttachShader(r->program, fragment_shader);
    glBindAttribLocation(r->program, 0, "pos");
    glBindAttribLocation(r->program, 1, "texCoord");
    glLinkProgram(r->program);
    glGetProgramiv(r->program, GL_LINK_STATUS, &success);
    if (!success) {
        glGetProgramInfoLog(r->program, sizeof(info), NULL, info);
        LOG_ERROR("着色器链接失败: %s", info);
        return -1;
    }
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);
    glUseProgram(r->program);

    glGenVertexArrays(1, &r->vao);
    glBindVertexArray(r->vao);
    glGenBuffers(1, &r->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    r->uniform_y  = glGetUniformLocation(r->program, "dataY");
    r->uniform_uv = glGetUniformLocation(r->program, "dataUV");

    glGenTextures(1, &r->texture_y);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, r->texture_y);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glUniform1i(r->uniform_y, 0);

    glGenTextures(1, &r->texture_uv);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, r->texture_uv);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glUniform1i(r->uniform_uv, 1);

    glUseProgram(0);
    return 0;
}

static void on_key(GLFWwindow *window, int key, int scancode, int action, int mods)
{
    (void)scancode;
    (void)mods;
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
        glfwSetWindowShouldClose(window, GLFW_TRUE);
}

Renderer *renderer_create(int width, int height, const char *title)
{
    Renderer *r;
    int window_width  = width  > 1280 ? width / 2  : width;
    int window_height = height > 720 ? height / 2 : height;

    r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->width  = width;
    r->height = height;

    if (!glfwInit()) {
        LOG_ERROR("初始化 GLFW 失败（当前环境可能没有图形界面）");
        free(r);
        return NULL;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    r->window = glfwCreateWindow(window_width, window_height,
                                 title ? title : "VAAPI Player", NULL, NULL);
    if (!r->window) {
        LOG_ERROR("创建窗口失败（请检查 DISPLAY 是否可用）");
        glfwTerminate();
        free(r);
        return NULL;
    }
    glfwMakeContextCurrent(r->window);
    glfwSetKeyCallback(r->window, on_key);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        LOG_ERROR("加载 OpenGL 函数失败");
        glfwDestroyWindow(r->window);
        glfwTerminate();
        free(r);
        return NULL;
    }
    glViewport(0, 0, window_width, window_height);
    if (renderer_init_gl(r) < 0) {
        renderer_destroy(r);
        return NULL;
    }
    return r;
}

void renderer_destroy(Renderer *r)
{
    if (!r)
        return;
    if (r->window) {
        glDeleteTextures(1, &r->texture_y);
        glDeleteTextures(1, &r->texture_uv);
        glDeleteVertexArrays(1, &r->vao);
        glDeleteBuffers(1, &r->vbo);
        glDeleteProgram(r->program);
        glfwDestroyWindow(r->window);
        glfwTerminate();
    }
    free(r);
}

int renderer_draw_nv12(Renderer *r, const uint8_t *data,
                       int linesize_y, int linesize_uv)
{
    if (!r || !data)
        return -1;

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindVertexArray(r->vao);
    glUseProgram(r->program);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, r->texture_y);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, linesize_y);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, r->width, r->height, 0,
                 GL_RED, GL_UNSIGNED_BYTE, data);
    glUniform1i(r->uniform_y, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, r->texture_uv);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, linesize_uv / 2);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG, r->width / 2, r->height / 2, 0,
                 GL_RG, GL_UNSIGNED_BYTE, data + (size_t)linesize_y * r->height);
    glUniform1i(r->uniform_uv, 1);

    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glUseProgram(0);
    glBindVertexArray(0);
    return 0;
}

int renderer_should_close(Renderer *r)
{
    return r && r->window ? glfwWindowShouldClose(r->window) : 1;
}

void renderer_swap_buffers(Renderer *r)
{
    if (r && r->window)
        glfwSwapBuffers(r->window);
}

void renderer_poll_events(Renderer *r)
{
    (void)r;
    glfwPollEvents();
}

double renderer_time(void)
{
    return glfwGetTime();
}

#else /* !HAVE_GLFW */

struct Renderer {
    int unused;
};

int renderer_available(void)
{
    return 0;
}

Renderer *renderer_create(int width, int height, const char *title)
{
    (void)width;
    (void)height;
    (void)title;
    LOG_WARN("本版本编译时未找到 glfw3，无法显示视频窗口");
    LOG_WARN("如需播放功能请先安装: sudo apt install libglfw3-dev 然后重新 make");
    return NULL;
}

void renderer_destroy(Renderer *renderer)
{
    (void)renderer;
}

int renderer_draw_nv12(Renderer *renderer, const uint8_t *data,
                       int linesize_y, int linesize_uv)
{
    (void)renderer;
    (void)data;
    (void)linesize_y;
    (void)linesize_uv;
    return -1;
}

int renderer_should_close(Renderer *renderer)
{
    (void)renderer;
    return 1;
}

void renderer_swap_buffers(Renderer *renderer)
{
    (void)renderer;
}

void renderer_poll_events(Renderer *renderer)
{
    (void)renderer;
}

double renderer_time(void)
{
    return 0.0;
}

#endif /* HAVE_GLFW */
