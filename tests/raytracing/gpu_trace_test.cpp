// Headless GPU test: runs rt_common.glsl traversal (closest + any hit) in a GL 4.5 compute shader
// (EGL, works on Mesa llvmpipe) and compares against the CPU BVH. Build: see tests/raytracing/README.md
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include "modules/raytracing/bvh.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <string>
using namespace gfx::rt;

static std::string shaderRoot;
static std::string readFile(const std::string& p) { std::ifstream f(p); std::stringstream s; s << f.rdbuf(); return s.str(); }
static std::string expand(const std::string& path, std::set<std::string>& seen) {
    std::string src = readFile(path), out; std::istringstream in(src); std::string line;
    std::regex inc("^#include \"([^\"]+)\"");
    while (std::getline(in, line)) { std::smatch m;
        if (std::regex_search(line, m, inc)) { if (seen.insert(m[1]).second) out += expand(shaderRoot + "/" + m[1].str(), seen); }
        else out += line + "\n"; }
    return out;
}

static const char* kTest = R"(#version 450
layout(local_size_x = 64) in;
#include "raytracing/rt_common.glsl"
struct TRay { vec4 o; vec4 d; };
layout(std430, binding = 0) readonly buffer Rays { TRay rays[]; };
layout(std430, binding = 1) writeonly buffer Res { vec4 res[]; };
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= uint(rays.length())) return;
    Hit h; bool hit = traceClosest(rays[i].o.xyz, rays[i].d.xyz, 1e30, h);
    bool any = traceAny(rays[i].o.xyz, rays[i].d.xyz, 1e30);
    res[i] = vec4(hit ? h.t : -1.0, hit ? float(h.tri) : -1.0, any ? 1.0 : 0.0, 0.0);
}
)";

int main(int argc, char** argv) {
    shaderRoot = argc > 1 ? argv[1] : "shaders";
    // Prefer the Mesa surfaceless platform (no X server needed), fall back to the default display.
    auto getPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy = getPlatformDisplay ? getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr) : EGL_NO_DISPLAY;
    if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(dpy, nullptr, nullptr)) { std::printf("EGL init failed\n"); return 2; }
    EGLint cfgAttr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
    EGLConfig cfg; EGLint n; eglChooseConfig(dpy, cfgAttr, &cfg, 1, &n);
    eglBindAPI(EGL_OPENGL_API);
    EGLint ctxAttr[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5, EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, n ? cfg : nullptr, EGL_NO_CONTEXT, ctxAttr);
    if (!ctx || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) { std::printf("GL 4.5 context failed\n"); return 2; }
    std::printf("GL: %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    // Scene: random soup + a ground plane
    std::mt19937 rng(7); std::uniform_real_distribution<float> U(-10.f, 10.f), S(-0.6f, 0.6f);
    std::vector<BuildTriangle> tris(30000);
    for (auto& t : tris) { glm::vec3 c(U(rng), U(rng) * 0.5f + 3.f, U(rng));
        for (int k = 0; k < 3; ++k) { t.p[k] = c + glm::vec3(S(rng), S(rng), S(rng)); t.n[k] = {0, 1, 0}; } t.material = 0; }
    BuildTriangle g0{{{-50, 0, -50}, {50, 0, -50}, {50, 0, 50}}, {{0, 1, 0}, {0, 1, 0}, {0, 1, 0}}, 0};
    BuildTriangle g1{{{-50, 0, -50}, {50, 0, 50}, {-50, 0, 50}}, {{0, 1, 0}, {0, 1, 0}, {0, 1, 0}}, 0};
    tris.push_back(g0); tris.push_back(g1);
    BVH bvh; bvh.build(tris);
    std::printf("BVH: %u tris, %u nodes, depth %u, %.1f ms\n", bvh.stats().triangles, bvh.stats().nodes, bvh.stats().maxDepth, bvh.stats().buildMs);

    const int N = 4096;
    std::vector<glm::vec4> rays(N * 2);
    for (int i = 0; i < N; ++i) { rays[i * 2] = glm::vec4(U(rng), U(rng) * 0.5f + 4.f, U(rng), 0);
        rays[i * 2 + 1] = glm::vec4(glm::normalize(glm::vec3(U(rng), U(rng), U(rng))), 0); }

    std::set<std::string> seen;
    std::string src = kTest;
    { std::string tmp = "/tmp/rt_gpu_test.comp"; std::ofstream(tmp) << src; src = expand(tmp, seen); }
    GLuint sh = glCreateShader(GL_COMPUTE_SHADER); const char* s = src.c_str(); glShaderSource(sh, 1, &s, nullptr); glCompileShader(sh);
    GLint ok; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[4096]; glGetShaderInfoLog(sh, 4096, nullptr, log); std::printf("compile: %s\n", log); return 1; }
    GLuint prog = glCreateProgram(); glAttachShader(prog, sh); glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok); if (!ok) { std::printf("link failed\n"); return 1; }

    auto ssbo = [](GLuint binding, const void* data, size_t bytes) { GLuint b; glCreateBuffers(1, &b);
        glNamedBufferData(b, GLsizeiptr(bytes), data, GL_STATIC_DRAW); glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, b); return b; };
    std::vector<GpuMaterial> mats(1, GpuMaterial{glm::vec4(0.8f), glm::vec4(0.f), glm::vec4(0.f)});
    ssbo(10, bvh.nodes().data(), bvh.nodes().size() * sizeof(BVHNode));
    ssbo(11, bvh.gpuTris().data(), bvh.gpuTris().size() * sizeof(GpuTri));
    ssbo(12, bvh.gpuAttrs().data(), bvh.gpuAttrs().size() * sizeof(GpuTriAttr));
    ssbo(13, mats.data(), mats.size() * sizeof(GpuMaterial));
    std::vector<char> ubo(1024, 0); GLuint u; glCreateBuffers(1, &u); glNamedBufferData(u, 1024, ubo.data(), GL_STATIC_DRAW); glBindBufferBase(GL_UNIFORM_BUFFER, 10, u);
    ssbo(0, rays.data(), rays.size() * sizeof(glm::vec4));
    GLuint resBuf = ssbo(1, nullptr, N * sizeof(glm::vec4));

    glUseProgram(prog);
    GLuint q; glGenQueries(1, &q); glBeginQuery(GL_TIME_ELAPSED, q);
    glDispatchCompute((N + 63) / 64, 1, 1);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT); glEndQuery(GL_TIME_ELAPSED);
    GLuint64 ns = 0; glGetQueryObjectui64v(q, GL_QUERY_RESULT, &ns);
    std::vector<glm::vec4> res(N); glGetNamedBufferSubData(resBuf, 0, N * sizeof(glm::vec4), res.data());

    int bad = 0, hits = 0, anyBad = 0;
    for (int i = 0; i < N; ++i) {
        uint32_t tri = 0; float t = bvh.intersect(glm::vec3(rays[i * 2]), glm::vec3(rays[i * 2 + 1]), 1e30f, &tri);
        bool cpuHit = t > 0.f, gpuHit = res[i].x > 0.f;
        if (cpuHit) hits++;
        if (cpuHit != gpuHit || (cpuHit && std::fabs(t - res[i].x) > 1e-3f * std::max(1.f, t))) bad++;
        if ((res[i].z > 0.5f) != cpuHit) anyBad++;
    }
    std::printf("GPU rays %d, hits %d, closest-hit mismatches %d, any-hit mismatches %d\n",
                N, hits, bad, anyBad); (void)ns;
    return (bad || anyBad) ? 1 : 0;
}
