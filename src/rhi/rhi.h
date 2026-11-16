// gfx::rhi — thin rendering hardware interface.
//
// Modules talk to the GPU ONLY through this header (no raw gl* / ID3D11* calls in
// src/modules/**), so the same module runs on the OpenGL 4.5 and Direct3D 11 backends.
//
// Conventions (identical on every backend):
//   * Clip space is D3D style: NDC z in [0,1], uv (0,0) = top-left of a texture/target.
//     (GL backend enforces this with glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE).)
//   * Depth is REVERSED-Z: near = 1, far = 0, clear depth to 0, compare GREATER.
//   * Shaders are GLSL 450 with explicit layout(binding = N). D3D11 cross-compiles them.
//   * Every resource takes an `owner` tag (module name) so the profiler can attribute VRAM.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gfx::rhi {

// ---------------------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------------------
enum class Backend : uint8_t { OpenGL, D3D11 };

enum class Format : uint8_t {
    Unknown,
    R8, RG8, RGBA8, RGBA8_SRGB, BGRA8_SRGB,
    R16F, RG16F, RGBA16F,
    R32F, RG32F, RGBA32F,
    R11G11B10F, RGB10A2,
    R32UI, RG32UI, RGBA32UI,
    D32F, D24S8,
    BC1_SRGB, BC3_SRGB, BC4, BC5, BC7_SRGB,
};
bool     isDepthFormat(Format f);
bool     isCompressedFormat(Format f);
uint32_t formatBytesPerPixel(Format f);   // block formats: bytes per 4x4 block / 16

enum class TextureType : uint8_t { Tex2D, Tex2DArray, TexCube, Tex3D };

enum TextureUsage : uint32_t {
    TexUsageSampled      = 1u << 0,
    TexUsageRenderTarget = 1u << 1,
    TexUsageDepthStencil = 1u << 2,
    TexUsageStorage      = 1u << 3,   // image load/store (UAV)
};

enum class Filter : uint8_t { Nearest, Linear };
enum class AddressMode : uint8_t { Repeat, Clamp, Mirror, Border };
enum class CompareOp : uint8_t { Never, Less, LessEqual, Equal, GreaterEqual, Greater, NotEqual, Always };

enum BufferUsage : uint32_t {
    BufUsageVertex   = 1u << 0,
    BufUsageIndex    = 1u << 1,
    BufUsageUniform  = 1u << 2,
    BufUsageStorage  = 1u << 3,   // SSBO / structured buffer UAV
    BufUsageIndirect = 1u << 4,
};

enum class IndexType : uint8_t { U16, U32 };
enum class Topology : uint8_t { Triangles, TriangleStrip, Lines, Points, Patches };
enum class CullMode : uint8_t { None, Back, Front };
enum class FillMode : uint8_t { Solid, Wireframe };
enum class BlendMode : uint8_t { Opaque, Alpha, Additive, Premultiplied, Multiply };
enum class LoadOp : uint8_t { Load, Clear, DontCare };
enum class Access : uint8_t { Read, Write, ReadWrite };

enum class VertexFormat : uint8_t { Float, Float2, Float3, Float4, UByte4Norm, UInt };

enum BarrierBits : uint32_t {
    BarrierTextureFetch = 1u << 0,   // after image stores, before sampling
    BarrierImage        = 1u << 1,   // image store -> image load
    BarrierStorage      = 1u << 2,   // SSBO writes -> reads
    BarrierIndirect     = 1u << 3,   // buffer writes -> indirect args
    BarrierVertex       = 1u << 4,   // buffer writes -> vertex/index fetch
    BarrierFramebuffer  = 1u << 5,
    BarrierAll          = 0xFFFFFFFFu,
};

// ---------------------------------------------------------------------------------------
// Descriptors
// ---------------------------------------------------------------------------------------
struct TextureDesc {
    TextureType type = TextureType::Tex2D;
    Format      format = Format::RGBA8;
    uint32_t    width = 1, height = 1;
    uint32_t    depthOrLayers = 1;      // layers for arrays (cube: 6), depth for 3D
    uint32_t    mipLevels = 1;          // 0 = full chain
    uint32_t    samples = 1;            // MSAA (Tex2D only)
    uint32_t    usage = TexUsageSampled;
    std::string name;                   // debug label
    std::string owner = "core";         // VRAM attribution tag
};

