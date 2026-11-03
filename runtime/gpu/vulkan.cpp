// The title's Direct3D 8 calls drawn with Vulkan 1.3. The shaders are the HLSL the Direct3D 12 renderer compiles,
// compiled to SPIR-V by DXC at run time (dxcompiler.dll beside the executable, else the Vulkan SDK's).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgiformat.h>

#include <vulkan/vulkan.h>

#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#define VMA_VULKAN_VERSION 1003000
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <wrl/client.h>

#include <dxc/dxcapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>
#include <backends/imgui_impl_win32.h>

#include "app/app.h"
#include "core/diag.h"
#include "core/log.h"

// the backend's skipped work and validation messages are shown in release logs too
#undef LOG_FIRST
#define LOG_FIRST(n, ...)                                  \
    do {                                                   \
        static std::atomic<int> _c{0};                     \
        if (_c.fetch_add(1, std::memory_order_relaxed) < (n)) LOGR(__VA_ARGS__); \
    } while (0)
#include "gpu/backend.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/ui.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"

using Microsoft::WRL::ComPtr;

namespace gpu {

namespace {

constexpr int kFrames = 3;
constexpr uint64_t kStaleFrames = 8;  // frames a target may go unused and still be read back
constexpr uint64_t kUploadSize = 64ull << 20;
// a draw starting with less than this left in the frame's ring waits for the GPU and starts the ring over
constexpr uint64_t kUploadReserve = 8ull << 20;
constexpr VkFormat kBgra = VK_FORMAT_B8G8R8A8_UNORM;
constexpr float kDepthFar = 0;  // depth is stored reversed
// binding numbers of the one push descriptor set, from the DXC register shifts in CompileSpirv
constexpr uint32_t kBindVc = 0;
constexpr uint32_t kBindDc = 1;
constexpr uint32_t kBindTex = 2;
constexpr uint32_t kBindSmp = 6;
constexpr uint32_t kBindCount = 10;

const char* kBlitHlsl = R"(
cbuffer B : register(b1) { float4 uvrect; float4 opts; };
Texture2D img : register(t0);
Texture2D lut : register(t1);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uvrect.xy + t * uvrect.zw;
    return o;
}
float4 psmain(O i) : SV_Target {
    float4 c = 0;
    int n = max((int)opts.z, 1);
    float2 px = fwidth(i.uv);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) c += img.SampleLevel(ls, i.uv + px * ((float2(x, y) + 0.5) / n - 0.5), 0);
    c /= n * n;
    c.rgb *= opts.w;
    if (opts.x > 0) c.rgb = float3(lut.Sample(ps, float2(c.r, 0.5)).r, lut.Sample(ps, float2(c.g, 0.5)).g, lut.Sample(ps, float2(c.b, 0.5)).b);
    return float4(c.rgb, opts.y > 0 ? c.a : 1);
}
)";

const char* kClearHlsl = R"(
cbuffer B : register(b1) { float4 colour; };
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 psmain() : SV_Target { return colour; }
)";

float Bits(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void Color(uint32_t argb, float* o) {
    o[0] = ((argb >> 16) & 255) / 255.0f;
    o[1] = ((argb >> 8) & 255) / 255.0f;
    o[2] = (argb & 255) / 255.0f;
    o[3] = (argb >> 24) / 255.0f;
}

uint32_t SimpleState(const State& s, uint32_t method) { return s.simple[method >> 2]; }

// D3DBLEND, the console uses GL values
constexpr uint32_t kBlendDestColor = 0x306;  // D3DBLEND_DESTCOLOR

VkBlendFactor Blend(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return VK_BLEND_FACTOR_ZERO;
        case 1: return VK_BLEND_FACTOR_ONE;
        case 0x300: return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
        case 0x301: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 0x304: return VK_BLEND_FACTOR_DST_ALPHA;
        case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case kBlendDestColor: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
        case 0x307: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 0x308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x8001: case 0x8003: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 0x8002: case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        default: return VK_BLEND_FACTOR_ONE;
    }
}

// D3DBLENDOP, GL values
VkBlendOp BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return VK_BLEND_OP_SUBTRACT;
        case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case 0x8007: return VK_BLEND_OP_MIN;
        case 0x8008: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
    }
}

// D3DCMP_NEVER 0x200 to D3DCMP_ALWAYS 0x207 as 0 to 7, the order of VkCompareOp
uint32_t CmpIndex(uint32_t v) { return v >= 0x200 && v <= 0x207 ? v - 0x200 : 7; }
VkCompareOp Cmp(uint32_t v) { return VkCompareOp(CmpIndex(v)); }

// a D3DCMP test on reversed depth: less and greater trade places
VkCompareOp ReversedCmp(uint32_t v) {
    static const uint8_t mirror[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    return VkCompareOp(mirror[CmpIndex(v)]);
}

// D3DSTENCILOP, GL values
VkStencilOp StencilOp(uint32_t v) {
    switch (v) {
        case 0: return VK_STENCIL_OP_ZERO;
        case 0x1E01: return VK_STENCIL_OP_REPLACE;
        case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case 0x150A: return VK_STENCIL_OP_INVERT;
        case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default: return VK_STENCIL_OP_KEEP;
    }
}

// D3DTADDRESS: 1 wrap, 2 mirror, 3 clamp, 4 border, 5 clamp to edge
VkSamplerAddressMode Address(uint32_t v) {
    switch (v) {
        case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 3: case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

VkFormat HostFormat(Host h) {
    switch (h) {
        case Host::BC1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case Host::BC2: return VK_FORMAT_BC2_UNORM_BLOCK;
        case Host::BC3: return VK_FORMAT_BC3_UNORM_BLOCK;
        case Host::R32F: return VK_FORMAT_D32_SFLOAT;
        case Host::R16Unorm: return VK_FORMAT_D16_UNORM;
        default: return kBgra;
    }
}

// the vertex formats Assemble hands out, as DXGI_FORMAT values
VkFormat VertexFormat(uint8_t f) {
    switch (f) {
        case DXGI_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
        case DXGI_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
        case DXGI_FORMAT_R32G32B32_FLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case DXGI_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R16_SNORM: return VK_FORMAT_R16_SNORM;
        case DXGI_FORMAT_R16G16_SNORM: return VK_FORMAT_R16G16_SNORM;
        case DXGI_FORMAT_R16G16B16A16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
        case DXGI_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
        case DXGI_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R16_SINT: return VK_FORMAT_R16_SINT;
        case DXGI_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SINT;
        case DXGI_FORMAT_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
        case DXGI_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT;
        default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

constexpr uint32_t kFmtD16 = 0x2C;     // D3DFMT_D16
constexpr uint32_t kFmtLinD16 = 0x30;  // D3DFMT_LIN_D16

// TexInfo::shadow of a depth texture
uint8_t ShadowDepthKind(uint32_t format) { return format == kFmtD16 || format == kFmtLinD16 ? 2 : 1; }

// bytes per row of a 32 bit surface in guest memory, rounded to 64
uint32_t GuestPitch(uint32_t width) { return (width * 4 + 63) & ~63u; }

// MemSig hashes a surface up to kSigWholeMax bytes whole, a bigger one as evenly spaced samples
constexpr uint32_t kSigWholeMax = 65536;
constexpr uint32_t kSigSamples = 64;
constexpr uint32_t kSigSampleBytes = 64;

uint64_t MemSig(uint32_t data, uint32_t total) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    uint64_t sig = Hash(&total, sizeof(total));
    if (total <= kSigWholeMax) return Hash(p, total, sig);
    for (uint32_t i = 0; i < kSigSamples; i++) {
        const uint8_t* sample = p + uint64_t(total - kSigSampleBytes) * i / (kSigSamples - 1);
        sig = Hash(sample, kSigSampleBytes, sig);
    }
    return sig;
}

// a host image: its views, size and the layout the commands recorded so far leave it in
struct Img {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VkImageView view = VK_NULL_HANDLE;    // sampled: colour, or the depth aspect
    VkImageView attach = VK_NULL_HANDLE;  // render target views of a target, depth and stencil together
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t w = 0, h = 0, levels = 1, layers = 1;
    VkImageAspectFlags aspect = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool Depth() const { return (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0; }
};

// a surface the GPU draws: the console's size and the host image at render scale
struct Tgt {
    Img img;
    bool depth = false;
    uint32_t data = 0;
    uint32_t gw = 0, gh = 0;
    uint32_t w = 0, h = 0;
    uint32_t bytes = 0;
    uint64_t epoch = 0;
    uint64_t sig = 0;
    bool drawn = false;           // the GPU drew since the guest memory was written
    std::vector<uint8_t> read;    // what the last Sync wrote to guest memory
};

struct Tex {
    Img img;
    TextureDesc d;
    uint32_t levels = 1;
    uint64_t sig = 0;
    uint64_t frame = 0;
    uint64_t palette = 0;  // hash of the colours a P8 texture was decoded with
};

// a draw's four texture stages
struct DrawTextures {
    TexInfo info[4];
    const Img* img[4] = {};
    bool rt[4] = {};            // stage samples a render target
    float k[4] = {1, 1, 1, 1};
    float wide[4] = {};         // host texels per console texel of a target read into a smaller one, 0 otherwise
};

struct PsoKey {
    uint64_t vs;
    uint64_t ps;
    uint32_t regs;
    uint32_t topo;
    uint32_t strip;
    uint32_t has_rt;
    uint32_t has_ds;
    uint32_t elements[16];  // per register: DXGI format, buffer << 8, offset << 16
    uint32_t blend;
    uint32_t src;
    uint32_t dst;
    uint32_t op;
    uint32_t wmask;
    uint32_t zen;
    uint32_t zwrite;
    uint32_t zfunc;
    uint32_t sten;
    uint32_t sfunc;
    uint32_t smask;
    uint32_t swmask;
    uint32_t sfail;
    uint32_t szfail;
    uint32_t spass;
    uint32_t cull;
    uint32_t ccw;
    uint32_t fill;
    float zslope;
    uint32_t sprite;
    uint32_t depth_format;
};

// a draw's shaders: what they are made from, their cache keys and the modules
struct DrawShaders {
    bool prog = false;  // a vertex program, else the fixed function pipeline
    bool quad = false;
    bool fixed_transform = false;  // fixed function on untransformed vertices: the shader reads matrices and lights
    bool sprite = false;           // a program's points drawn as point sprites (SpriteHlsl)
    FixedKey fk;
    uint64_t vkey = 0;
    uint64_t vh = 0;
    uint64_t pkey = 0;
    uint64_t ph = 0;
    VkShaderModule vs_code = VK_NULL_HANDLE;
    VkShaderModule ps_code = VK_NULL_HANDLE;
};

// the render target's console size in pixels, or the depth buffer's without one
struct GuestExtent {
    uint32_t w;
    uint32_t h;
};

// DXC's loader: dxcompiler.dll beside the executable, else wherever Windows finds it
class Dxc {
public:
    bool Load() {
        if (compiler_) return true;
        HMODULE dll = LoadLibraryW((app::ExeDir() / L"dxcompiler.dll").c_str());
        if (!dll) dll = LoadLibraryW(L"dxcompiler.dll");
        if (!dll) {
            LOGE("vulkan: dxcompiler.dll not found beside the executable");
            return false;
        }
        auto create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(dll, "DxcCreateInstance"));
        if (!create || FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils_))) ||
            FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler_)))) {
            LOGE("vulkan: DxcCreateInstance failed");
            compiler_.Reset();
            return false;
        }
        return true;
    }

    // HLSL to SPIR-V; false with the compiler's message in err
    bool Compile(const std::string& src, const char* entry, const char* profile, std::vector<uint32_t>& out,
                 std::string& err) {
        if (!Load()) return false;
        std::wstring e(entry, entry + std::strlen(entry));
        std::wstring p(profile, profile + std::strlen(profile));
        // textures at bindings 2 to 5 and samplers at 6 to 9, the constant buffers keep their register numbers
        std::vector<LPCWSTR> args = {L"-E", e.c_str(), L"-T", p.c_str(), L"-spirv", L"-fspv-target-env=vulkan1.2",
                                     L"-fvk-use-dx-layout", L"-fvk-t-shift", L"2", L"0", L"-fvk-s-shift", L"6",
                                     L"0", L"-HV", L"2018", L"-Wno-conversion"};
        DxcBuffer buf{src.data(), src.size(), DXC_CP_UTF8};
        ComPtr<IDxcResult> result;
        if (FAILED(compiler_->Compile(&buf, args.data(), UINT32(args.size()), nullptr, IID_PPV_ARGS(&result)))) {
            err = "Compile call failed";
            return false;
        }
        HRESULT status = S_OK;
        result->GetStatus(&status);
        ComPtr<IDxcBlobUtf8> msgs;
        result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&msgs), nullptr);
        if (msgs && msgs->GetStringLength()) err.assign(msgs->GetStringPointer(), msgs->GetStringLength());
        if (FAILED(status)) return false;
        ComPtr<IDxcBlob> code;
        if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&code), nullptr)) || !code) return false;
        out.resize(code->GetBufferSize() / 4);
        std::memcpy(out.data(), code->GetBufferPointer(), out.size() * 4);
        return true;
    }

private:
    ComPtr<IDxcUtils> utils_;
    ComPtr<IDxcCompiler3> compiler_;
};

// MemSig's partner: the whole of a colour surface compared with what the last Sync wrote
bool Changed(const Tgt& t) {
    if (t.drawn || t.read.empty()) return false;
    uint32_t row = t.gw * 4;
    uint32_t pitch = GuestPitch(t.gw);
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        if (std::memcmp(p + size_t(y) * pitch, &t.read[size_t(y) * row], row)) return true;
    }
    return false;
}

// a lock alone proves nothing, reads lock too: only changed bytes mean the CPU wrote it
bool LatchCpuWrite(Tgt& t) {
    uint64_t e = DirtyEpoch(t.data);
    if (e <= t.epoch) return false;
    t.epoch = e;
    uint64_t sig = MemSig(t.data, t.bytes);
    if (sig == t.sig && !Changed(t)) return false;
    t.sig = sig;
    return true;
}

// targets_ key of a surface's colour or depth target
uint64_t TargetMapKey(uint32_t data, bool depth) { return uint64_t(data) << 1 | (depth ? 1 : 0); }

// a smaller depth surface's console size, above the address bits
constexpr int kKeyWidthShift = 34;
constexpr int kKeyHeightShift = 48;

// D3DTEXF: 1 point, 2 linear, 3 anisotropic
constexpr uint32_t kTexfLinear = 2;
constexpr uint32_t kTexfAnisotropic = 3;
// D3DTADDRESS values the samplers test
constexpr uint32_t kTaddressWrap = 1;
constexpr uint32_t kTaddressClamp = 3;
constexpr uint32_t kFillWireframe = 0x1B01;  // D3DFILL_WIREFRAME
constexpr uint32_t kCullCw = 0x900;          // D3DCULL_CW
constexpr uint32_t kZUseW = 2;               // D3DZB_USEW
constexpr uint32_t kQuadVertices = 6;        // a quad drawn as two triangles
constexpr uint32_t kPointList = 1;           // D3DPT_POINTLIST
constexpr uint32_t kTopDisable = 1;          // the XDK's D3DTOP_DISABLE
constexpr int kSpriteStage = 3;

