// Direct3D 11 backend (Windows). Same conventions as GL (D3D clip space is native here).
//
// Shaders: the GLSL 450 sources are compiled to SPIR-V with glslang, translated to HLSL SM 5.0 with
// SPIRV-Cross and compiled with D3DCompile, so modules never write HLSL and hot reload works.
// Register mapping from GLSL bindings:
//   uniform block binding N      -> cbuffer       b N
//   sampler binding N            -> Texture tN + SamplerState sN
//   readonly SSBO binding N      -> ByteAddressBuffer   t(16 + N)
//   writable SSBO binding N      -> RWByteAddressBuffer u(8 + N)
//   image binding N              -> RWTexture         u N
// Logical bindings are recorded by the command list and applied to every stage right before a
// draw/dispatch using the current program's reflection (which SSBOs are read-only).
// Not supported here: tessellation and geometry stages (SPIRV-Cross can't emit HLSL hull/domain/geometry
// shaders); Caps::tessellation = false so modules fall back.
#if defined(_WIN32)

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>   // FetchContent build tree layout
#include <spirv_hlsl.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

#include "rhi/backends.h"
#include "rhi/rhi.h"

namespace gfx::rhi {
namespace {

template <class T> void safeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

constexpr UINT kSsboSrvBase = 16, kSsboUavBase = 8, kMaxTex = 32, kMaxUav = 16, kMaxCb = 14;

DXGI_FORMAT dxFormat(Format f) {
    switch (f) {
        case Format::R8: return DXGI_FORMAT_R8_UNORM;
        case Format::RG8: return DXGI_FORMAT_R8G8_UNORM;
        case Format::RGBA8: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case Format::RGBA8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case Format::BGRA8_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case Format::R16F: return DXGI_FORMAT_R16_FLOAT;
        case Format::RG16F: return DXGI_FORMAT_R16G16_FLOAT;
        case Format::RGBA16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case Format::R32F: return DXGI_FORMAT_R32_FLOAT;
        case Format::RG32F: return DXGI_FORMAT_R32G32_FLOAT;
        case Format::RGBA32F: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case Format::R11G11B10F: return DXGI_FORMAT_R11G11B10_FLOAT;
        case Format::RGB10A2: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case Format::R32UI: return DXGI_FORMAT_R32_UINT;
        case Format::RG32UI: return DXGI_FORMAT_R32G32_UINT;
        case Format::RGBA32UI: return DXGI_FORMAT_R32G32B32A32_UINT;
        case Format::D32F: return DXGI_FORMAT_D32_FLOAT;
        case Format::D24S8: return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case Format::BC1_SRGB: return DXGI_FORMAT_BC1_UNORM_SRGB;
        case Format::BC3_SRGB: return DXGI_FORMAT_BC3_UNORM_SRGB;
        case Format::BC4: return DXGI_FORMAT_BC4_UNORM;
        case Format::BC5: return DXGI_FORMAT_BC5_UNORM;
        case Format::BC7_SRGB: return DXGI_FORMAT_BC7_UNORM_SRGB;
        default: return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}
// Depth textures are created typeless so they can be sampled.
DXGI_FORMAT resourceFormat(Format f) {
    if (f == Format::D32F) return DXGI_FORMAT_R32_TYPELESS;
    if (f == Format::D24S8) return DXGI_FORMAT_R24G8_TYPELESS;
    return dxFormat(f);
}
DXGI_FORMAT srvFormat(Format f) {
    if (f == Format::D32F) return DXGI_FORMAT_R32_FLOAT;
    if (f == Format::D24S8) return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    return dxFormat(f);
}
D3D11_COMPARISON_FUNC dxCompare(CompareOp op) {
    switch (op) {
        case CompareOp::Never: return D3D11_COMPARISON_NEVER; case CompareOp::Less: return D3D11_COMPARISON_LESS;
        case CompareOp::LessEqual: return D3D11_COMPARISON_LESS_EQUAL; case CompareOp::Equal: return D3D11_COMPARISON_EQUAL;
        case CompareOp::GreaterEqual: return D3D11_COMPARISON_GREATER_EQUAL; case CompareOp::Greater: return D3D11_COMPARISON_GREATER;
        case CompareOp::NotEqual: return D3D11_COMPARISON_NOT_EQUAL; default: return D3D11_COMPARISON_ALWAYS;
    }
}
D3D11_TEXTURE_ADDRESS_MODE dxAddress(AddressMode m) {
    switch (m) {
        case AddressMode::Repeat: return D3D11_TEXTURE_ADDRESS_WRAP; case AddressMode::Clamp: return D3D11_TEXTURE_ADDRESS_CLAMP;
        case AddressMode::Mirror: return D3D11_TEXTURE_ADDRESS_MIRROR; default: return D3D11_TEXTURE_ADDRESS_BORDER;
    }
}
uint32_t mipCountFor(uint32_t w, uint32_t h, uint32_t d = 1) {
    uint32_t m = 1, s = std::max({w, h, d});
    while (s > 1) { s >>= 1; ++m; }
    return m;
}
size_t surfaceBytes(Format f, uint32_t w, uint32_t h) {
    if (isCompressedFormat(f)) return size_t((w + 3) / 4) * ((h + 3) / 4) * ((f == Format::BC1_SRGB || f == Format::BC4) ? 8 : 16);
    return size_t(w) * h * formatBytesPerPixel(f);
}
size_t rowPitch(Format f, uint32_t w) {
    if (isCompressedFormat(f)) return size_t((w + 3) / 4) * ((f == Format::BC1_SRGB || f == Format::BC4) ? 8 : 16);
    return size_t(w) * formatBytesPerPixel(f);
}

struct MemTracker {
    std::mutex mtx;
    std::map<std::string, size_t> perOwner;
    size_t tex = 0, buf = 0;
    void add(const std::string& o, size_t b, bool t) { std::lock_guard<std::mutex> l(mtx); perOwner[o] += b; (t ? tex : buf) += b; }
    void sub(const std::string& o, size_t b, bool t) { std::lock_guard<std::mutex> l(mtx); perOwner[o] -= b; (t ? tex : buf) -= b; }
};

// ---------------------------------------------------------------------------------------
class DxTexture final : public Texture {
public:
    DxTexture(ID3D11Device* dev, const TextureDesc& d, MemTracker& mem) : dev_(dev), mem_(mem) {
        desc_ = d;
        if (desc_.mipLevels == 0) desc_.mipLevels = mipCountFor(d.width, d.height, d.type == TextureType::Tex3D ? d.depthOrLayers : 1);
        bool depth = isDepthFormat(d.format);
        UINT bind = 0;
        if (d.usage & TexUsageSampled) bind |= D3D11_BIND_SHADER_RESOURCE;
        if (d.usage & TexUsageRenderTarget) bind |= D3D11_BIND_RENDER_TARGET;
        if (d.usage & TexUsageDepthStencil) bind |= D3D11_BIND_DEPTH_STENCIL;
        if (d.usage & TexUsageStorage) bind |= D3D11_BIND_UNORDERED_ACCESS;
        if (!bind) bind = D3D11_BIND_SHADER_RESOURCE;
        UINT misc = 0;
        if (d.type == TextureType::TexCube) misc |= D3D11_RESOURCE_MISC_TEXTURECUBE;
        if ((bind & D3D11_BIND_RENDER_TARGET) && desc_.mipLevels > 1 && !depth) misc |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
        HRESULT hr;
        if (d.type == TextureType::Tex3D) {
            D3D11_TEXTURE3D_DESC td{};
            td.Width = d.width; td.Height = d.height; td.Depth = d.depthOrLayers; td.MipLevels = desc_.mipLevels;
            td.Format = resourceFormat(d.format); td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = bind; td.MiscFlags = misc;
            ID3D11Texture3D* t = nullptr;
            hr = dev->CreateTexture3D(&td, nullptr, &t);
            res = t;
        } else {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = d.width; td.Height = d.height; td.MipLevels = desc_.mipLevels;
            td.ArraySize = d.type == TextureType::Tex2D ? 1 : d.depthOrLayers;
            td.Format = resourceFormat(d.format); td.SampleDesc.Count = std::max(1u, d.samples);
            td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = bind; td.MiscFlags = misc;
            ID3D11Texture2D* t = nullptr;
            hr = dev->CreateTexture2D(&td, nullptr, &t);
            res = t;
        }
        if (FAILED(hr)) std::fprintf(stderr, "[D3D11] CreateTexture '%s' failed 0x%08lx\n", d.name.c_str(), hr);
        if (res && !d.name.empty()) res->SetPrivateData(WKPDID_D3DDebugObjectName, UINT(d.name.size()), d.name.data());
        if (res && (bind & D3D11_BIND_SHADER_RESOURCE)) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = srvFormat(d.format);
            switch (d.type) {
                case TextureType::Tex2D:
                    if (d.samples > 1) sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
                    else { sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = desc_.mipLevels; }
                    break;
                case TextureType::Tex2DArray:
                    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY; sd.Texture2DArray.MipLevels = desc_.mipLevels; sd.Texture2DArray.ArraySize = d.depthOrLayers; break;
                case TextureType::TexCube:
                    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE; sd.TextureCube.MipLevels = desc_.mipLevels; break;
                case TextureType::Tex3D:
                    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D; sd.Texture3D.MipLevels = desc_.mipLevels; break;
            }
            dev->CreateShaderResourceView(res, &sd, &srv);
        }
        size_t layers = d.type == TextureType::Tex3D ? 1 : d.depthOrLayers;
        for (uint32_t m = 0; m < desc_.mipLevels; ++m) {
            size_t s = surfaceBytes(d.format, width(m), height(m)) * layers;
            if (d.type == TextureType::Tex3D) s *= std::max(1u, d.depthOrLayers >> m);
            gpuBytes_ += s;
        }
        gpuBytes_ *= std::max(1u, d.samples);
        mem_.add(desc_.owner, gpuBytes_, true);
    }
    ~DxTexture() override {
        for (auto& [k, v] : rtvs) v->Release();
        for (auto& [k, v] : dsvs) v->Release();
        for (auto& [k, v] : uavs) v->Release();
        safeRelease(srv);
        safeRelease(res);
        mem_.sub(desc_.owner, gpuBytes_, true);
    }
    uintptr_t nativeHandle() const override { return reinterpret_cast<uintptr_t>(srv); }

    ID3D11RenderTargetView* rtv(uint32_t mip, uint32_t layer) {
        uint64_t key = (uint64_t(mip) << 32) | layer;
        if (auto it = rtvs.find(key); it != rtvs.end()) return it->second;
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = dxFormat(desc_.format);
        if (desc_.type == TextureType::Tex3D) {
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D; rd.Texture3D.MipSlice = mip; rd.Texture3D.FirstWSlice = layer; rd.Texture3D.WSize = 1;
        } else if (desc_.type == TextureType::Tex2D) {
            if (desc_.samples > 1) rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
            else { rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D; rd.Texture2D.MipSlice = mip; }
        } else {
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY; rd.Texture2DArray.MipSlice = mip;
            rd.Texture2DArray.FirstArraySlice = layer; rd.Texture2DArray.ArraySize = 1;
        }
        ID3D11RenderTargetView* v = nullptr;
        dev_->CreateRenderTargetView(res, &rd, &v);
        rtvs[key] = v;
        return v;
    }
    ID3D11DepthStencilView* dsv(uint32_t mip, uint32_t layer, bool readOnly) {
        uint64_t key = (uint64_t(mip) << 33) | (uint64_t(layer) << 1) | (readOnly ? 1 : 0);
        if (auto it = dsvs.find(key); it != dsvs.end()) return it->second;
        D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
        dd.Format = dxFormat(desc_.format);
        dd.Flags = readOnly ? (D3D11_DSV_READ_ONLY_DEPTH | (desc_.format == Format::D24S8 ? D3D11_DSV_READ_ONLY_STENCIL : 0)) : 0;
        if (desc_.type == TextureType::Tex2D) {
            if (desc_.samples > 1) dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
            else { dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D; dd.Texture2D.MipSlice = mip; }
        } else {
            dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY; dd.Texture2DArray.MipSlice = mip;
            dd.Texture2DArray.FirstArraySlice = layer; dd.Texture2DArray.ArraySize = 1;
        }
        ID3D11DepthStencilView* v = nullptr;
        dev_->CreateDepthStencilView(res, &dd, &v);
        dsvs[key] = v;
        return v;
    }
    ID3D11UnorderedAccessView* uav(uint32_t mip) {
        if (auto it = uavs.find(mip); it != uavs.end()) return it->second;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = dxFormat(desc_.format);
        switch (desc_.type) {
            case TextureType::Tex2D: ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D; ud.Texture2D.MipSlice = mip; break;
            case TextureType::Tex3D:
                ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D; ud.Texture3D.MipSlice = mip; ud.Texture3D.WSize = UINT(-1); break;
            default:   // arrays and cubes: all slices (layered)
                ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY; ud.Texture2DArray.MipSlice = mip;
                ud.Texture2DArray.ArraySize = desc_.depthOrLayers; break;
        }
        ID3D11UnorderedAccessView* v = nullptr;
        dev_->CreateUnorderedAccessView(res, &ud, &v);
        uavs[mip] = v;
        return v;
    }

    ID3D11Resource* res = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
private:
    ID3D11Device* dev_;
    MemTracker& mem_;
    std::map<uint64_t, ID3D11RenderTargetView*> rtvs;
    std::map<uint64_t, ID3D11DepthStencilView*> dsvs;
    std::map<uint32_t, ID3D11UnorderedAccessView*> uavs;
};

class DxSampler final : public Sampler {
public:
    DxSampler(ID3D11Device* dev, const SamplerDesc& d) {
        desc_ = d;
        D3D11_SAMPLER_DESC sd{};
        bool aniso = d.maxAnisotropy > 1.0f && d.minFilter == Filter::Linear;
        int f = (d.minFilter == Filter::Linear ? 0x10 : 0) | (d.magFilter == Filter::Linear ? 0x4 : 0) |
                ((d.mipFilter == Filter::Linear && d.useMips) ? 0x1 : 0);
        sd.Filter = aniso ? D3D11_FILTER_ANISOTROPIC : D3D11_FILTER(f);
        if (d.compare) sd.Filter = D3D11_FILTER(sd.Filter | 0x80);
        sd.AddressU = dxAddress(d.addressU); sd.AddressV = dxAddress(d.addressV); sd.AddressW = dxAddress(d.addressW);
        sd.MipLODBias = d.mipLodBias;
        sd.MaxAnisotropy = UINT(std::clamp(d.maxAnisotropy, 1.0f, 16.0f));
        sd.ComparisonFunc = d.compare ? dxCompare(d.compareOp) : D3D11_COMPARISON_NEVER;
        std::memcpy(sd.BorderColor, d.borderColor, sizeof(sd.BorderColor));
        sd.MinLOD = d.minLod;
        sd.MaxLOD = d.useMips ? d.maxLod : 0.0f;
        dev->CreateSamplerState(&sd, &state);
    }
    ~DxSampler() override { safeRelease(state); }
    ID3D11SamplerState* state = nullptr;
};

class DxBuffer final : public Buffer {
public:
    DxBuffer(ID3D11Device* dev, const BufferDesc& d, const void* data, MemTracker& mem) : mem_(mem) {
        desc_ = d;
        D3D11_BUFFER_DESC bd{};
        bool cb = (d.usage & BufUsageUniform) != 0;
        bd.ByteWidth = UINT((std::max<size_t>(d.size, 16) + 15) & ~size_t(15));
        bd.Usage = D3D11_USAGE_DEFAULT;
        if (cb) {
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        } else {
            if (d.usage & BufUsageVertex) bd.BindFlags |= D3D11_BIND_VERTEX_BUFFER;
            if (d.usage & BufUsageIndex) bd.BindFlags |= D3D11_BIND_INDEX_BUFFER;
            if (d.usage & (BufUsageStorage | BufUsageIndirect)) {
                bd.BindFlags |= D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
                bd.MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
                bd.ByteWidth = (bd.ByteWidth + 3) & ~3u;
            }
            if (d.usage & BufUsageIndirect) bd.MiscFlags |= D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
        }
        D3D11_SUBRESOURCE_DATA init{data, 0, 0};
        std::vector<uint8_t> padded;
        if (data && bd.ByteWidth != d.size) { padded.resize(bd.ByteWidth); std::memcpy(padded.data(), data, d.size); init.pSysMem = padded.data(); }
        dev->CreateBuffer(&bd, data ? &init : nullptr, &buf);
        if (buf && (bd.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_R32_TYPELESS; sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
            sd.BufferEx.NumElements = bd.ByteWidth / 4; sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
            dev->CreateShaderResourceView(buf, &sd, &srv);
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_R32_TYPELESS; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            ud.Buffer.NumElements = bd.ByteWidth / 4; ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
            dev->CreateUnorderedAccessView(buf, &ud, &uav);
        }
        if (buf && !d.name.empty()) buf->SetPrivateData(WKPDID_D3DDebugObjectName, UINT(d.name.size()), d.name.data());
        byteWidth = bd.ByteWidth;
        mem_.add(desc_.owner, byteWidth, false);
    }
    ~DxBuffer() override { safeRelease(srv); safeRelease(uav); safeRelease(buf); mem_.sub(desc_.owner, byteWidth, false); }
    ID3D11Buffer* buf = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
    UINT byteWidth = 0;
private:
    MemTracker& mem_;
};

// Reflection needed to route SSBO bindings to SRV or UAV registers.
struct ProgramReflection {
    uint32_t readOnlySsbos = 0;   // bit N: binding N is a read-only SSBO (t16+N)
    uint32_t writableSsbos = 0;   // bit N: binding N is a writable SSBO (u8+N)
};

class DxProgram final : public Program {
public:
    explicit DxProgram(const ProgramDesc& d) { desc_ = d; }
    ~DxProgram() override { release(); }
    void release() { safeRelease(vs); safeRelease(ps); safeRelease(cs); safeRelease(vsBlob); }
    void setResult(bool ok, std::string log) { if (ok) { valid_ = true; ++generation_; } log_ = std::move(log); }
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11ComputeShader* cs = nullptr;
    ID3DBlob* vsBlob = nullptr;   // for input layouts
    ProgramReflection refl;
};

class DxPipeline final : public Pipeline {
public:
    DxPipeline(ID3D11Device* dev, const PipelineDesc& d) : dev_(dev) {
        desc_ = d;
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = d.raster.fill == FillMode::Wireframe ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
        rd.CullMode = d.raster.cull == CullMode::None ? D3D11_CULL_NONE : d.raster.cull == CullMode::Back ? D3D11_CULL_BACK : D3D11_CULL_FRONT;
        rd.FrontCounterClockwise = d.raster.frontCCW;
        rd.DepthBias = INT(d.raster.depthBiasConstant);
        rd.SlopeScaledDepthBias = d.raster.depthBiasSlope;
        rd.DepthClipEnable = !d.raster.depthClamp;
        rd.ScissorEnable = d.raster.scissor;
        rd.MultisampleEnable = TRUE;
        dev->CreateRasterizerState(&rd, &raster);
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = d.depth.test;
        dd.DepthWriteMask = d.depth.write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = dxCompare(d.depth.compare);
        dev->CreateDepthStencilState(&dd, &depth);
        D3D11_BLEND_DESC bd{};
        bd.IndependentBlendEnable = TRUE;
        for (int i = 0; i < 8; ++i) {
            auto& t = bd.RenderTarget[i];
            const auto& b = d.blend[i];
            t.RenderTargetWriteMask = b.writeMask;
            t.BlendOp = t.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            switch (b.mode) {
                case BlendMode::Opaque: t.BlendEnable = FALSE; t.SrcBlend = t.SrcBlendAlpha = D3D11_BLEND_ONE; t.DestBlend = t.DestBlendAlpha = D3D11_BLEND_ZERO; break;
                case BlendMode::Alpha: t.BlendEnable = TRUE; t.SrcBlend = D3D11_BLEND_SRC_ALPHA; t.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
                    t.SrcBlendAlpha = D3D11_BLEND_ONE; t.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; break;
                case BlendMode::Additive: t.BlendEnable = TRUE; t.SrcBlend = t.SrcBlendAlpha = D3D11_BLEND_ONE; t.DestBlend = t.DestBlendAlpha = D3D11_BLEND_ONE; break;
                case BlendMode::Premultiplied: t.BlendEnable = TRUE; t.SrcBlend = t.SrcBlendAlpha = D3D11_BLEND_ONE; t.DestBlend = t.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; break;
                case BlendMode::Multiply: t.BlendEnable = TRUE; t.SrcBlend = t.SrcBlendAlpha = D3D11_BLEND_DEST_COLOR; t.DestBlend = t.DestBlendAlpha = D3D11_BLEND_ZERO; break;
            }
        }
        dev->CreateBlendState(&bd, &blend);
    }
    ~DxPipeline() override { safeRelease(raster); safeRelease(depth); safeRelease(blend); safeRelease(layout); }

