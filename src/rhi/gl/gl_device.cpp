// OpenGL 4.5 core backend (DSA). Enforces D3D conventions with glClipControl(UPPER_LEFT, ZERO_TO_ONE).
#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <unordered_map>

#include "rhi/backends.h"
#include "rhi/rhi.h"

#ifndef GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX
#define GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX 0x9047
#define GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX 0x9048
#define GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX 0x9049
#endif
#ifndef GL_TEXTURE_FREE_MEMORY_ATI
#define GL_TEXTURE_FREE_MEMORY_ATI 0x87FC
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY
#define GL_MAX_TEXTURE_MAX_ANISOTROPY 0x84FF
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#endif

namespace gfx::rhi {
namespace {

struct GLFormat { GLenum internal, format, type; };

GLFormat glFormat(Format f) {
    switch (f) {
        case Format::R8:          return {GL_R8, GL_RED, GL_UNSIGNED_BYTE};
        case Format::RG8:         return {GL_RG8, GL_RG, GL_UNSIGNED_BYTE};
        case Format::RGBA8:       return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE};
        case Format::RGBA8_SRGB:  return {GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE};
        case Format::BGRA8_SRGB:  return {GL_SRGB8_ALPHA8, GL_BGRA, GL_UNSIGNED_BYTE};
        case Format::R16F:        return {GL_R16F, GL_RED, GL_HALF_FLOAT};
        case Format::RG16F:       return {GL_RG16F, GL_RG, GL_HALF_FLOAT};
        case Format::RGBA16F:     return {GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT};
        case Format::R32F:        return {GL_R32F, GL_RED, GL_FLOAT};
        case Format::RG32F:       return {GL_RG32F, GL_RG, GL_FLOAT};
        case Format::RGBA32F:     return {GL_RGBA32F, GL_RGBA, GL_FLOAT};
        case Format::R11G11B10F:  return {GL_R11F_G11F_B10F, GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV};
        case Format::RGB10A2:     return {GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV};
        case Format::R32UI:       return {GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT};
        case Format::RG32UI:      return {GL_RG32UI, GL_RG_INTEGER, GL_UNSIGNED_INT};
        case Format::RGBA32UI:    return {GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT};
        case Format::D32F:        return {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT};
        case Format::D24S8:       return {GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8};
        case Format::BC1_SRGB:    return {0x8C4D /*GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT*/, 0, 0};
        case Format::BC3_SRGB:    return {0x8C4F /*GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT*/, 0, 0};
        case Format::BC4:         return {GL_COMPRESSED_RED_RGTC1, 0, 0};
        case Format::BC5:         return {GL_COMPRESSED_RG_RGTC2, 0, 0};
        case Format::BC7_SRGB:    return {GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, 0, 0};
        default:                  return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE};
    }
}

size_t blockBytes(Format f) { return (f == Format::BC1_SRGB || f == Format::BC4) ? 8 : 16; }

size_t surfaceBytes(Format f, uint32_t w, uint32_t h) {
    if (isCompressedFormat(f)) return size_t((w + 3) / 4) * ((h + 3) / 4) * blockBytes(f);
    return size_t(w) * h * formatBytesPerPixel(f);
}

GLenum glCompare(CompareOp op) {
    switch (op) {
        case CompareOp::Never: return GL_NEVER;       case CompareOp::Less: return GL_LESS;
        case CompareOp::LessEqual: return GL_LEQUAL;  case CompareOp::Equal: return GL_EQUAL;
        case CompareOp::GreaterEqual: return GL_GEQUAL; case CompareOp::Greater: return GL_GREATER;
        case CompareOp::NotEqual: return GL_NOTEQUAL; default: return GL_ALWAYS;
    }
}
GLenum glAddress(AddressMode m) {
    switch (m) {
        case AddressMode::Repeat: return GL_REPEAT; case AddressMode::Clamp: return GL_CLAMP_TO_EDGE;
        case AddressMode::Mirror: return GL_MIRRORED_REPEAT; default: return GL_CLAMP_TO_BORDER;
    }
}

uint32_t mipCountFor(uint32_t w, uint32_t h, uint32_t d = 1) {
    uint32_t m = 1, s = std::max({w, h, d});
    while (s > 1) { s >>= 1; ++m; }
    return m;
}

// ---------------------------------------------------------------------------------------
struct MemTracker {
    std::mutex mtx;
    std::map<std::string, size_t> perOwner;
    size_t tex = 0, buf = 0;
    uint64_t texturesDestroyed = 0;   // GL reuses texture names: cached FBOs must be dropped when one dies
    void add(const std::string& owner, size_t bytes, bool isTex) {
        std::lock_guard<std::mutex> l(mtx);
        perOwner[owner] += bytes; (isTex ? tex : buf) += bytes;
    }
    void sub(const std::string& owner, size_t bytes, bool isTex) {
        std::lock_guard<std::mutex> l(mtx);
        perOwner[owner] -= bytes; (isTex ? tex : buf) -= bytes;
    }
};