struct SamplerDesc {
    Filter      minFilter = Filter::Linear;
    Filter      magFilter = Filter::Linear;
    Filter      mipFilter = Filter::Linear;
    bool        useMips = true;
    AddressMode addressU = AddressMode::Repeat, addressV = AddressMode::Repeat, addressW = AddressMode::Repeat;
    float       maxAnisotropy = 1.0f;   // 1 = off, up to Device::caps().maxAnisotropy
    float       mipLodBias = 0.0f;      // negative when upscaling: log2(renderW/outputW)
    float       minLod = 0.0f, maxLod = 1000.0f;
    bool        compare = false;        // shadow sampler (sampler2DShadow)
    CompareOp   compareOp = CompareOp::Greater;   // reversed-Z friendly default
    float       borderColor[4] = {0, 0, 0, 0};
};

struct BufferDesc {
    size_t      size = 0;
    uint32_t    usage = BufUsageUniform;
    bool        dynamic = true;         // CPU updates every frame
    uint32_t    structStride = 0;       // D3D11 structured buffers (SSBO), 0 = raw
    std::string name;
    std::string owner = "core";
};

struct VertexAttrib {
    uint32_t     location;
    VertexFormat format;
    uint32_t     offset;
    uint32_t     bufferSlot = 0;
    bool         perInstance = false;
};
struct VertexLayout {
    std::vector<VertexAttrib> attribs;
    uint32_t strides[4] = {0, 0, 0, 0};
};

// Shader program: file paths are relative to the shaders/ folder, e.g. "lighting/ssao.frag".
// `defines` are injected after #version as "#define NAME VALUE" ("NAME" or "NAME=VALUE").
// #include "file" is supported (paths relative to shaders/).
struct ProgramDesc {
    std::string vs, tcs, tes, gs, fs, cs;   // tcs = hull, tes = domain
    std::vector<std::string> defines;
    std::string name;
};

struct RasterState {
    CullMode cull = CullMode::Back;
    FillMode fill = FillMode::Solid;
    bool     frontCCW = true;
    float    depthBiasConstant = 0.0f;   // shadow maps
    float    depthBiasSlope = 0.0f;
    bool     depthClamp = false;         // pancaking for shadow maps
    bool     scissor = false;
    bool     conservative = false;       // ignored where unsupported
};
struct DepthState {
    bool      test = true;
    bool      write = true;
    CompareOp compare = CompareOp::Greater;  // reversed-Z
};
struct BlendState {
    BlendMode mode = BlendMode::Opaque;
    uint8_t   writeMask = 0xF;               // RGBA bits
};

class Program;
struct PipelineDesc {
    Program*     program = nullptr;    // owned by ShaderLibrary; survives hot reload
    VertexLayout layout;               // empty = no vertex buffers (fullscreen / vertex pulling)
    Topology     topology = Topology::Triangles;
    uint32_t     patchControlPoints = 3;
    RasterState  raster;
    DepthState   depth;
    BlendState   blend[8];             // per render target
    std::string  name;
};

class Texture;
struct ColorAttachment {
    Texture* texture = nullptr;
    uint32_t mip = 0;
    uint32_t layer = 0;                // array layer / cube face / 3D slice
    LoadOp   load = LoadOp::Load;
    float    clear[4] = {0, 0, 0, 0};
};
struct DepthAttachment {
    Texture* texture = nullptr;
    uint32_t mip = 0;
    uint32_t layer = 0;
    LoadOp   load = LoadOp::Load;
    float    clearDepth = 0.0f;        // reversed-Z: 0 = far
    uint8_t  clearStencil = 0;
    bool     readOnly = false;         // depth test without write while sampling elsewhere
};
struct RenderPassDesc {
    ColorAttachment colors[8];
    uint32_t        colorCount = 0;
    DepthAttachment depth;
    const char*     name = "pass";
};

// ---------------------------------------------------------------------------------------
// Resources (opaque; created by Device, owned by the caller through unique_ptr)
// ---------------------------------------------------------------------------------------
class Texture {
public:
    virtual ~Texture() = default;
    const TextureDesc& desc() const { return desc_; }
    uint32_t width(uint32_t mip = 0) const  { uint32_t w = desc_.width >> mip;  return w ? w : 1; }
    uint32_t height(uint32_t mip = 0) const { uint32_t h = desc_.height >> mip; return h ? h : 1; }
    size_t   gpuBytes() const { return gpuBytes_; }
    // Backend-native handle for interop/debug ONLY (GLuint or ID3D11ShaderResourceView*).
    virtual uintptr_t nativeHandle() const = 0;
protected:
    TextureDesc desc_;
    size_t gpuBytes_ = 0;
};