constexpr float kAlphaTestAlways = 7.0f;  // Cmp index of D3DCMP_ALWAYS

// D3DRS_FOGTABLEMODE values
constexpr uint32_t kFogTableNone = 0;
constexpr float kFogTableLinear = 3.0f;

// table mode none under a vertex program: oFog is the blend factor, passed on as linear fog from 1 to 0
void FillFogConstants(const State& s, bool vertex_program, DrawConstants& c) {
    if (!s.rs[kRsFogEnable]) return;
    if (vertex_program && s.rs[kRsFogTableMode] == kFogTableNone) {
        c.fog[0] = 1.0f;
        c.fog[1] = 0.0f;
        c.fog[2] = 0.0f;
        c.fog[3] = kFogTableLinear;
        return;
    }
    c.fog[0] = Bits(s.rs[kRsFogStart]);
    c.fog[1] = Bits(s.rs[kRsFogEnd]);
    c.fog[2] = Bits(s.rs[kRsFogDensity]);
    c.fog[3] = float(s.rs[kRsFogTableMode]);
}

void FillSurfaceConstants(const State& s, GuestExtent size, DrawConstants& c) {
    c.surf[0] = float(size.w);
    c.surf[1] = float(size.h);
    c.surf[2] = 1.0f / s.zscale;
    Color(s.rs[kRsFogColor], c.fog_color);
    c.misc[0] = float(SimpleState(s, kMAlphaRef) & 0xFF);
    c.misc[1] = SimpleState(s, kMAlphaTestEnable) ? float(CmpIndex(SimpleState(s, kMAlphaFunc))) : kAlphaTestAlways;
    c.misc[2] = float(CmpIndex(s.rs[kRsShadowFunc]));
    c.misc[3] = s.rs[kRsSpecularEnable] ? 1.0f : 0.0f;
}

void FillCombinerConstants(const State& s, DrawConstants& c) {
    for (int i = 0; i < 8; i++) {
        Color(SimpleState(s, kMPsConstant0 + i * 4), c.pc[i * 2]);
        Color(SimpleState(s, kMPsConstant1 + i * 4), c.pc[i * 2 + 1]);
    }
    Color(SimpleState(s, kMPsFinalConstant0), c.pc[16]);
    Color(SimpleState(s, kMPsFinalConstant1), c.pc[17]);
    Color(s.rs[kRsTextureFactor], c.tfactor);
}

constexpr float kGlowBoxTexels = 3.0f;  // console texels across the box each wide tap averages

void FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c) {
    c.texsize[i][0] = float(d.width);
    c.texsize[i][1] = float(d.height);
    if (!stages.rt[i]) return;
    float k = stages.k[i];
    // a 3 console texel box per tap keeps the console chain's glow width and brightness
    if (stages.wide[i] > 0) {
        float n = std::ceil(kGlowBoxTexels * stages.wide[i] * 0.5f);
        c.spread[i][0] = kGlowBoxTexels / (n * float(d.width) * k);
        c.spread[i][1] = kGlowBoxTexels / (n * float(d.height) * k);
        c.spread[i][2] = n;
    }
}

void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) {
            o[r * 4 + col] = x[r * 4] * y[col] + x[r * 4 + 1] * y[4 + col] + x[r * 4 + 2] * y[8 + col] +
                             x[r * 4 + 3] * y[12 + col];
        }
    }
}

// HLSL mul(row vector, matrix) with column major packing wants the transpose
void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) o[col * 4 + r] = m[r * 4 + col];
    }
}

void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0 ? 1.0f / det : 0.0f;
    for (int r = 0; r < 3; r++) {
        for (int col = 0; col < 3; col++) {
            int r1 = (r + 1) % 3;
            int r2 = (r + 2) % 3;
            int c1 = (col + 1) % 3;
            int c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
    }
}

// D3DTRANSFORMSTATETYPE indices into s.transforms
constexpr size_t kTsView = 2;
constexpr size_t kTsProjection = 3;
constexpr size_t kTsTexture0 = 16;
constexpr size_t kTsWorld = 256;

void FillTransformConstants(const State& s, GuestExtent size, DrawConstants& c) {
    uint32_t gw = size.w;
    uint32_t gh = size.h;
    // world, view, projection, then the viewport inside the target
    float wv[16];
    float wvp[16];
    MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
    MatMul(wv, s.transforms[kTsProjection].data(), wvp);
    const auto& v = s.viewport;
    float sx = float(v.w) / gw;
    float sy = float(v.h) / gh;
    float ox = (2.0f * v.x + v.w) / gw - 1;
    float oy = 1 - (2.0f * v.y + v.h) / gh;
    float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
    float full[16];
    MatMul(wvp, vpm, full);
    Transpose(full, c.wvp);
    Transpose(wv, c.wv);
    // normals take the inverse transpose of the upper 3x3
    float n[16] = {};
    InverseTranspose3x3(wv, n);
    Transpose(n, c.wvn);
    for (int t = 0; t < 4; t++) Transpose(s.transforms[kTsTexture0 + size_t(t)].data(), c.tm[t]);
}

// D3DLIGHT8 as floats: Type, Diffuse[4], Specular[4], Ambient[4], Position[3], Direction[3], Range, Falloff,
// Attenuation0 to 2, Theta, Phi
constexpr int kLightFloats = 26;
constexpr int kLightDiffuse = 1;
constexpr int kLightSpecular = 5;
constexpr int kLightAmbient = 9;
constexpr int kLightPosition = 13;
constexpr int kLightDirection = 16;
constexpr int kLightRange = 19;
constexpr int kLightFalloff = 20;
constexpr int kLightAttenuation0 = 21;
constexpr int kLightAttenuation1 = 22;
constexpr int kLightAttenuation2 = 23;
constexpr int kLightTheta = 24;
constexpr int kLightPhi = 25;

void LightToView(const std::array<uint32_t, kLightFloats>& l, const float* vm, float (&o)[7][4]) {
    float f[kLightFloats];
    std::memcpy(f, l.data(), sizeof(f));
    for (int j = 0; j < 4; j++) {
        o[0][j] = f[kLightDiffuse + j];
        o[1][j] = f[kLightSpecular + j];
        o[2][j] = f[kLightAmbient + j];
    }
    const float* pos = f + kLightPosition;
    const float* dir = f + kLightDirection;
    for (int j = 0; j < 3; j++) {
        o[3][j] = pos[0] * vm[j] + pos[1] * vm[4 + j] + pos[2] * vm[8 + j] + vm[12 + j];
        o[4][j] = dir[0] * vm[j] + dir[1] * vm[4 + j] + dir[2] * vm[8 + j];
    }
    o[3][3] = f[kLightRange];
    o[4][3] = f[kLightFalloff];
    o[5][0] = f[kLightAttenuation0];
    o[5][1] = f[kLightAttenuation1];
    o[5][2] = f[kLightAttenuation2];
    o[6][0] = std::cos(f[kLightTheta] * 0.5f);
    o[6][1] = std::cos(f[kLightPhi] * 0.5f);
}

void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c) {
    std::memcpy(c.mat, s.material.data(), sizeof(float) * 16);
    c.mat[4][0] = s.material[16];
    Color(s.rs[kRsAmbient], c.amb);
    // lights to view space
    const float* vm = s.transforms[kTsView].data();
    for (int i = 0; i < 8; i++) {
        if (!((fk.types >> (i * 2)) & 3)) continue;
        LightToView(s.lights[size_t(i)], vm, c.lt[i]);
    }
}

// a w buffer's clip range in the units of a draw's clip w
struct WRange {
    float near_w;
    float far_w;
};

// the projection's near and far clip w as D3DDevice_SetTransform finds them
WRange ProjectionWRange(const float* m) {
    float m33 = m[10];
    float m34 = m[11];
    float m43 = m[14];
    float m44 = m[15];
    if (m33 == m34 || m33 == 0) return {0, 1};
    return {m44 - m43 / m33 * m34, (m44 - m43) / (m33 - m34) * m34 + m44};
}

// NV097_SET_CLIP_MIN and MAX in buffer units, sent by D3DDevice_SetDepthClipPlanes
constexpr uint32_t kMClipMin = 0x394;
constexpr uint32_t kMClipMax = 0x398;
constexpr float kFallbackNear = 1.0f / 65536;  // of the far w, when no range is known

// Fixed function clip w is the host's view depth over the projection's range; a program's is in buffer units up to
// zscale, its near end the clip minimum the title sent
WRange DrawWRange(const State& s, bool program) {
    WRange r{0, s.zscale};
    if (!program) {
        r = ProjectionWRange(s.transforms[kTsProjection].data());
    } else if (SimpleState(s, kMClipMax)) {
        r.near_w = Bits(SimpleState(s, kMClipMin));
    }
    if (r.near_w <= 0 || r.near_w >= r.far_w) r.near_w = r.far_w * kFallbackNear;
    return r;
}

bool OffsetsPolygons(const State& s, Topology topology) {
    return topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable);
}

// ReversedDepthHlsl's zparams: x the w buffer's scale (0 for a z buffer), y its far w, z the polygon offset in units of
// the clip w for a w buffer, of depth for a z buffer
void FillDepthConstants(const State& s, const DrawShaders& sh, Topology topo, DrawConstants& c) {
    bool offset = OffsetsPolygons(s, topo);
    float units = offset ? Bits(SimpleState(s, kMDepthOffset)) : 0;
    if (s.rs[kRsZEnable] != kZUseW) {
        c.zparams[2] = units / s.zscale;
        return;
    }
    WRange r = DrawWRange(s, sh.prog);
    c.zparams[0] = r.near_w / (r.far_w - r.near_w);
    c.zparams[1] = r.far_w;
    c.zparams[2] = sh.prog ? units : units * r.far_w / s.zscale;
}

TexKind TextureKind(const TextureDesc& d) {
    if (d.cube) return kTexCube;
    if (d.depth > 1) return kTex3D;
    return kTex2D;
}

// a P8 texture's palette: its colours, their address (part of the texture's key) and their hash, all 0 without one
struct StagePalette {
    uint32_t colours[256] = {};
    uint32_t va = 0;
    uint64_t hash = 0;
};

// a P8 texture is decoded with the stage's palette: 256 colours, fewer by the size code in the top two bits
void ReadStagePalette(const State& s, int stage, const TextureDesc& d, StagePalette& palette) {
    constexpr uint32_t kFmtP8 = 0x0B;       // D3DFMT_P8
    constexpr uint32_t kPaletteCommon = 0;  // D3DPalette: Common, its top two bits the size code
    constexpr uint32_t kPaletteData = 4;    // D3DPalette: Data, physical address of the colours
    constexpr uint32_t kPaletteSizeShift = 30;
    if (d.format != kFmtP8 || !s.palettes[size_t(stage)]) return;
    uint32_t header = s.palettes[size_t(stage)];
    palette.va = mem::VirtualFromPhysical(kernel::R32(header + kPaletteData));
    uint32_t size_code = kernel::R32(header + kPaletteCommon) >> kPaletteSizeShift;
    size_t bytes = (std::size(palette.colours) >> size_code) * sizeof(uint32_t);
    std::memcpy(palette.colours, kernel::HostPtr(palette.va), bytes);
    palette.hash = Hash(palette.colours, sizeof(palette.colours));
}

bool SameTexture(const Tex& tx, const TextureDesc& d) {
    return tx.img.image && tx.d.format == d.format && tx.d.width == d.width && tx.d.height == d.height &&
           tx.d.depth == d.depth && tx.d.levels == d.levels && tx.d.cube == d.cube && tx.d.pitch == d.pitch;
}

DrawShaders ClassifyShaders(const State& s, const Assembled& a, uint32_t count) {
    DrawShaders sh;
    // a shader made from a declaration alone (no program) is the fixed function pipeline on those inputs
    sh.prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    // up to a quad with no depth test: it may be a pass over the whole target, see QuadHlsl; points never are
    bool points = a.topology == Topology::Points;
    sh.quad = count <= kQuadVertices && !s.rs[kRsZEnable] && !points;
    sh.fixed_transform = !sh.prog && !a.pretransformed;
    sh.sprite = points && sh.prog && s.rs[kRsPointSpriteEnable];
    if (!sh.prog) sh.fk = MakeFixedKey(s);
    return sh;
}

// the program's hash is cached in the guest shader object
void EnsureProgramHash(const State& s) {
    if (!s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
}

// vertex shader cache key salts
constexpr uint64_t kDeclarationSalt = 0x10000;    // fixed function seed, above the 16 bit register mask
constexpr uint64_t kPretransformedSalt = 0x5A5A;  // screen space fixed function vertices, apart from untransformed
constexpr uint64_t kQuadSalt = 0xC3C30000;        // the QuadHlsl variant, apart from the plain shader
constexpr uint64_t kSpriteSalt = 0x3C3C0000;      // the SpriteSizeHlsl variant
constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;

uint64_t VertexKey(const State& s, const Assembled& a, const DrawShaders& sh) {
    uint64_t seed = sh.prog ? s.vs->hash ^ a.regs : Hash(&s.vs_handle, 4, a.regs | kDeclarationSalt);
    uint64_t vkey = Hash(&a.decode, 8, seed);
    if (!sh.prog && a.pretransformed) vkey ^= kPretransformedSalt;
    if (sh.quad) vkey ^= kQuadSalt;
    if (sh.sprite) vkey ^= kSpriteSalt;
    if (!sh.prog) vkey = Hash(&sh.fk, sizeof(sh.fk), vkey);
    return vkey;
}

// a point sprite's program hands its point size to SpriteHlsl in tc3, whose coordinates the sprite makes
std::string SpriteSizeHlsl(std::string s) {
    const char* tc3 = "    o.tc3 = oT3;\n";
    size_t at = s.find(tc3);
    if (at == std::string::npos) {
        LOGE("vulkan: a vertex shader no longer fits SpriteSizeHlsl");
        return s;
    }
    s.replace(at, std::strlen(tc3), "    o.tc3 = oPts;\n");
    return s;
}

std::string VertexHlsl(const State& s, const Assembled& a, const DrawShaders& sh) {
    if (sh.prog) {
        std::string src = VertexProgramHlsl(s.vs->program, a.regs, a.decode);
        return sh.sprite ? SpriteSizeHlsl(std::move(src)) : src;
    }
    return FixedVertexHlsl(a.regs, a.pretransformed, a.decode, sh.fk);
}

// A point sprite is a square of the program's point size in console pixels (oPts, which SpriteSizeHlsl passes in
// tc3.x) with the sprite's coordinates across it, 0 to 1 from the top left, as the console makes them for stage 3.
std::string SpriteHlsl() {
    std::string s = PointHlsl();
    s.erase(s.find("[maxvertexcount"));
    return s + R"([maxvertexcount(4)]
void main(point VOut v[1], inout TriangleStream<VOut> strip) {
    float2 half_size = v[0].tc3.x * v[0].pos.w / surf.xy;  // in clip units, the target spans 2 across surf.xy pixels
    for (int i = 0; i < 4; i++) {
        VOut o = v[0];
        float2 corner = float2(i & 1, i >> 1);
        o.pos.x += (corner.x * 2 - 1) * half_size.x;
        o.pos.y -= (corner.y * 2 - 1) * half_size.y;
        o.tc0 = float4(corner, 0, 1);  // stage 3 drawn as stage 0 (SpriteStageZero)
        o.tc3 = o.tc0;
        strip.Append(o);
    }
}
)";
}