    ID3D11InputLayout* inputLayout() {
        auto* prog = static_cast<DxProgram*>(desc_.program);
        if (desc_.layout.attribs.empty() || !prog || !prog->vsBlob) return nullptr;
        if (layout && layoutGen == prog->generation()) return layout;
        safeRelease(layout);
        std::vector<D3D11_INPUT_ELEMENT_DESC> el;
        for (auto& a : desc_.layout.attribs) {
            D3D11_INPUT_ELEMENT_DESC e{};
            e.SemanticName = "TEXCOORD";   // SPIRV-Cross maps location N -> TEXCOORDN
            e.SemanticIndex = a.location;
            switch (a.format) {
                case VertexFormat::Float: e.Format = DXGI_FORMAT_R32_FLOAT; break;
                case VertexFormat::Float2: e.Format = DXGI_FORMAT_R32G32_FLOAT; break;
                case VertexFormat::Float3: e.Format = DXGI_FORMAT_R32G32B32_FLOAT; break;
                case VertexFormat::Float4: e.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; break;
                case VertexFormat::UByte4Norm: e.Format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
                case VertexFormat::UInt: e.Format = DXGI_FORMAT_R32_UINT; break;
            }
            e.InputSlot = a.bufferSlot;
            e.AlignedByteOffset = a.offset;
            e.InputSlotClass = a.perInstance ? D3D11_INPUT_PER_INSTANCE_DATA : D3D11_INPUT_PER_VERTEX_DATA;
            e.InstanceDataStepRate = a.perInstance ? 1 : 0;
            el.push_back(e);
        }
        dev_->CreateInputLayout(el.data(), UINT(el.size()), prog->vsBlob->GetBufferPointer(), prog->vsBlob->GetBufferSize(), &layout);
        layoutGen = prog->generation();
        return layout;
    }