class GLTexture final : public Texture {
public:
    GLTexture(const TextureDesc& d, MemTracker& mem) : mem_(mem) {
        desc_ = d;
        if (desc_.mipLevels == 0) desc_.mipLevels = mipCountFor(d.width, d.height, d.type == TextureType::Tex3D ? d.depthOrLayers : 1);
        GLFormat f = glFormat(d.format);
        switch (d.type) {
            case TextureType::Tex2D:      target = d.samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D; break;
            case TextureType::Tex2DArray: target = GL_TEXTURE_2D_ARRAY; break;
            case TextureType::TexCube:    target = GL_TEXTURE_CUBE_MAP; break;
            case TextureType::Tex3D:      target = GL_TEXTURE_3D; break;
        }
        glCreateTextures(target, 1, &id);
        if (target == GL_TEXTURE_2D_MULTISAMPLE)
            glTextureStorage2DMultisample(id, d.samples, f.internal, d.width, d.height, GL_TRUE);
        else if (target == GL_TEXTURE_2D || target == GL_TEXTURE_CUBE_MAP)
            glTextureStorage2D(id, desc_.mipLevels, f.internal, d.width, d.height);
        else
            glTextureStorage3D(id, desc_.mipLevels, f.internal, d.width, d.height, d.depthOrLayers);

        if (target != GL_TEXTURE_2D_MULTISAMPLE) {
            bool depth = isDepthFormat(d.format);
            bool integer = d.format == Format::R32UI || d.format == Format::RG32UI || d.format == Format::RGBA32UI;
            GLenum minF = (depth || integer) ? GL_NEAREST : (desc_.mipLevels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
            glTextureParameteri(id, GL_TEXTURE_MIN_FILTER, minF);
            glTextureParameteri(id, GL_TEXTURE_MAG_FILTER, (depth || integer) ? GL_NEAREST : GL_LINEAR);
            glTextureParameteri(id, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTextureParameteri(id, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTextureParameteri(id, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        }
        if (!d.name.empty()) glObjectLabel(GL_TEXTURE, id, -1, d.name.c_str());

        size_t layers = (d.type == TextureType::Tex3D) ? 1 : d.depthOrLayers;
        for (uint32_t m = 0; m < desc_.mipLevels; ++m) {
            size_t s = surfaceBytes(d.format, width(m), height(m)) * layers;
            if (d.type == TextureType::Tex3D) s *= std::max(1u, d.depthOrLayers >> m);
            gpuBytes_ += s;
        }
        gpuBytes_ *= std::max(1u, d.samples);
        mem_.add(desc_.owner, gpuBytes_, true);
    }
    ~GLTexture() override { glDeleteTextures(1, &id); mem_.sub(desc_.owner, gpuBytes_, true); ++mem_.texturesDestroyed; }
    uintptr_t nativeHandle() const override { return id; }
    GLuint id = 0;
    GLenum target = GL_TEXTURE_2D;
private:
    MemTracker& mem_;
};

class GLSampler final : public Sampler {
public:
    GLSampler(const SamplerDesc& d, float maxAniso) {
        desc_ = d;
        glCreateSamplers(1, &id);
        GLenum minF;
        if (!d.useMips) minF = d.minFilter == Filter::Linear ? GL_LINEAR : GL_NEAREST;
        else if (d.minFilter == Filter::Linear) minF = d.mipFilter == Filter::Linear ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST;
        else minF = d.mipFilter == Filter::Linear ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST;
        glSamplerParameteri(id, GL_TEXTURE_MIN_FILTER, minF);
        glSamplerParameteri(id, GL_TEXTURE_MAG_FILTER, d.magFilter == Filter::Linear ? GL_LINEAR : GL_NEAREST);
        glSamplerParameteri(id, GL_TEXTURE_WRAP_S, glAddress(d.addressU));
        glSamplerParameteri(id, GL_TEXTURE_WRAP_T, glAddress(d.addressV));
        glSamplerParameteri(id, GL_TEXTURE_WRAP_R, glAddress(d.addressW));
        glSamplerParameterf(id, GL_TEXTURE_LOD_BIAS, d.mipLodBias);
        glSamplerParameterf(id, GL_TEXTURE_MIN_LOD, d.minLod);
        glSamplerParameterf(id, GL_TEXTURE_MAX_LOD, d.maxLod);
        glSamplerParameterfv(id, GL_TEXTURE_BORDER_COLOR, d.borderColor);
        if (maxAniso > 1.0f) glSamplerParameterf(id, GL_TEXTURE_MAX_ANISOTROPY, std::clamp(d.maxAnisotropy, 1.0f, maxAniso));
        if (d.compare) {
            glSamplerParameteri(id, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
            glSamplerParameteri(id, GL_TEXTURE_COMPARE_FUNC, glCompare(d.compareOp));
        }
    }
    ~GLSampler() override { glDeleteSamplers(1, &id); }
    GLuint id = 0;
};

class GLBuffer final : public Buffer {
public:
    GLBuffer(const BufferDesc& d, const void* data, MemTracker& mem) : mem_(mem) {
        desc_ = d;
        glCreateBuffers(1, &id);
        glNamedBufferStorage(id, std::max<size_t>(d.size, 16), data, GL_DYNAMIC_STORAGE_BIT);
        if (!d.name.empty()) glObjectLabel(GL_BUFFER, id, -1, d.name.c_str());
        mem_.add(desc_.owner, d.size, false);
    }
    ~GLBuffer() override { glDeleteBuffers(1, &id); mem_.sub(desc_.owner, desc_.size, false); }
    GLuint id = 0;
private:
    MemTracker& mem_;
};

class GLProgram final : public Program {
public:
    explicit GLProgram(const ProgramDesc& d) { desc_ = d; }
    ~GLProgram() override { if (id) glDeleteProgram(id); }
    GLuint id = 0;
    friend class GLDevice;
    void setResult(GLuint newId, bool ok, std::string log) {
        if (ok) {
            if (id) glDeleteProgram(id);
            id = newId; valid_ = true; ++generation_;
        }
        log_ = std::move(log);
    }
};

class GLPipeline final : public Pipeline {
public:
    explicit GLPipeline(const PipelineDesc& d) {
        desc_ = d;
        glCreateVertexArrays(1, &vao);
        for (auto& a : d.layout.attribs) {
            glEnableVertexArrayAttrib(vao, a.location);
            GLint comps = 1; GLenum type = GL_FLOAT; GLboolean norm = GL_FALSE; bool integer = false;
            switch (a.format) {
                case VertexFormat::Float: comps = 1; break;
                case VertexFormat::Float2: comps = 2; break;
                case VertexFormat::Float3: comps = 3; break;
                case VertexFormat::Float4: comps = 4; break;
                case VertexFormat::UByte4Norm: comps = 4; type = GL_UNSIGNED_BYTE; norm = GL_TRUE; break;
                case VertexFormat::UInt: comps = 1; type = GL_UNSIGNED_INT; integer = true; break;
            }
            if (integer) glVertexArrayAttribIFormat(vao, a.location, comps, type, a.offset);
            else glVertexArrayAttribFormat(vao, a.location, comps, type, norm, a.offset);
            glVertexArrayAttribBinding(vao, a.location, a.bufferSlot);
            if (a.perInstance) glVertexArrayBindingDivisor(vao, a.bufferSlot, 1);
        }
    }
    ~GLPipeline() override { glDeleteVertexArrays(1, &vao); }
    GLuint vao = 0;
};

class GLTimer final : public TimerQuery {
public:
    GLTimer() { glCreateQueries(GL_TIMESTAMP, 2, q); }
    ~GLTimer() override { glDeleteQueries(2, q); }
    bool resultReady() override {
        if (!issued) return false;
        GLint avail = 0;
        glGetQueryObjectiv(q[1], GL_QUERY_RESULT_AVAILABLE, &avail);
        return avail != 0;
    }
    double resultMs() override {
        GLuint64 a = 0, b = 0;
        glGetQueryObjectui64v(q[0], GL_QUERY_RESULT, &a);
        glGetQueryObjectui64v(q[1], GL_QUERY_RESULT, &b);
        issued = false;
        return double(b - a) * 1e-6;
    }
    GLuint q[2] = {0, 0};
    bool issued = false;
};

// ---------------------------------------------------------------------------------------
class GLCommandList final : public CommandList {
public:
    explicit GLCommandList(class GLDevice& d) : dev_(d) {}
    ~GLCommandList() override {
        for (auto& [k, fbo] : fboCache_) glDeleteFramebuffers(1, &fbo);
    }

    void beginRenderPass(const RenderPassDesc& pass) override;
    void endRenderPass() override { inPass_ = false; }
    void setViewport(float x, float y, float w, float h) override { glViewport(GLint(x), GLint(y), GLsizei(w), GLsizei(h)); }
    void setScissor(int x, int y, int w, int h) override { glScissor(x, y, w, h); }

    void bindPipeline(Pipeline* p) override;
    void bindComputeProgram(Program* p) override { glUseProgram(static_cast<GLProgram*>(p)->id); }

    void bindTexture(uint32_t slot, Texture* tex, Sampler* s) override {
        auto* t = static_cast<GLTexture*>(tex);
        glBindTextureUnit(slot, t ? t->id : 0);
        glBindSampler(slot, (s && t && t->target != GL_TEXTURE_2D_MULTISAMPLE) ? static_cast<GLSampler*>(s)->id : 0);
    }
    void bindImage(uint32_t slot, Texture* tex, uint32_t mip, Access access) override {
        auto* t = static_cast<GLTexture*>(tex);
        GLenum acc = access == Access::Read ? GL_READ_ONLY : access == Access::Write ? GL_WRITE_ONLY : GL_READ_WRITE;
        GLboolean layered = (t->target == GL_TEXTURE_2D || t->target == GL_TEXTURE_2D_MULTISAMPLE) ? GL_FALSE : GL_TRUE;
        glBindImageTexture(slot, t->id, mip, layered, 0, acc, glFormat(t->desc().format).internal);
    }
    void bindUniformBuffer(uint32_t slot, Buffer* b, size_t offset, size_t size) override {
        auto* gb = static_cast<GLBuffer*>(b);
        if (!size) size = b->size() - offset;
        glBindBufferRange(GL_UNIFORM_BUFFER, slot, gb->id, offset, size);
    }
    void bindStorageBuffer(uint32_t slot, Buffer* b, size_t offset, size_t size) override {
        auto* gb = static_cast<GLBuffer*>(b);
        if (!size) size = b->size() - offset;
        glBindBufferRange(GL_SHADER_STORAGE_BUFFER, slot, gb->id, offset, size);
    }
    void bindVertexBuffer(uint32_t slot, Buffer* b, size_t offset) override {
        if (!pipeline_) return;
        glVertexArrayVertexBuffer(pipeline_->vao, slot, static_cast<GLBuffer*>(b)->id, offset, pipeline_->desc().layout.strides[slot]);
    }
    void bindIndexBuffer(Buffer* b, IndexType type, size_t offset) override {
        indexType_ = type == IndexType::U16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
        indexOffset_ = offset;
        indexBuffer_ = static_cast<GLBuffer*>(b)->id;
        if (pipeline_) glVertexArrayElementBuffer(pipeline_->vao, indexBuffer_);
    }

    void draw(uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) override {
        glDrawArraysInstancedBaseInstance(topology_, fv, vc, ic, fi);
    }
    void drawIndexed(uint32_t count, uint32_t ic, uint32_t first, int32_t baseVertex, uint32_t fi) override {
        size_t isz = indexType_ == GL_UNSIGNED_SHORT ? 2 : 4;
        glDrawElementsInstancedBaseVertexBaseInstance(topology_, count, indexType_,
            reinterpret_cast<const void*>(indexOffset_ + first * isz), ic, baseVertex, fi);
    }
    void drawIndirect(Buffer* args, size_t offset, uint32_t count, uint32_t stride) override {
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, static_cast<GLBuffer*>(args)->id);
        glMultiDrawArraysIndirect(topology_, reinterpret_cast<const void*>(offset), count, stride);
    }
    void drawIndexedIndirect(Buffer* args, size_t offset, uint32_t count, uint32_t stride) override {
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, static_cast<GLBuffer*>(args)->id);
        glMultiDrawElementsIndirect(topology_, indexType_, reinterpret_cast<const void*>(offset), count, stride);
    }
    void dispatch(uint32_t x, uint32_t y, uint32_t z) override { glDispatchCompute(x, y, z); }
    void dispatchIndirect(Buffer* args, size_t offset) override {
        glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, static_cast<GLBuffer*>(args)->id);
        glDispatchComputeIndirect(GLintptr(offset));
    }
    void barrier(uint32_t bits) override {
        GLbitfield b = 0;
        if (bits == BarrierAll) b = GL_ALL_BARRIER_BITS;
        else {
            if (bits & BarrierTextureFetch) b |= GL_TEXTURE_FETCH_BARRIER_BIT;
            if (bits & BarrierImage) b |= GL_SHADER_IMAGE_ACCESS_BARRIER_BIT;
            if (bits & BarrierStorage) b |= GL_SHADER_STORAGE_BARRIER_BIT;
            if (bits & BarrierIndirect) b |= GL_COMMAND_BARRIER_BIT;
            if (bits & BarrierVertex) b |= GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT | GL_ELEMENT_ARRAY_BARRIER_BIT;
            if (bits & BarrierFramebuffer) b |= GL_FRAMEBUFFER_BARRIER_BIT;
        }
        glMemoryBarrier(b);
    }

    void updateBuffer(Buffer* b, const void* data, size_t size, size_t offset) override {
        glNamedBufferSubData(static_cast<GLBuffer*>(b)->id, offset, size, data);
    }
    void copyTexture(Texture* src, Texture* dst) override {
        auto* s = static_cast<GLTexture*>(src); auto* d = static_cast<GLTexture*>(dst);
        glCopyImageSubData(s->id, s->target, 0, 0, 0, 0, d->id, d->target, 0, 0, 0, 0,
                           src->width(), src->height(), 1);
    }
    void resolveTexture(Texture* src, Texture* dst) override { blit(src, dst, Filter::Nearest); }
    void blitTexture(Texture* src, Texture* dst, Filter f) override { blit(src, dst, f); }
    void generateMips(Texture* tex) override { glGenerateTextureMipmap(static_cast<GLTexture*>(tex)->id); }
    void clearTexture(Texture* tex, const float rgba[4], uint32_t mip) override {
        auto* t = static_cast<GLTexture*>(tex);
        if (isDepthFormat(t->desc().format)) glClearTexImage(t->id, mip, GL_DEPTH_COMPONENT, GL_FLOAT, rgba);
        else glClearTexImage(t->id, mip, GL_RGBA, GL_FLOAT, rgba);
    }

    void beginTimer(TimerQuery* q) override { glQueryCounter(static_cast<GLTimer*>(q)->q[0], GL_TIMESTAMP); }
    void endTimer(TimerQuery* q) override {
        auto* t = static_cast<GLTimer*>(q);
        glQueryCounter(t->q[1], GL_TIMESTAMP);
        t->issued = true;
    }
    void pushDebugGroup(const char* name) override { if (debugGroups) glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, name); }
    void popDebugGroup() override { if (debugGroups) glPopDebugGroup(); }

    GLuint fboFor(const RenderPassDesc& pass);
    void blit(Texture* src, Texture* dst, Filter f);
    void invalidateState() { pipeline_ = nullptr; }
    bool debugGroups = false;

private:
    GLDevice& dev_;
    GLPipeline* pipeline_ = nullptr;
    GLenum topology_ = GL_TRIANGLES;
    GLenum indexType_ = GL_UNSIGNED_INT;
    size_t indexOffset_ = 0;
    GLuint indexBuffer_ = 0;
    bool inPass_ = false;
    bool readOnlyDepth_ = false;
    uint64_t seenDestroyed_ = 0;
    std::unordered_map<std::string, GLuint> fboCache_;
};

// ---------------------------------------------------------------------------------------
class GLDevice final : public Device {
public:
    explicit GLDevice(GLFWwindow* w) : window_(w), cmd_(*this) {
        caps_.backend = Backend::OpenGL;
        caps_.apiName = "OpenGL";
        caps_.vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
        caps_.renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        caps_.version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        GLint v = 0;
        glGetIntegerv(GL_MAX_SAMPLES, &v); caps_.maxSamples = uint32_t(v);
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &v); caps_.maxTextureSize = uint32_t(v);
        if (GLAD_GL_EXT_texture_filter_anisotropic || GLAD_GL_ARB_texture_filter_anisotropic) {
            float a = 1.0f; glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &a); caps_.maxAnisotropy = a;
        }
        GLint bits = 0;
        glGetQueryiv(GL_TIMESTAMP, GL_QUERY_COUNTER_BITS, &bits);
        caps_.timestampQueries = bits > 0;
        caps_.tessellation = true;
        caps_.computeShaders = true;
        caps_.hdrOutput = false;   // GL default framebuffers are 8-bit SDR on Windows/Linux
        hasNVX_ = glfwExtensionSupported("GL_NVX_gpu_memory_info");
        hasATI_ = glfwExtensionSupported("GL_ATI_meminfo");
        if (hasNVX_) { GLint kb = 0; glGetIntegerv(GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX, &kb); caps_.dedicatedVideoMemory = size_t(kb) * 1024; }