// a program's point sprites under the fixed function stages: the XDK builds their combiners from stage 3 alone,
// the stage with the sprite's coordinates; a pixel shader reads t3 itself. SpriteStageZero disables stage 3.
bool SpriteOnStage3(const State& s, uint32_t prim) {
    bool prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    bool stage3 = s.tss[kSpriteStage][kTssColorOp] != kTopDisable;
    return prim == kPointList && prog && !s.ps && s.rs[kRsPointSpriteEnable] && stage3;
}

// stage 3 drawn as stage 0, the only stage
State SpriteStageZero(const State& s) {
    State moved = s;
    moved.tss[0] = s.tss[kSpriteStage];
    moved.textures[0] = s.textures[kSpriteStage];
    moved.palettes[0] = s.palettes[kSpriteStage];
    for (int i = 1; i < 4; i++) {
        moved.tss[i][kTssColorOp] = kTopDisable;
        moved.tss[i][kTssAlphaOp] = kTopDisable;
        moved.textures[i] = 0;
    }
    return moved;
}

// A small draw whose every float2 input lies in 0..1: sprites, HUD pieces, text, full screen passes. The console's
// pixel centres sample such a quad from its first texel to half a texel short of its far edge; upscaled, the far
// edge's last half texel is drawn too, and wrapped it blends in the opposite edge (thin dark lines on HUD sprites).
// Clamped, it takes the edge texel instead, which a wrapped texture used once over 0..1 has there anyway.
bool UnitTexcoords(const Assembled& a) {
    constexpr float kLo = -1e-4f;
    constexpr float kHi = 1.0001f;
    constexpr uint32_t kSmallDrawVertices = 4096;  // most vertices a small draw has
    if (!a.vertices.empty() || !a.vertex_count || a.vertex_count > kSmallDrawVertices) return false;
    bool any = false;
    for (int r = 0; r < 16; r++) {
        const VertexElement& e = a.elements[size_t(r)];
        if (!(a.regs >> r & 1) || e.format != DXGI_FORMAT_R32G32_FLOAT) continue;
        const VertexBuffer& vb = a.buffers[e.buffer];
        if (!vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) return false;
        for (uint32_t v = 0; v < a.vertex_count; v++) {
            float uv[2];
            std::memcpy(uv, vb.src + size_t(v) * vb.stride + e.offset, sizeof(uv));
            if (!(uv[0] >= kLo && uv[0] <= kHi && uv[1] >= kLo && uv[1] <= kHi)) return false;
        }
        any = true;
    }
    return any;
}

// UnitTexcoords only changes stages that wrap
bool AnyStageWraps(const State& s) {
    for (const auto& t : s.tss) {
        if (t[kTssAddressU] == kTaddressWrap || t[kTssAddressV] == kTaddressWrap) return true;
    }
    return false;
}

void KeyBlend(const State& s, PsoKey& k) {
    k.blend = SimpleState(s, kMBlendEnable) != 0;
    if (k.blend) {
        k.src = SimpleState(s, kMSrcBlend);
        k.dst = SimpleState(s, kMDestBlend);
        k.op = SimpleState(s, kMBlendOp);
    }
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    // console ARGB bytes to RGBA bits, the order of VkColorComponentFlagBits
    k.wmask = ((cw >> 16) & 1) | ((cw >> 8) & 1) << 1 | (cw & 1) << 2 | ((cw >> 24) & 1) << 3;
}

void KeyRasterizer(const State& s, const Assembled& a, PsoKey& k) {
    uint32_t cull = s.rs[kRsCullMode];
    k.cull = cull != 0 && a.topology == Topology::Triangles;
    k.ccw = cull == kCullCw;  // culling clockwise leaves counter clockwise as the front
    k.fill = s.rs[kRsFillMode];
}

// CopyRects' guest lists: src_rects RECTs, dst_points POINTs, all int32
constexpr uint32_t kGuestRectBytes = 16;
constexpr uint32_t kGuestPointBytes = 8;

struct GuestRect {
    int32_t left, top, right, bottom;
};
struct GuestPoint {
    int32_t x, y;
};

GuestRect ReadGuestRect(uint32_t rects, uint32_t i) {
    uint32_t r = rects + i * kGuestRectBytes;
    return {int32_t(kernel::R32(r)), int32_t(kernel::R32(r + 4)), int32_t(kernel::R32(r + 8)),
            int32_t(kernel::R32(r + 12))};
}

GuestPoint ReadGuestPoint(uint32_t points, uint32_t i) {
    uint32_t p = points + i * kGuestPointBytes;
    return {int32_t(kernel::R32(p)), int32_t(kernel::R32(p + 4))};
}

// a guest coordinate read as unsigned, times the host scale
uint32_t ScaleUnsigned(int32_t v, float sc) { return uint32_t(uint32_t(v) * sc); }

// a guest rect scaled to the target and clamped to it
VkRect2D ScaledRect(const Tgt& t, int x0, int y0, int x1, int y1) {
    float sc = float(t.w) / t.gw;
    auto cl = [](int v, uint32_t hi) { return int32_t(std::clamp<int64_t>(v, 0, hi)); };
    int32_t ax = cl(int(x0 * sc), t.w), ay = cl(int(y0 * sc), t.h);
    int32_t bx = cl(int(x1 * sc), t.w), by = cl(int(y1 * sc), t.h);
    return {{ax, ay}, {uint32_t(std::max(bx - ax, 0)), uint32_t(std::max(by - ay, 0))}};
}

VKAPI_ATTR VkBool32 VKAPI_CALL DebugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                            const VkDebugUtilsMessengerCallbackDataEXT* d, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) LOG_FIRST(300, "vulkan layer: %s", d->pMessage);
    return VK_FALSE;
}

bool Ok(VkResult r, const char* what) {
    if (r == VK_SUCCESS) return true;
    LOGE("vulkan: %s failed (%d)", what, int(r));
    return false;
}

constexpr VkImageLayout kAttachLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
constexpr VkImageLayout kDepthAttachLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

// the layout a descriptor reads an image in
VkImageLayout ReadLayout(const Img& im) {
    return im.Depth() ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

// Blit's settings past the source and its rectangle
struct BlitOptions {
    bool gamma = false;  // through the gamma lookup
    bool alpha = false;  // the source's alpha, else 1
    uint32_t taps = 1;   // n x n box filter per pixel
    float mul = 1;       // colour scale
    bool over = false;   // premultiplied over the target, which keeps its alpha
};

// everything that makes one VkSampler
struct SamplerKey {
    uint32_t mag, min, mip;
    uint32_t u, v, w;
    float bias, min_lod, max_lod;
    float aniso;
    uint32_t compare;  // 0 off, else VkCompareOp + 1
    uint32_t border;
};

class VulkanBackend final : public Backend {
public:
    ~VulkanBackend() override {
        if (dev_) vkDeviceWaitIdle(dev_);
    }

    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override;
    void Present(const Surface& front) override;
    void WaitGpuFrame(uint64_t n) override;
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override { Sync(data, 0); }
    void Flush() override { Submit(true); }
    void SetGamma(const uint8_t* ramp) override {
        std::memcpy(ramp_, ramp, sizeof(ramp_));
        ramp_dirty_ = true;
    }
    void ClearReport() override {}
    // no occlusion queries: the report is written at once, as if the GPU had finished, with a count that reads as visible
    void GetReport(uint32_t report) override {
        constexpr uint32_t kTime = 0, kTimeHigh = 4, kCount = 8, kStatus = 12;
        constexpr uint32_t kVisiblePixels = 1;
        kernel::W32(report + kTime, uint32_t(presents_));
        kernel::W32(report + kTimeHigh, 0);
        kernel::W32(report + kCount, kVisiblePixels);
        std::atomic_thread_fence(std::memory_order_release);
        kernel::W32(report + kStatus, 0);
    }

private:
    struct FrameSlot {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkBuffer ring = VK_NULL_HANDLE;
        VmaAllocation ring_alloc = nullptr;
        uint8_t* map = nullptr;
        uint64_t used = 0;
        bool pending = false;  // submitted, its fence not waited for
        std::vector<std::function<void()>> garbage;
    };
    struct Alloc {
        VkBuffer buf;
        uint64_t off;
        uint8_t* cpu;
    };

    // ---- device ----
    bool CreateInstance();
    bool CreateDevice();
    bool CreateSwapchain();
    void DestroySwapchain();
    bool CreateFrames();
    bool CreateLayouts();
    bool CreateDummies();
    bool CreateImGui();
    void ResizeSwap();

    // ---- recording ----
    FrameSlot& Slot() { return frames_[frame_ % kFrames]; }
    void BeginCommands();
    void Submit(bool wait, VkSemaphore wait_sem = VK_NULL_HANDLE, VkSemaphore signal_sem = VK_NULL_HANDLE,
                uint64_t timeline = 0);
    void NextFrame();
    void RunGarbage(FrameSlot& f);
    Alloc Upload(uint64_t size, uint64_t align = 256);
    void Later(std::function<void()> f) { Slot().garbage.push_back(std::move(f)); }
    void EndPass();
    void BeginPass(Img* color, Img* depth, const float* clear_color = nullptr);
    void Transition(Img& im, VkImageLayout to);
    void MemoryBarrier();

    // ---- images ----
    bool MakeImage(Img& im, VkImageType type, VkFormat fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t levels,
                   uint32_t layers, VkImageUsageFlags usage, bool cube, bool target);
    void Retire(Img& im);
    void CopyBufferToImage(const Img& im, const Alloc& a, uint32_t level, uint32_t layer, uint32_t w, uint32_t h,
                           uint32_t d);
    VkSampler EnsureSampler(const SamplerKey& k);

    // ---- shaders and pipelines ----
    VkShaderModule Module(const std::string& src, const char* entry, const char* profile, uint64_t& hash);
    VkPipeline EnsurePipeline(const PsoKey& k, const Assembled& a, VkShaderModule vs, VkShaderModule ps);
    VkPipeline EnsureFullscreenPipeline(const char* src, uint32_t mask, bool over, uint64_t seed);
    void FullScreen(VkPipeline pso, const Img* t0, const Img* t1, VkSampler s0, VkSampler s1, const float* cb,
                    uint32_t cb_bytes);

    // ---- targets and textures ----
    uint64_t SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const;
    Tgt* FindTarget(const Surface& s, bool depth);
    Tgt* EnsureTarget(const Surface& s, bool depth);
    void ReleaseTarget(std::unordered_map<uint64_t, Tgt>::iterator it);
    void UpdateTargetUsed(uint32_t data);
    bool TargetStale(uint32_t data) const;
    bool BindTargets(const State& s);
    void UploadTarget(Tgt& t, const Surface& s);
    Img MakePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h);
    void Blit(const Img& src, const float uvrect[4], const BlitOptions& o);
    const Img* Feedback(Tgt& t);
    const Img* BindTargetTexture(const State& s, int stage, const TextureDesc& d, Tgt& t, bool box,
                                 DrawTextures& stages);
    const Img* BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                DrawTextures& stages);
    const Img* BindTexture(const State& s, int stage, DrawTextures& stages);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key);
    void UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p, const uint32_t* colours);
    void ReleaseOldTextures();
    bool ReadBack(Tgt& t, std::vector<uint8_t>& px);
    float TargetScale() const;
    float RenderTargetShift(const State& s, const DrawShaders& sh) const;

    // ---- draws ----
    void EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh);
    void EnsurePixelShader(const PixelKey& pk, DrawShaders& sh);
    void BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k);
    void KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k);
    SamplerKey StageSamplerKey(const State& s, int i, const DrawTextures& stages, bool clamp, uint32_t aniso);
    void FillTextureConstants(const State& s, const DrawShaders& sh, const DrawTextures& stages, DrawConstants& c);
    void UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                         GuestExtent size, DrawConstants& c);
    bool BindStageTexturesAndTargets(const State& s, DrawTextures& stages);
    void PushDescriptors(const State& s, const Assembled& a, const DrawTextures& stages);
    void SetViewportScissor(const State& s, uint32_t gw);
    void SetBlendFactorStencilRef(const State& s);
    void DrawGeometry(const Assembled& a);
    void SetPassViewport(uint32_t w, uint32_t h);
    void CopyByRegion(Tgt& src_t, Tgt& dst_t, uint32_t count, uint32_t src_rects, uint32_t dst_points);
    void ClearTarget(Tgt& t, const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color,
                     float z, uint32_t stencil);
    void ClearChannels(Tgt& t, const float col[4], uint32_t mask, const std::vector<VkRect2D>& rs);

    // ---- presenting ----
    void UploadGammaRamp();
    void DrawFrame(Tgt* t, uint32_t image);
    void BlitFrame(Tgt* t);
    void RenderUi();
    void CountFps();
    void ApplySettings();

    // ---- state ----
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkPhysicalDeviceLimits limits_{};
    VkDevice dev_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t family_ = 0;
    VmaAllocator vma_ = nullptr;
    PFN_vkCmdPushDescriptorSetKHR push_descriptor_ = nullptr;
    VkFormat depth_format_ = VK_FORMAT_D32_SFLOAT_S8_UINT;

    VkSwapchainKHR swap_ = VK_NULL_HANDLE;
    std::vector<Img> swap_images_;
    std::vector<VkSemaphore> render_done_;
    uint32_t bb_w_ = 0, bb_h_ = 0;
    bool swap_vsync_ = true;
    bool swap_usable_ = false;
    bool swap_stale_ = false;
    HWND hwnd_ = nullptr;
    std::array<Img, 4> feedback_;
    uint32_t feedback_next_ = 0;

    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool ui_pool_ = VK_NULL_HANDLE;
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    std::atomic<uint64_t> submitted_presents_{0};

    FrameSlot frames_[kFrames];
    uint64_t frame_ = 0;
    uint64_t presents_ = 0;
    VkCommandBuffer cb_ = VK_NULL_HANDLE;  // the current slot's

    Dxc dxc_;
    std::unordered_map<uint64_t, VkShaderModule> modules_;
    std::unordered_map<uint64_t, uint64_t> shader_src_;       // vertex / pixel key to module hash
    std::unordered_map<uint64_t, VkShaderModule> hash_modules_;  // module hash to module
    std::unordered_map<uint64_t, VkPipeline> psos_;
    std::unordered_map<uint64_t, VkPipeline> blit_psos_;
    std::unordered_map<uint64_t, VkSampler> samplers_;
    VkShaderModule point_gs_ = VK_NULL_HANDLE;
    VkShaderModule sprite_gs_ = VK_NULL_HANDLE;
    VkSampler blit_linear_ = VK_NULL_HANDLE;
    VkSampler blit_point_ = VK_NULL_HANDLE;
    VkSampler dummy_compare_ = VK_NULL_HANDLE;
    Img dummy_2d_, dummy_3d_, dummy_cube_, dummy_depth_;

    uint32_t guest_w_ = 0, guest_h_ = 0;
    float scale_ = 1.0f;
    std::unordered_map<uint64_t, Tgt> targets_;
    std::unordered_map<uint32_t, uint64_t> target_used_;
    std::unordered_map<uint64_t, Tex> textures_;
    Tgt* bound_rt_ = nullptr;
    Tgt* bound_ds_ = nullptr;
    Tgt* draw_rt_ = nullptr;
    Tgt* draw_ds_ = nullptr;
    bool pass_active_ = false;
    Img* pass_color_ = nullptr;
    Img* pass_depth_ = nullptr;
    VkPipeline bound_pipeline_ = VK_NULL_HANDLE;
    Assembled asm_;

    // constants uploaded for the last draw, shared by the next one when they are the same
    std::array<std::array<float, 4>, 192> last_vsc_{};
    DrawConstants last_dc_{};
    Alloc last_vsc_alloc_{};
    Alloc last_dc_alloc_{};
    bool last_vsc_valid_ = false;
    bool last_dc_valid_ = false;

    uint8_t ramp_[768];
    bool ramp_dirty_ = true;
    bool gamma_ = false;
    Img lut_;

    VkBuffer readback_ = VK_NULL_HANDLE;
    VmaAllocation readback_alloc_ = nullptr;
    uint8_t* readback_map_ = nullptr;
    uint64_t readback_size_ = 0;

    double fps_ = 0;
    uint32_t fps_n_ = 0;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
};