    ID3D11RasterizerState* raster = nullptr;
    ID3D11DepthStencilState* depth = nullptr;
    ID3D11BlendState* blend = nullptr;
private:
    ID3D11Device* dev_;
    ID3D11InputLayout* layout = nullptr;
    uint32_t layoutGen = ~0u;
};

class DxTimer final : public TimerQuery {
public:
    explicit DxTimer(ID3D11Device* dev) {
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        dev->CreateQuery(&qd, &disjoint);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        dev->CreateQuery(&qd, &begin);
        dev->CreateQuery(&qd, &end);
    }
    ~DxTimer() override { safeRelease(disjoint); safeRelease(begin); safeRelease(end); }
    bool resultReady() override {
        if (!issued) return false;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        return ctx->GetData(disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
    }
    double resultMs() override {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        UINT64 a = 0, b = 0;
        while (ctx->GetData(disjoint, &dj, sizeof(dj), 0) != S_OK) {}
        ctx->GetData(begin, &a, sizeof(a), 0);
        ctx->GetData(end, &b, sizeof(b), 0);
        issued = false;
        if (dj.Disjoint || dj.Frequency == 0) return 0.0;
        return double(b - a) * 1000.0 / double(dj.Frequency);
    }
    ID3D11Query *disjoint = nullptr, *begin = nullptr, *end = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    bool issued = false;
};

// ---------------------------------------------------------------------------------------
// GLSL -> SPIR-V -> HLSL -> DXBC
// ---------------------------------------------------------------------------------------
struct GlslangInit {
    GlslangInit() { glslang::InitializeProcess(); }
    ~GlslangInit() { glslang::FinalizeProcess(); }
};

bool glslToSpirv(const std::string& src, EShLanguage stage, std::vector<uint32_t>& spirv, std::string& log) {
    static GlslangInit init;
    glslang::TShader shader(stage);
    const char* s = src.c_str();
    shader.setStrings(&s, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientOpenGL, 450);
    shader.setEnvClient(glslang::EShClientOpenGL, glslang::EShTargetOpenGL_450);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    shader.setAutoMapLocations(true);
    EShMessages msgs = EShMessages(EShMsgSpvRules | EShMsgDefault);
    if (!shader.parse(GetDefaultResources(), 450, false, msgs)) { log += shader.getInfoLog(); return false; }
    glslang::TProgram prog;
    prog.addShader(&shader);
    if (!prog.link(msgs)) { log += prog.getInfoLog(); return false; }
    glslang::GlslangToSpv(*prog.getIntermediate(stage), spirv);
    return true;
}

bool spirvToHlsl(std::vector<uint32_t> spirv, std::string& hlsl, ProgramReflection& refl, std::string& log) {
    try {
        spirv_cross::CompilerHLSL comp(std::move(spirv));
        spirv_cross::CompilerHLSL::Options ho;
        ho.shader_model = 50;
        comp.set_hlsl_options(ho);
        spirv_cross::CompilerGLSL::Options go = comp.get_common_options();
        go.vertex.flip_vert_y = false;      // our GLSL already uses D3D clip conventions
        go.vertex.fixup_clipspace = false;  // and [0,1] depth
        comp.set_common_options(go);
        auto res = comp.get_shader_resources();
        for (auto& sb : res.storage_buffers) {
            uint32_t b = comp.get_decoration(sb.id, spv::DecorationBinding);
            bool ro = comp.get_buffer_block_flags(sb.id).get(spv::DecorationNonWritable);
            comp.set_decoration(sb.id, spv::DecorationBinding, ro ? kSsboSrvBase + b : kSsboUavBase + b);
            (ro ? refl.readOnlySsbos : refl.writableSsbos) |= 1u << b;
        }
        hlsl = comp.compile();
        return true;
    } catch (const std::exception& e) {
        log += std::string("SPIRV-Cross: ") + e.what() + "\n";
        return false;
    }
}

ID3DBlob* compileHlsl(const std::string& hlsl, const char* target, std::string& log) {
    ID3DBlob *code = nullptr, *errors = nullptr;
    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS;
    HRESULT hr = D3DCompile(hlsl.data(), hlsl.size(), nullptr, nullptr, nullptr, "main", target, flags, 0, &code, &errors);
    if (errors) { log.append(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()); errors->Release(); }
    if (FAILED(hr)) { safeRelease(code); return nullptr; }
    return code;
}

// Built-in HLSL for backbuffer copy and MSAA depth resolve.
const char* kBlitHlsl = R"(
Texture2D<float4> src : register(t0);
SamplerState samp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vsMain(uint id : SV_VertexID) {
    VSOut o; float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv; o.pos = float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1); return o;
}
float4 psMain(VSOut i) : SV_Target { return src.SampleLevel(samp, i.uv, 0); }
Texture2DMS<float> msDepth : register(t1);
float psDepth(VSOut i) : SV_Depth { return msDepth.Load(int2(i.pos.xy), 0); }
)";

// ---------------------------------------------------------------------------------------
class DxDevice;

class DxCommandList final : public CommandList {
public:
    explicit DxCommandList(DxDevice& d);
    void beginRenderPass(const RenderPassDesc& pass) override;
    void endRenderPass() override {}
    void setViewport(float x, float y, float w, float h) override {
        D3D11_VIEWPORT vp{x, y, w, h, 0.0f, 1.0f};
        ctx_->RSSetViewports(1, &vp);
    }
    void setScissor(int x, int y, int w, int h) override { D3D11_RECT r{x, y, x + w, y + h}; ctx_->RSSetScissorRects(1, &r); }
    void bindPipeline(Pipeline* p) override;
    void bindComputeProgram(Program* p) override { program_ = static_cast<DxProgram*>(p); ctx_->CSSetShader(program_->cs, nullptr, 0); }
    void bindTexture(uint32_t slot, Texture* tex, Sampler* s) override {
        if (slot >= kMaxTex) return;
        srvs_[slot] = tex ? static_cast<DxTexture*>(tex)->srv : nullptr;
        srvTex_[slot] = tex;
        samplers_[slot] = s ? static_cast<DxSampler*>(s)->state : defaultSampler_;
        dirty_ = true;
    }
    void bindImage(uint32_t slot, Texture* tex, uint32_t mip, Access) override {
        if (slot >= kSsboUavBase) return;
        uavs_[slot] = tex ? static_cast<DxTexture*>(tex)->uav(mip) : nullptr;
        dirty_ = true;
    }
    void bindUniformBuffer(uint32_t slot, Buffer* b, size_t offset, size_t size) override {
        if (slot >= kMaxCb) return;
        auto* db = static_cast<DxBuffer*>(b);
        cbs_[slot] = db->buf;
        if (!size) size = db->byteWidth - offset;
        cbFirst_[slot] = UINT(offset / 16);
        cbNum_[slot] = UINT(((size + 255) / 256) * 16);   // D3D11.1: multiple of 16 constants
        dirty_ = true;
    }
    void bindStorageBuffer(uint32_t slot, Buffer* b, size_t, size_t) override {
        if (slot >= 16) return;
        ssbos_[slot] = static_cast<DxBuffer*>(b);
        dirty_ = true;
    }
    void bindVertexBuffer(uint32_t slot, Buffer* b, size_t offset) override {
        auto* db = static_cast<DxBuffer*>(b);
        UINT stride = pipeline_ ? pipeline_->desc().layout.strides[slot] : 0, off = UINT(offset);
        ctx_->IASetVertexBuffers(slot, 1, &db->buf, &stride, &off);
    }
    void bindIndexBuffer(Buffer* b, IndexType type, size_t offset) override {
        indexSize_ = type == IndexType::U16 ? 2 : 4;
        ctx_->IASetIndexBuffer(static_cast<DxBuffer*>(b)->buf, type == IndexType::U16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT, UINT(offset));
    }
    void draw(uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) override { flush(false); ctx_->DrawInstanced(vc, ic, fv, fi); }
    void drawIndexed(uint32_t c, uint32_t ic, uint32_t fi, int32_t bv, uint32_t fin) override { flush(false); ctx_->DrawIndexedInstanced(c, ic, fi, bv, fin); }
    void drawIndirect(Buffer* args, size_t offset, uint32_t count, uint32_t stride) override {
        flush(false);
        for (uint32_t i = 0; i < count; ++i) ctx_->DrawInstancedIndirect(static_cast<DxBuffer*>(args)->buf, UINT(offset + i * stride));
    }
    void drawIndexedIndirect(Buffer* args, size_t offset, uint32_t count, uint32_t stride) override {
        flush(false);
        for (uint32_t i = 0; i < count; ++i) ctx_->DrawIndexedInstancedIndirect(static_cast<DxBuffer*>(args)->buf, UINT(offset + i * stride));
    }
    void dispatch(uint32_t x, uint32_t y, uint32_t z) override { flush(true); ctx_->Dispatch(x, y, z); unbindCompute(); }
    void dispatchIndirect(Buffer* args, size_t offset) override { flush(true); ctx_->DispatchIndirect(static_cast<DxBuffer*>(args)->buf, UINT(offset)); unbindCompute(); }
    void barrier(uint32_t) override {}   // D3D11 tracks hazards itself
    void updateBuffer(Buffer* b, const void* data, size_t size, size_t offset) override {
        auto* db = static_cast<DxBuffer*>(b);
        D3D11_BOX box{UINT(offset), 0, 0, UINT(offset + size), 1, 1};
        if ((db->desc().usage & BufUsageUniform) && (offset != 0 || size < db->byteWidth)) {
            // Constant buffers: partial updates need D3D11.1 and 16-byte aligned ranges.
            box.left = UINT(offset & ~size_t(15));
            box.right = UINT((offset + size + 15) & ~size_t(15));
            std::vector<uint8_t> tmp(box.right - box.left, 0);
            std::memcpy(tmp.data() + (offset - box.left), data, size);
            ctx1_->UpdateSubresource1(db->buf, 0, &box, tmp.data(), 0, 0, D3D11_COPY_DISCARD);
            return;
        }
        ctx_->UpdateSubresource(db->buf, 0, (db->desc().usage & BufUsageUniform) ? nullptr : &box, data, 0, 0);
    }
    void copyTexture(Texture* src, Texture* dst) override { ctx_->CopyResource(static_cast<DxTexture*>(dst)->res, static_cast<DxTexture*>(src)->res); }
    void resolveTexture(Texture* src, Texture* dst) override;
    void blitTexture(Texture* src, Texture* dst, Filter f) override;
    void generateMips(Texture* tex) override { if (auto* t = static_cast<DxTexture*>(tex); t->srv) ctx_->GenerateMips(t->srv); }
    void clearTexture(Texture* tex, const float rgba[4], uint32_t mip) override {
        auto* t = static_cast<DxTexture*>(tex);
        if (isDepthFormat(t->desc().format)) ctx_->ClearDepthStencilView(t->dsv(mip, 0, false), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, rgba[0], 0);
        else if (t->desc().usage & TexUsageRenderTarget)
            for (uint32_t l = 0; l < (t->desc().type == TextureType::Tex2D ? 1 : t->desc().depthOrLayers); ++l) ctx_->ClearRenderTargetView(t->rtv(mip, l), rgba);
        else if (t->desc().usage & TexUsageStorage) ctx_->ClearUnorderedAccessViewFloat(t->uav(mip), rgba);
    }
    void beginTimer(TimerQuery* q) override {
        auto* t = static_cast<DxTimer*>(q);
        t->ctx = ctx_;
        ctx_->Begin(t->disjoint);
        ctx_->End(t->begin);
    }
    void endTimer(TimerQuery* q) override {
        auto* t = static_cast<DxTimer*>(q);
        ctx_->End(t->end);
        ctx_->End(t->disjoint);
        t->issued = true;
    }
    void pushDebugGroup(const char* name) override {
        if (!annot_) return;
        wchar_t w[128]; size_t n = std::min<size_t>(127, std::strlen(name));
        for (size_t i = 0; i < n; ++i) w[i] = wchar_t(name[i]);
        w[n] = 0;
        annot_->BeginEvent(w);
    }
    void popDebugGroup() override { if (annot_) annot_->EndEvent(); }

