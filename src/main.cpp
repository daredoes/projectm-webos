// projectM for LG webOS: MilkDrop visuals driven by whatever the TV is playing.
//
// Audio comes from the TV's pulseaudio mixer monitor (everything the TV outputs,
// including the Sendspin player), so no app has to hand PCM over. projectM renders at
// a fraction of the panel resolution and the result is stretched to full screen, which
// keeps the frame rate up on TV-class GPUs.
//
// Remote: Left/Right change preset, Up/Down toggle automatic preset changes (off until
// turned on, then remembered), Back exits. The preset name shows in a corner for a
// few seconds after each change. Launched with {"screensaver":true}, any other key or a pointer
// click exits, so it behaves like a screensaver.
#include <SDL.h>
#include <GLES3/gl3.h>
#include <pulse/error.h>
#include <pulse/simple.h>
#include <projectM-4/playlist.h>
#include <projectM-4/projectM.h>

#include "stb_easy_font.h"

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
// A preset that renders slower than this, measured after its blend-in, is skipped.
constexpr float kSlowFps = 30.0f;
constexpr Uint32 kBlendMs = 3000;
constexpr Uint32 kMeasureMs = 3000;

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

bool isArrowKey(SDL_Keycode sym) {
    return sym == SDLK_LEFT || sym == SDLK_RIGHT || sym == SDLK_UP || sym == SDLK_DOWN;
}

Uint32 g_presetStart = 0;          // SDL ticks when the current preset was switched to
std::atomic<int> g_switchedTo{-1};  // playlist index of a switch not yet shown on screen

void onPresetSwitched(bool, unsigned int index, void*) {
    g_presetStart = SDL_GetTicks();
    g_switchedTo = static_cast<int>(index);
}

// The auto-change toggle survives restarts in a one-character file.
std::string settingsPath() {
    const char* home = getenv("HOME");
    return std::string(home && *home ? home : "/tmp") + "/.projectm-webos-autochange";
}

bool loadAutoChange() {
    FILE* f = fopen(settingsPath().c_str(), "r");
    if (!f) {
        return false;
    }
    const int c = fgetc(f);
    fclose(f);
    return c == '1';
}

void saveAutoChange(bool on) {
    FILE* f = fopen(settingsPath().c_str(), "w");
    if (f) {
        fputc(on ? '1' : '0', f);
        fclose(f);
    }
}