// ================================ device ================================

bool VulkanBackend::CreateInstance() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "xbe recomp";
    app.apiVersion = VK_API_VERSION_1_3;
    std::vector<const char*> exts = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    bool validate = GetEnvironmentVariableW(L"XBE_VK_VALIDATE", nullptr, 0) != 0;
    const char* layer = "VK_LAYER_KHRONOS_validation";
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    if (validate) {
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        ci.enabledLayerCount = 1;
        ci.ppEnabledLayerNames = &layer;
    }
    ci.enabledExtensionCount = uint32_t(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    VkResult r = vkCreateInstance(&ci, nullptr, &instance_);
    if (r != VK_SUCCESS && validate) {
        LOGE("vulkan: validation layer unavailable (%d)", int(r));
        validate = false;
        exts.pop_back();
        ci.enabledLayerCount = 0;
        ci.enabledExtensionCount = uint32_t(exts.size());
        r = vkCreateInstance(&ci, nullptr, &instance_);
    }
    if (!Ok(r, "vkCreateInstance")) return false;
    if (validate) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mi.pfnUserCallback = DebugMessage;
        if (create) create(instance_, &mi, nullptr, &messenger_);
    }
    return true;
}

bool VulkanBackend::CreateDevice() {
    hwnd_ = static_cast<HWND>(app::Window());
    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = GetModuleHandleW(nullptr);
    sci.hwnd = hwnd_;
    if (!Ok(vkCreateWin32SurfaceKHR(instance_, &sci, nullptr, &surface_), "vkCreateWin32SurfaceKHR")) return false;

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> devices(n);
    vkEnumeratePhysicalDevices(instance_, &n, devices.data());
    int best_score = -1;
    for (VkPhysicalDevice d : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(d, &props);
        if (props.apiVersion < VK_API_VERSION_1_3) continue;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qs.data());
        int family = -1;
        for (uint32_t i = 0; i < qn && family < 0; i++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &present);
            if ((qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) family = int(i);
        }
        if (family < 0) continue;
        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties(d, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> have(en);
        vkEnumerateDeviceExtensionProperties(d, nullptr, &en, have.data());
        auto has = [&](const char* name) {
            return std::any_of(have.begin(), have.end(), [&](const VkExtensionProperties& e) {
                return !std::strcmp(e.extensionName, name);
            });
        };
        if (!has(VK_KHR_SWAPCHAIN_EXTENSION_NAME) || !has(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) continue;
        int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
                    : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                                                                                 : 1;
        if (score > best_score) {
            best_score = score;
            phys_ = d;
            family_ = uint32_t(family);
            limits_ = props.limits;
            LOGR("Vulkan adapter: %s (API %u.%u.%u)", props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
                 VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));
        }
    }
    if (!phys_) {
        LOGE("vulkan: no Vulkan 1.3 device with swapchain and push descriptor support");
        return false;
    }

    VkPhysicalDeviceFeatures have{};
    vkGetPhysicalDeviceFeatures(phys_, &have);
    if (!have.geometryShader || !have.textureCompressionBC || !have.samplerAnisotropy) {
        LOGE("vulkan: device lacks geometry shaders, BC textures or anisotropy");
    }
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.timelineSemaphore = VK_TRUE;
    f12.pNext = &f13;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.features.geometryShader = have.geometryShader;
    f2.features.samplerAnisotropy = have.samplerAnisotropy;
    f2.features.textureCompressionBC = have.textureCompressionBC;
    f2.features.fillModeNonSolid = have.fillModeNonSolid;
    f2.pNext = &f12;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family_;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pNext = &f2;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = 2;
    di.ppEnabledExtensionNames = dev_exts;
    if (!Ok(vkCreateDevice(phys_, &di, nullptr, &dev_), "vkCreateDevice")) return false;
    vkGetDeviceQueue(dev_, family_, 0, &queue_);
    push_descriptor_ = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(vkGetDeviceProcAddr(dev_, "vkCmdPushDescriptorSetKHR"));
    if (!push_descriptor_) return false;

    // the depth buffer: float depth with stencil, else 24 bit
    constexpr VkFormatFeatureFlags kWant = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                           VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                           VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(phys_, VK_FORMAT_D32_SFLOAT_S8_UINT, &fp);
    depth_format_ = (fp.optimalTilingFeatures & kWant) == kWant ? VK_FORMAT_D32_SFLOAT_S8_UINT
                                                                  : VK_FORMAT_D24_UNORM_S8_UINT;

    VmaAllocatorCreateInfo ai{};
    ai.physicalDevice = phys_;
    ai.device = dev_;
    ai.instance = instance_;
    ai.vulkanApiVersion = VK_API_VERSION_1_3;
    if (!Ok(vmaCreateAllocator(&ai, &vma_), "vmaCreateAllocator")) return false;

    VkSemaphoreTypeCreateInfo ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    si.pNext = &ti;
    return Ok(vkCreateSemaphore(dev_, &si, nullptr, &timeline_), "timeline semaphore");
}

void VulkanBackend::DestroySwapchain() {
    for (Img& im : swap_images_) {
        if (im.view) vkDestroyImageView(dev_, im.view, nullptr);
    }
    swap_images_.clear();
    for (VkSemaphore s : render_done_) vkDestroySemaphore(dev_, s, nullptr);
    render_done_.clear();
}

bool VulkanBackend::CreateSwapchain() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    bb_w_ = uint32_t(std::max<LONG>(rc.right - rc.left, 0));
    bb_h_ = uint32_t(std::max<LONG>(rc.bottom - rc.top, 0));
    swap_vsync_ = app::Get().vsync;
    swap_stale_ = false;
    swap_usable_ = false;
    if (!bb_w_ || !bb_h_) return true;  // minimised

    vkDeviceWaitIdle(dev_);
    VkSurfaceCapabilitiesKHR caps;
    if (!Ok(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps), "surface capabilities")) return false;
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        ext.width = std::clamp(bb_w_, caps.minImageExtent.width, caps.maxImageExtent.width);
        ext.height = std::clamp(bb_h_, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (!ext.width || !ext.height) return true;

    uint32_t mn = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &mn, nullptr);
    std::vector<VkPresentModeKHR> modes(mn);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &mn, modes.data());
    auto has_mode = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!swap_vsync_) {
        if (has_mode(VK_PRESENT_MODE_MAILBOX_KHR)) mode = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (has_mode(VK_PRESENT_MODE_IMMEDIATE_KHR)) mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }

    VkSwapchainKHR old = swap_;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = std::max<uint32_t>(caps.minImageCount, 3);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = kBgra;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    VkSwapchainKHR fresh = VK_NULL_HANDLE;
    VkResult r = vkCreateSwapchainKHR(dev_, &ci, nullptr, &fresh);
    DestroySwapchain();
    if (old) vkDestroySwapchainKHR(dev_, old, nullptr);
    swap_ = VK_NULL_HANDLE;
    if (!Ok(r, "vkCreateSwapchainKHR")) return false;
    swap_ = fresh;

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(dev_, swap_, &count, nullptr);
    std::vector<VkImage> images(count);
    vkGetSwapchainImagesKHR(dev_, swap_, &count, images.data());
    for (VkImage image : images) {
        Img im;
        im.image = image;
        im.format = kBgra;
        im.w = ext.width;
        im.h = ext.height;
        im.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = kBgra;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!Ok(vkCreateImageView(dev_, &vi, nullptr, &im.view), "swapchain view")) return false;
        im.attach = im.view;
        swap_images_.push_back(im);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore done;
        vkCreateSemaphore(dev_, &si, nullptr, &done);
        render_done_.push_back(done);
    }
    swap_usable_ = true;
    return true;
}

void VulkanBackend::ResizeSwap() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    uint32_t w = uint32_t(std::max<LONG>(rc.right - rc.left, 0));
    uint32_t h = uint32_t(std::max<LONG>(rc.bottom - rc.top, 0));
    if (swap_ && !swap_stale_ && w == bb_w_ && h == bb_h_ && app::Get().vsync == swap_vsync_) return;
    if (!swap_ && !w && !h && !swap_stale_) return;
    Flush();
    CreateSwapchain();
}

bool VulkanBackend::CreateFrames() {
    for (FrameSlot& f : frames_) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = family_;
        if (!Ok(vkCreateCommandPool(dev_, &pi, nullptr, &f.pool), "command pool")) return false;
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (!Ok(vkAllocateCommandBuffers(dev_, &ai, &f.cb), "command buffer")) return false;
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (!Ok(vkCreateFence(dev_, &fi, nullptr, &f.fence), "fence")) return false;
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (!Ok(vkCreateSemaphore(dev_, &si, nullptr, &f.acquired), "semaphore")) return false;

        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = kUploadSize;
        bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                   VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo ac{};
        ac.usage = VMA_MEMORY_USAGE_AUTO;
        ac.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        ac.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        VmaAllocationInfo info;
        if (!Ok(vmaCreateBuffer(vma_, &bi, &ac, &f.ring, &f.ring_alloc, &info), "upload ring")) return false;
        f.map = static_cast<uint8_t*>(info.pMappedData);
    }
    return true;
}

bool VulkanBackend::CreateLayouts() {
    VkDescriptorSetLayoutBinding b[kBindCount] = {};
    for (uint32_t i = 0; i < kBindCount; i++) {
        b[i].binding = i;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
        b[i].descriptorType = i < kBindTex    ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                              : i < kBindSmp ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                             : VK_DESCRIPTOR_TYPE_SAMPLER;
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = kBindCount;
    li.pBindings = b;
    if (!Ok(vkCreateDescriptorSetLayout(dev_, &li, nullptr, &set_layout_), "descriptor set layout")) return false;
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &set_layout_;
    return Ok(vkCreatePipelineLayout(dev_, &pi, nullptr, &layout_), "pipeline layout");
}

VkSampler VulkanBackend::EnsureSampler(const SamplerKey& k) {
    uint64_t key = Hash(&k, sizeof(k));
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = k.mag ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.minFilter = k.min ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = k.mip ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VkSamplerAddressMode(k.u);
    si.addressModeV = VkSamplerAddressMode(k.v);
    si.addressModeW = VkSamplerAddressMode(k.w);
    si.mipLodBias = k.bias;
    si.anisotropyEnable = k.aniso > 1 ? VK_TRUE : VK_FALSE;
    si.maxAnisotropy = std::min(k.aniso, std::min(16.0f, limits_.maxSamplerAnisotropy));
    si.compareEnable = k.compare ? VK_TRUE : VK_FALSE;
    si.compareOp = k.compare ? VkCompareOp(k.compare - 1) : VK_COMPARE_OP_ALWAYS;
    si.minLod = k.min_lod;
    si.maxLod = k.max_lod;
    si.borderColor = VkBorderColor(k.border);
    VkSampler s = VK_NULL_HANDLE;
    if (!Ok(vkCreateSampler(dev_, &si, nullptr, &s), "vkCreateSampler")) return VK_NULL_HANDLE;
    samplers_[key] = s;
    return s;
}

// ================================ images ================================

bool VulkanBackend::MakeImage(Img& im, VkImageType type, VkFormat fmt, uint32_t w, uint32_t h, uint32_t d,
                              uint32_t levels, uint32_t layers, VkImageUsageFlags usage, bool cube, bool target) {
    im = Img{};
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ci.imageType = type;
    ci.format = fmt;
    ci.extent = {w, h, d};
    ci.mipLevels = levels;
    ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ac{};
    ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (target) ac.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    if (!Ok(vmaCreateImage(vma_, &ci, &ac, &im.image, &im.alloc, nullptr), "vmaCreateImage")) {
        im = Img{};
        return false;
    }
    bool depth = fmt == VK_FORMAT_D32_SFLOAT || fmt == VK_FORMAT_D16_UNORM || fmt == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                 fmt == VK_FORMAT_D24_UNORM_S8_UINT;
    bool stencil = fmt == VK_FORMAT_D32_SFLOAT_S8_UINT || fmt == VK_FORMAT_D24_UNORM_S8_UINT;
    im.format = fmt;
    im.w = w;
    im.h = h;
    im.levels = levels;
    im.layers = layers;
    im.aspect = depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0))
                      : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.image;
    vi.viewType = cube                        ? VK_IMAGE_VIEW_TYPE_CUBE
                  : type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D
                                              : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    VkImageAspectFlags sampled = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange = {sampled, 0, levels, 0, layers};
    if (!Ok(vkCreateImageView(dev_, &vi, nullptr, &im.view), "vkCreateImageView")) {
        Retire(im);
        return false;
    }
    if (target && depth) {
        vi.subresourceRange.aspectMask = im.aspect;
        vi.subresourceRange.levelCount = 1;
        if (!Ok(vkCreateImageView(dev_, &vi, nullptr, &im.attach), "depth attachment view")) {
            Retire(im);
            return false;
        }
    } else {
        im.attach = im.view;
    }
    return true;
}

void VulkanBackend::Retire(Img& im) {
    if (!im.image) return;
    VkImage image = im.image;
    VmaAllocation alloc = im.alloc;
    VkImageView view = im.view;
    VkImageView attach = im.attach;
    Later([this, image, alloc, view, attach] {
        if (attach && attach != view) vkDestroyImageView(dev_, attach, nullptr);
        if (view) vkDestroyImageView(dev_, view, nullptr);
        vmaDestroyImage(vma_, image, alloc);
    });
    im = Img{};
}

void VulkanBackend::MemoryBarrier() {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cb_, &dep);
}

void VulkanBackend::Transition(Img& im, VkImageLayout to) {
    if (im.layout == to) return;
    EndPass();
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = im.layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im.image;
    b.subresourceRange = {im.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb_, &dep);
    im.layout = to;
}