        if (GLAD_GL_KHR_debug || GLAD_GL_VERSION_4_3) {
            glEnable(GL_DEBUG_OUTPUT);
            glDebugMessageCallback(&GLDevice::debugCallback, nullptr);
            glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_NOTIFICATION, 0, nullptr, GL_FALSE);
            cmd_.debugGroups = true;
        }
        // D3D conventions
        glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);
        glEnable(GL_FRAMEBUFFER_SRGB);
        glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
        glEnable(GL_PROGRAM_POINT_SIZE);
        glClearDepth(0.0);
        glDepthFunc(GL_GREATER);
        glCreateVertexArrays(1, &emptyVao_);
        glCreateFramebuffers(1, &readFbo_);
        glCreateFramebuffers(1, &drawFbo_);
    }
    ~GLDevice() override {
        for (GLsync s : fences_) glDeleteSync(s);
        glDeleteVertexArrays(1, &emptyVao_);
        glDeleteFramebuffers(1, &readFbo_);
        glDeleteFramebuffers(1, &drawFbo_);
    }

    static void APIENTRY debugCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei, const GLchar* msg, const void*) {
        if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
        if (type == GL_DEBUG_TYPE_PERFORMANCE || type == GL_DEBUG_TYPE_OTHER) return;
        static int count = 0;
        if (++count > 200) return;
        std::fprintf(stderr, "[GL] %s\n", msg);
    }

    const Caps& caps() const override { return caps_; }

    TexturePtr createTexture(const TextureDesc& d, const void* data) override {
        auto t = std::make_unique<GLTexture>(d, mem_);
        if (data) uploadTexture(t.get(), 0, 0, data, 0);
        return t;
    }
    void uploadTexture(Texture* tex, uint32_t mip, uint32_t layer, const void* data, size_t dataSize) override {
        auto* t = static_cast<GLTexture*>(tex);
        const auto& d = t->desc();
        GLFormat f = glFormat(d.format);
        uint32_t w = t->width(mip), h = t->height(mip);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (isCompressedFormat(d.format)) {
            size_t sz = dataSize ? dataSize : surfaceBytes(d.format, w, h);
            if (t->target == GL_TEXTURE_2D) glCompressedTextureSubImage2D(t->id, mip, 0, 0, w, h, f.internal, GLsizei(sz), data);
            else glCompressedTextureSubImage3D(t->id, mip, 0, 0, layer, w, h, 1, f.internal, GLsizei(sz), data);
        } else if (t->target == GL_TEXTURE_2D) {
            glTextureSubImage2D(t->id, mip, 0, 0, w, h, f.format, f.type, data);
        } else if (t->target == GL_TEXTURE_3D) {
            glTextureSubImage3D(t->id, mip, 0, 0, 0, w, h, std::max(1u, d.depthOrLayers >> mip), f.format, f.type, data);
        } else {
            glTextureSubImage3D(t->id, mip, 0, 0, layer, w, h, 1, f.format, f.type, data);
        }
    }
    SamplerPtr createSampler(const SamplerDesc& d) override { return std::make_unique<GLSampler>(d, caps_.maxAnisotropy); }
    BufferPtr createBuffer(const BufferDesc& d, const void* data) override { return std::make_unique<GLBuffer>(d, data, mem_); }
    ProgramPtr createProgram(const ProgramDesc& d) override { return std::make_unique<GLProgram>(d); }

    void setProgramCacheDir(const std::string& dir) override {
        cacheDir_ = dir;
        if (!dir.empty()) { std::error_code ec; std::filesystem::create_directories(dir, ec); }
    }
    ShaderCacheStats shaderCacheStats() const override { return cacheStats_; }

    bool rebuildProgram(Program* program, const ProgramSources& src) override {
        auto t0 = std::chrono::high_resolution_clock::now();
        bool ok = rebuildProgramImpl(program, src);
        cacheStats_.compileMs += std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
        return ok;
    }

    bool rebuildProgramImpl(Program* program, const ProgramSources& src) {
        auto* p = static_cast<GLProgram*>(program);
        std::string log;
        // ---- shader cache: program binary keyed by sources + driver ----
        std::string cacheFile;
        if (!cacheDir_.empty()) {
            size_t h = std::hash<std::string>{}(caps_.renderer + caps_.version + "|" + src.vs + "|" + src.tcs + "|" + src.tes + "|" + src.gs + "|" + src.fs + "|" + src.cs);
            char name[32]; std::snprintf(name, sizeof(name), "%016zx.bin", h);
            cacheFile = cacheDir_ + "/" + name;
            std::ifstream in(cacheFile, std::ios::binary);
            if (in) {
                GLenum fmt = 0;
                in.read(reinterpret_cast<char*>(&fmt), sizeof(fmt));
                std::vector<char> bin((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                GLuint prog = glCreateProgram();
                glProgramBinary(prog, fmt, bin.data(), GLsizei(bin.size()));
                GLint status = 0; glGetProgramiv(prog, GL_LINK_STATUS, &status);
                if (status) { p->setResult(prog, true, ""); ++cacheStats_.hits; return true; }
                glDeleteProgram(prog);   // driver changed: fall through to compile
            }
            ++cacheStats_.misses;
        }
        GLuint prog = glCreateProgram();
        if (!cacheFile.empty()) glProgramParameteri(prog, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
        std::vector<GLuint> shaders;
        bool ok = true;
        auto stage = [&](const std::string& code, GLenum type, const char* label) {
            if (code.empty() || !ok) return;
            GLuint s = glCreateShader(type);
            const char* c = code.c_str();
            glShaderSource(s, 1, &c, nullptr);
            glCompileShader(s);
            GLint status = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &status);
            if (!status) {
                char buf[8192]; glGetShaderInfoLog(s, sizeof(buf), nullptr, buf);
                log += std::string(label) + ": " + buf;
                ok = false;
            }
            glAttachShader(prog, s);
            shaders.push_back(s);
        };
        stage(src.vs, GL_VERTEX_SHADER, p->desc().vs.c_str());
        stage(src.tcs, GL_TESS_CONTROL_SHADER, p->desc().tcs.c_str());
        stage(src.tes, GL_TESS_EVALUATION_SHADER, p->desc().tes.c_str());
        stage(src.gs, GL_GEOMETRY_SHADER, p->desc().gs.c_str());
        stage(src.fs, GL_FRAGMENT_SHADER, p->desc().fs.c_str());
        stage(src.cs, GL_COMPUTE_SHADER, p->desc().cs.c_str());
        if (ok) {
            glLinkProgram(prog);
            GLint status = 0; glGetProgramiv(prog, GL_LINK_STATUS, &status);
            if (!status) {
                char buf[8192]; glGetProgramInfoLog(prog, sizeof(buf), nullptr, buf);
                log += std::string("link: ") + buf;
                ok = false;
            }
        }
        for (GLuint s : shaders) { glDetachShader(prog, s); glDeleteShader(s); }
        if (!ok) glDeleteProgram(prog);
        else if (!p->desc().name.empty()) glObjectLabel(GL_PROGRAM, prog, -1, p->desc().name.c_str());
        if (ok && !cacheFile.empty()) {
            GLint len = 0; glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &len);
            if (len > 0) {
                std::vector<char> bin(static_cast<size_t>(len)); GLenum fmt = 0;
                glGetProgramBinary(prog, len, nullptr, &fmt, bin.data());
                std::ofstream out(cacheFile, std::ios::binary);
                out.write(reinterpret_cast<const char*>(&fmt), sizeof(fmt));
                out.write(bin.data(), len);
                ++cacheStats_.stores;
            }
        }
        p->setResult(ok ? prog : 0, ok, log);
        return ok;
    }

    PipelinePtr createPipeline(const PipelineDesc& d) override { return std::make_unique<GLPipeline>(d); }
    TimerPtr createTimer() override { return std::make_unique<GLTimer>(); }
    CommandList& cmd() override { return cmd_; }

    void resizeSwapchain(uint32_t w, uint32_t h) override { bbW_ = w; bbH_ = h; }
    void setPresentMode(PresentMode m) override {
        int interval = 0;
        switch (m) {
            case PresentMode::Immediate: interval = 0; break;
            case PresentMode::VSync: interval = 1; break;
            case PresentMode::AdaptiveVSync:
                interval = (glfwExtensionSupported("WGL_EXT_swap_control_tear") || glfwExtensionSupported("GLX_EXT_swap_control_tear")) ? -1 : 1;
                break;
            case PresentMode::HalfRateVSync: interval = 2; break;
        }
        glfwSwapInterval(interval);
    }
    bool setOutputColorSpace(OutputColorSpace cs) override { return cs == OutputColorSpace::SDR_sRGB; }

    void copyToBackbuffer(Texture* finalImage) override {
        auto* t = static_cast<GLTexture*>(finalImage);
        glNamedFramebufferTexture(readFbo_, GL_COLOR_ATTACHMENT0, t->id, 0);
        glNamedFramebufferReadBuffer(readFbo_, GL_COLOR_ATTACHMENT0);
        glDisable(GL_FRAMEBUFFER_SRGB);
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        // Our images are stored top row first (D3D convention); the GL backbuffer is bottom-up: flip Y.
        glBlitNamedFramebuffer(readFbo_, 0, 0, 0, t->width(), t->height(), 0, GLint(bbH_), GLint(bbW_), 0,
                               GL_COLOR_BUFFER_BIT, (t->width() == bbW_ && t->height() == bbH_) ? GL_NEAREST : GL_LINEAR);
        glEnable(GL_FRAMEBUFFER_SRGB);
    }
    void beginUiOverlay() override {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, GLsizei(bbW_), GLsizei(bbH_));
        glDisable(GL_FRAMEBUFFER_SRGB);
        glClipControl(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);
        cmd_.invalidateState();
    }
    void endUiOverlay() override {
        glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);
        glEnable(GL_FRAMEBUFFER_SRGB);
        glDisable(GL_SCISSOR_TEST);
        cmd_.invalidateState();
    }
    void swapBuffers() override {
        glfwSwapBuffers(window_);
        fences_.push_back(glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0));
        while (fences_.size() > 8) { glDeleteSync(fences_.front()); fences_.pop_front(); }
    }
    void waitIdle() override { glFinish(); }
    void setMaxFrameLatency(uint32_t frames) override { maxLatency_ = std::max(1u, frames); }
    void waitForFrameLatency() override {
        // Block the CPU until at most `maxLatency_` frames are queued on the GPU.
        while (fences_.size() >= maxLatency_) {
            GLsync s = fences_[fences_.size() - maxLatency_];
            glClientWaitSync(s, GL_SYNC_FLUSH_COMMANDS_BIT, 100'000'000);   // 100 ms cap
            // Drop everything up to and including the waited fence.
            while (!fences_.empty() && fences_.front() != s) { glDeleteSync(fences_.front()); fences_.pop_front(); }
            if (!fences_.empty()) { glDeleteSync(fences_.front()); fences_.pop_front(); }
        }
    }

    MemoryStats memoryStats() const override {
        MemoryStats m;
        {
            std::lock_guard<std::mutex> l(const_cast<MemTracker&>(mem_).mtx);
            m.textureBytes = mem_.tex; m.bufferBytes = mem_.buf;
            for (auto& [k, v] : mem_.perOwner) if (v) m.perOwner.emplace_back(k, v);
        }
        if (hasNVX_) {
            GLint total = 0, avail = 0;
            glGetIntegerv(GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX, &total);
            glGetIntegerv(GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &avail);
            if (total > 0 && avail >= 0 && avail <= total) {
                m.driverBudgetBytes = size_t(total) * 1024;
                m.driverUsedBytes = size_t(total - avail) * 1024;
            }
        } else if (hasATI_) {
            GLint info[4] = {0, 0, 0, 0};
            glGetIntegerv(GL_TEXTURE_FREE_MEMORY_ATI, info);
            m.driverBudgetBytes = 0;
            m.driverUsedBytes = 0;
        }
        return m;
    }

    bool readBackbuffer(std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h) override {
        w = bbW_; h = bbH_;
        rgba.resize(size_t(w) * h * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadBuffer(GL_BACK);
        glReadPixels(0, 0, GLsizei(w), GLsizei(h), GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        // GL rows are bottom-up; flip to top-down.
        std::vector<uint8_t> row(size_t(w) * 4);
        for (uint32_t y = 0; y < h / 2; ++y) {
            uint8_t* a = rgba.data() + size_t(y) * w * 4; uint8_t* b = rgba.data() + size_t(h - 1 - y) * w * 4;
            std::memcpy(row.data(), a, row.size()); std::memcpy(a, b, row.size()); std::memcpy(b, row.data(), row.size());
        }
        return true;
    }

    uint64_t texturesDestroyed() const { return mem_.texturesDestroyed; }
    GLuint emptyVao_ = 0, readFbo_ = 0, drawFbo_ = 0;

private:
    GLFWwindow* window_;
    Caps caps_;
    GLCommandList cmd_;
    MemTracker mem_;
    std::deque<GLsync> fences_;
    uint32_t maxLatency_ = 2;
    uint32_t bbW_ = 1, bbH_ = 1;
    bool hasNVX_ = false, hasATI_ = false;
    std::string cacheDir_;
    ShaderCacheStats cacheStats_;
};

// ---------------------------------------------------------------------------------------
GLuint GLCommandList::fboFor(const RenderPassDesc& pass) {
    if (dev_.texturesDestroyed() != seenDestroyed_) {
        for (auto& [k, f] : fboCache_) glDeleteFramebuffers(1, &f);
        fboCache_.clear();
        seenDestroyed_ = dev_.texturesDestroyed();
    }
    std::string key;
    key.reserve(96);
    auto addKey = [&](Texture* t, uint32_t mip, uint32_t layer) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%u:%u:%u|", t ? static_cast<GLTexture*>(t)->id : 0u, mip, layer);
        key += buf;
    };
    for (uint32_t i = 0; i < pass.colorCount; ++i) addKey(pass.colors[i].texture, pass.colors[i].mip, pass.colors[i].layer);
    addKey(pass.depth.texture, pass.depth.mip, pass.depth.layer);
    auto it = fboCache_.find(key);
    if (it != fboCache_.end()) return it->second;

    GLuint fbo = 0;
    glCreateFramebuffers(1, &fbo);
    auto attach = [&](GLenum point, Texture* tex, uint32_t mip, uint32_t layer) {
        auto* t = static_cast<GLTexture*>(tex);
        if (t->target == GL_TEXTURE_2D || t->target == GL_TEXTURE_2D_MULTISAMPLE) glNamedFramebufferTexture(fbo, point, t->id, mip);
        else glNamedFramebufferTextureLayer(fbo, point, t->id, mip, layer);
    };
    GLenum bufs[8];
    for (uint32_t i = 0; i < pass.colorCount; ++i) {
        attach(GL_COLOR_ATTACHMENT0 + i, pass.colors[i].texture, pass.colors[i].mip, pass.colors[i].layer);
        bufs[i] = GL_COLOR_ATTACHMENT0 + i;
    }
    if (pass.colorCount) glNamedFramebufferDrawBuffers(fbo, GLsizei(pass.colorCount), bufs);
    else glNamedFramebufferDrawBuffer(fbo, GL_NONE);
    if (pass.depth.texture) {
        GLenum point = pass.depth.texture->desc().format == Format::D24S8 ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
        attach(point, pass.depth.texture, pass.depth.mip, pass.depth.layer);
    }
    GLenum status = glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) std::fprintf(stderr, "[GL] framebuffer incomplete for pass '%s' (0x%x)\n", pass.name, status);
    // Textures get recreated on resize; keep the cache bounded.
    if (fboCache_.size() > 256) {
        for (auto& [k, f] : fboCache_) glDeleteFramebuffers(1, &f);
        fboCache_.clear();
    }
    fboCache_[key] = fbo;
    return fbo;
}