    void unbindAllSrvs() {
        ID3D11ShaderResourceView* nullSrv[kMaxTex] = {};
        ctx_->VSSetShaderResources(0, kMaxTex, nullSrv);
        ctx_->PSSetShaderResources(0, kMaxTex, nullSrv);
        ctx_->CSSetShaderResources(0, kMaxTex, nullSrv);
        dirty_ = true;
    }
    void invalidate() { pipeline_ = nullptr; dirty_ = true; }
    ID3D11SamplerState* defaultSampler_ = nullptr;

private:
    void flush(bool compute);
    void unbindCompute() {
        ID3D11UnorderedAccessView* nullUav[kMaxUav] = {};
        ctx_->CSSetUnorderedAccessViews(0, kMaxUav, nullUav, nullptr);
        ID3D11ShaderResourceView* nullSrv[kMaxTex] = {};
        ctx_->CSSetShaderResources(0, kMaxTex, nullSrv);
        dirty_ = true;
    }
    DxDevice& dev_;
    ID3D11DeviceContext* ctx_;
    ID3D11DeviceContext1* ctx1_;
    ID3DUserDefinedAnnotation* annot_ = nullptr;
    DxPipeline* pipeline_ = nullptr;
    DxProgram* program_ = nullptr;
    ID3D11ShaderResourceView* srvs_[kMaxTex] = {};
    Texture* srvTex_[kMaxTex] = {};
    ID3D11SamplerState* samplers_[kMaxTex] = {};
    ID3D11UnorderedAccessView* uavs_[kMaxUav] = {};
    ID3D11Buffer* cbs_[kMaxCb] = {};
    UINT cbFirst_[kMaxCb] = {}, cbNum_[kMaxCb] = {};
    DxBuffer* ssbos_[16] = {};
    ID3D11RenderTargetView* rtvs_[8] = {};
    ID3D11DepthStencilView* dsv_ = nullptr;
    UINT numRtvs_ = 0;
    bool hadPsUav_ = false;
    UINT indexSize_ = 4;
    bool dirty_ = true;
    friend class DxDevice;
};

class DxDevice final : public Device {
public:
    explicit DxDevice(GLFWwindow* window) : window_(window), cmd_(nullptr) {}
    bool init();
    ~DxDevice() override {
        if (ctx_) ctx_->ClearState();
        for (auto* q : fences_) q->Release();
        delete cmdPtr_;
        safeRelease(blitVs_); safeRelease(blitPs_); safeRelease(depthPs_); safeRelease(linearSampler_); safeRelease(pointSampler_);
        safeRelease(blitDepthState_); safeRelease(noDepthState_); safeRelease(blitRaster_);
        safeRelease(backbufferRtv_); safeRelease(swapchain_); safeRelease(ctx1_); safeRelease(ctx_); safeRelease(device_); safeRelease(adapter_);
    }

