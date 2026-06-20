#include <jni.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "pl/Gloss.h"

#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

static bool g_Initialized = false;
static int g_Width = 0, g_Height = 0;
static EGLContext g_TargetContext = EGL_NO_CONTEXT;
static EGLSurface g_TargetSurface = EGL_NO_SURFACE;

static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;

struct ShaderEntry {
    std::string source;
};
static std::map<GLuint, ShaderEntry> g_Shaders;

struct ProgramInfo {
    uint32_t hash;
    char label[64];
    bool enabled;
};
static std::map<GLuint, ProgramInfo> g_Programs;
static std::vector<GLuint> g_ProgramOrder;
static int g_LastProgram = -1;
static int g_SeenPrograms[256] = {0};
static int g_SeenCount = 0;
static const uint32_t g_DefaultHashes[] = {0x3F94D1B0, 0x6E7CA2C7, 0x151CB0FF, 0x30D52ED0, 0xB91FC562};
static const char* g_DefaultLabels[] = {"3F94D1B0", "6E7CA2C7", "151CB0FF", "30D52ED0", "B91FC562"};
static const int g_DefaultHashCount = 5;
static char g_HashInput[16] = "";

static uint32_t computeHash(const std::string& s) {
    uint32_t h = 0x811c9dc5;
    for (char c : s) {
        h ^= (uint8_t)c;
        h *= 0x01000193;
    }
    return h;
}

static void matchLabel(const std::string& src, char* out, size_t maxLen) {
    out[0] = '\0';
    // 先找更具体的特征
    struct { const char* key; const char* name; } specific[] = {
        {"flat_color_line", "flat_color_line"},
        {"entity_static", "entity_static"},
        {"entity_lead", "entity_lead"},
        {"entity_beam", "entity_beam"},
        {"entity_named", "entity_named"},
        {"entity_alphatest", "entity_alpha"},
        {"renderchunk", "renderchunk"},
        {"particle", "particle"},
        {"weather", "weather"},
        {"clouds", "clouds"},
        {"water", "water"},
        {"translucent", "translucent"},
        {"glint", "glint"},
        {"shadow", "shadow"},
        {"ui_screen", "ui_screen"},
        {"ui_item", "ui_item"},
        {"ui_text", "ui_text"},
        {"sky", "sky"},
        {"line", "line"},
        {"depth", "depth"},
    };
    for (auto& kw : specific) {
        if (src.find(kw.key) != std::string::npos) {
            strncpy(out, kw.name, maxLen - 1);
            out[maxLen - 1] = '\0';
            return;
        }
    }
    // entity 相关：显示第一个不同的 uniform 名
    if (src.find("entity") != std::string::npos) {
        const char* uniforms[] = {"BONE", "COLOR", "UV", "NOSHADE", "FOG", "LIGHT", "TEXCOORD", "ATTRIB"};
        for (auto& u : uniforms) {
            if (src.find(u) != std::string::npos) {
                snprintf(out, maxLen, "entity_%s", u);
                return;
            }
        }
        // 显示源码前20字符作为特征
        size_t pos = src.find("entity");
        if (pos + 30 < src.size()) {
            char excerpt[32];
            strncpy(excerpt, src.c_str() + pos, 20);
            excerpt[20] = '\0';
            snprintf(out, maxLen, "entity...%s", excerpt);
        } else {
            strncpy(out, "entity", maxLen - 1);
            out[maxLen - 1] = '\0';
        }
        return;
    }
    // ui 相关
    if (src.find("ui") != std::string::npos) {
        strncpy(out, "ui", maxLen - 1);
        out[maxLen - 1] = '\0';
        return;
    }
    snprintf(out, maxLen, "prog_%u", computeHash(src) % 10000);
}

static void (*orig_glShaderSource)(GLuint, GLsizei, const GLchar**, const GLint*) = nullptr;
static void hook_glShaderSource(GLuint shader, GLsizei count, const GLchar** strings, const GLint* lengths) {
    if (orig_glShaderSource) orig_glShaderSource(shader, count, strings, lengths);
    std::string src;
    for (GLsizei i = 0; i < count; i++) {
        if (lengths && lengths[i] > 0)
            src.append(strings[i], lengths[i]);
        else
            src.append(strings[i] ? strings[i] : "");
    }
    g_Shaders[shader].source = src;
}

static void (*orig_glLinkProgram)(GLuint) = nullptr;
static void hook_glLinkProgram(GLuint program) {
    if (orig_glLinkProgram) orig_glLinkProgram(program);

    GLuint shaders[16];
    GLsizei count = 0;
    glGetAttachedShaders(program, 16, &count, shaders);

    std::string combined;
    for (GLsizei i = 0; i < count; i++) {
        auto it = g_Shaders.find(shaders[i]);
        if (it != g_Shaders.end())
            combined += it->second.source;
    }

    uint32_t hash = computeHash(combined);

    bool exists = false;
    for (auto& p : g_Programs) {
        if (p.second.hash == hash) { exists = true; break; }
    }

    if (!exists) {
        ProgramInfo info;
        info.hash = hash;
        info.enabled = (hash == 0x30D52ED0);
        matchLabel(combined, info.label, sizeof(info.label));
        g_Programs[program] = info;
        g_ProgramOrder.push_back(program);
    }
}