void VulkanBackend::CopyBufferToImage(const Img& im, const Alloc& a, uint32_t level, uint32_t layer, uint32_t w,
                                      uint32_t h, uint32_t d) {
    VkBufferImageCopy r{};
    r.bufferOffset = a.off;
    const VkImageAspectFlags aspect = im.Depth() ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource = {aspect, level, layer, 1};
    r.imageExtent = {w, h, d};
    vkCmdCopyBufferToImage(cb_, a.buf, im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
}

// ================================ recording ================================

void VulkanBackend::BeginCommands() {
    FrameSlot& f = Slot();
    cb_ = f.cb;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb_, &bi);
    bound_rt_ = bound_ds_ = nullptr;
    pass_active_ = false;
    pass_color_ = pass_depth_ = nullptr;
    bound_pipeline_ = VK_NULL_HANDLE;
    last_vsc_valid_ = last_dc_valid_ = false;
}

void VulkanBackend::RunGarbage(FrameSlot& f) {
    for (auto& g : f.garbage) g();
    f.garbage.clear();
}

void VulkanBackend::Submit(bool wait, VkSemaphore wait_sem, VkSemaphore signal_sem, uint64_t timeline) {
    EndPass();
    FrameSlot& f = Slot();
    Ok(vkEndCommandBuffer(cb_), "vkEndCommandBuffer");
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = cb_;
    VkSemaphoreSubmitInfo waits[1] = {};
    VkSemaphoreSubmitInfo signals[2] = {};
    uint32_t nw = 0, ns = 0;
    if (wait_sem) {
        waits[nw] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        waits[nw].semaphore = wait_sem;
        waits[nw].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        nw++;
    }
    if (signal_sem) {
        signals[ns] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signals[ns].semaphore = signal_sem;
        signals[ns].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        ns++;
    }
    if (timeline) {
        signals[ns] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signals[ns].semaphore = timeline_;
        signals[ns].value = timeline;
        signals[ns].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        ns++;
    }
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = nw;
    si.pWaitSemaphoreInfos = waits;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = ns;
    si.pSignalSemaphoreInfos = signals;
    vkResetFences(dev_, 1, &f.fence);
    Ok(vkQueueSubmit2(queue_, 1, &si, f.fence), "vkQueueSubmit2");
    f.pending = true;
    last_vsc_valid_ = last_dc_valid_ = false;
    cb_ = VK_NULL_HANDLE;
    if (wait) {
        vkWaitForFences(dev_, 1, &f.fence, VK_TRUE, UINT64_MAX);
        f.pending = false;
        RunGarbage(f);
        f.used = 0;
        vkResetCommandPool(dev_, f.pool, 0);
        BeginCommands();
    }
}

// the next slot, once the GPU is done with it
void VulkanBackend::NextFrame() {
    frame_++;
    FrameSlot& f = Slot();
    if (f.pending) {
        vkWaitForFences(dev_, 1, &f.fence, VK_TRUE, UINT64_MAX);
        f.pending = false;
    }
    RunGarbage(f);
    f.used = 0;
    vkResetCommandPool(dev_, f.pool, 0);
    BeginCommands();
}

VulkanBackend::Alloc VulkanBackend::Upload(uint64_t size, uint64_t align) {
    FrameSlot& f = Slot();
    uint64_t off = (f.used + align - 1) & ~(align - 1);
    if (off + size <= kUploadSize) {
        f.used = off + size;
        return {f.ring, off, f.map + off};
    }
    // past the ring: a buffer of its own, freed with the frame
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = std::max<uint64_t>(size, 16);
    bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
               VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo ac{};
    ac.usage = VMA_MEMORY_USAGE_AUTO;
    ac.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    ac.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VmaAllocationInfo info;
    if (!Ok(vmaCreateBuffer(vma_, &bi, &ac, &buf, &alloc, &info), "oversize upload")) {
        LOGE("vulkan: out of upload memory");
        std::abort();
    }
    Later([this, buf, alloc] { vmaDestroyBuffer(vma_, buf, alloc); });
    return {buf, 0, static_cast<uint8_t*>(info.pMappedData)};
}

void VulkanBackend::EndPass() {
    if (!pass_active_) return;
    vkCmdEndRendering(cb_);
    pass_active_ = false;
}

void VulkanBackend::BeginPass(Img* color, Img* depth, const float* clear_color) {
    if (pass_active_ && pass_color_ == color && pass_depth_ == depth && !clear_color) return;
    EndPass();
    if (color) Transition(*color, kAttachLayout);
    if (depth) Transition(*depth, kDepthAttachLayout);
    MemoryBarrier();
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    const Img& ref = color ? *color : *depth;
    ri.renderArea = {{0, 0}, {ref.w, ref.h}};
    ri.layerCount = 1;
    if (color) {
        ca.imageView = color->attach;
        ca.imageLayout = kAttachLayout;
        ca.loadOp = clear_color ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (clear_color) std::memcpy(ca.clearValue.color.float32, clear_color, 4 * sizeof(float));
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
    }
    if (depth) {
        da.imageView = depth->attach;
        da.imageLayout = kDepthAttachLayout;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.pDepthAttachment = &da;
        ri.pStencilAttachment = &da;
    }
    vkCmdBeginRendering(cb_, &ri);
    pass_active_ = true;
    pass_color_ = color;
    pass_depth_ = depth;
}

// ================================ shaders and pipelines ================================

constexpr uint64_t kSpirvCacheVersion = 1;  // bump when the DXC arguments change

VkShaderModule VulkanBackend::Module(const std::string& src, const char* entry, const char* profile, uint64_t& hash) {
    hash = Hash(src.data(), src.size(), Hash(entry, std::strlen(entry), Hash(profile, std::strlen(profile), kSpirvCacheVersion)));
    auto it = hash_modules_.find(hash);
    if (it != hash_modules_.end()) return it->second;
    VkShaderModule module = VK_NULL_HANDLE;
    std::vector<uint32_t> code;
    char name[48];
    std::snprintf(name, sizeof(name), "vk_%016llx.spv", static_cast<unsigned long long>(hash));
    std::error_code ec;
    std::filesystem::path cache = app::ExeDir() / "cache";
    std::filesystem::path file = cache / name;
    if (std::ifstream in{file, std::ios::binary | std::ios::ate}) {
        size_t bytes = size_t(in.tellg());
        if (bytes && bytes % 4 == 0) {
            code.resize(bytes / 4);
            in.seekg(0);
            in.read(reinterpret_cast<char*>(code.data()), std::streamsize(bytes));
            if (!in) code.clear();
        }
    }
    if (code.empty()) {
        std::string err;
        if (!dxc_.Compile(src, entry, profile, code, err)) {
            std::filesystem::path dump = app::ExeDir() / "logs" / (std::string("vk_failed_") + name + ".hlsl");
            std::filesystem::create_directories(dump.parent_path(), ec);
            if (std::ofstream out{dump, std::ios::binary}) out << "// " << entry << " " << profile << "\n" << src;
            LOGE("vulkan: DXC failed for %s (%s), source in %s: %s", entry, profile, dump.string().c_str(), err.c_str());
            hash_modules_[hash] = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
        std::filesystem::create_directories(cache, ec);
        if (std::ofstream out{file, std::ios::binary}) {
            out.write(reinterpret_cast<const char*>(code.data()), std::streamsize(code.size() * 4));
        }
    }
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = code.size() * 4;
    mi.pCode = code.data();
    if (!Ok(vkCreateShaderModule(dev_, &mi, nullptr, &module), "vkCreateShaderModule")) module = VK_NULL_HANDLE;
    hash_modules_[hash] = module;
    return module;
}

void VulkanBackend::EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh) {
    auto it = shader_src_.find(sh.vkey);
    if (it == shader_src_.end()) {
        std::string src = ReversedDepthHlsl(VertexHlsl(s, a, sh));
        if (sh.quad) src = QuadHlsl(std::move(src));
        uint64_t h = 0;
        Module(src, "main", "vs_6_0", h);
        it = shader_src_.emplace(sh.vkey, h).first;
    }
    sh.vh = it->second;
    sh.vs_code = hash_modules_[sh.vh];
}

void VulkanBackend::EnsurePixelShader(const PixelKey& pk, DrawShaders& sh) {
    auto it = shader_src_.find(sh.pkey ^ kPixelKeySalt);
    if (it == shader_src_.end()) {
        std::string src = PixelHlsl(pk);  // PixelHlsl already applies QuadHlsl
        uint64_t h = 0;
        Module(src, "main", "ps_6_0", h);
        it = shader_src_.emplace(sh.pkey ^ kPixelKeySalt, h).first;
    }
    sh.ph = it->second;
    sh.ps_code = hash_modules_[sh.ph];
}

VkPipeline VulkanBackend::EnsurePipeline(const PsoKey& k, const Assembled& a, VkShaderModule vs, VkShaderModule ps) {
    uint64_t key = Hash(&k, sizeof(k));
    auto it = psos_.find(key);
    if (it != psos_.end()) return it->second;

    VkShaderModule gs = VK_NULL_HANDLE;
    if (a.topology == Topology::Points) {
        VkShaderModule& slot = k.sprite ? sprite_gs_ : point_gs_;
        if (!slot) {
            uint64_t h;
            slot = Module(k.sprite ? SpriteHlsl() : PointHlsl(), "main", "gs_6_0", h);
        }
        gs = slot;
        if (!gs) return psos_[key] = VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo stages[3] = {};
    uint32_t ns = 0;
    auto stage = [&](VkShaderStageFlagBits bit, VkShaderModule m) {
        stages[ns] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[ns].stage = bit;
        stages[ns].module = m;
        stages[ns].pName = "main";
        ns++;
    };
    stage(VK_SHADER_STAGE_VERTEX_BIT, vs);
    if (gs) stage(VK_SHADER_STAGE_GEOMETRY_BIT, gs);
    stage(VK_SHADER_STAGE_FRAGMENT_BIT, ps);

    VkVertexInputAttributeDescription attrs[16];
    VkVertexInputBindingDescription binds[16];
    uint32_t na = 0, nb = 0;
    bool used[16] = {};
    for (int r = 0; r < 16; r++) {
        if (!(a.regs >> r & 1)) continue;
        const VertexElement& e = a.elements[size_t(r)];
        attrs[na] = {na, e.buffer, VertexFormat(e.format), e.offset};
        na++;
        used[e.buffer] = true;
    }
    for (uint32_t i = 0; i < 16; i++) {
        if (used[i]) binds[nb++] = {i, 0, VK_VERTEX_INPUT_RATE_VERTEX};
    }
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vin.vertexBindingDescriptionCount = nb;
    vin.pVertexBindingDescriptions = binds;
    vin.vertexAttributeDescriptionCount = na;
    vin.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    if (a.topology == Topology::Points) ia.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    else if (a.topology == Topology::Lines) ia.topology = a.strip ? VK_PRIMITIVE_TOPOLOGY_LINE_STRIP : VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    else ia.topology = a.strip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    bool points = a.topology == Topology::Points;
    rs.polygonMode = !points && k.fill == kFillWireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = k.cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = !points && k.ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.depthBiasEnable = k.zslope != 0 ? VK_TRUE : VK_FALSE;
    rs.depthBiasSlopeFactor = k.zslope;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = k.zen ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = k.zen && k.zwrite ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = ReversedCmp(k.zfunc);
    ds.stencilTestEnable = k.sten ? VK_TRUE : VK_FALSE;
    ds.front.failOp = StencilOp(k.sfail);
    ds.front.passOp = StencilOp(k.spass);
    ds.front.depthFailOp = StencilOp(k.szfail);
    ds.front.compareOp = Cmp(k.sfunc);
    ds.front.compareMask = k.smask;
    ds.front.writeMask = k.swmask;
    ds.back = ds.front;

    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = k.blend ? VK_TRUE : VK_FALSE;
    cba.srcColorBlendFactor = Blend(k.src, false);
    cba.dstColorBlendFactor = Blend(k.dst, false);
    cba.colorBlendOp = BlendOp(k.op);
    cba.srcAlphaBlendFactor = Blend(k.src, true);
    cba.dstAlphaBlendFactor = Blend(k.dst, true);
    cba.alphaBlendOp = BlendOp(k.op);
    cba.colorWriteMask = k.wmask;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = k.has_rt ? 1 : 0;
    cb.pAttachments = &cba;

    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                  VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = uint32_t(std::size(dyn));
    dy.pDynamicStates = dyn;

    VkFormat color = kBgra;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = k.has_rt ? 1 : 0;
    ri.pColorAttachmentFormats = &color;
    if (k.has_ds) ri.depthAttachmentFormat = ri.stencilAttachmentFormat = VkFormat(k.depth_format);

    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.pNext = &ri;
    pi.stageCount = ns;
    pi.pStages = stages;
    pi.pVertexInputState = &vin;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = layout_;
    VkPipeline pso = VK_NULL_HANDLE;
    if (!Ok(vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &pi, nullptr, &pso), "vkCreateGraphicsPipelines")) {
        pso = VK_NULL_HANDLE;
    }
    psos_[key] = pso;
    return pso;
}

VkPipeline VulkanBackend::EnsureFullscreenPipeline(const char* src, uint32_t mask, bool over, uint64_t seed) {
    uint64_t key = Hash(src, std::strlen(src), seed << 8 | mask << 1 | (over ? 1 : 0));
    auto it = blit_psos_.find(key);
    if (it != blit_psos_.end()) return it->second;
    uint64_t h;
    VkShaderModule vs = Module(src, "vs", "vs_6_0", h);
    VkShaderModule ps = Module(src, "psmain", "ps_6_0", h);
    if (!vs || !ps) return blit_psos_[key] = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "vs";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps;
    stages[1].pName = "psmain";
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = mask;
    if (over) {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkFormat color = kBgra;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &color;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.pNext = &ri;
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vin;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = layout_;
    VkPipeline pso = VK_NULL_HANDLE;
    if (!Ok(vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &pi, nullptr, &pso), "fullscreen pipeline")) {
        pso = VK_NULL_HANDLE;
    }
    blit_psos_[key] = pso;
    return pso;
}