    const Caps& caps() const override { return caps_; }
    TexturePtr createTexture(const TextureDesc& d, const void* data) override {
        auto t = std::make_unique<DxTexture>(device_, d, mem_);
        if (data) uploadTexture(t.get(), 0, 0, data, 0);
        return t;
    }
    void uploadTexture(Texture* tex, uint32_t mip, uint32_t layer, const void* data, size_t) override {
        auto* t = static_cast<DxTexture*>(tex);
        const auto& d = t->desc();
        UINT sub = D3D11CalcSubresource(mip, d.type == TextureType::Tex3D ? 0 : layer, d.mipLevels);
        size_t pitch = rowPitch(d.format, t->width(mip));
        size_t rows = isCompressedFormat(d.format) ? (t->height(mip) + 3) / 4 : t->height(mip);
        ctx_->UpdateSubresource(t->res, sub, nullptr, data, UINT(pitch), UINT(pitch * rows));
    }
    SamplerPtr createSampler(const SamplerDesc& d) override { return std::make_unique<DxSampler>(device_, d); }
    BufferPtr createBuffer(const BufferDesc& d, const void* data) override { return std::make_unique<DxBuffer>(device_, d, data, mem_); }
    ProgramPtr createProgram(const ProgramDesc& d) override { return std::make_unique<DxProgram>(d); }

    void setProgramCacheDir(const std::string& dir) override {
        cacheDir_ = dir;
        if (!dir.empty()) { std::error_code ec; std::filesystem::create_directories(dir, ec); }
    }
    ShaderCacheStats shaderCacheStats() const override { return cacheStats_; }