static void (*orig_glDepthFunc)(GLenum) = nullptr;
static void hook_glDepthFunc(GLenum func) {
    if (!orig_glDepthFunc) return;
    GLint prog = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    g_LastProgram = prog;

    bool seen = false;
    for (int i = 0; i < g_SeenCount; i++) {
        if (g_SeenPrograms[i] == prog) { seen = true; break; }
    }
    if (!seen && g_SeenCount < 256) {
        g_SeenPrograms[g_SeenCount++] = prog;
    }

    auto it = g_Programs.find(prog);
    if (it != g_Programs.end() && it->second.enabled) {
        orig_glDepthFunc(GL_ALWAYS);
        return;
    }
    orig_glDepthFunc(func);
}

static void (*orig_Input1)(void*, void*, void*) = nullptr;
static void hook_Input1(void* thiz, void* a1, void* a2) {
    if (orig_Input1) orig_Input1(thiz, a1, a2);
    if (thiz && g_Initialized) ImGui_ImplAndroid_HandleInputEvent((AInputEvent*)thiz);
}

static int32_t (*orig_Input2)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;
static int32_t hook_Input2(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** event) {
    int32_t result = orig_Input2 ? orig_Input2(thiz, a1, a2, a3, a4, event) : 0;
    if (result == 0 && event && *event && g_Initialized) {
        ImGui_ImplAndroid_HandleInputEvent(*event);
    }
    return result;
}

struct GLState {
    GLint prog, tex, aTex, aBuf, eBuf, vao, fbo, vp[4], sc[4], bSrc, bDst;
    GLboolean blend, cull, depth, scissor;
};

static void SaveGL(GLState& s) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.prog);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.aTex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.aBuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.eBuf);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fbo);
    glGetIntegerv(GL_VIEWPORT, s.vp);
    glGetIntegerv(GL_SCISSOR_BOX, s.sc);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s.bSrc);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s.bDst);
    s.blend = glIsEnabled(GL_BLEND);
    s.cull = glIsEnabled(GL_CULL_FACE);
    s.depth = glIsEnabled(GL_DEPTH_TEST);
    s.scissor = glIsEnabled(GL_SCISSOR_TEST);
}

static void RestoreGL(const GLState& s) {
    glUseProgram(s.prog);
    glActiveTexture(s.aTex);
    glBindTexture(GL_TEXTURE_2D, s.tex);
    glBindBuffer(GL_ARRAY_BUFFER, s.aBuf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.eBuf);
    glBindVertexArray(s.vao);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glViewport(s.vp[0], s.vp[1], s.vp[2], s.vp[3]);
    glScissor(s.sc[0], s.sc[1], s.sc[2], s.sc[3]);
    glBlendFunc(s.bSrc, s.bDst);
    s.blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    s.cull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
    s.depth ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
    s.scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
}