class Sampler {
public:
    virtual ~Sampler() = default;
    const SamplerDesc& desc() const { return desc_; }
protected:
    SamplerDesc desc_;
};

class Buffer {
public:
    virtual ~Buffer() = default;
    const BufferDesc& desc() const { return desc_; }
    size_t size() const { return desc_.size; }
protected:
    BufferDesc desc_;
};

class Program {
public:
    virtual ~Program() = default;
    const ProgramDesc& desc() const { return desc_; }
    bool valid() const { return valid_; }
    const std::string& log() const { return log_; }
    uint32_t generation() const { return generation_; }   // bumps on every successful reload
    bool isCompute() const { return !desc_.cs.empty(); }
protected:
    ProgramDesc desc_;
    bool valid_ = false;
    std::string log_;
    uint32_t generation_ = 0;
};

class Pipeline {
public:
    virtual ~Pipeline() = default;
    const PipelineDesc& desc() const { return desc_; }
protected:
    PipelineDesc desc_;
};

class TimerQuery {   // GPU timestamp pair; result is available a few frames later
public:
    virtual ~TimerQuery() = default;
    virtual bool resultReady() = 0;
    virtual double resultMs() = 0;
};

using TexturePtr  = std::unique_ptr<Texture>;
using SamplerPtr  = std::unique_ptr<Sampler>;
using BufferPtr   = std::unique_ptr<Buffer>;
using ProgramPtr  = std::unique_ptr<Program>;
using PipelinePtr = std::unique_ptr<Pipeline>;
using TimerPtr    = std::unique_ptr<TimerQuery>;

// ---------------------------------------------------------------------------------------
// Command list (immediate context; one per device)
// ---------------------------------------------------------------------------------------
class CommandList {
public:
    virtual ~CommandList() = default;

    virtual void beginRenderPass(const RenderPassDesc& pass) = 0;   // also sets viewport to attachment size
    virtual void endRenderPass() = 0;
    virtual void setViewport(float x, float y, float w, float h) = 0;
    virtual void setScissor(int x, int y, int w, int h) = 0;

    virtual void bindPipeline(Pipeline* pipeline) = 0;
    virtual void bindComputeProgram(Program* program) = 0;

    virtual void bindTexture(uint32_t slot, Texture* tex, Sampler* sampler = nullptr) = 0;
    virtual void bindImage(uint32_t slot, Texture* tex, uint32_t mip = 0, Access access = Access::Write) = 0;
    virtual void bindUniformBuffer(uint32_t slot, Buffer* buf, size_t offset = 0, size_t size = 0) = 0;
    virtual void bindStorageBuffer(uint32_t slot, Buffer* buf, size_t offset = 0, size_t size = 0) = 0;
    virtual void bindVertexBuffer(uint32_t slot, Buffer* buf, size_t offset = 0) = 0;
    virtual void bindIndexBuffer(Buffer* buf, IndexType type, size_t offset = 0) = 0;

    virtual void draw(uint32_t vertexCount, uint32_t instanceCount = 1, uint32_t firstVertex = 0, uint32_t firstInstance = 0) = 0;
    virtual void drawIndexed(uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0, int32_t baseVertex = 0, uint32_t firstInstance = 0) = 0;
    virtual void drawIndirect(Buffer* args, size_t offset, uint32_t drawCount, uint32_t stride) = 0;
    virtual void drawIndexedIndirect(Buffer* args, size_t offset, uint32_t drawCount, uint32_t stride) = 0;
    virtual void dispatch(uint32_t x, uint32_t y = 1, uint32_t z = 1) = 0;
    virtual void dispatchIndirect(Buffer* args, size_t offset) = 0;
    virtual void barrier(uint32_t bits = BarrierAll) = 0;

    virtual void updateBuffer(Buffer* buf, const void* data, size_t size, size_t offset = 0) = 0;
    virtual void copyTexture(Texture* src, Texture* dst) = 0;           // same size/format
    virtual void resolveTexture(Texture* msaaSrc, Texture* dst) = 0;    // MSAA -> single sample
    virtual void blitTexture(Texture* src, Texture* dst, Filter filter) = 0;  // scaled copy (color)
    virtual void generateMips(Texture* tex) = 0;
    virtual void clearTexture(Texture* tex, const float rgba[4], uint32_t mip = 0) = 0;