    // HLSL bytecode cache keyed by the generated HLSL.
    ID3DBlob* compileCached(const std::string& hlsl, const char* target, std::string& log) {
        std::string file;
        if (!cacheDir_.empty()) {
            char name[48]; std::snprintf(name, sizeof(name), "%016zx_%s.dxbc", std::hash<std::string>{}(hlsl), target);
            file = cacheDir_ + "/" + name;
            std::ifstream in(file, std::ios::binary);
            if (in) {
                std::vector<char> bin((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                ID3DBlob* blob = nullptr;
                if (SUCCEEDED(D3DCreateBlob(bin.size(), &blob))) { std::memcpy(blob->GetBufferPointer(), bin.data(), bin.size()); ++cacheStats_.hits; return blob; }
            }
            ++cacheStats_.misses;
        }
        ID3DBlob* blob = compileHlsl(hlsl, target, log);
        if (blob && !file.empty()) {
            std::ofstream out(file, std::ios::binary);
            out.write(static_cast<const char*>(blob->GetBufferPointer()), std::streamsize(blob->GetBufferSize()));
            ++cacheStats_.stores;
        }
        return blob;
    }

    bool rebuildProgram(Program* program, const ProgramSources& src) override {
        auto t0 = std::chrono::high_resolution_clock::now();
        auto* p = static_cast<DxProgram*>(program);
        std::string log;
        if (!src.tcs.empty() || !src.tes.empty() || !src.gs.empty()) {
            p->setResult(false, "D3D11 backend: tessellation/geometry stages are not supported (SPIRV-Cross cannot emit HLSL hull/domain/geometry shaders)");
            return false;
        }
        ProgramReflection refl;
        auto stage = [&](const std::string& code, EShLanguage lang, const char* target) -> ID3DBlob* {
            std::vector<uint32_t> spv;
            std::string hlsl;
            if (!glslToSpirv(code, lang, spv, log)) return nullptr;
            if (!spirvToHlsl(std::move(spv), hlsl, refl, log)) return nullptr;
            return compileCached(hlsl, target, log);
        };
        ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr; ID3D11ComputeShader* cs = nullptr;
        ID3DBlob* vsBlob = nullptr;
        bool ok = true;
        if (!src.cs.empty()) {
            ID3DBlob* b = stage(src.cs, EShLangCompute, "cs_5_0");
            ok = b && SUCCEEDED(device_->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &cs));
            safeRelease(b);
        } else {
            vsBlob = stage(src.vs, EShLangVertex, "vs_5_0");
            ok = vsBlob && SUCCEEDED(device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs));
            if (ok && !src.fs.empty()) {
                ID3DBlob* b = stage(src.fs, EShLangFragment, "ps_5_0");
                ok = b && SUCCEEDED(device_->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps));
                safeRelease(b);
            }
        }
        if (ok) {
            p->release();
            p->vs = vs; p->ps = ps; p->cs = cs; p->vsBlob = vsBlob; p->refl = refl;
        } else {
            safeRelease(vs); safeRelease(ps); safeRelease(cs); safeRelease(vsBlob);
        }
        p->setResult(ok, log);
        cacheStats_.compileMs += std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
        return ok;
    }

    PipelinePtr createPipeline(const PipelineDesc& d) override { return std::make_unique<DxPipeline>(device_, d); }
    TimerPtr createTimer() override { return std::make_unique<DxTimer>(device_); }
    CommandList& cmd() override { return *cmdPtr_; }

    void resizeSwapchain(uint32_t w, uint32_t h) override {
        if (w == bbW_ && h == bbH_ && backbufferRtv_) return;
        bbW_ = w; bbH_ = h;
        recreateBackbuffer();
    }
    void setPresentMode(PresentMode m) override { presentMode_ = m; }
    bool setOutputColorSpace(OutputColorSpace cs) override {
        DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
        DXGI_COLOR_SPACE_TYPE space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        if (cs == OutputColorSpace::HDR10_PQ) { fmt = DXGI_FORMAT_R10G10B10A2_UNORM; space = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020; }
        if (cs == OutputColorSpace::scRGB_Linear) { fmt = DXGI_FORMAT_R16G16B16A16_FLOAT; space = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709; }
        IDXGISwapChain3* sc3 = nullptr;
        if (FAILED(swapchain_->QueryInterface(IID_PPV_ARGS(&sc3)))) return cs == OutputColorSpace::SDR_sRGB;
        UINT support = 0;
        bool ok = SUCCEEDED(sc3->CheckColorSpaceSupport(space, &support)) && (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT);
        if (cs != OutputColorSpace::SDR_sRGB && !(ok && caps_.hdrOutput)) { sc3->Release(); setOutputColorSpace(OutputColorSpace::SDR_sRGB); return false; }
        backbufferFormat_ = fmt;
        recreateBackbuffer();
        sc3->SetColorSpace1(space);
        sc3->Release();
        return true;
    }
    void copyToBackbuffer(Texture* finalImage) override {
        auto* t = static_cast<DxTexture*>(finalImage);
        cmdPtr_->unbindAllSrvs();
        ctx_->OMSetRenderTargets(1, &backbufferRtv_, nullptr);
        D3D11_VIEWPORT vp{0, 0, float(bbW_), float(bbH_), 0, 1};
        ctx_->RSSetViewports(1, &vp);
        drawBlit(t->srv, (t->width() == bbW_ && t->height() == bbH_) ? pointSampler_ : linearSampler_, blitPs_);
        cmdPtr_->invalidate();
    }
    void drawBlit(ID3D11ShaderResourceView* srv, ID3D11SamplerState* s, ID3D11PixelShader* ps) {
        ctx_->IASetInputLayout(nullptr);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->VSSetShader(blitVs_, nullptr, 0);
        ctx_->PSSetShader(ps, nullptr, 0);
        ctx_->PSSetShaderResources(ps == depthPs_ ? 1 : 0, 1, &srv);
        ctx_->PSSetSamplers(0, 1, &s);
        ctx_->RSSetState(blitRaster_);
        ctx_->OMSetDepthStencilState(ps == depthPs_ ? blitDepthState_ : noDepthState_, 0);
        ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        ctx_->Draw(3, 0);
        ID3D11ShaderResourceView* nullSrv[2] = {};
        ctx_->PSSetShaderResources(0, 2, nullSrv);
    }
    void beginUiOverlay() override {
        cmdPtr_->unbindAllSrvs();
        ctx_->OMSetRenderTargets(1, &backbufferRtv_, nullptr);
        D3D11_VIEWPORT vp{0, 0, float(bbW_), float(bbH_), 0, 1};
        ctx_->RSSetViewports(1, &vp);
    }
    void endUiOverlay() override { cmdPtr_->invalidate(); }
    void swapBuffers() override {
        UINT interval = 0, flags = 0;
        switch (presentMode_) {
            case PresentMode::Immediate: interval = 0; flags = tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0; break;
            case PresentMode::VSync: case PresentMode::AdaptiveVSync: interval = 1; break;
            case PresentMode::HalfRateVSync: interval = 2; break;
        }
        swapchain_->Present(interval, flags);
        D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
        ID3D11Query* q = nullptr;
        device_->CreateQuery(&qd, &q);
        ctx_->End(q);
        fences_.push_back(q);
        while (fences_.size() > 8) { fences_.front()->Release(); fences_.pop_front(); }
    }
    void waitIdle() override {
        D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
        ID3D11Query* q = nullptr;
        device_->CreateQuery(&qd, &q);
        ctx_->End(q);
        BOOL done = FALSE;
        while (ctx_->GetData(q, &done, sizeof(done), 0) != S_OK || !done) {}
        q->Release();
    }
    void setMaxFrameLatency(uint32_t frames) override { maxLatency_ = std::max(1u, frames); }
    void waitForFrameLatency() override {
        while (fences_.size() >= maxLatency_) {
            ID3D11Query* q = fences_[fences_.size() - maxLatency_];
            BOOL done = FALSE;
            auto start = std::chrono::steady_clock::now();
            while (ctx_->GetData(q, &done, sizeof(done), 0) != S_OK || !done) {
                if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(100)) break;
                std::this_thread::yield();
            }
            while (!fences_.empty() && fences_.front() != q) { fences_.front()->Release(); fences_.pop_front(); }
            if (!fences_.empty()) { fences_.front()->Release(); fences_.pop_front(); }
        }
    }
    MemoryStats memoryStats() const override {
        MemoryStats m;
        {
            std::lock_guard<std::mutex> l(const_cast<MemTracker&>(mem_).mtx);
            m.textureBytes = mem_.tex; m.bufferBytes = mem_.buf;
            for (auto& [k, v] : mem_.perOwner) if (v) m.perOwner.emplace_back(k, v);
        }
        IDXGIAdapter3* a3 = nullptr;
        if (adapter_ && SUCCEEDED(adapter_->QueryInterface(IID_PPV_ARGS(&a3)))) {
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
                m.driverUsedBytes = size_t(info.CurrentUsage);
                m.driverBudgetBytes = size_t(info.Budget);
            }
            a3->Release();
        }
        return m;
    }
    bool readBackbuffer(std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h) override {
        ID3D11Texture2D* bb = nullptr;
        if (FAILED(swapchain_->GetBuffer(0, IID_PPV_ARGS(&bb)))) return false;
        D3D11_TEXTURE2D_DESC td{};
        bb->GetDesc(&td);
        if (td.Format != DXGI_FORMAT_R8G8B8A8_UNORM) { bb->Release(); return false; }
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
        ID3D11Texture2D* staging = nullptr;
        device_->CreateTexture2D(&td, nullptr, &staging);
        ctx_->CopyResource(staging, bb);
        D3D11_MAPPED_SUBRESOURCE ms{};
        ctx_->Map(staging, 0, D3D11_MAP_READ, 0, &ms);
        w = td.Width; h = td.Height;
        rgba.resize(size_t(w) * h * 4);
        for (uint32_t y = 0; y < h; ++y) std::memcpy(rgba.data() + size_t(y) * w * 4, static_cast<uint8_t*>(ms.pData) + size_t(y) * ms.RowPitch, size_t(w) * 4);
        ctx_->Unmap(staging, 0);
        staging->Release(); bb->Release();
        return true;
    }
    void* nativeDevice() const override { return device_; }
    void* nativeContext() const override { return ctx_; }

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    ID3D11DeviceContext1* ctx1_ = nullptr;
    ID3D11VertexShader* blitVs_ = nullptr;
    ID3D11PixelShader *blitPs_ = nullptr, *depthPs_ = nullptr;
    ID3D11SamplerState *linearSampler_ = nullptr, *pointSampler_ = nullptr;
    ID3D11DepthStencilState *blitDepthState_ = nullptr, *noDepthState_ = nullptr;
    ID3D11RasterizerState* blitRaster_ = nullptr;