void GLCommandList::beginRenderPass(const RenderPassDesc& pass) {
    GLuint fbo = fboFor(pass);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    inPass_ = true;
    Texture* sizeTex = pass.colorCount ? pass.colors[0].texture : pass.depth.texture;
    uint32_t mip = pass.colorCount ? pass.colors[0].mip : pass.depth.mip;
    if (sizeTex) glViewport(0, 0, sizeTex->width(mip), sizeTex->height(mip));
    glDisable(GL_SCISSOR_TEST);
    for (uint32_t i = 0; i < pass.colorCount; ++i) {
        if (pass.colors[i].load == LoadOp::Clear) {
            glColorMaski(i, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            Format f = pass.colors[i].texture->desc().format;
            if (f == Format::R32UI || f == Format::RG32UI || f == Format::RGBA32UI) {
                GLuint v[4] = {GLuint(pass.colors[i].clear[0]), GLuint(pass.colors[i].clear[1]), GLuint(pass.colors[i].clear[2]), GLuint(pass.colors[i].clear[3])};
                glClearNamedFramebufferuiv(fbo, GL_COLOR, GLint(i), v);
            } else {
                glClearNamedFramebufferfv(fbo, GL_COLOR, GLint(i), pass.colors[i].clear);
            }
        }
    }
    if (pass.depth.texture && pass.depth.load == LoadOp::Clear) {
        glDepthMask(GL_TRUE);
        if (pass.depth.texture->desc().format == Format::D24S8)
            glClearNamedFramebufferfi(fbo, GL_DEPTH_STENCIL, 0, pass.depth.clearDepth, pass.depth.clearStencil);
        else
            glClearNamedFramebufferfv(fbo, GL_DEPTH, 0, &pass.depth.clearDepth);
    }
    readOnlyDepth_ = pass.depth.readOnly;
    pipeline_ = nullptr;   // force state re-apply (depth write may differ)
}

static GLenum glTopology(Topology t) {
    switch (t) {
        case Topology::Triangles: return GL_TRIANGLES; case Topology::TriangleStrip: return GL_TRIANGLE_STRIP;
        case Topology::Lines: return GL_LINES; case Topology::Points: return GL_POINTS; default: return GL_PATCHES;
    }
}

void GLCommandList::bindPipeline(Pipeline* p) {
    auto* gp = static_cast<GLPipeline*>(p);
    const auto& d = gp->desc();
    pipeline_ = gp;
    glUseProgram(d.program ? static_cast<GLProgram*>(d.program)->id : 0);
    glBindVertexArray(d.layout.attribs.empty() ? dev_.emptyVao_ : gp->vao);
    if (!d.layout.attribs.empty() && indexBuffer_) glVertexArrayElementBuffer(gp->vao, indexBuffer_);
    topology_ = glTopology(d.topology);
    if (d.topology == Topology::Patches) glPatchParameteri(GL_PATCH_VERTICES, GLint(d.patchControlPoints));

    // Raster
    if (d.raster.cull == CullMode::None) glDisable(GL_CULL_FACE);
    else { glEnable(GL_CULL_FACE); glCullFace(d.raster.cull == CullMode::Back ? GL_BACK : GL_FRONT); }
    // GL already reverses facing for GL_UPPER_LEFT clip control, so CCW stays CCW (same as D3D11 FrontCounterClockwise).
    glFrontFace(d.raster.frontCCW ? GL_CCW : GL_CW);
    glPolygonMode(GL_FRONT_AND_BACK, d.raster.fill == FillMode::Wireframe ? GL_LINE : GL_FILL);
    if (d.raster.depthBiasConstant != 0.0f || d.raster.depthBiasSlope != 0.0f) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(d.raster.depthBiasSlope, d.raster.depthBiasConstant);
    } else glDisable(GL_POLYGON_OFFSET_FILL);
    if (d.raster.depthClamp) glEnable(GL_DEPTH_CLAMP); else glDisable(GL_DEPTH_CLAMP);
    if (d.raster.scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);

    // Depth
    if (d.depth.test) { glEnable(GL_DEPTH_TEST); glDepthFunc(glCompare(d.depth.compare)); }
    else glDisable(GL_DEPTH_TEST);
    glDepthMask((d.depth.write && !readOnlyDepth_) ? GL_TRUE : GL_FALSE);

    // Blend
    for (GLuint i = 0; i < 8; ++i) {
        const auto& b = d.blend[i];
        glColorMaski(i, (b.writeMask & 1) != 0, (b.writeMask & 2) != 0, (b.writeMask & 4) != 0, (b.writeMask & 8) != 0);
        switch (b.mode) {
            case BlendMode::Opaque: glDisablei(GL_BLEND, i); break;
            case BlendMode::Alpha: glEnablei(GL_BLEND, i); glBlendEquationi(i, GL_FUNC_ADD);
                glBlendFuncSeparatei(i, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
            case BlendMode::Additive: glEnablei(GL_BLEND, i); glBlendEquationi(i, GL_FUNC_ADD); glBlendFunci(i, GL_ONE, GL_ONE); break;
            case BlendMode::Premultiplied: glEnablei(GL_BLEND, i); glBlendEquationi(i, GL_FUNC_ADD); glBlendFunci(i, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
            case BlendMode::Multiply: glEnablei(GL_BLEND, i); glBlendEquationi(i, GL_FUNC_ADD); glBlendFunci(i, GL_DST_COLOR, GL_ZERO); break;
        }
    }
}

void GLCommandList::blit(Texture* src, Texture* dst, Filter f) {
    auto* s = static_cast<GLTexture*>(src); auto* d = static_cast<GLTexture*>(dst);
    bool depth = isDepthFormat(s->desc().format);
    GLenum point = depth ? GL_DEPTH_ATTACHMENT : GL_COLOR_ATTACHMENT0;
    glNamedFramebufferTexture(dev_.readFbo_, point, s->id, 0);
    glNamedFramebufferTexture(dev_.drawFbo_, point, d->id, 0);
    if (!depth) { glNamedFramebufferReadBuffer(dev_.readFbo_, GL_COLOR_ATTACHMENT0); glNamedFramebufferDrawBuffer(dev_.drawFbo_, GL_COLOR_ATTACHMENT0); }
    glDisable(GL_SCISSOR_TEST);
    if (depth) glDepthMask(GL_TRUE); else glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBlitNamedFramebuffer(dev_.readFbo_, dev_.drawFbo_, 0, 0, src->width(), src->height(), 0, 0, dst->width(), dst->height(),
                           depth ? GL_DEPTH_BUFFER_BIT : GL_COLOR_BUFFER_BIT,
                           (depth || f == Filter::Nearest) ? GL_NEAREST : GL_LINEAR);
    glNamedFramebufferTexture(dev_.readFbo_, point, 0, 0);
    glNamedFramebufferTexture(dev_.drawFbo_, point, 0, 0);
    pipeline_ = nullptr;
}

}  // namespace

std::unique_ptr<Device> createGLDevice(GLFWwindow* window) {
    return std::make_unique<GLDevice>(window);
}

}  // namespace gfx::rhi