// "Dancer/Geiss - Feedback (Rovastar mix).milk" -> "Geiss - Feedback (Rovastar mix)"
std::string presetTitle(const char* path) {
    std::string name = path ? path : "";
    name = name.substr(name.find_last_of('/') + 1);
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

// A line of text in the top-left corner for a few seconds, drawn with stb_easy_font's
// quads so no font file or texture is needed.
class Overlay {
public:
    void init() {
        const char* vs =
            "#version 300 es\n"
            "layout(location = 0) in vec2 pos;\n"
            "uniform vec2 screen;\n"
            "uniform vec2 offset;\n"
            "uniform float scale;\n"
            "void main() {\n"
            "  vec2 p = pos * scale + offset;\n"
            "  gl_Position = vec4(p.x / screen.x * 2.0 - 1.0, 1.0 - p.y / screen.y * 2.0, 0.0, 1.0);\n"
            "}\n";
        const char* fs =
            "#version 300 es\n"
            "precision mediump float;\n"
            "uniform vec4 color;\n"
            "out vec4 fragColor;\n"
            "void main() { fragColor = color; }\n";
        program_ = glCreateProgram();
        glAttachShader(program_, compile(GL_VERTEX_SHADER, vs));
        glAttachShader(program_, compile(GL_FRAGMENT_SHADER, fs));
        glLinkProgram(program_);
        glGenVertexArrays(1, &vao_);
        glGenBuffers(1, &vbo_);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    void show(std::string text, Uint32 ms) {
        if (text.size() > 80) {
            text.resize(80);
        }
        for (char& c : text) {  // stb_easy_font only has printable ASCII glyphs
            if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) > 126) {
                c = '?';
            }
        }
        static char quads[64000];
        const int n = stb_easy_font_print(0, 0, &text[0], nullptr, quads, sizeof(quads));
        std::vector<float> tris;
        tris.reserve(static_cast<size_t>(n) * 12);
        for (int q = 0; q < n; ++q) {
            const float* v = reinterpret_cast<const float*>(quads + q * 64);  // 4 vertices x 16 bytes
            for (int i : {0, 1, 2, 0, 2, 3}) {
                tris.push_back(v[i * 4]);
                tris.push_back(v[i * 4 + 1]);
            }
        }
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(tris.size() * sizeof(float)), tris.data(),
                     GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        count_ = static_cast<GLsizei>(tris.size() / 2);
        until_ = SDL_GetTicks() + ms;
    }

    void draw(int width, int height) {
        if (count_ == 0 || SDL_GetTicks() > until_) {
            return;
        }
        const float scale = static_cast<float>(height) / 1080.0f * 4.0f;
        const float margin = static_cast<float>(height) / 1080.0f * 48.0f;
        glViewport(0, 0, width, height);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glUseProgram(program_);
        glUniform2f(glGetUniformLocation(program_, "screen"), static_cast<float>(width), static_cast<float>(height));
        glUniform1f(glGetUniformLocation(program_, "scale"), scale);
        glBindVertexArray(vao_);
        glUniform2f(glGetUniformLocation(program_, "offset"), margin + scale, margin + scale);  // shadow
        glUniform4f(glGetUniformLocation(program_, "color"), 0.0f, 0.0f, 0.0f, 1.0f);
        glDrawArrays(GL_TRIANGLES, 0, count_);
        glUniform2f(glGetUniformLocation(program_, "offset"), margin, margin);
        glUniform4f(glGetUniformLocation(program_, "color"), 1.0f, 1.0f, 1.0f, 1.0f);
        glDrawArrays(GL_TRIANGLES, 0, count_);
        glBindVertexArray(0);
        glUseProgram(0);
    }

private:
    GLuint program_ = 0, vao_ = 0, vbo_ = 0;
    GLsizei count_ = 0;
    Uint32 until_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = appDir();
    // webOS passes the launch parameters as a JSON string in argv[1].
    const bool screensaver = argc > 1 && std::string(argv[1]).find("\"screensaver\":true") != std::string::npos;
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
    bool autoChange = loadAutoChange();
    projectm_set_preset_locked(pm, !autoChange);
    fprintf(stderr, "auto preset change: %s (%s)\n", autoChange ? "on" : "off", settingsPath().c_str());

    projectm_playlist_handle playlist = projectm_playlist_create(pm);
    const std::string presets = dir + "/presets";
    fprintf(stderr, "presets: %u from %s\n", projectm_playlist_add_path(playlist, presets.c_str(), true, false),
            presets.c_str());
    projectm_playlist_set_shuffle(playlist, true);
    projectm_playlist_set_preset_switched_event_callback(playlist, onPresetSwitched, nullptr);
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

    Overlay overlay;
    overlay.init();

    std::thread audio(captureAudio, std::string(source));
    std::vector<float> pending;
    Uint32 fpsStart = SDL_GetTicks();
    int frames = 0;
    Uint32 measureStart = 0;
    int measureFrames = 0;
    fprintf(stderr, "mode: %s\n", screensaver ? "screensaver" : "app");

    while (g_running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            const SDL_Keycode sym = ev.type == SDL_KEYDOWN ? ev.key.keysym.sym : 0;
            if (ev.type == SDL_QUIT || (ev.type == SDL_KEYDOWN && isExitKey(ev.key.keysym)) ||
                (screensaver && ev.type == SDL_KEYDOWN && !isArrowKey(sym)) ||
                (screensaver && ev.type == SDL_MOUSEBUTTONDOWN)) {
                g_running = false;
            } else if (sym == SDLK_RIGHT) {
                projectm_playlist_play_next(playlist, true);
            } else if (sym == SDLK_LEFT) {
                projectm_playlist_play_previous(playlist, true);
            } else if (sym == SDLK_UP || sym == SDLK_DOWN) {
                autoChange = !autoChange;
                projectm_set_preset_locked(pm, !autoChange);
                saveAutoChange(autoChange);
                overlay.show(autoChange ? "Auto change: on" : "Auto change: off", 3000);
                fprintf(stderr, "auto preset change: %s\n", autoChange ? "on" : "off");
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

        const int switched = g_switchedTo.exchange(-1);
        if (switched >= 0) {
            char* path = projectm_playlist_item(playlist, static_cast<uint32_t>(switched));
            overlay.show(presetTitle(path), 4000);
            projectm_playlist_free_string(path);
        }
        overlay.draw(width, height);

        SDL_GL_SwapWindow(window);

        // Skip presets this GPU cannot render smoothly, once their blend-in is over.
        const Uint32 now = SDL_GetTicks();
        if (now - g_presetStart < kBlendMs) {
            measureStart = 0;
        } else if (measureStart == 0) {
            measureStart = now;
            measureFrames = 0;
        } else if (++measureFrames, now - measureStart >= kMeasureMs) {
            const float presetFps = measureFrames * 1000.0f / static_cast<float>(now - measureStart);
            if (presetFps < kSlowFps) {
                fprintf(stderr, "slow preset (%.1f fps), skipping\n", presetFps);
                projectm_playlist_play_next(playlist, true);
            }
            measureStart = 0;
        }

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