private:
    void recreateBackbuffer() {
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
        safeRelease(backbufferRtv_);
        ctx_->Flush();
        swapchain_->ResizeBuffers(0, bbW_, bbH_, backbufferFormat_, tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
        ID3D11Texture2D* bb = nullptr;
        swapchain_->GetBuffer(0, IID_PPV_ARGS(&bb));
        device_->CreateRenderTargetView(bb, nullptr, &backbufferRtv_);
        bb->Release();
    }

    GLFWwindow* window_;
    DxCommandList* cmd_;
    DxCommandList* cmdPtr_ = nullptr;
    IDXGIAdapter1* adapter_ = nullptr;
    IDXGISwapChain1* swapchain_ = nullptr;
    ID3D11RenderTargetView* backbufferRtv_ = nullptr;
    DXGI_FORMAT backbufferFormat_ = DXGI_FORMAT_R8G8B8A8_UNORM;
    Caps caps_;
    MemTracker mem_;
    std::deque<ID3D11Query*> fences_;
    uint32_t maxLatency_ = 2, bbW_ = 0, bbH_ = 0;
    PresentMode presentMode_ = PresentMode::VSync;
    bool tearing_ = false;
    std::string cacheDir_;
    ShaderCacheStats cacheStats_;
    friend class DxCommandList;
};


// ---------------------------------------------------------------------------------------
DxCommandList::DxCommandList(DxDevice& d) : dev_(d), ctx_(d.ctx_), ctx1_(d.ctx1_) {
    ctx_->QueryInterface(IID_PPV_ARGS(&annot_));
    defaultSampler_ = d.linearSampler_;
    for (auto& s : samplers_) s = defaultSampler_;
}

void DxCommandList::beginRenderPass(const RenderPassDesc& pass) {
    unbindAllSrvs();
    ID3D11RenderTargetView* rtvs[8] = {};
    uint32_t w = 0, h = 0;
    for (uint32_t i = 0; i < pass.colorCount; ++i) {
        auto* t = static_cast<DxTexture*>(pass.colors[i].texture);
        if (!t) continue;
        rtvs[i] = t->rtv(pass.colors[i].mip, pass.colors[i].layer);
        if (pass.colors[i].load == LoadOp::Clear) ctx_->ClearRenderTargetView(rtvs[i], pass.colors[i].clear);
        w = t->width(pass.colors[i].mip); h = t->height(pass.colors[i].mip);
    }
    ID3D11DepthStencilView* dsv = nullptr;
    if (auto* t = static_cast<DxTexture*>(pass.depth.texture)) {
        bool clear = pass.depth.load == LoadOp::Clear;
        dsv = t->dsv(pass.depth.mip, pass.depth.layer, pass.depth.readOnly && !clear);
        if (clear) {
            UINT flags = D3D11_CLEAR_DEPTH | (t->desc().format == Format::D24S8 ? D3D11_CLEAR_STENCIL : 0);
            ctx_->ClearDepthStencilView(dsv, flags, pass.depth.clearDepth, pass.depth.clearStencil);
        }
        if (!w) { w = t->width(pass.depth.mip); h = t->height(pass.depth.mip); }
    }
    ctx_->OMSetRenderTargets(pass.colorCount, rtvs, dsv);
    numRtvs_ = pass.colorCount;
    for (uint32_t i = 0; i < 8; ++i) rtvs_[i] = rtvs[i];
    dsv_ = dsv;
    if (w) setViewport(0, 0, float(w), float(h));
}

void DxCommandList::bindPipeline(Pipeline* p) {
    pipeline_ = static_cast<DxPipeline*>(p);
    const auto& d = pipeline_->desc();
    program_ = static_cast<DxProgram*>(d.program);
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    switch (d.topology) {
        case Topology::Triangles: topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
        case Topology::TriangleStrip: topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
        case Topology::Lines: topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; break;
        case Topology::Points: topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; break;
        case Topology::Patches:   // no hull/domain shaders on this backend: draw the control points as triangles
            topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
    }
    ctx_->IASetPrimitiveTopology(topo);
    ctx_->IASetInputLayout(pipeline_->inputLayout());
    ctx_->VSSetShader(program_ ? program_->vs : nullptr, nullptr, 0);
    ctx_->PSSetShader(program_ ? program_->ps : nullptr, nullptr, 0);
    ctx_->RSSetState(pipeline_->raster);
    ctx_->OMSetDepthStencilState(pipeline_->depth, 0);
    ctx_->OMSetBlendState(pipeline_->blend, nullptr, 0xFFFFFFFF);
    dirty_ = true;
}

void DxCommandList::flush(bool compute) {
    if (!dirty_ || !program_) return;
    dirty_ = false;
    // t0..15 textures, t16+N read-only SSBOs.
    ID3D11ShaderResourceView* srv[kMaxTex] = {};
    for (uint32_t i = 0; i < 16; ++i) srv[i] = srvs_[i];
    ID3D11UnorderedAccessView* uav[kMaxUav] = {};
    for (uint32_t i = 0; i < kSsboUavBase; ++i) uav[i] = uavs_[i];
    for (uint32_t b = 0; b < 16; ++b) {
        if (!ssbos_[b]) continue;
        if ((program_->refl.readOnlySsbos >> b) & 1) { if (kSsboSrvBase + b < kMaxTex) srv[kSsboSrvBase + b] = ssbos_[b]->srv; }
        else if ((program_->refl.writableSsbos >> b) & 1) { if (kSsboUavBase + b < kMaxUav) uav[kSsboUavBase + b] = ssbos_[b]->uav; }
    }
    if (compute) {
        // A UAV and SRV of the same resource can't be bound together: UAVs win.
        ctx_->CSSetUnorderedAccessViews(0, kMaxUav, uav, nullptr);
        ctx_->CSSetShaderResources(0, kMaxTex, srv);
        ctx_->CSSetSamplers(0, 16, samplers_);
        ctx1_->CSSetConstantBuffers1(0, kMaxCb, cbs_, cbFirst_, cbNum_);
    } else {
        ctx_->VSSetShaderResources(0, kMaxTex, srv);
        ctx_->PSSetShaderResources(0, kMaxTex, srv);
        ctx_->VSSetSamplers(0, 16, samplers_);
        ctx_->PSSetSamplers(0, 16, samplers_);
        ctx1_->VSSetConstantBuffers1(0, kMaxCb, cbs_, cbFirst_, cbNum_);
        ctx1_->PSSetConstantBuffers1(0, kMaxCb, cbs_, cbFirst_, cbNum_);
        bool anyUav = false;
        for (auto* u : uav) anyUav |= u != nullptr;
        if (anyUav || hadPsUav_) {
            // Pixel-shader UAVs share slots with render targets: start after the bound RTVs.
            UINT start = std::max<UINT>(numRtvs_, 1);
            ctx_->OMSetRenderTargetsAndUnorderedAccessViews(numRtvs_, rtvs_, dsv_, start, anyUav ? kMaxUav - start : 0,
                                                            anyUav ? uav + start : nullptr, nullptr);
            hadPsUav_ = anyUav;
        }
    }
}