// a fullscreen triangle in the current pass; the sources are already in their read layouts
void VulkanBackend::FullScreen(VkPipeline pso, const Img* t0, const Img* t1, VkSampler s0, VkSampler s1, const float* cb,
                               uint32_t cb_bytes) {
    Alloc a = Upload(std::max<uint32_t>(cb_bytes, 16), 256);
    std::memcpy(a.cpu, cb, cb_bytes);
    vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
    bound_pipeline_ = pso;
    VkDescriptorBufferInfo bi{a.buf, a.off, std::max<uint32_t>(cb_bytes, 16)};
    const Img* srcs[2] = {t0 ? t0 : &dummy_2d_, t1 ? t1 : &dummy_2d_};
    VkDescriptorImageInfo ii[2], si[2];
    for (int i = 0; i < 2; i++) {
        ii[i] = {VK_NULL_HANDLE, srcs[i]->view, ReadLayout(*srcs[i])};
        si[i] = {i ? (s1 ? s1 : blit_point_) : (s0 ? s0 : blit_point_), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    }
    VkWriteDescriptorSet w[5] = {};
    for (auto& x : w) x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstBinding = kBindDc;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[0].pBufferInfo = &bi;
    for (int i = 0; i < 2; i++) {
        w[1 + i].dstBinding = kBindTex + uint32_t(i);
        w[1 + i].descriptorCount = 1;
        w[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w[1 + i].pImageInfo = &ii[i];
        w[3 + i].dstBinding = kBindSmp + uint32_t(i);
        w[3 + i].descriptorCount = 1;
        w[3 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w[3 + i].pImageInfo = &si[i];
    }
    push_descriptor_(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 5, w);
    vkCmdDraw(cb_, 3, 1, 0, 0);
}

// ================================ targets ================================

// a smaller depth surface laid on the depth buffer's memory is a target of its own
uint64_t VulkanBackend::SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const {
    uint64_t key = TargetMapKey(data, depth);
    bool small_depth = depth && (gw != guest_w_ || gh != guest_h_);
    if (!small_depth) return key;
    return key | uint64_t(gw) << kKeyWidthShift | uint64_t(gh) << kKeyHeightShift;
}

// the surface's target at its size, null when there is none
Tgt* VulkanBackend::FindTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint32_t data = mem::VirtualFromPhysical(s.data);
    uint32_t gw, gh;
    SurfaceSize(s, gw, gh);
    auto it = targets_.find(SizedTargetKey(data, depth, gw, gh));
    if (it == targets_.end() || it->second.gw != gw || it->second.gh != gh) return nullptr;
    UpdateTargetUsed(data);
    return &it->second;
}

void VulkanBackend::UpdateTargetUsed(uint32_t data) { target_used_[data] = frame_; }

// long unused: the title freed it and other data is at this address now
bool VulkanBackend::TargetStale(uint32_t data) const {
    auto it = target_used_.find(data);
    uint64_t used = it == target_used_.end() ? 0 : it->second;
    return frame_ - used > kStaleFrames;
}

void VulkanBackend::ReleaseTarget(std::unordered_map<uint64_t, Tgt>::iterator it) {
    EndPass();
    Tgt* t = &it->second;
    if (bound_rt_ == t) bound_rt_ = nullptr;
    if (bound_ds_ == t) bound_ds_ = nullptr;
    if (draw_rt_ == t) draw_rt_ = nullptr;
    if (draw_ds_ == t) draw_ds_ = nullptr;
    Retire(t->img);
    targets_.erase(it);
}

// the surface's target, made again at its size when that changed
Tgt* VulkanBackend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint32_t data = mem::VirtualFromPhysical(s.data);
    uint32_t gw, gh;
    SurfaceSize(s, gw, gh);
    uint64_t key = SizedTargetKey(data, depth, gw, gh);
    UpdateTargetUsed(data);
    auto it = targets_.find(key);
    if (it != targets_.end()) {
        if (it->second.gw == gw && it->second.gh == gh) return &it->second;
        ReleaseTarget(it);
    }
    Tgt t;
    t.depth = depth;
    t.gw = gw;
    t.gh = gh;
    t.w = std::max(1u, uint32_t(std::lround(gw * scale_)));
    t.h = std::max(1u, uint32_t(std::lround(gh * scale_)));
    VkImageUsageFlags usage = (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!MakeImage(t.img, VK_IMAGE_TYPE_2D, depth ? depth_format_ : kBgra, t.w, t.h, 1, 1, 1, usage, false, true)) {
        return nullptr;
    }
    Transition(t.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageSubresourceRange range{t.img.aspect, 0, 1, 0, 1};
    if (depth) {
        VkClearDepthStencilValue v{kDepthFar, 0};
        vkCmdClearDepthStencilImage(cb_, t.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
    } else {
        VkClearColorValue v{};
        vkCmdClearColorImage(cb_, t.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
    }
    t.data = data;
    t.bytes = depth ? gw * gh * 4 : GuestPitch(gw) * gh;
    t.epoch = DirtyEpoch(data);
    t.sig = MemSig(data, t.bytes);
    return &(targets_[key] = std::move(t));
}

bool VulkanBackend::BindTargets(const State& s) {
    Tgt* rt = EnsureTarget(s.rt, false);
    Tgt* ds = s.rs[kRsZEnable] || s.rs[kRsStencilEnable] ? EnsureTarget(s.zs, true) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h)) ds = nullptr;
    if (!rt && !ds) return false;
    if (rt) {
        if (LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
        rt->drawn = true;
    }
    if (ds) ds->drawn = true;
    bound_rt_ = rt;
    bound_ds_ = ds;
    return true;
}

float VulkanBackend::TargetScale() const {
    const Tgt* t = bound_rt_ ? bound_rt_ : bound_ds_;
    if (!t || !t->gw) return 1;
    return float(t->w) / float(t->gw);
}

// the XDK's screen space offset puts the console's positions half a pixel right and down of the host's, unless a
// program writes them without c-37; render target reads move back that far (TexUv)
float VulkanBackend::RenderTargetShift(const State& s, const DrawShaders& sh) const {
    bool offset = !sh.prog || ReadsViewportOffset(s.vs->program);
    return offset ? -0.5f * TargetScale() : 0.0f;
}

// the pixels the CPU changed since the last Sync, opaque, the rest 0; t.read takes them
std::vector<uint8_t> LatchCpuChanges(Tgt& t) {
    std::vector<uint8_t> px(t.read.size(), 0);
    uint32_t guest_pitch = GuestPitch(t.gw);
    const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        for (uint32_t x = 0; x < t.gw; x++) {
            const uint8_t* c = guest + size_t(y) * guest_pitch + x * 4;
            size_t i = (size_t(y) * t.gw + x) * 4;
            if (!std::memcmp(c, &t.read[i], 4)) continue;
            std::memcpy(&t.read[i], c, 4);
            std::memcpy(&px[i], c, 3);
            px[i + 3] = 255;
        }
    }
    return px;
}

// the whole target as the pass's viewport and scissor
void VulkanBackend::SetPassViewport(uint32_t w, uint32_t h) {
    VkViewport vp{0, float(h), float(w), -float(h), 0, 1};
    VkRect2D sc{{0, 0}, {w, h}};
    vkCmdSetViewport(cb_, 0, 1, &vp);
    vkCmdSetScissor(cb_, 0, 1, &sc);
}

// BGRA pixels into a new w x h texture, left as a shader resource
Img VulkanBackend::MakePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h) {
    Img im;
    if (!MakeImage(im, VK_IMAGE_TYPE_2D, kBgra, w, h, 1, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   false, false)) {
        return Img{};
    }
    Alloc a = Upload(uint64_t(w) * h * 4, 16);
    for (uint32_t y = 0; y < h; y++) {
        size_t at = size_t(y) * pitch;
        if (at + w * 4 <= px.size()) std::memcpy(a.cpu + size_t(y) * w * 4, &px[at], w * 4);
        else std::memset(a.cpu + size_t(y) * w * 4, 0, w * 4);
    }
    Transition(im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    CopyBufferToImage(im, a, 0, 0, w, h, 1);
    Transition(im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return im;
}

// src, in its read layout, drawn over the pass's viewport
void VulkanBackend::Blit(const Img& src, const float uvrect[4], const BlitOptions& o) {
    VkPipeline pso = EnsureFullscreenPipeline(kBlitHlsl, 0xF, o.over, 0);
    if (!pso) return;
    float c[8] = {uvrect[0], uvrect[1], uvrect[2], uvrect[3], o.gamma ? 1.0f : 0.0f, o.alpha ? 1.0f : 0.0f,
                  float(o.taps), o.mul};
    FullScreen(pso, &src, &lut_, blit_linear_, blit_point_, c, sizeof(c));
}

// CPU pixels of a color surface into its host target, scaled
void VulkanBackend::UploadTarget(Tgt& t, const Surface& s) {
    TextureDesc d;
    if (!Describe(s.header, d) || Format(d.format).depth) return;
    std::vector<uint8_t> px;
    uint32_t pitch;
    // after a Sync only the pixels the CPU changed go over the picture, which stays at render scale
    bool over = !t.drawn && d.width == t.gw && d.height == t.gh && t.read.size() == size_t(t.gw) * t.gh * 4;
    if (over) {
        LOG_FIRST(4, "vulkan: CPU pixels drawn over target %08X", t.data);
        pitch = t.gw * 4;
        px = LatchCpuChanges(t);
    } else {
        DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    }
    Img tex = MakePixelTexture(px, pitch, d.width, d.height);
    if (!tex.image) return;
    BeginPass(&t.img, nullptr);
    SetPassViewport(t.w, t.h);
    float uv[4] = {0, 0, 1, 1};
    Blit(tex, uv, {.alpha = true, .over = over});
    Retire(tex);
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
}

// the target's pixels at the console's size, BGRA; false when it can't be read
bool VulkanBackend::ReadBack(Tgt& t, std::vector<uint8_t>& px) {
    Img shrunk;
    Img* src = &t.img;
    if (t.w != t.gw || t.h != t.gh) {
        if (!MakeImage(shrunk, VK_IMAGE_TYPE_2D, kBgra, t.gw, t.gh, 1, 1, 1,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                       false, false)) {
            return false;
        }
        Transition(t.img, ReadLayout(t.img));
        BeginPass(&shrunk, nullptr);
        SetPassViewport(t.gw, t.gh);
        float uv[4] = {0, 0, 1, 1};
        uint32_t taps = std::max(1u, uint32_t(std::ceil(float(t.w) / float(t.gw))));
        Blit(t.img, uv, {.alpha = true, .taps = taps});
        src = &shrunk;
    }
    Transition(*src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    uint64_t bytes = uint64_t(t.gw) * t.gh * 4;
    if (readback_size_ < bytes) {
        if (readback_) vmaDestroyBuffer(vma_, readback_, readback_alloc_);
        readback_ = VK_NULL_HANDLE;
        readback_size_ = 0;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo ac{};
        ac.usage = VMA_MEMORY_USAGE_AUTO;
        ac.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info;
        if (!Ok(vmaCreateBuffer(vma_, &bi, &ac, &readback_, &readback_alloc_, &info), "readback buffer")) {
            Retire(shrunk);
            return false;
        }
        readback_map_ = static_cast<uint8_t*>(info.pMappedData);
        readback_size_ = bytes;
    }
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {t.gw, t.gh, 1};
    vkCmdCopyImageToBuffer(cb_, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_, 1, &r);
    Submit(true);
    vmaInvalidateAllocation(vma_, readback_alloc_, 0, VK_WHOLE_SIZE);
    px.assign(readback_map_, readback_map_ + bytes);
    Retire(shrunk);
    return true;
}

// the CPU reads a surface the GPU drew: bring the pixels back
void VulkanBackend::Sync(uint32_t data, uint32_t) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || !it->second.drawn || TargetStale(data)) return;
    Tgt& t = it->second;
    std::vector<uint8_t> px;
    if (!ReadBack(t, px)) return;
    // color targets are 32 bit linear in guest memory, the pitch rounds to 64 bytes
    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    for (uint32_t y = 0; y < t.gh; y++) std::memcpy(guest + size_t(y) * pitch, &px[size_t(y) * t.gw * 4], t.gw * 4);
    t.read = std::move(px);
    t.drawn = false;
    t.sig = MemSig(t.data, t.bytes);
}

// ================================ textures ================================

constexpr float kUpscaledMin = 1.01f;  // host texels per console texel past rounding: the target is upscaled

// the console's texel copy of a target the draw both reads and writes
const Img* VulkanBackend::Feedback(Tgt& t) {
    Img& f = feedback_[feedback_next_++ % feedback_.size()];
    if (!f.image || f.w != t.w || f.h != t.h || f.format != t.img.format) {
        Retire(f);
        if (!MakeImage(f, VK_IMAGE_TYPE_2D, t.img.format, t.w, t.h, 1, 1, 1,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false, false)) {
            return nullptr;
        }
    }
    Transition(t.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(f, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy r{};
    r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.extent = {t.w, t.h, 1};
    vkCmdCopyImage(cb_, t.img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, f.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &r);
    Transition(f, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return &f;
}

const Img* VulkanBackend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, Tgt& t, bool box,
                                            DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    if (&t == draw_ds_) return nullptr;
    bool feedback = &t == draw_rt_;
    stages.rt[stage] = true;
    info.flags |= kTexTarget;
    if (!t.depth && LatchCpuWrite(t)) {
        Surface su;
        su.header = s.textures[size_t(stage)];
        UploadTarget(t, su);
    }
    info.kind = kTex2D;
    // a pass shrinking it averages each tap over a console texel, else upscaled taps skip texels and ghost
    Tgt* rt = FindTarget(s.rt, false);
    float k = float(t.w) / (t.gw * stages.k[stage]);
    if (!t.depth && box && k > kUpscaledMin && rt && rt != &t && (t.gw > rt->gw || t.gh > rt->gh)) {
        stages.wide[stage] = k;
        info.flags |= kTexWide;
    }
    // the console reads the pixels it is about to overwrite: give the draw a copy
    if (feedback) return Feedback(t);
    Transition(t.img, ReadLayout(t.img));
    if (t.depth) info.shadow = ShadowDepthKind(d.format);
    return &t.img;
}

// false when the texture could not be made, its entry dropped
bool VulkanBackend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key) {
    Retire(tx.img);
    tx = Tex{};
    tx.d = d;
    uint32_t w = d.width;
    uint32_t h = d.height;
    tx.levels = d.levels;
    if (fi.compressed && ((w & 3) || (h & 3))) {
        w = (w + 3) & ~3u;
        h = (h + 3) & ~3u;
        tx.levels = 1;
    }
    bool volume = d.depth > 1;
    if (!MakeImage(tx.img, volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, HostFormat(fi.host), w, h, volume ? d.depth : 1,
                   tx.levels, d.cube ? 6 : 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, d.cube,
                   false)) {
        textures_.erase(key);
        return false;
    }
    return true;
}

// bytes per row and rows of a level in the upload buffer, which is tightly packed
struct LevelBytes {
    uint32_t row;
    uint32_t rows;
};

LevelBytes LevelLayout(VkFormat f, uint32_t w, uint32_t h) {
    switch (f) {
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return {((w + 3) / 4) * 8, (h + 3) / 4};
        case VK_FORMAT_BC2_UNORM_BLOCK:
        case VK_FORMAT_BC3_UNORM_BLOCK: return {((w + 3) / 4) * 16, (h + 3) / 4};
        case VK_FORMAT_D16_UNORM: return {w * 2, h};
        default: return {w * 4, h};
    }
}

void VulkanBackend::UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo&, const uint8_t* p,
                                  const uint32_t* colours) {
    Transition(tx.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    uint32_t faces = d.cube ? 6 : 1;
    bool volume = d.depth > 1;
    std::vector<uint8_t> px;
    for (uint32_t f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < tx.levels; l++) {
            uint32_t pitch;
            DecodeLevel(d, l, f, p, colours, px, pitch);
            uint32_t lw = std::max(1u, tx.img.w >> l);
            uint32_t lh = std::max(1u, tx.img.h >> l);
            uint32_t ld = volume ? std::max(1u, d.depth >> l) : 1;
            LevelBytes lb = LevelLayout(tx.img.format, lw, lh);
            Alloc a = Upload(uint64_t(lb.row) * lb.rows * ld, 16);
            size_t src_rows = pitch ? px.size() / pitch / ld : 0;
            for (uint32_t z = 0; z < ld; z++) {
                for (uint32_t y = 0; y < lb.rows; y++) {
                    uint8_t* dst = a.cpu + (size_t(z) * lb.rows + y) * lb.row;
                    size_t n = y < src_rows ? std::min<size_t>(pitch, lb.row) : 0;
                    if (n) std::memcpy(dst, &px[(size_t(z) * src_rows + y) * pitch], n);
                    if (n < lb.row) std::memset(dst + n, 0, lb.row - n);
                }
            }
            CopyBufferToImage(tx.img, a, l, f, lw, lh, ld);
        }
    }
    Transition(tx.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// a texture in guest memory: made again when its layout changed, uploaded again when its bytes did
const Img* VulkanBackend::BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                           DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    StagePalette palette;
    ReadStagePalette(s, stage, d, palette);
    const uint64_t key = uint64_t(palette.va) << 32 | d.data;
    const uint32_t* colours = palette.va ? palette.colours : nullptr;
    Tex& tx = textures_[key];
    uint32_t total = TotalSize(d);
    bool same = SameTexture(tx, d);
    if (!same && !CreateTexture(tx, d, fi, key)) return nullptr;
    // cheap change check once per frame: lock epochs plus a sample of the bytes
    if (tx.frame != presents_ + 1 || !same || tx.palette != palette.hash) {
        tx.frame = presents_ + 1;
        tx.palette = palette.hash;
        const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
        uint64_t sig = MemSig(d.data, total) ^ DirtyEpoch(d.data) ^ palette.hash;
        if (sig != tx.sig || !same) {
            tx.sig = sig;
            UploadTexture(tx, d, fi, p, colours);
        }
    }
    if (fi.depth) info.shadow = ShadowDepthKind(d.format);
    return &tx.img;
}

const Img* VulkanBackend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    info = {};
    stages.img[stage] = nullptr;
    stages.rt[stage] = false;
    stages.k[stage] = 1;
    stages.wide[stage] = 0;
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return nullptr;
    const FormatInfo& fi = Format(d.format);
    info.kind = TextureKind(d);
    info.rect = d.linear;
    // rendered by the GPU: sample the target itself; DXT data loaded over a target's memory is not it
    auto tit = targets_.find(SizedTargetKey(d.data, fi.depth, d.width, d.height));
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed) {
        return BindTargetTexture(s, stage, d, tit->second, true, stages);
    }
    return BindGuestTexture(s, stage, d, fi, stages);
}

bool VulkanBackend::BindStageTexturesAndTargets(const State& s, DrawTextures& stages) {
    draw_rt_ = bound_rt_;
    draw_ds_ = bound_ds_;
    for (int i = 0; i < 4; i++) stages.img[i] = BindTexture(s, i, stages);
    // a texture may have made a target again
    if (bound_rt_ != draw_rt_ || bound_ds_ != draw_ds_) return BindTargets(s);
    return true;
}

void VulkanBackend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;    // presents between sweeps
    constexpr size_t kTexturesKept = 2048;  // no sweep below this many
    constexpr uint64_t kIdleFrames = 600;
    // textures unused for ten seconds go, levels stream theirs in and out
    if (presents_ % kSweepEvery != 0 || textures_.size() <= kTexturesKept) return;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.frame + kIdleFrames < presents_) {
            Retire(it->second.img);
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }
}