static void DrawMenu() {
    ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_FirstUseEver);
    ImGui::Begin("FPS");
    ImGui::Text("%.1f FPS", ImGui::GetIO().Framerate);
    ImGui::Text("Last: %d", g_LastProgram);
    ImGui::Separator();

    if (g_SeenCount > 0) {
        if (ImGui::CollapsingHeader("Seen", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (int i = 0; i < g_SeenCount; i++) {
                if (i > 0 && i % 10 == 0) ImGui::NewLine();
                ImGui::SameLine();
                ImGui::Text("%d ", g_SeenPrograms[i]);
            }
        }
    }
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Quick")) {
        if (ImGui::Button("All Entity")) {
            for (auto& p : g_Programs) {
                if (strstr(p.second.label, "entity")) { p.second.enabled = true; }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("None")) {
            for (auto& p : g_Programs) { p.second.enabled = false; }
        }
        ImGui::Separator();
        for (int i = 0; i < g_DefaultHashCount; i++) {
            bool enabled = false;
            for (auto& p : g_Programs) {
                if (p.second.hash == g_DefaultHashes[i]) { enabled = p.second.enabled; break; }
            }
            char label[32];
            snprintf(label, sizeof(label), "%s##%d", g_DefaultLabels[i], i);
            if (ImGui::Checkbox(label, &enabled)) {
                for (auto& p : g_Programs) {
                    if (p.second.hash == g_DefaultHashes[i]) { p.second.enabled = enabled; break; }
                }
            }
        }
    }

    if (ImGui::CollapsingHeader("Shaders", ImGuiTreeNodeFlags_DefaultOpen)) {
        int count = 0;
        for (int i = 0; i < g_SeenCount; i++) {
            GLuint prog = g_SeenPrograms[i];
            auto it = g_Programs.find(prog);

            char label[128];
            if (it != g_Programs.end()) {
                auto& info = it->second;
                snprintf(label, sizeof(label), "P%u [%08X] %s##%u", prog, info.hash, info.label, prog);
                ImGui::Checkbox(label, &info.enabled);
            } else {
                snprintf(label, sizeof(label), "P%u [unknown]##%u", prog, prog);
                static bool dummy = false;
                ImGui::Checkbox(label, &dummy);
            }
            if (++count % 2 == 0) ImGui::SameLine();
        }
    }

    ImGui::Separator();
    ImGui::Text("Add Hash:");
    ImGui::SetNextItemWidth(120);
    ImGui::InputText("##hash", g_HashInput, sizeof(g_HashInput));
    ImGui::SameLine();
    if (ImGui::Button("Enable")) {
        uint32_t h = (uint32_t)strtoul(g_HashInput, nullptr, 16);
        if (h != 0) {
            for (auto& p : g_Programs) {
                if (p.second.hash == h) { p.second.enabled = true; break; }
            }
            g_HashInput[0] = '\0';
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Disable")) {
        uint32_t h = (uint32_t)strtoul(g_HashInput, nullptr, 16);
        if (h != 0) {
            for (auto& p : g_Programs) {
                if (p.second.hash == h) { p.second.enabled = false; break; }
            }
            g_HashInput[0] = '\0';
        }
    }

    ImGui::End();
}

static void Setup() {
    if (g_Initialized || g_Width <= 0 || g_Height <= 0) return;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    float scale = (float)g_Height / 720.0f;
    if (scale < 1.5f) scale = 1.5f;
    if (scale > 4.0f) scale = 4.0f;
    ImFontConfig cfg;
    cfg.SizePixels = 32.0f * scale;
    io.Fonts->AddFontDefault(&cfg);
    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().ScrollbarSize = 20.0f;
    g_Initialized = true;
}

static void Render() {
    if (!g_Initialized) return;
    GLState s;
    SaveGL(s);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_Width, (float)g_Height);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_Width, g_Height);
    ImGui::NewFrame();
    DrawMenu();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    RestoreGL(s);
}

static EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!orig_eglSwapBuffers) return EGL_FALSE;
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return orig_eglSwapBuffers(dpy, surf);
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w < 500 || h < 500) return orig_eglSwapBuffers(dpy, surf);
    if (g_TargetContext == EGL_NO_CONTEXT) {
        EGLint buf = 0;
        eglQuerySurface(dpy, surf, EGL_RENDER_BUFFER, &buf);
        if (buf == EGL_BACK_BUFFER) {
            g_TargetContext = ctx;
            g_TargetSurface = surf;
        }
    }
    if (ctx != g_TargetContext || surf != g_TargetSurface)
        return orig_eglSwapBuffers(dpy, surf);
    g_Width = w;
    g_Height = h;
    Setup();
    Render();
    return orig_eglSwapBuffers(dpy, surf);
}

static void HookInput() {
    void* sym1 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE", nullptr);
    if (sym1) {
        GHook h = GlossHook(sym1, (void*)hook_Input1, (void**)&orig_Input1);
        if (h) return;
    }
    void* sym2 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE", nullptr);
    if (sym2) {
        GHook h = GlossHook(sym2, (void*)hook_Input2, (void**)&orig_Input2);
        if (h) return;
    }
}

static void* MainThread(void*) {
    sleep(3);
    GlossInit(true);
    GHandle hEGL = GlossOpen("libEGL.so");
    if (!hEGL) return nullptr;
    void* swap = (void*)GlossSymbol(hEGL, "eglSwapBuffers", nullptr);
    if (!swap) return nullptr;
    GHook h = GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
    if (!h) return nullptr;
    HookInput();
    GHandle hGL = GlossOpen("libGLESv2.so");
    if (hGL) {
        void* depthFunc = (void*)GlossSymbol(hGL, "glDepthFunc", nullptr);
        if (depthFunc)
            GlossHook(depthFunc, (void*)hook_glDepthFunc, (void**)&orig_glDepthFunc);
        void* shaderSource = (void*)GlossSymbol(hGL, "glShaderSource", nullptr);
        if (shaderSource)
            GlossHook(shaderSource, (void*)hook_glShaderSource, (void**)&orig_glShaderSource);
        void* linkProgram = (void*)GlossSymbol(hGL, "glLinkProgram", nullptr);
        if (linkProgram)
            GlossHook(linkProgram, (void*)hook_glLinkProgram, (void**)&orig_glLinkProgram);
    }
    return nullptr;
}

__attribute__((constructor))
void DisplayFPS_Init() {
    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
}