void DxCommandList::resolveTexture(Texture* src, Texture* dst) {
    auto* s = static_cast<DxTexture*>(src);
    auto* d = static_cast<DxTexture*>(dst);
    if (!isDepthFormat(s->desc().format)) {
        ctx_->ResolveSubresource(d->res, 0, s->res, 0, dxFormat(s->desc().format));
        return;
    }
    // Depth can't be resolved by ResolveSubresource: write sample 0 through SV_Depth.
    unbindAllSrvs();
    ID3D11DepthStencilView* dsv = d->dsv(0, 0, false);
    ctx_->OMSetRenderTargets(0, nullptr, dsv);
    setViewport(0, 0, float(d->width()), float(d->height()));
    dev_.drawBlit(s->srv, dev_.pointSampler_, dev_.depthPs_);
    invalidate();
}

void DxCommandList::blitTexture(Texture* src, Texture* dst, Filter f) {
    auto* s = static_cast<DxTexture*>(src);
    auto* d = static_cast<DxTexture*>(dst);
    if (s->desc().samples > 1) { resolveTexture(src, dst); return; }
    unbindAllSrvs();
    ID3D11RenderTargetView* rtv = d->rtv(0, 0);
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    setViewport(0, 0, float(d->width()), float(d->height()));
    dev_.drawBlit(s->srv, f == Filter::Linear ? dev_.linearSampler_ : dev_.pointSampler_, dev_.blitPs_);
    invalidate();
}

bool DxDevice::init() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if !defined(NDEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &device_, &got, &ctx_);
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {   // debug layer not installed
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &device_, &got, &ctx_);
    }
    if (FAILED(hr)) { std::fprintf(stderr, "[D3D11] D3D11CreateDevice failed 0x%08lx\n", hr); return false; }
    if (FAILED(ctx_->QueryInterface(IID_PPV_ARGS(&ctx1_)))) { std::fprintf(stderr, "[D3D11] D3D11.1 context required\n"); return false; }

    IDXGIDevice* dxgiDev = nullptr;
    device_->QueryInterface(IID_PPV_ARGS(&dxgiDev));
    IDXGIAdapter* adapter = nullptr;
    dxgiDev->GetAdapter(&adapter);
    adapter->QueryInterface(IID_PPV_ARGS(&adapter_));
    adapter->Release();
    dxgiDev->Release();
    IDXGIFactory2* factory = nullptr;
    adapter_->GetParent(IID_PPV_ARGS(&factory));
    IDXGIFactory5* f5 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&f5)))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow)))) tearing_ = allow;
        f5->Release();
    }

    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    bbW_ = uint32_t(std::max(1, w)); bbH_ = uint32_t(std::max(1, h));
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = bbW_; sd.Height = bbH_; sd.Format = backbufferFormat_;
    sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 3;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    HWND hwnd = glfwGetWin32Window(window_);
    hr = factory->CreateSwapChainForHwnd(device_, hwnd, &sd, nullptr, nullptr, &swapchain_);
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);   // GLFW handles fullscreen
    factory->Release();
    if (FAILED(hr)) { std::fprintf(stderr, "[D3D11] CreateSwapChainForHwnd failed 0x%08lx\n", hr); return false; }
    ID3D11Texture2D* bb = nullptr;
    swapchain_->GetBuffer(0, IID_PPV_ARGS(&bb));
    device_->CreateRenderTargetView(bb, nullptr, &backbufferRtv_);
    bb->Release();

    // Caps
    DXGI_ADAPTER_DESC1 ad{};
    adapter_->GetDesc1(&ad);
    caps_.backend = Backend::D3D11;
    caps_.apiName = "Direct3D 11";
    char name[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
    caps_.renderer = name;
    caps_.vendor = ad.VendorId == 0x10DE ? "NVIDIA" : ad.VendorId == 0x1002 ? "AMD" : ad.VendorId == 0x8086 ? "Intel" : "Unknown";
    caps_.version = got == D3D_FEATURE_LEVEL_11_1 ? "FL 11_1" : "FL 11_0";
    caps_.maxAnisotropy = 16.0f;
    caps_.maxTextureSize = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    caps_.maxSamples = 1;
    for (UINT s : {2u, 4u, 8u}) {
        UINT q = 0;
        if (SUCCEEDED(device_->CheckMultisampleQualityLevels(DXGI_FORMAT_R16G16B16A16_FLOAT, s, &q)) && q > 0) caps_.maxSamples = s;
    }
    caps_.timestampQueries = true;
    caps_.tessellation = false;
    caps_.computeShaders = true;
    caps_.dedicatedVideoMemory = ad.DedicatedVideoMemory;
    D3D11_FEATURE_DATA_D3D11_OPTIONS2 o2{};
    if (SUCCEEDED(device_->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &o2, sizeof(o2))))
        caps_.conservativeRaster = o2.ConservativeRasterizationTier != D3D11_CONSERVATIVE_RASTERIZATION_NOT_SUPPORTED;
    IDXGIOutput* out = nullptr;
    if (SUCCEEDED(swapchain_->GetContainingOutput(&out))) {
        IDXGIOutput6* o6 = nullptr;
        if (SUCCEEDED(out->QueryInterface(IID_PPV_ARGS(&o6)))) {
            DXGI_OUTPUT_DESC1 od{};
            if (SUCCEEDED(o6->GetDesc1(&od))) caps_.hdrOutput = od.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            o6->Release();
        }
        out->Release();
    }

    // Built-in blit shaders.
    auto compile = [&](const char* entry, const char* target) -> ID3DBlob* {
        ID3DBlob *code = nullptr, *err = nullptr;
        D3DCompile(kBlitHlsl, std::strlen(kBlitHlsl), "blit", nullptr, nullptr, entry, target, 0, 0, &code, &err);
        if (err) { std::fprintf(stderr, "[D3D11] %s\n", static_cast<const char*>(err->GetBufferPointer())); err->Release(); }
        return code;
    };
    if (ID3DBlob* b = compile("vsMain", "vs_5_0")) { device_->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &blitVs_); b->Release(); }
    if (ID3DBlob* b = compile("psMain", "ps_5_0")) { device_->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &blitPs_); b->Release(); }
    if (ID3DBlob* b = compile("psDepth", "ps_5_0")) { device_->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &depthPs_); b->Release(); }

    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    device_->CreateSamplerState(&smp, &linearSampler_);
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    device_->CreateSamplerState(&smp, &pointSampler_);
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    device_->CreateDepthStencilState(&dd, &blitDepthState_);
    dd.DepthEnable = FALSE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    device_->CreateDepthStencilState(&dd, &noDepthState_);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    device_->CreateRasterizerState(&rd, &blitRaster_);

    cmdPtr_ = new DxCommandList(*this);
    return true;
}

}  // namespace

std::unique_ptr<Device> createD3D11Device(GLFWwindow* window) {
    auto dev = std::make_unique<DxDevice>(window);
    if (!dev->init()) return nullptr;
    return dev;
}

}  // namespace gfx::rhi
#endif  // _WIN32