// ================================ draws ================================

void VulkanBackend::KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k) {
    k.zen = s.rs[kRsZEnable] != 0;
    if (k.zen) {
        k.zwrite = SimpleState(s, kMZWriteEnable) != 0;
        k.zfunc = SimpleState(s, kMZFunc);
    }
    k.sten = s.rs[kRsStencilEnable] != 0;
    if (k.sten) {
        k.sfunc = SimpleState(s, kMStencilFunc);
        k.smask = SimpleState(s, kMStencilMask) & 0xFF;
        k.swmask = SimpleState(s, kMStencilWriteMask) & 0xFF;
        k.sfail = s.rs[kRsStencilFail];
        k.szfail = SimpleState(s, kMStencilZFail);
        k.spass = SimpleState(s, kMStencilPass);
    }
    // polygon offset: the slope here, toward the camera is up on reversed depth; the units are in ReversedDepthHlsl
    if (OffsetsPolygons(s, a.topology)) {
        float scale = bound_ds_ && bound_ds_->gw ? float(bound_ds_->w) / bound_ds_->gw : 1.0f;
        k.zslope = -Bits(SimpleState(s, kMSlopeScale)) * scale;
    }
}

void VulkanBackend::BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k) {
    std::memset(&k, 0, sizeof(k));
    k.vs = vh;
    k.ps = ph;
    k.regs = a.regs;
    for (int r = 0; r < 16; r++) {
        if ((a.regs >> r) & 1) {
            const VertexElement& e = a.elements[size_t(r)];
            k.elements[r] = e.format | uint32_t(e.buffer) << 8 | uint32_t(e.offset) << 16;
        }
    }
    k.topo = uint32_t(a.topology);
    k.strip = a.strip ? 1 : 0;
    k.has_rt = bound_rt_ != nullptr;
    k.has_ds = bound_ds_ != nullptr;
    k.depth_format = uint32_t(depth_format_);
    KeyBlend(s, k);
    if (k.has_ds) KeyDepthStencil(s, a, k);
    KeyRasterizer(s, a, k);
}

SamplerKey VulkanBackend::StageSamplerKey(const State& s, int i, const DrawTextures& stages, bool clamp,
                                          uint32_t aniso) {
    SamplerKey k{};
    const auto& t = s.tss[size_t(i)];
    uint32_t mag = t[kTssMagFilter];
    uint32_t min = t[kTssMinFilter];
    uint32_t mip = t[kTssMipFilter];
    auto lin = [](uint32_t f) { return f >= kTexfLinear; };
    k.mag = lin(mag);
    k.min = lin(min);
    k.mip = mip == kTexfLinear;
    bool anisotropic = min == kTexfAnisotropic || mag == kTexfAnisotropic;
    uint32_t an = anisotropic ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1) : 1;
    // forced only where the title filters across mip levels and the texture has them
    bool mipped = stages.img[i] && stages.img[i]->levels > 1;
    if (aniso && lin(min) && mip && mipped && !stages.info[i].shadow) an = aniso;
    k.aniso = an > 1 ? float(std::min<uint32_t>(an, 16)) : 0.0f;
    // clamp: the draw's coordinates all lie in 0..1 (UnitTexcoords), so wrap is read as clamp
    uint32_t u = clamp && t[kTssAddressU] == kTaddressWrap ? kTaddressClamp : t[kTssAddressU];
    uint32_t v = clamp && t[kTssAddressV] == kTaddressWrap ? kTaddressClamp : t[kTssAddressV];
    uint32_t w = t[kTssAddressW];
    k.u = Address(u);
    k.v = Address(v);
    k.w = Address(w);
    k.bias = Bits(t[kTssMipLodBias]);
    k.min_lod = float(t[kTssMaxMipLevel]);
    k.max_lod = mip ? VK_LOD_CLAMP_NONE : k.min_lod;
    k.border = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (u == 4 || v == 4 || w == 4) {
        float c[4];
        Color(t[kTssBorderColor], c);
        if (c[3] < 0.5f) k.border = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        else if (c[0] + c[1] + c[2] < 0.5f) k.border = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        else k.border = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    }
    if (stages.info[i].shadow) {
        // the console tests texel OP r, Vulkan tests r OP texel; with both reversed (g_reversed_depth) it is the same OP
        k.mag = k.min = lin(min) || lin(mag);
        k.mip = 0;
        k.aniso = 0;
        k.max_lod = k.min_lod;
        k.compare = uint32_t(Cmp(s.rs[kRsShadowFunc])) + 1;
    }
    return k;
}

void VulkanBackend::FillTextureConstants(const State& s, const DrawShaders& sh, const DrawTextures& stages,
                                         DrawConstants& c) {
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (s.textures[size_t(i)] && Describe(s.textures[size_t(i)], d)) FillTexelConstants(i, d, stages, c);
        if (stages.rt[i]) c.rtshift[i] = RenderTargetShift(s, sh);
        const auto& t = s.tss[size_t(i)];
        c.bump[i][0] = Bits(t[kTssBumpEnvMat00]);
        c.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
        c.bump[i][2] = Bits(t[kTssBumpEnvMat10]);
        c.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
        c.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]);
        c.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
    }
}

constexpr uint32_t kVsConstantBytes = 192 * 16;
constexpr uint32_t kDrawConstantBytes = (uint32_t(sizeof(DrawConstants)) + 15) & ~15u;

void VulkanBackend::UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                                    GuestExtent size, DrawConstants& c) {
    bool same = last_vsc_valid_ && !std::memcmp(last_vsc_.data(), s.vsc.data(), sizeof(s.vsc));
    if (!same) {
        Alloc vc = Upload(kVsConstantBytes, 256);
        std::memcpy(vc.cpu, s.vsc.data(), sizeof(s.vsc));
        std::memcpy(last_vsc_.data(), s.vsc.data(), sizeof(s.vsc));
        last_vsc_alloc_ = vc;
        last_vsc_valid_ = true;
    }
    FillSurfaceConstants(s, size, c);
    // points: host pixels per console pixel, also keeps them off the far edge pull out of screen passes
    if (k.topo == uint32_t(Topology::Points)) c.surf[3] = TargetScale();
    FillFogConstants(s, sh.prog, c);
    FillCombinerConstants(s, c);
    FillDepthConstants(s, sh, Topology(k.topo), c);
    FillTextureConstants(s, sh, stages, c);
    if (sh.fixed_transform) {
        FillTransformConstants(s, size, c);
        FillLightConstants(s, sh.fk, c);
    }
    if (!last_dc_valid_ || std::memcmp(&last_dc_, &c, sizeof(c))) {
        Alloc dc = Upload(kDrawConstantBytes, 256);
        std::memset(dc.cpu, 0, kDrawConstantBytes);
        std::memcpy(dc.cpu, &c, sizeof(c));
        last_dc_ = c;
        last_dc_alloc_ = dc;
        last_dc_valid_ = true;
    }
}

