// projectM for LG webOS: MilkDrop visuals driven by whatever the TV is playing.
//
// Audio comes from the TV's pulseaudio mixer monitor (everything the TV outputs,
// including the Sendspin player), so no app has to hand PCM over. projectM renders at
// a fraction of the panel resolution and the result is stretched to full screen, which
// keeps the frame rate up on TV-class GPUs.
#include <SDL.h>
#include <GLES3/gl3.h>
#include <pulse/error.h>
#include <pulse/simple.h>
#include <projectM-4/playlist.h>
#include <projectM-4/projectM.h>

#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr unsigned kSampleRate = 44100;
constexpr size_t kChunkFrames = 512;

std::mutex g_pcmLock;
std::vector<float> g_pcm;  // interleaved stereo float samples awaiting projectM
std::atomic<bool> g_running{true};

// Reads the mixer monitor in small chunks so the visuals stay close to the sound.
void captureAudio(std::string source) {
    pa_sample_spec spec{PA_SAMPLE_FLOAT32LE, kSampleRate, 2};
    pa_buffer_attr attr{static_cast<uint32_t>(-1), static_cast<uint32_t>(-1), static_cast<uint32_t>(-1),
                        static_cast<uint32_t>(-1), static_cast<uint32_t>(kChunkFrames * 2 * sizeof(float))};
    int err = 0;
    pa_simple* in = pa_simple_new(nullptr, "projectM", PA_STREAM_RECORD, source.c_str(), "visualizer",
                                  &spec, nullptr, &attr, &err);
    if (!in) {
        fprintf(stderr, "audio: cannot open %s: %s\n", source.c_str(), pa_strerror(err));
        return;
    }
    std::vector<float> chunk(kChunkFrames * 2);
    while (g_running) {
        if (pa_simple_read(in, chunk.data(), chunk.size() * sizeof(float), &err) < 0) {
            fprintf(stderr, "audio: read failed: %s\n", pa_strerror(err));
            break;
        }
        std::lock_guard<std::mutex> guard(g_pcmLock);
        g_pcm.insert(g_pcm.end(), chunk.begin(), chunk.end());
        if (g_pcm.size() > kSampleRate * 2) {  // drop anything older than a second
            g_pcm.erase(g_pcm.begin(), g_pcm.end() - kSampleRate * 2);
        }
    }
    pa_simple_free(in);
}

std::string appDir() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        return ".";
    }
    std::string path(buf, static_cast<size_t>(n));
    return path.substr(0, path.find_last_of('/'));
}

float envFloat(const char* name, float fallback) {
    const char* v = getenv(name);
    return v ? static_cast<float>(atof(v)) : fallback;
}

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    return s;
}

// Draws a texture over the whole viewport with one oversized triangle.
GLuint upscaleProgram() {
    const char* vs =
        "#version 300 es\n"
        "out vec2 uv;\n"
        "void main() {\n"
        "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
        "  uv = p;\n"
        "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
    const char* fs =
        "#version 300 es\n"
        "precision mediump float;\n"
        "uniform sampler2D tex;\n"
        "uniform vec2 scale;\n"
        "in vec2 uv;\n"
        "out vec4 color;\n"
        "void main() { color = texture(tex, uv * scale); }\n";
    GLuint prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(prog);
    return prog;
}

bool isExitKey(const SDL_Keysym& key) {
    return key.sym == SDLK_ESCAPE || key.sym == SDLK_AC_BACK || key.sym == SDLK_BACKSPACE ||
           key.scancode == SDL_SCANCODE_AC_BACK;
}

}  // namespace

int main(int, char**) {
    const std::string dir = appDir();
    freopen("/tmp/projectm-webos.log", "w", stderr);
    setvbuf(stderr, nullptr, _IOLBF, 0);

    const float renderScale = envFloat("PROJECTM_SCALE", 0.5f);
    const char* source = getenv("PROJECTM_SOURCE") ? getenv("PROJECTM_SOURCE") : "pcm_output.monitor";

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_Window* window = SDL_CreateWindow("projectM", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 1920, 1080,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GLContext gl = SDL_GL_CreateContext(window);
    if (!gl) {
        fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetSwapInterval(1);

    int width = 0, height = 0;
    SDL_GL_GetDrawableSize(window, &width, &height);
    const int rw = renderScale < 1.0f ? static_cast<int>(width * renderScale) : width;
    const int rh = renderScale < 1.0f ? static_cast<int>(height * renderScale) : height;
    fprintf(stderr, "GL: %s | %s | screen %dx%d, render %dx%d\n", glGetString(GL_RENDERER), glGetString(GL_VERSION),
            width, height, rw, rh);

    projectm_handle pm = projectm_create();
    if (!pm) {
        fprintf(stderr, "projectm_create failed\n");
        return 1;
    }
    projectm_set_window_size(pm, rw, rh);
    projectm_set_fps(pm, 60);
    projectm_set_mesh_size(pm, 48, 32);
    projectm_set_preset_duration(pm, 25.0);
    projectm_set_soft_cut_duration(pm, 3.0);
    projectm_set_hard_cut_enabled(pm, false);

    projectm_playlist_handle playlist = projectm_playlist_create(pm);
    const std::string presets = dir + "/presets";
    fprintf(stderr, "presets: %u from %s\n", projectm_playlist_add_path(playlist, presets.c_str(), true, false),
            presets.c_str());
    projectm_playlist_set_shuffle(playlist, true);
    projectm_playlist_play_next(playlist, true);

    GLuint program = 0, texture = 0, vao = 0;
    if (rw != width || rh != height) {
        program = upscaleProgram();
        glGenVertexArrays(1, &vao);
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rw, rh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    std::thread audio(captureAudio, std::string(source));
    std::vector<float> pending;
    Uint32 fpsStart = SDL_GetTicks();
    int frames = 0;

    while (g_running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT || (ev.type == SDL_KEYDOWN && isExitKey(ev.key.keysym))) {
                g_running = false;
            } else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_RIGHT) {
                projectm_playlist_play_next(playlist, true);
            } else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_LEFT) {
                projectm_playlist_play_previous(playlist, true);
            }
        }

        {
            std::lock_guard<std::mutex> guard(g_pcmLock);
            pending.swap(g_pcm);
        }
        if (!pending.empty()) {
            projectm_pcm_add_float(pm, pending.data(), static_cast<unsigned>(pending.size() / 2), PROJECTM_STEREO);
            pending.clear();
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, rw, rh);
        projectm_opengl_render_frame(pm);

        if (program) {
            // projectM drew into the lower-left rw x rh corner; stretch it over the screen.
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texture);
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, rw, rh);
            glViewport(0, 0, width, height);
            glDisable(GL_BLEND);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_SCISSOR_TEST);
            glUseProgram(program);
            glUniform1i(glGetUniformLocation(program, "tex"), 0);
            glUniform2f(glGetUniformLocation(program, "scale"), 1.0f, 1.0f);
            glBindVertexArray(vao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindVertexArray(0);
            glUseProgram(0);
        }

        SDL_GL_SwapWindow(window);

        if (++frames == 300) {
            Uint32 now = SDL_GetTicks();
            fprintf(stderr, "fps: %.1f\n", frames * 1000.0f / static_cast<float>(now - fpsStart));
            fpsStart = now;
            frames = 0;
        }
    }

    g_running = false;
    audio.join();
    projectm_playlist_destroy(playlist);
    projectm_destroy(pm);
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