    virtual void beginTimer(TimerQuery* q) = 0;
    virtual void endTimer(TimerQuery* q) = 0;
    virtual void pushDebugGroup(const char* name) = 0;
    virtual void popDebugGroup() = 0;

    template <class T> void updateBuffer(Buffer* buf, const T& value) { updateBuffer(buf, &value, sizeof(T), 0); }
};

// ---------------------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------------------
struct Caps {
    Backend     backend = Backend::OpenGL;
    std::string apiName, vendor, renderer, version;
    float       maxAnisotropy = 1.0f;
    uint32_t    maxSamples = 1;
    uint32_t    maxTextureSize = 4096;
    bool        timestampQueries = false;
    bool        tessellation = true;
    bool        computeShaders = true;
    bool        conservativeRaster = false;
    bool        hdrOutput = false;          // swapchain can present HDR10/scRGB
    size_t      dedicatedVideoMemory = 0;   // bytes, 0 if unknown
};

struct MemoryStats {
    size_t textureBytes = 0, bufferBytes = 0;
    size_t driverUsedBytes = 0;     // from driver extension if available (NVX/ATI/DXGI), else 0
    size_t driverBudgetBytes = 0;
    std::vector<std::pair<std::string, size_t>> perOwner;   // owner tag -> bytes
};

// Preprocessed sources (after #include and #define injection), one per stage.
struct ProgramSources {
    std::string vs, tcs, tes, gs, fs, cs;
};

enum class PresentMode : uint8_t { Immediate, VSync, AdaptiveVSync, HalfRateVSync };
enum class OutputColorSpace : uint8_t { SDR_sRGB, HDR10_PQ, scRGB_Linear };

class Device {
public:
    virtual ~Device() = default;
    virtual const Caps& caps() const = 0;

    virtual TexturePtr  createTexture(const TextureDesc& desc, const void* mip0Data = nullptr) = 0;
    virtual void        uploadTexture(Texture* tex, uint32_t mip, uint32_t layer, const void* data, size_t dataSize) = 0;
    virtual SamplerPtr  createSampler(const SamplerDesc& desc) = 0;
    virtual BufferPtr   createBuffer(const BufferDesc& desc, const void* initialData = nullptr) = 0;
    virtual ProgramPtr  createProgram(const ProgramDesc& desc) = 0;   // use ShaderLibrary instead
    // Recompiles in place from preprocessed sources; on failure keeps the old binary.
    virtual bool        rebuildProgram(Program* program, const ProgramSources& src) = 0;
    // On-disk program binary cache (shader cache). Empty dir = disabled.
    virtual void        setProgramCacheDir(const std::string& dir) { (void)dir; }
    struct ShaderCacheStats { uint32_t hits = 0, misses = 0, stores = 0; double compileMs = 0.0; };
    virtual ShaderCacheStats shaderCacheStats() const { return {}; }
    virtual PipelinePtr createPipeline(const PipelineDesc& desc) = 0;
    virtual TimerPtr    createTimer() = 0;
    virtual CommandList& cmd() = 0;

    // Swapchain
    virtual void   resizeSwapchain(uint32_t w, uint32_t h) = 0;
    virtual void   setPresentMode(PresentMode mode) = 0;
    virtual bool   setOutputColorSpace(OutputColorSpace cs) = 0;   // false = unsupported, stays SDR
    virtual void   copyToBackbuffer(Texture* finalImage) = 0;        // scaled/flipped copy into the swapchain image
    virtual void   beginUiOverlay() = 0;                             // backbuffer bound for ImGui
    virtual void   endUiOverlay() = 0;
    virtual void   swapBuffers() = 0;
    virtual void   waitIdle() = 0;                                   // glFinish / Flush+query
    virtual void   setMaxFrameLatency(uint32_t frames) = 0;          // render-ahead queue depth
    virtual void   waitForFrameLatency() = 0;                        // blocks until GPU within latency budget

    virtual MemoryStats memoryStats() const = 0;
    // Reads the current backbuffer (after copyToBackbuffer/UI) as top-down RGBA8 (screenshots, tests).
    virtual bool readBackbuffer(std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h) = 0;
};


}  // namespace gfx::rhi