void VulkanBackend::PushDescriptors(const State& s, const Assembled& a, const DrawTextures& stages) {
    VkDescriptorBufferInfo bi[2] = {{last_vsc_alloc_.buf, last_vsc_alloc_.off, kVsConstantBytes},
                                    {last_dc_alloc_.buf, last_dc_alloc_.off, kDrawConstantBytes}};
    VkDescriptorImageInfo ii[4], si[4];
    bool clamp = AnyStageWraps(s) && UnitTexcoords(a);
    uint32_t aniso = app::Get().aniso;
    for (int i = 0; i < 4; i++) {
        const Img* im = stages.img[i];
        if (!im) {
            uint32_t kind = stages.info[i].kind;
            im = kind == kTex3D ? &dummy_3d_ : kind == kTexCube ? &dummy_cube_ : &dummy_2d_;
        }
        ii[i] = {VK_NULL_HANDLE, im->view, ReadLayout(*im)};
        VkSampler smp = EnsureSampler(StageSamplerKey(s, i, stages, clamp, aniso));
        si[i] = {smp ? smp : blit_point_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    }
    VkWriteDescriptorSet w[kBindCount] = {};
    for (uint32_t i = 0; i < kBindCount; i++) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
    }
    for (uint32_t i = 0; i < 2; i++) {
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[i].pBufferInfo = &bi[i];
    }
    for (uint32_t i = 0; i < 4; i++) {
        w[kBindTex + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w[kBindTex + i].pImageInfo = &ii[i];
        w[kBindSmp + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w[kBindSmp + i].pImageInfo = &si[i];
    }
    push_descriptor_(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, kBindCount, w);
}

// viewport covers the target, the guest viewport is in the shader; scissor clips to it
void VulkanBackend::SetViewportScissor(const State& s, uint32_t gw) {
    int32_t W = int32_t(bound_rt_ ? bound_rt_->w : bound_ds_->w);
    int32_t H = int32_t(bound_rt_ ? bound_rt_->h : bound_ds_->h);
    VkViewport vp{0, float(H), float(W), -float(H), 0, 1};
    float sc = float(W) / float(gw);
    int32_t x0 = int32_t(s.viewport.x * sc);
    int32_t y0 = int32_t(s.viewport.y * sc);
    int32_t x1 = int32_t((s.viewport.x + s.viewport.w) * sc);
    int32_t y1 = int32_t((s.viewport.y + s.viewport.h) * sc);
    if (s.scissor_count) {
        x0 = std::max(x0, int32_t(s.scissors[0] * sc));
        y0 = std::max(y0, int32_t(s.scissors[1] * sc));
        x1 = std::min(x1, int32_t(s.scissors[2] * sc));
        y1 = std::min(y1, int32_t(s.scissors[3] * sc));
    }
    x0 = std::clamp(x0, 0, W);
    y0 = std::clamp(y0, 0, H);
    x1 = std::clamp(x1, 0, W);
    y1 = std::clamp(y1, 0, H);
    VkRect2D rect{{x0, y0}, {uint32_t(std::max(x1 - x0, 0)), uint32_t(std::max(y1 - y0, 0))}};
    vkCmdSetViewport(cb_, 0, 1, &vp);
    vkCmdSetScissor(cb_, 0, 1, &rect);
}

void VulkanBackend::SetBlendFactorStencilRef(const State& s) {
    float bf[4];
    Color(SimpleState(s, kMBlendColor), bf);
    vkCmdSetBlendConstants(cb_, bf);
    vkCmdSetStencilReference(cb_, VK_STENCIL_FACE_FRONT_AND_BACK, SimpleState(s, kMStencilRef) & 0xFF);
}

void VulkanBackend::DrawGeometry(const Assembled& a) {
    if (a.buffer_count) {
        VkBuffer bufs[16];
        VkDeviceSize offs[16];
        VkDeviceSize strides[16];
        for (uint32_t i = 0; i < a.buffer_count; i++) {
            const VertexBuffer& b = a.buffers[i];
            Alloc vb = Upload(std::max<uint64_t>(b.bytes, 16), 16);
            std::memcpy(vb.cpu, b.src, b.bytes);
            bufs[i] = vb.buf;
            offs[i] = vb.off;
            strides[i] = b.stride;
        }
        vkCmdBindVertexBuffers2(cb_, 0, a.buffer_count, bufs, offs, nullptr, strides);
    }
    if (a.index_count) {
        uint32_t ibytes = a.index_count * (a.index16 ? 2 : 4);
        Alloc ib = Upload(ibytes, 4);
        std::memcpy(ib.cpu, a.index_src, ibytes);
        vkCmdBindIndexBuffer(cb_, ib.buf, ib.off, a.index16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cb_, a.index_count, 1, 0, int32_t(a.base_vertex), 0);
    } else {
        vkCmdDraw(cb_, a.draw_count, 1, 0, 0);
    }
}

void VulkanBackend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    if (SpriteOnStage3(s, prim)) {
        Draw(SpriteStageZero(s), prim, start, count, indices);
        return;
    }
    Assembled& a = asm_;
    if (!Assemble(s, prim, start, count, indices, a)) return;
    // past the ring every upload is a buffer of its own: seconds lost on a frame that draws a whole level once
    if (Slot().used > kUploadSize - kUploadReserve) Submit(true);
    if (!BindTargets(s)) return;

    DrawShaders sh = ClassifyShaders(s, a, count);
    if (sh.prog) EnsureProgramHash(s);
    sh.vkey = VertexKey(s, a, sh);
    EnsureVertexShader(s, a, sh);

    DrawTextures stages;
    if (!BindStageTexturesAndTargets(s, stages)) return;
    PixelKey pk = MakePixelKey(s, stages.info);
    pk.quad = sh.quad;
    sh.pkey = Hash(&pk, sizeof(pk));
    EnsurePixelShader(pk, sh);
    if (!sh.vs_code || !sh.ps_code) return;

    PsoKey k;
    BuildPipelineKey(s, a, sh.vh, sh.ph, k);
    k.sprite = sh.sprite;
    VkPipeline pso = EnsurePipeline(k, a, sh.vs_code, sh.ps_code);
    if (!pso) return;

    GuestExtent size;
    SurfaceSize(bound_rt_ ? s.rt : s.zs, size.w, size.h);
    DrawConstants c{};
    UploadConstants(s, sh, k, stages, size, c);

    BeginPass(bound_rt_ ? &bound_rt_->img : nullptr, bound_ds_ ? &bound_ds_->img : nullptr);
    if (pso != bound_pipeline_) vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
    bound_pipeline_ = pso;
    SetViewportScissor(s, size.w);
    SetBlendFactorStencilRef(s);
    PushDescriptors(s, a, stages);
    DrawGeometry(a);
}

// ================================ clear and copy ================================

// some colour channels cleared: drawn over each rect writing only those
void VulkanBackend::ClearChannels(Tgt& t, const float col[4], uint32_t mask, const std::vector<VkRect2D>& rs) {
    VkPipeline pso = EnsureFullscreenPipeline(kClearHlsl, mask, false, 0);
    if (!pso) {
        LOG_FIRST(4, "vulkan: channel clear pipeline failed, mask %X", mask);
        return;
    }
    VkViewport vp{0, float(t.h), float(t.w), -float(t.h), 0, 1};
    vkCmdSetViewport(cb_, 0, 1, &vp);
    for (const VkRect2D& r : rs) {
        vkCmdSetScissor(cb_, 0, 1, &r);
        FullScreen(pso, nullptr, nullptr, nullptr, nullptr, col, 4 * sizeof(float));
    }
}

void VulkanBackend::ClearTarget(Tgt& t, const State& s, uint32_t count, const uint8_t* rects, uint32_t flags,
                                uint32_t color, float z, uint32_t stencil) {
    std::vector<VkRect2D> rs;
    auto add = [&](int x0, int y0, int x1, int y1) {
        VkRect2D r = ScaledRect(t, x0, y0, x1, y1);
        if (r.extent.width && r.extent.height) rs.push_back(r);
    };
    if (count && rects) {
        for (uint32_t i = 0; i < count; i++) {
            int32_t r[4];
            std::memcpy(r, rects + size_t(i) * sizeof(r), sizeof(r));
            add(r[0], r[1], r[2], r[3]);
        }
    } else {
        const auto& v = s.viewport;
        add(int(v.x), int(v.y), int(v.x + v.w), int(v.y + v.h));
    }
    if (t.depth) {
        VkImageAspectFlags aspect = ((flags & kClearZ) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                                    ((flags & kClearStencil) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        BeginPass(nullptr, &t.img);
        VkClearAttachment ca{};
        ca.aspectMask = aspect;
        ca.clearValue.depthStencil = {1 - std::clamp(z, 0.0f, 1.0f), stencil & 0xFF};
        for (const VkRect2D& r : rs) {
            VkClearRect cr{r, 0, 1};
            vkCmdClearAttachments(cb_, 1, &ca, 1, &cr);
        }
    } else {
        // pixels the CPU wrote before this clear go in first, a later upload would paint over the clear
        if (LatchCpuWrite(t)) UploadTarget(t, s.rt);
        BeginPass(&t.img, nullptr);
        float col[4];
        Color(color, col);
        // D3DCLEAR_TARGET_R/G/B/A are bits 4 to 7, the write mask has the same order
        uint32_t mask = (flags & kClearTarget) >> 4;
        if (mask == 0xF) {
            VkClearAttachment ca{};
            ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            std::memcpy(ca.clearValue.color.float32, col, sizeof(col));
            for (const VkRect2D& r : rs) {
                VkClearRect cr{r, 0, 1};
                vkCmdClearAttachments(cb_, 1, &ca, 1, &cr);
            }
        } else {
            ClearChannels(t, col, mask, rs);
        }
    }
    t.drawn = true;
}

void VulkanBackend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                          uint32_t stencil) {
    Tgt* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    Tgt* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    for (Tgt* t : {rt, ds}) {
        if (t) ClearTarget(*t, s, count, rects, flags, color, z, stencil);
    }
}

// one vkCmdCopyImage per rect, clamped to both targets
void VulkanBackend::CopyByRegion(Tgt& src_t, Tgt& dst_t, uint32_t count, uint32_t src_rects, uint32_t dst_points) {
    Transition(src_t.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(dst_t.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    float dst_scale = float(dst_t.w) / dst_t.gw;
    bool listed = count && src_rects;
    uint32_t n = listed ? count : 1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t left = 0, top = 0;
        uint32_t right = std::min(src_t.w, dst_t.w);
        uint32_t bottom = std::min(src_t.h, dst_t.h);
        uint32_t dx = 0, dy = 0;
        if (listed) {
            GuestRect r = ReadGuestRect(src_rects, i);
            left = ScaleUnsigned(r.left, dst_scale);
            top = ScaleUnsigned(r.top, dst_scale);
            right = std::min(ScaleUnsigned(r.right, dst_scale), src_t.w);
            bottom = std::min(ScaleUnsigned(r.bottom, dst_scale), src_t.h);
            dx = left;
            dy = top;
            if (dst_points) {
                GuestPoint p = ReadGuestPoint(dst_points, i);
                dx = ScaleUnsigned(p.x, dst_scale);
                dy = ScaleUnsigned(p.y, dst_scale);
            }
        }
        if (right <= left || bottom <= top || dx >= dst_t.w || dy >= dst_t.h) continue;
        right = std::min(right, left + dst_t.w - dx);
        bottom = std::min(bottom, top + dst_t.h - dy);
        VkImageCopy r{};
        r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.srcOffset = {int32_t(left), int32_t(top), 0};
        r.dstOffset = {int32_t(dx), int32_t(dy), 0};
        r.extent = {right - left, bottom - top, 1};
        vkCmdCopyImage(cb_, src_t.img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_t.img.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    }
    dst_t.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

void VulkanBackend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects,
                         uint32_t dst_points) {
    Tgt* src_t = FindTarget(src, false);
    // pixels the CPU made: the copy happens in memory, textures and targets pick it up from there
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "vulkan: CopyRects %08X to %08X skipped", src.data, dst.data);
        } else if (Tgt* dst_t = FindTarget(dst, false)) {
            UploadTarget(*dst_t, dst);
        }
        return;
    }
    Tgt* dst_t = EnsureTarget(dst, false);
    if (!dst_t || src_t == dst_t) return;
    CopyByRegion(*src_t, *dst_t, count, src_rects, dst_points);
}

// ================================ presenting ================================

// ramp_ holds the red, green and blue ramps, 256 entries each
constexpr int kRampRed = 0;
constexpr int kRampGreen = 256;
constexpr int kRampBlue = 512;

void VulkanBackend::UploadGammaRamp() {
    constexpr uint32_t kRampBytes = 256 * 4;  // one BGRA texel per ramp entry
    ramp_dirty_ = false;
    Alloc a = Upload(kRampBytes, 16);
    bool identity = true;
    for (int i = 0; i < 256; i++) {
        uint8_t* p = a.cpu + i * 4;
        p[0] = ramp_[kRampBlue + i];
        p[1] = ramp_[kRampGreen + i];
        p[2] = ramp_[kRampRed + i];
        p[3] = 255;
        identity &= ramp_[kRampRed + i] == i && ramp_[kRampGreen + i] == i && ramp_[kRampBlue + i] == i;
    }
    gamma_ = !identity;
    Transition(lut_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    CopyBufferToImage(lut_, a, 0, 0, 256, 1, 1);
    Transition(lut_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void VulkanBackend::BlitFrame(Tgt* t) {
    if (!t) return;
    // 16:9 when the console reports a widescreen display, else 4:3
    float aspect = app::Widescreen() ? 16.0f / 9.0f : 4.0f / 3.0f;
    float w = float(bb_w_);
    float h = float(bb_h_);
    float vw = std::min(w, h * aspect);
    float vh = vw / aspect;
    VkViewport vp{(w - vw) / 2, (h - vh) / 2 + vh, vw, -vh, 0, 1};
    VkRect2D scissor{{0, 0}, {bb_w_, bb_h_}};
    vkCmdSetViewport(cb_, 0, 1, &vp);
    vkCmdSetScissor(cb_, 0, 1, &scissor);
    float uv[4] = {0, 0, 1, 1};
    Blit(t->img, uv, {.gamma = gamma_});
}

void VulkanBackend::RenderUi() {
    std::lock_guard lock(app::UiLock());
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (app::kGame.ui) app::kGame.ui(fps_);
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cb_);
    bound_pipeline_ = VK_NULL_HANDLE;
}

void VulkanBackend::DrawFrame(Tgt* t, uint32_t image) {
    Img& bb = swap_images_[image];
    if (t) Transition(t->img, ReadLayout(t->img));
    float black[4] = {0, 0, 0, 1};
    BeginPass(&bb, nullptr, black);
    bound_rt_ = bound_ds_ = nullptr;
    BlitFrame(t);
    RenderUi();
    EndPass();
    Transition(bb, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

void VulkanBackend::Present(const Surface& front) {
    const bool show = true;
    ResizeSwap();
    Tgt* t = EnsureTarget(front, false);
    if (t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    if (ramp_dirty_) UploadGammaRamp();

    const uint64_t signal = presents_ + 1;
    uint32_t image = 0;
    bool shown = false;
    FrameSlot& f = Slot();
    if (show && swap_usable_) {
        VkResult r = vkAcquireNextImageKHR(dev_, swap_, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &image);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
            shown = true;
            if (r == VK_SUBOPTIMAL_KHR) swap_stale_ = true;
        } else {
            LOG_FIRST(16, "vulkan: acquire failed (%d)", int(r));
            swap_stale_ = true;
        }
    }
    if (presents_ % 300 == 0)
        LOGR("vulkan: present %llu show=%d usable=%d shown=%d target=%p bb=%ux%u", (unsigned long long)presents_,
             int(show), int(swap_usable_), int(shown), (void*)t, bb_w_, bb_h_);
    if (shown) {
        DrawFrame(t, image);
        Submit(false, f.acquired, render_done_[image], signal);
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &render_done_[image];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swap_;
        pi.pImageIndices = &image;
        VkResult r = vkQueuePresentKHR(queue_, &pi);
        if (r != VK_SUCCESS) LOG_FIRST(16, "vulkan: present returned %d", int(r));
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) swap_stale_ = true;
    } else {
        Submit(false, VK_NULL_HANDLE, VK_NULL_HANDLE, signal);
    }
    submitted_presents_.store(signal);
    presents_++;
    ReleaseOldTextures();
    NextFrame();
    if (show) CountFps();
    ApplySettings();
}

// the slot is not reused before the caller's wait: the title runs at most one present ahead
void VulkanBackend::WaitGpuFrame(uint64_t n) {
    uint64_t value = n + 1;
    if (value > submitted_presents_.load()) return;
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline_;
    wi.pValues = &value;
    constexpr uint64_t kWaitNs = 2'000'000'000ull;
    vkWaitSemaphores(dev_, &wi, kWaitNs);
}

void VulkanBackend::CountFps() {
    fps_n_++;
    auto now = std::chrono::steady_clock::now();
    double el = std::chrono::duration<double>(now - fps_t_).count();
    if (el >= 0.5) {
        fps_ = fps_n_ / el;
        fps_n_ = 0;
        fps_t_ = now;
    }
}

void VulkanBackend::ApplySettings() {
    float want = app::RenderScale(guest_h_);
    if (std::fabs(want - scale_) <= 1e-4f) return;
    Flush();
    for (auto& [key, tg] : targets_) Retire(tg.img);
    targets_.clear();
    scale_ = want;
    bound_rt_ = bound_ds_ = draw_rt_ = draw_ds_ = nullptr;
}

// ================================ init ================================

bool VulkanBackend::CreateDummies() {
    auto make = [&](Img& im, VkImageType type, uint32_t layers, bool cube) {
        if (!MakeImage(im, type, kBgra, 1, 1, 1, 1, layers, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       cube, false)) {
            return false;
        }
        Transition(im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue v{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        vkCmdClearColorImage(cb_, im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
        Transition(im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return true;
    };
    return make(dummy_2d_, VK_IMAGE_TYPE_2D, 1, false) && make(dummy_3d_, VK_IMAGE_TYPE_3D, 1, false) &&
           make(dummy_cube_, VK_IMAGE_TYPE_2D, 6, true);
}

bool VulkanBackend::CreateImGui() {
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = 64;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    if (!Ok(vkCreateDescriptorPool(dev_, &pi, nullptr, &ui_pool_), "ImGui descriptor pool")) return false;
    static const VkFormat format = kBgra;
    ImGui_ImplVulkan_InitInfo ii{};
    ii.Instance = instance_;
    ii.PhysicalDevice = phys_;
    ii.Device = dev_;
    ii.QueueFamily = family_;
    ii.Queue = queue_;
    ii.DescriptorPool = ui_pool_;
    ii.MinImageCount = 2;
    ii.ImageCount = std::max<uint32_t>(2, uint32_t(swap_images_.size()));
    ii.UseDynamicRendering = true;
    ii.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &format;
    return ImGui_ImplVulkan_Init(&ii);
}

bool VulkanBackend::Init(uint32_t width, uint32_t height) {
    g_reversed_depth = true;
    guest_w_ = width;
    guest_h_ = height;
    for (int i = 0; i < 256; i++) ramp_[kRampRed + i] = ramp_[kRampGreen + i] = ramp_[kRampBlue + i] = uint8_t(i);
    if (!CreateInstance() || !CreateDevice() || !CreateFrames() || !CreateLayouts()) return false;

    SamplerKey sk{};
    sk.u = sk.v = sk.w = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sk.max_lod = VK_LOD_CLAMP_NONE;
    sk.mag = sk.min = 1;
    blit_linear_ = EnsureSampler(sk);
    sk.mag = sk.min = 0;
    blit_point_ = EnsureSampler(sk);
    sk.compare = uint32_t(VK_COMPARE_OP_ALWAYS) + 1;
    dummy_compare_ = EnsureSampler(sk);
    if (!blit_linear_ || !blit_point_) return false;

    BeginCommands();
    if (!CreateDummies()) return false;
    if (!MakeImage(lut_, VK_IMAGE_TYPE_2D, kBgra, 256, 1, 1, 1, 1,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false, false)) {
        return false;
    }
    UploadGammaRamp();
    Submit(true);

    if (!CreateSwapchain()) return false;
    if (!CreateImGui()) return false;
    scale_ = app::RenderScale(guest_h_);
    LOGI("vulkan: ready, %ux%u guest, render scale %.2f, depth format %d", guest_w_, guest_h_, scale_,
         int(depth_format_));
    return true;
}

}  // namespace

std::unique_ptr<Backend> CreateVulkanBackend() { return std::make_unique<VulkanBackend>(); }

}  // namespace gpu
