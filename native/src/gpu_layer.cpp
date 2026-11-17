#include "gpu_layer.h"

#include <chrono>
#include <cstddef>
#include <cstdio>

#if defined(BS_GLES3)
#include <GLES3/gl3.h>
#include <android/log.h>
#define BS_GPU_LOG(...) __android_log_print(ANDROID_LOG_INFO, "BS1-GPU", __VA_ARGS__)
#else
#define BS_GPU_LOG(...) ((void)0)
#endif

namespace gpu {
namespace {

FrameStats g_stats;
xenos::pm4::CommandBufferStats g_pm4_total;
xenos::RegisterFile g_regs;
std::vector<xenos::DrawCommand> g_draws;
int g_width = 1;
int g_height = 1;
bool g_ready = false;

#if defined(BS_GLES3)
GLuint g_program = 0;
GLuint g_vao = 0;
GLuint g_vbo = 0;
GLint g_time_loc = -1;
GLint g_res_loc = -1;

constexpr const char* kVertexShader = R"GLSL(
#version 300 es
out vec2 v_uv;
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

constexpr const char* kFragmentShader = R"GLSL(
#version 300 es
precision highp float;
in vec2 v_uv;
uniform float u_time;
uniform vec2 u_resolution;
out vec4 frag_color;

void main() {
    // Diagnostic pattern: proves the GLES3 pipeline, timestamps and the
    // frame loop are alive before the Xenos command translator is wired in.
    vec2 uv = v_uv;
    float wave = sin(uv.x * 12.0 + u_time) * sin(uv.y * 8.0 - u_time * 0.7);
    float grid = step(0.97, fract(uv.x * 16.0)) + step(0.97, fract(uv.y * 9.0));
    vec3 col = vec3(0.04, 0.09, 0.16);
    col += vec3(0.10, 0.35, 0.55) * (wave * 0.5 + 0.5);
    col += vec3(0.25, 0.55, 0.35) * grid * 0.6;
    float vignette = smoothstep(1.2, 0.2, length(uv - 0.5));
    frag_color = vec4(col * vignette, 1.0);
}
)GLSL";

GLuint compile_shader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[gpu] shader compile error: %s\n", log);
        BS_GPU_LOG("shader compile error: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    BS_GPU_LOG("shader compiled ok (type=%d)", static_cast<int>(type));
    return shader;
}
#endif

float now_seconds() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return duration<float>(steady_clock::now() - start).count();
}

} // namespace

bool initialize(int width, int height) {
    g_width = width > 0 ? width : 1;
    g_height = height > 0 ? height : 1;
#if defined(BS_GLES3)
    GLuint vs = compile_shader(GL_VERTEX_SHADER, kVertexShader);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vs || !fs) return false;

    g_program = glCreateProgram();
    glAttachShader(g_program, vs);
    glAttachShader(g_program, fs);
    glLinkProgram(g_program);
    GLint ok = 0;
    glGetProgramiv(g_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(g_program, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[gpu] program link error: %s\n", log);
        return false;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);

    g_time_loc = glGetUniformLocation(g_program, "u_time");
    g_res_loc = glGetUniformLocation(g_program, "u_resolution");

    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    // A single unused buffer keeps strict GLES3 drivers happy; the vertex
    // shader generates its own triangle from gl_VertexID.
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBindVertexArray(0);
    BS_GPU_LOG("initialize done: width=%d height=%d glError=0x%X",
               g_width, g_height, glGetError());
#endif
    g_ready = true;
    BS_GPU_LOG("ready=%d", g_ready ? 1 : 0);
    return true;
}

void shutdown() {
#if defined(BS_GLES3)
    if (g_vbo) glDeleteBuffers(1, &g_vbo);
    if (g_vao) glDeleteVertexArrays(1, &g_vao);
    if (g_program) glDeleteProgram(g_program);
    g_vbo = g_vao = g_program = 0;
#endif
    g_ready = false;
}

void resize(int width, int height) {
    g_width = width > 0 ? width : 1;
    g_height = height > 0 ? height : 1;
#if defined(BS_GLES3)
    glViewport(0, 0, g_width, g_height);
#endif
}

xenos::pm4::CommandBufferStats submit_command_buffer(const void* data, size_t size_bytes) {
    const auto stats = xenos::pm4::parse(data, size_bytes, &g_regs, &g_draws);
    g_pm4_total.packets += stats.packets;
    g_pm4_total.register_writes += stats.register_writes;
    g_pm4_total.draws += stats.draws;
    g_pm4_total.swaps += stats.swaps;
    g_pm4_total.interrupts += stats.interrupts;
    g_pm4_total.shader_loads += stats.shader_loads;
    g_pm4_total.malformed += stats.malformed;
    return stats;
}

const xenos::pm4::CommandBufferStats& pm4_stats() { return g_pm4_total; }
const xenos::RegisterFile& register_file() { return g_regs; }
const std::vector<xenos::DrawCommand>& draw_commands() { return g_draws; }

void draw_test_pattern(float time_seconds) {
#if defined(BS_GLES3)
    glUseProgram(g_program);
    glUniform1f(g_time_loc, time_seconds);
    glUniform2f(g_res_loc, static_cast<float>(g_width), static_cast<float>(g_height));
    glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    ++g_stats.draws;
#else
    (void)time_seconds;
#endif
}

void present() {
    if (!g_ready) return;
    draw_test_pattern(now_seconds());
    ++g_stats.frames;
}

uint32_t upload_texture(const void* data, int width, int height, uint32_t format) {
    (void)format; // BCn/ASTC selection lands here with the detiler.
    ++g_stats.textures;
#if defined(BS_GLES3)
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    return static_cast<uint32_t>(tex);
#else
    (void)data; (void)width; (void)height;
    return 0;
#endif
}

const FrameStats& stats() { return g_stats; }

} // namespace gpu
