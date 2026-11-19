#include "render_d3d11.h"

#include "pm4_processor.h"
#include "render_pipeline.h"
#include "render_resources.h"

#include <d3dcompiler.h>
#include <imgui.h>
#include <rex/logging/log_category.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

using Microsoft::WRL::ComPtr;

namespace bs1::renderer {

namespace {

using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*,
                                      ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**,
                                      ID3DBlob**);

D3DCompileFn LoadD3DCompile() {
  static HMODULE mod = LoadLibraryW(L"d3dcompiler_47.dll");
  if (!mod) return nullptr;
  return reinterpret_cast<D3DCompileFn>(GetProcAddress(mod, "D3DCompile"));
}

ComPtr<ID3DBlob> CompileSource(D3DCompileFn compileFn, const char* src, const char* entry,
                               const char* profile, const char* label,
                               const D3D_SHADER_MACRO* defines = nullptr) {
  if (!compileFn) return nullptr;
  ComPtr<ID3DBlob> code;
  ComPtr<ID3DBlob> errors;
  const HRESULT hr = compileFn(src, std::strlen(src), label, defines, nullptr, entry, profile,
                               D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
  if (FAILED(hr)) {
    REXLOG_ERROR("custom-renderer: {} compile failed: {}", label,
                 errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown error");
    return nullptr;
  }
  return code;
}

const char* const kRectangleGS = R"HLSL(
struct V {
    float4 position : SV_Position;
    float4 o0 : TEXCOORD0;  float4 o1 : TEXCOORD1;  float4 o2 : TEXCOORD2;  float4 o3 : TEXCOORD3;
    float4 o4 : TEXCOORD4;  float4 o5 : TEXCOORD5;  float4 o6 : TEXCOORD6;  float4 o7 : TEXCOORD7;
    float4 o8 : TEXCOORD8;  float4 o9 : TEXCOORD9;  float4 o10 : TEXCOORD10; float4 o11 : TEXCOORD11;
    float4 o12 : TEXCOORD12; float4 o13 : TEXCOORD13; float4 o14 : TEXCOORD14; float4 o15 : TEXCOORD15;
};
V fourth(V a, V b, V c) {
    V d;
    d.position = a.position + c.position - b.position;
    d.o0 = a.o0 + c.o0 - b.o0;     d.o1 = a.o1 + c.o1 - b.o1;
    d.o2 = a.o2 + c.o2 - b.o2;     d.o3 = a.o3 + c.o3 - b.o3;
    d.o4 = a.o4 + c.o4 - b.o4;     d.o5 = a.o5 + c.o5 - b.o5;
    d.o6 = a.o6 + c.o6 - b.o6;     d.o7 = a.o7 + c.o7 - b.o7;
    d.o8 = a.o8 + c.o8 - b.o8;     d.o9 = a.o9 + c.o9 - b.o9;
    d.o10 = a.o10 + c.o10 - b.o10; d.o11 = a.o11 + c.o11 - b.o11;
    d.o12 = a.o12 + c.o12 - b.o12; d.o13 = a.o13 + c.o13 - b.o13;
    d.o14 = a.o14 + c.o14 - b.o14; d.o15 = a.o15 + c.o15 - b.o15;
    return d;
}
[maxvertexcount(4)]
void main(triangle V input[3], inout TriangleStream<V> stream) {
    float2 p0 = input[0].position.xy / input[0].position.w;
    float2 p1 = input[1].position.xy / input[1].position.w;
    float2 p2 = input[2].position.xy / input[2].position.w;
    float d01 = dot(p1 - p0, p1 - p0);
    float d12 = dot(p2 - p1, p2 - p1);
    float d02 = dot(p2 - p0, p2 - p0);
    uint c = (d02 >= d01 && d02 >= d12) ? 1u : (d01 >= d12 ? 2u : 0u);
    uint a = (c + 1u) % 3u;
    uint b = (c + 2u) % 3u;
    stream.Append(input[c]);
    stream.Append(input[a]);
    stream.Append(input[b]);
    stream.Append(fourth(input[a], input[c], input[b]));
    stream.RestartStrip();
}
)HLSL";

const char* const kQuadHLSL = R"HLSL(
Texture2D source : register(t0);
SamplerState smp : register(s0);
Texture1D gammaTable : register(t1);
cbuffer Quad : register(b0) { float4 uvRect; float4 texel; };

struct V {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

V vsmain(uint id : SV_VertexID) {
    V v;
    float2 t = float2((id << 1) & 2, id & 2);
    v.position = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    v.uv = uvRect.xy + t * uvRect.zw;
    return v;
}

float3 gamma(float3 c) {
    float3 at = saturate(c) * (255.0 / 256.0) + (0.5 / 256.0);
    return float3(
        gammaTable.SampleLevel(smp, at.r, 0).r,
        gammaTable.SampleLevel(smp, at.g, 0).g,
        gammaTable.SampleLevel(smp, at.b, 0).b);
}

float4 copy(V v) : SV_Target {
    float4 c = source.Sample(smp, v.uv) * texel.z;
    if (texel.w > 1.5) return clamp(c, -1.0, 1.0);
    if (texel.w > 0.5) return saturate(c);
    return c;
}

float4 present_bilinear(V v) : SV_Target {
    return float4(gamma(source.Sample(smp, v.uv).rgb), 1.0);
}

float lanczos2(float x) {
    x = abs(x);
    if (x < 1e-5) return 1.0;
    if (x >= 2.0) return 0.0;
    float px = 3.14159265 * x;
    return (2.0 * sin(px) * sin(px * 0.5)) / (px * px);
}

float4 present_lanczos(V v) : SV_Target {
    float2 texSize = 1.0 / texel.xy;
    float2 tc = v.uv * texSize - 0.5;
    float2 f = frac(tc);
    float2 base = floor(tc);

    float wx[4];
    float wy[4];
    float sumX = 0.0, sumY = 0.0;
    [unroll] for (int i = 0; i < 4; ++i) {
        wx[i] = lanczos2(float(i - 1) - f.x);
        wy[i] = lanczos2(float(i - 1) - f.y);
        sumX += wx[i];
        sumY += wy[i];
    }
    [unroll] for (int j = 0; j < 4; ++j) {
        wx[j] /= sumX;
        wy[j] /= sumY;
    }

    float3 col = 0.0;
    float3 mn = 1e9;
    float3 mx = -1e9;
    [unroll] for (int row = 0; row < 4; ++row) {
        float3 rowCol = 0.0;
        [unroll] for (int c = 0; c < 4; ++c) {
            float2 coord = (base + float2(float(c - 1), float(row - 1)) + 0.5) * texel.xy;
            float3 s = source.SampleLevel(smp, coord, 0).rgb;
            if (c >= 1 && c <= 2 && row >= 1 && row <= 2) {
                mn = min(mn, s);
                mx = max(mx, s);
            }
            rowCol += s * wx[c];
        }
        col += rowCol * wy[row];
    }
    col = clamp(col, mn, mx);
    return float4(gamma(col), 1.0);
}

float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float4 present_fxaa(V v) : SV_Target {
    float2 uv = v.uv;
    float2 px = texel.xy;
    float3 rgbM = source.Sample(smp, uv).rgb;
    float lM = luma(rgbM);
    float lN = luma(source.Sample(smp, uv + float2(0.0, -px.y)).rgb);
    float lS = luma(source.Sample(smp, uv + float2(0.0, px.y)).rgb);
    float lW = luma(source.Sample(smp, uv + float2(-px.x, 0.0)).rgb);
    float lE = luma(source.Sample(smp, uv + float2(px.x, 0.0)).rgb);
    float lMin = min(lM, min(min(lN, lS), min(lW, lE)));
    float lMax = max(lM, max(max(lN, lS), max(lW, lE)));
    float range = lMax - lMin;
    if (range < max(0.0312, lMax * 0.125)) return float4(gamma(rgbM), 1.0);
    float lNW = luma(source.Sample(smp, uv + float2(-px.x, -px.y)).rgb);
    float lNE = luma(source.Sample(smp, uv + float2(px.x, -px.y)).rgb);
    float lSW = luma(source.Sample(smp, uv + float2(-px.x, px.y)).rgb);
    float lSE = luma(source.Sample(smp, uv + float2(px.x, px.y)).rgb);
    float lNS = lN + lS, lWE = lW + lE, lWc = lNW + lSW, lEc = lNE + lSE, lNc = lNW + lNE, lSc = lSW + lSE;
    float edgeH = abs(-2.0 * lW + lWc) + abs(-2.0 * lM + lNS) * 2.0 + abs(-2.0 * lE + lEc);
    float edgeV = abs(-2.0 * lN + lNc) + abs(-2.0 * lM + lWE) * 2.0 + abs(-2.0 * lS + lSc);
    bool horizontal = edgeH >= edgeV;
    float l1 = horizontal ? lS : lE;
    float l2 = horizontal ? lN : lW;
    float g1 = l1 - lM, g2 = l2 - lM;
    bool steep1 = abs(g1) >= abs(g2);
    float gradScaled = 0.25 * max(abs(g1), abs(g2));
    float step = horizontal ? px.y : px.x;
    float lAvg = steep1 ? 0.5 * (l1 + lM) : 0.5 * (l2 + lM);
    if (!steep1) step = -step;
    float2 cur = uv + (horizontal ? float2(0.0, step * 0.5) : float2(step * 0.5, 0.0));
    float2 off = horizontal ? float2(px.x, 0.0) : float2(0.0, px.y);
    float2 uv1 = cur - off, uv2 = cur + off;
    float e1 = luma(source.Sample(smp, uv1).rgb) - lAvg;
    float e2 = luma(source.Sample(smp, uv2).rgb) - lAvg;
    bool r1 = abs(e1) >= gradScaled, r2 = abs(e2) >= gradScaled;
    if (!r1) uv1 -= off;
    if (!r2) uv2 += off;
    [loop] for (int i = 2; i < 10 && !(r1 && r2); ++i) {
        if (!r1) e1 = luma(source.Sample(smp, uv1).rgb) - lAvg;
        if (!r2) e2 = luma(source.Sample(smp, uv2).rgb) - lAvg;
        r1 = abs(e1) >= gradScaled;
        r2 = abs(e2) >= gradScaled;
        float q = i < 5 ? 1.5 : 2.5;
        if (!r1) uv1 -= off * q;
        if (!r2) uv2 += off * q;
    }
    float d1 = horizontal ? (uv.x - uv1.x) : (uv.y - uv1.y);
    float d2 = horizontal ? (uv2.x - uv.x) : (uv2.y - uv.y);
    bool dir1 = d1 < d2;
    float dist = min(d1, d2);
    float edgeLen = max(d1 + d2, 1e-5);
    float pixelOffset = -dist / edgeLen + 0.5;
    bool correct = ((dir1 ? e1 : e2) < 0.0) != (lM < lAvg);
    float finalOffset = correct ? pixelOffset : 0.0;
    float2 finalUv = uv + (horizontal ? float2(0.0, finalOffset * step) : float2(finalOffset * step, 0.0));
    return float4(gamma(source.Sample(smp, finalUv).rgb), 1.0);
}
)HLSL";

const char* const kCopyMSHLSL = R"HLSL(
cbuffer Quad : register(b0) { float4 uvRect; float4 texel; };
struct V { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Texture2DMS<float4, SAMPLES> sourceMS : register(t0);
float4 copyColourMS(V v) : SV_Target {
    int2 p = int2(v.uv / texel.xy);
    float4 sum = 0;
    [unroll] for (int i = 0; i < SAMPLES; ++i) sum += sourceMS.Load(p, i);
    float4 c = (sum / SAMPLES) * texel.z;
    if (texel.w > 1.5) return clamp(c, -1.0, 1.0);
    if (texel.w > 0.5) return saturate(c);
    return c;
}
float4 copyDepthMS(V v) : SV_Target {
    int2 p = int2(v.uv / texel.xy);
    return sourceMS.Load(p, 0);
}
)HLSL";

const char* const kImGuiHLSL = R"HLSL(
cbuffer Projection : register(b0) { float4x4 mvp; };
struct VSIn {
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};
struct PSIn {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};
PSIn vs_imgui(VSIn i) {
    PSIn o;
    o.pos = mul(mvp, float4(i.pos, 0.0, 1.0));
    o.uv = i.uv;
    o.col = i.col;
    return o;
}
Texture2D tex : register(t0);
SamplerState smp : register(s0);
float4 ps_imgui(PSIn i) : SV_Target {
    return i.col * tex.Sample(smp, i.uv);
}
)HLSL";

}  // namespace

bool RenderD3D11::Initialize(RenderPipeline* pipeline, RenderResources* resources,
                             Pm4Processor* pm4) {
  pipeline_ = pipeline;
  resources_ = resources;
  pm4_ = pm4;

  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
  if (std::getenv("BS1_D3D11_DEBUG") != nullptr) flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
  D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_11_0;
  const HRESULT hr =
      D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 1,
                        D3D11_SDK_VERSION, &device_, &obtained, &context_);
  if (FAILED(hr) || !device_ || !context_) {
    REXLOG_ERROR("custom-renderer: D3D11CreateDevice failed (0x{:08X})", static_cast<uint32_t>(hr));
    return false;
  }
  context_.As(&context1_);

  ComPtr<IDXGIFactory5> factory5;
  if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory5)))) {
    BOOL allowTearing = FALSE;
    if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing,
                                                sizeof(allowTearing)))) {
      tearingSupported_ = (allowTearing == TRUE);
    }
  }

  auto makeCB = [&](uint32_t bytes, ComPtr<ID3D11Buffer>& out) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = bytes;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    return SUCCEEDED(device_->CreateBuffer(&bd, nullptr, &out));
  };
  if (!makeCB(4096, vsFloatCB_) || !makeCB(4096, psFloatCB_) || !makeCB(256, boolCB_) ||
      !makeCB((sizeof(RenderState::DrawConstants) + 255u) & ~255u, drawCB_)) {
    return false;
  }

  if (!BuildInternalShaders()) return false;
  ForgetBindings();
  return true;
}

bool RenderD3D11::BuildInternalShaders() {
  const D3DCompileFn compileFn = LoadD3DCompile();
  if (!compileFn) {
    REXLOG_ERROR("custom-renderer: d3dcompiler_47.dll unavailable");
    return false;
  }

  if (auto code = CompileSource(compileFn, kRectangleGS, "main", "gs_5_0", "rectangle-gs")) {
    device_->CreateGeometryShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                  &rectangleGS_);
  }
  if (auto code = CompileSource(compileFn, kQuadHLSL, "vsmain", "vs_5_0", "quad-vs")) {
    device_->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &quadVS_);
  }
  if (auto code = CompileSource(compileFn, kQuadHLSL, "copy", "ps_5_0", "copy-ps")) {
    device_->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &copyPS_);
  }
  if (auto code = CompileSource(compileFn, kQuadHLSL, "present_bilinear", "ps_5_0", "bilinear-ps")) {
    device_->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                               &presentBilinearPS_);
  }
  if (auto code = CompileSource(compileFn, kQuadHLSL, "present_lanczos", "ps_5_0", "lanczos-ps")) {
    device_->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                               &presentLanczosPS_);
  }
  if (auto code = CompileSource(compileFn, kQuadHLSL, "present_fxaa", "ps_5_0", "fxaa-ps")) {
    device_->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                               &presentFxaaPS_);
  }
  for (int i = 1; i <= 3; ++i) {
    const char* counts[4] = {"1", "2", "4", "8"};
    const D3D_SHADER_MACRO defs[2] = {{"SAMPLES", counts[i]}, {nullptr, nullptr}};
    if (auto code = CompileSource(compileFn, kCopyMSHLSL, "copyColourMS", "ps_5_0", "copy-ms", defs)) {
      device_->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                 &copyColorMS_[i]);
    }
    if (auto code = CompileSource(compileFn, kCopyMSHLSL, "copyDepthMS", "ps_5_0", "depth-ms", defs)) {
      device_->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                 &copyDepthMS_[i]);
    }
  }

  D3D11_SAMPLER_DESC smp{};
  smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  smp.MaxLOD = D3D11_FLOAT32_MAX;
  device_->CreateSamplerState(&smp, &pointSampler_);
  smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  device_->CreateSamplerState(&smp, &linearSampler_);

  D3D11_BUFFER_DESC qcb{};
  qcb.ByteWidth = 32;
  qcb.Usage = D3D11_USAGE_DEFAULT;
  qcb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  device_->CreateBuffer(&qcb, nullptr, &quadCB_);

  // ImGui 1.92 shader & state setup.
  if (auto code = CompileSource(compileFn, kImGuiHLSL, "vs_imgui", "vs_5_0", "imgui-vs")) {
    device_->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &imguiVS_);
    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(ImDrawVert, pos),
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(ImDrawVert, uv),
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(ImDrawVert, col),
         D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    device_->CreateInputLayout(layout, 3, code->GetBufferPointer(), code->GetBufferSize(),
                               &imguiLayout_);
  }
  if (auto code = CompileSource(compileFn, kImGuiHLSL, "ps_imgui", "ps_5_0", "imgui-ps")) {
    device_->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &imguiPS_);
  }
  {
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = 64;
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device_->CreateBuffer(&cb, nullptr, &imguiCB_);

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device_->CreateBlendState(&bd, &imguiBlend_);

    D3D11_DEPTH_STENCIL_DESC ds{};
    device_->CreateDepthStencilState(&ds, &imguiDepth_);

    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.ScissorEnable = TRUE;
    rs.DepthClipEnable = TRUE;
    device_->CreateRasterizerState(&rs, &imguiRasterizer_);
  }

  return quadVS_ && copyPS_ && presentBilinearPS_ && presentLanczosPS_;
}

void RenderD3D11::Shutdown() {
  std::lock_guard lock(contextMutex_);
  if (context_) {
    context_->ClearState();
    context_->Flush();
  }
  imguiTextures_.clear();
  imguiVB_.Reset();
  imguiIB_.Reset();
  imguiLayout_.Reset();
  imguiVS_.Reset();
  imguiPS_.Reset();
  imguiCB_.Reset();
  imguiBlend_.Reset();
  imguiDepth_.Reset();
  imguiRasterizer_.Reset();

  gammaSrv_.Reset();
  gammaTex_.Reset();
  quadCB_.Reset();
  pointSampler_.Reset();
  linearSampler_.Reset();
  for (auto& s : copyColorMS_) s.Reset();
  for (auto& s : copyDepthMS_) s.Reset();
  presentFxaaPS_.Reset();
  presentLanczosPS_.Reset();
  presentBilinearPS_.Reset();
  copyPS_.Reset();
  quadVS_.Reset();
  rectangleGS_.Reset();

  vsFloatCB_.Reset();
  psFloatCB_.Reset();
  boolCB_.Reset();
  drawCB_.Reset();

  backView_.Reset();
  backBuffer_.Reset();
  swapChain_.Reset();
  context1_.Reset();
  context_.Reset();
  device_.Reset();
  hwnd_ = nullptr;
  swapChainHwnd_ = nullptr;
}

void RenderD3D11::SetSurface(HWND hwnd, uint32_t width, uint32_t height) {
  std::lock_guard lock(contextMutex_);
  hwnd_ = hwnd;
  clientWidth_ = std::max(1u, width);
  clientHeight_ = std::max(1u, height);
}

void RenderD3D11::ResizeSurface(uint32_t width, uint32_t height) {
  std::lock_guard lock(contextMutex_);
  if (width == 0 || height == 0) return;
  clientWidth_ = width;
  clientHeight_ = height;
}

void RenderD3D11::SetPresentOptions(bool lanczos, bool fxaa, bool letterbox, bool vsync) {
  useLanczos_.store(lanczos, std::memory_order_relaxed);
  useFxaa_.store(fxaa, std::memory_order_relaxed);
  useLetterbox_.store(letterbox, std::memory_order_relaxed);
  useVsync_.store(vsync, std::memory_order_relaxed);
}

void RenderD3D11::ForgetBindings() {
  if (context_) {
    ID3D11RenderTargetView* noRtv[4] = {};
    context_->OMSetRenderTargets(4, noRtv, nullptr);
    ID3D11ShaderResourceView* noPsSrv[32] = {};
    context_->PSSetShaderResources(0, 32, noPsSrv);
    ID3D11ShaderResourceView* noVsSrv[128] = {};
    context_->VSSetShaderResources(0, 128, noVsSrv);
    context_->PSSetShader(nullptr, nullptr, 0);
    context_->GSSetShader(nullptr, nullptr, 0);
  }
  bound_ = BoundState();
  bound_.stencilRef = ~0u;
}

void RenderD3D11::UnbindResourceLocked(ID3D11ShaderResourceView* resource) {
  if (!resource) return;
  ID3D11ShaderResourceView* nullView[1] = {nullptr};
  for (uint32_t i = 0; i < 32; ++i) {
    if (bound_.psSrv[i] == resource) {
      context_->PSSetShaderResources(i, 1, nullView);
      bound_.psSrv[i] = nullptr;
    }
  }
  for (uint32_t i = 0; i < 128; ++i) {
    if (bound_.vsSrv[i] == resource) {
      context_->VSSetShaderResources(i, 1, nullView);
      bound_.vsSrv[i] = nullptr;
    }
  }
}

void RenderD3D11::ExecuteDraw(const RenderState::DrawCommand& command) {
  if (!context_) return;
  if (command.bindingsLost) ForgetBindings();

  for (uint32_t i = 0; i < 4; ++i) UnbindResourceLocked(command.targetResources[i]);
  UnbindResourceLocked(command.depthResource);

  if (!bound_.vsSet || bound_.vs != command.vs) {
    context_->VSSetShader(command.vs, nullptr, 0);
    bound_.vs = command.vs;
    bound_.vsSet = true;
  }
  ID3D11GeometryShader* gs = command.rectangles ? rectangleGS_.Get() : nullptr;
  if (!bound_.gsSet || bound_.gs != gs) {
    context_->GSSetShader(gs, nullptr, 0);
    bound_.gs = gs;
    bound_.gsSet = true;
  }
  if (!bound_.psSet || bound_.ps != command.ps) {
    context_->PSSetShader(command.ps, nullptr, 0);
    bound_.ps = command.ps;
    bound_.psSet = true;
  }

  bool sameTargets = bound_.targetsSet && bound_.targetCount == command.targetCount &&
                     bound_.depthView == command.depthView;
  if (sameTargets) {
    for (uint32_t i = 0; i < command.targetCount; ++i) {
      if (bound_.targets[i] != command.targets[i]) {
        sameTargets = false;
        break;
      }
    }
  }
  if (!sameTargets) {
    context_->OMSetRenderTargets(command.targetCount, command.targets, command.depthView);
    bound_.targetCount = command.targetCount;
    for (uint32_t i = 0; i < 4; ++i) {
      bound_.targets[i] = i < command.targetCount ? command.targets[i] : nullptr;
    }
    bound_.depthView = command.depthView;
    bound_.targetsSet = true;
  }

  if (bound_.targetWidth != command.targetWidth || bound_.targetHeight != command.targetHeight) {
    const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(command.targetWidth),
                            static_cast<float>(command.targetHeight), 0.0f, 1.0f};
    context_->RSSetViewports(1, &vp);
    bound_.targetWidth = command.targetWidth;
    bound_.targetHeight = command.targetHeight;
  }

  if (std::memcmp(bound_.scissor, command.scissor, sizeof(bound_.scissor)) != 0) {
    const D3D11_RECT rect{command.scissor[0], command.scissor[1], command.scissor[2],
                          command.scissor[3]};
    context_->RSSetScissorRects(1, &rect);
    std::memcpy(bound_.scissor, command.scissor, sizeof(bound_.scissor));
  }

  if (bound_.rasterizer != command.rasterizer) {
    context_->RSSetState(pipeline_->RasterizerObject(command.rasterizer));
    bound_.rasterizer = command.rasterizer;
  }

  if (bound_.blend != command.blend ||
      std::memcmp(bound_.blendFactor, command.blendFactor, sizeof(bound_.blendFactor)) != 0) {
    context_->OMSetBlendState(pipeline_->BlendObject(command.blend), command.blendFactor,
                              0xFFFFFFFFu);
    bound_.blend = command.blend;
    std::memcpy(bound_.blendFactor, command.blendFactor, sizeof(bound_.blendFactor));
  }

  if (bound_.depth != command.depth || bound_.stencilRef != command.stencilReference) {
    context_->OMSetDepthStencilState(pipeline_->DepthObject(command.depth),
                                     command.stencilReference);
    bound_.depth = command.depth;
    bound_.stencilRef = command.stencilReference;
  }

  if (bound_.topology != command.topology) {
    context_->IASetPrimitiveTopology(static_cast<D3D11_PRIMITIVE_TOPOLOGY>(command.topology));
    bound_.topology = command.topology;
  }
  context_->IASetInputLayout(nullptr);

  if (command.ring && context1_) {
    ID3D11Buffer* ring = resources_->ConstantRing();
    const UINT offsets[3] = {command.floatAt[0] / 16u, 0, command.drawAt / 16u};
    const UINT counts[3] = {command.floatCount[0], 16u, command.drawBytes / 16u};
    ID3D11Buffer* bufs[3] = {ring, boolCB_.Get(), ring};
    context1_->VSSetConstantBuffers1(0, 3, bufs, offsets, counts);
    const UINT psOffsets[3] = {command.floatAt[1] / 16u, 0, command.drawAt / 16u};
    const UINT psCounts[3] = {command.floatCount[1], 16u, command.drawBytes / 16u};
    context1_->PSSetConstantBuffers1(0, 3, bufs, psOffsets, psCounts);
    if (command.boolsChanged && command.bools) {
      context_->UpdateSubresource(boolCB_.Get(), 0, nullptr, command.bools, 0, 0);
    }
  } else {
    if (command.floatChanged[0] && command.floatFile[0]) {
      context_->UpdateSubresource(vsFloatCB_.Get(), 0, nullptr, command.floatFile[0], 0, 0);
    }
    if (command.floatChanged[1] && command.floatFile[1]) {
      context_->UpdateSubresource(psFloatCB_.Get(), 0, nullptr, command.floatFile[1], 0, 0);
    }
    if (command.boolsChanged && command.bools) {
      context_->UpdateSubresource(boolCB_.Get(), 0, nullptr, command.bools, 0, 0);
    }
    if (command.drawChanged && command.drawConstants) {
      context_->UpdateSubresource(drawCB_.Get(), 0, nullptr, command.drawConstants, 0, 0);
    }
    ID3D11Buffer* vsBufs[3] = {vsFloatCB_.Get(), boolCB_.Get(), drawCB_.Get()};
    ID3D11Buffer* psBufs[3] = {psFloatCB_.Get(), boolCB_.Get(), drawCB_.Get()};
    context_->VSSetConstantBuffers(0, 3, vsBufs);
    context_->PSSetConstantBuffers(0, 3, psBufs);
  }

  for (uint32_t i = 0; i < command.textureCount; ++i) {
    const RenderState::DrawCommand::Texture& tex = command.textures[i];
    if (tex.stage == 0) {
      if (tex.slot < 32 && bound_.psSrv[tex.slot] != tex.view) {
        context_->PSSetShaderResources(tex.slot, 1, &tex.view);
        bound_.psSrv[tex.slot] = tex.view;
      }
      if (tex.samplerSlot < 32 && bound_.psSamplers[tex.samplerSlot] != tex.sampler) {
        context_->PSSetSamplers(tex.samplerSlot, 1, &tex.sampler);
        bound_.psSamplers[tex.samplerSlot] = tex.sampler;
      }
    } else {
      if (tex.slot < 32 && bound_.vsSrv[tex.slot] != tex.view) {
        context_->VSSetShaderResources(tex.slot, 1, &tex.view);
        bound_.vsSrv[tex.slot] = tex.view;
      }
      if (tex.samplerSlot < 16 && bound_.vsSamplers[tex.samplerSlot] != tex.sampler) {
        context_->VSSetSamplers(tex.samplerSlot, 1, &tex.sampler);
        bound_.vsSamplers[tex.samplerSlot] = tex.sampler;
      }
    }
  }

  for (uint32_t i = 0; i < command.bufferCount; ++i) {
    const RenderState::DrawCommand::Buffer& buf = command.buffers[i];
    const uint32_t srvSlot = 32u + buf.slot;
    if (srvSlot < 128 && bound_.vsSrv[srvSlot] != buf.view) {
      context_->VSSetShaderResources(srvSlot, 1, &buf.view);
      bound_.vsSrv[srvSlot] = buf.view;
    }
  }

  if (command.indexed) {
    ID3D11Buffer* idxBuf = command.indexBuffer ? command.indexBuffer : resources_->IndexRing();
    if (!bound_.indexSet || bound_.indices32 != command.indices32 ||
        bound_.indexBuffer != idxBuf) {
      context_->IASetIndexBuffer(idxBuf,
                                 command.indices32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, 0);
      bound_.indexSet = true;
      bound_.indices32 = command.indices32;
      bound_.indexBuffer = idxBuf;
    }
    const UINT startIndex = command.indexRingOffset / (command.indices32 ? 4u : 2u);
    context_->DrawIndexed(command.indexCount, startIndex, command.baseVertex);
  } else {
    context_->Draw(command.indexCount, static_cast<UINT>(command.baseVertex));
  }
}

void RenderD3D11::DrawQuadLocked(ID3D11ShaderResourceView* source, uint32_t sourceWidth,
                                 uint32_t sourceHeight, float x0, float y0, float width,
                                 float height, ID3D11RenderTargetView* target,
                                 const D3D11_VIEWPORT& viewport, ID3D11PixelShader* program,
                                 ID3D11SamplerState* sampler, ID3D11ShaderResourceView* second,
                                 float colorScale, float clampMode) {
  if (!quadVS_ || !program || sourceWidth == 0 || sourceHeight == 0) return;
  const float constants[8] = {
      x0 / static_cast<float>(sourceWidth),
      y0 / static_cast<float>(sourceHeight),
      width / static_cast<float>(sourceWidth),
      height / static_cast<float>(sourceHeight),
      1.0f / static_cast<float>(sourceWidth),
      1.0f / static_cast<float>(sourceHeight),
      colorScale,
      clampMode,
  };
  context_->UpdateSubresource(quadCB_.Get(), 0, nullptr, constants, 0, 0);

  ID3D11ShaderResourceView* none[32] = {};
  context_->PSSetShaderResources(0, 32, none);
  context_->VSSetShaderResources(0, 32, none);

  ID3D11RenderTargetView* targets[1] = {target};
  context_->OMSetRenderTargets(1, targets, nullptr);
  context_->RSSetViewports(1, &viewport);

  context_->RSSetState(
      pipeline_->RasterizerObject(pipeline_->Rasterizer(0, false, true, false, 0.0f, 0)));
  context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
  context_->OMSetDepthStencilState(nullptr, 0);
  context_->VSSetShader(quadVS_.Get(), nullptr, 0);
  context_->GSSetShader(nullptr, nullptr, 0);
  context_->PSSetShader(program, nullptr, 0);

  ID3D11Buffer* cbs[1] = {quadCB_.Get()};
  context_->VSSetConstantBuffers(0, 1, cbs);
  context_->PSSetConstantBuffers(0, 1, cbs);

  ID3D11ShaderResourceView* srvs[2] = {source, second};
  context_->PSSetShaderResources(0, 2, srvs);
  ID3D11SamplerState* smps[1] = {sampler};
  context_->PSSetSamplers(0, 1, smps);
  context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context_->IASetInputLayout(nullptr);
  context_->Draw(3, 0);

  context_->PSSetShaderResources(0, 32, none);
  ID3D11RenderTargetView* noTarget[1] = {nullptr};
  context_->OMSetRenderTargets(1, noTarget, nullptr);
  ForgetBindings();
}

void RenderD3D11::ExecuteResolve(const RenderState::ResolveCommand& command) {
  if (!context_) return;
  if (command.bindingsLost) ForgetBindings();

  if (command.copy) {
    UnbindResourceLocked(command.destinationResource);
    const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(command.width),
                            static_cast<float>(command.height), 0.0f, 1.0f};
    ID3D11PixelShader* ps = copyPS_.Get();
    if (command.sourceSamples > 1) {
      const int slot = command.sourceSamples >= 8 ? 3 : command.sourceSamples >= 4 ? 2 : 1;
      ps = command.fromDepth ? copyDepthMS_[slot].Get() : copyColorMS_[slot].Get();
    }
    if (ps) {
      DrawQuadLocked(command.source, command.sourceWidth, command.sourceHeight, command.x0,
                     command.y0, static_cast<float>(command.width),
                     static_cast<float>(command.height), command.destination, vp, ps,
                     pointSampler_.Get(), nullptr, command.colorScale, command.clampMode);
    }
  }

  if (command.colorClear && command.clearTarget) {
    if (context1_) {
      const D3D11_RECT rect{command.clearRect[0], command.clearRect[1], command.clearRect[2],
                            command.clearRect[3]};
      context1_->ClearView(command.clearTarget, command.clearColor, &rect, 1);
    } else {
      context_->ClearRenderTargetView(command.clearTarget, command.clearColor);
    }
  }

  if (command.depthClear && command.clearDepth) {
    context_->ClearDepthStencilView(command.clearDepth, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                    command.clearDepthValue, command.clearStencilValue);
  }
}

void RenderD3D11::OnSwap(uint32_t frontbufferAddr, uint32_t guestWidth, uint32_t guestHeight) {
  std::lock_guard lock(contextMutex_);
  frontbufferAddr_ = frontbufferAddr & 0x1FFFFFFFu;
  if (guestWidth > 1 && guestHeight > 1) {
    frontGuestW_ = guestWidth;
    frontGuestH_ = guestHeight;
  }
  swapCount_.fetch_add(1, std::memory_order_relaxed);
}

bool RenderD3D11::EnsureSwapChainLocked() {
  if (!hwnd_ || clientWidth_ == 0 || clientHeight_ == 0 || !device_) return false;
  if (!swapChain_ || swapChainHwnd_ != hwnd_) {
    backView_.Reset();
    backBuffer_.Reset();
    swapChain_.Reset();
    ComPtr<IDXGIDevice> dxgiDev;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(device_.As(&dxgiDev)) || FAILED(dxgiDev->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
      return false;
    }
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = clientWidth_;
    desc.Height = clientHeight_;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 3;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Flags = tearingSupported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (FAILED(factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &desc, nullptr, nullptr,
                                               &swapChain_))) {
      return false;
    }
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    ComPtr<IDXGISwapChain2> chain2;
    if (SUCCEEDED(swapChain_.As(&chain2))) chain2->SetMaximumFrameLatency(2);
    swapChainHwnd_ = hwnd_;
    swapWidth_ = clientWidth_;
    swapHeight_ = clientHeight_;
  } else if (swapWidth_ != clientWidth_ || swapHeight_ != clientHeight_) {
    backView_.Reset();
    backBuffer_.Reset();
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    ForgetBindings();
    swapChain_->ResizeBuffers(0, clientWidth_, clientHeight_, DXGI_FORMAT_UNKNOWN,
                              tearingSupported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    swapWidth_ = clientWidth_;
    swapHeight_ = clientHeight_;
  }
  if (!backView_) {
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer_)))) return false;
    if (FAILED(device_->CreateRenderTargetView(backBuffer_.Get(), nullptr, &backView_))) {
      return false;
    }
  }
  return backView_ != nullptr;
}

void RenderD3D11::UpdateGammaLutLocked() {
  if (!gammaTex_) {
    D3D11_TEXTURE1D_DESC td{};
    td.Width = 256;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    uint32_t identity[256];
    for (uint32_t i = 0; i < 256; ++i) {
      const uint32_t v = (i << 2) | (i >> 6);
      identity[i] = v | (v << 10) | (v << 20) | (3u << 30);
    }
    const D3D11_SUBRESOURCE_DATA init{identity, 0, 0};
    if (SUCCEEDED(device_->CreateTexture1D(&td, &init, &gammaTex_))) {
      device_->CreateShaderResourceView(gammaTex_.Get(), nullptr, &gammaSrv_);
    }
  }
  if (pm4_ && !gammaTex_) {}
  uint32_t table[256] = {};
  uint64_t version = 0;
  if (pm4_ && pm4_->CopyGammaTable(table, version) && version != gammaVersion_) {
    gammaVersion_ = version;
    uint32_t texels[256];
    for (uint32_t i = 0; i < 256; ++i) {
      const uint32_t e = table[i];
      texels[i] =
          ((e >> 20) & 0x3FFu) | (((e >> 10) & 0x3FFu) << 10) | ((e & 0x3FFu) << 20) | (3u << 30);
    }
    context_->UpdateSubresource(gammaTex_.Get(), 0, nullptr, texels, 0, 0);
  }
}

bool RenderD3D11::BeginPresent() {
  std::lock_guard lock(contextMutex_);
  presentingFrame_ = false;
  if (!EnsureSwapChainLocked()) return false;

  const float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  context_->ClearRenderTargetView(backView_.Get(), clearColor);

  const RenderResources::Resolved* fb =
      frontbufferAddr_ ? resources_->ResolvedAt(frontbufferAddr_) : nullptr;
  if (!fb && frontbufferAddr_ != 0) {
    fb = resources_->UploadCpuFrontBuffer(frontbufferAddr_, frontGuestW_, frontGuestH_);
  }

  if (fb && fb->resource && fb->width > 0 && fb->height > 0) {
    UpdateGammaLutLocked();
    const float fbAspect = static_cast<float>(fb->width) / static_cast<float>(fb->height);
    const float winAspect = static_cast<float>(swapWidth_) / static_cast<float>(swapHeight_);
    float vpX = 0.0f;
    float vpY = 0.0f;
    float vpW = static_cast<float>(swapWidth_);
    float vpH = static_cast<float>(swapHeight_);
    if (useLetterbox_.load(std::memory_order_relaxed) && std::abs(fbAspect - winAspect) > 0.005f) {
      if (winAspect > fbAspect) {
        vpW = vpH * fbAspect;
        vpX = (static_cast<float>(swapWidth_) - vpW) * 0.5f;
      } else {
        vpH = vpW / fbAspect;
        vpY = (static_cast<float>(swapHeight_) - vpH) * 0.5f;
      }
    }
    const D3D11_VIEWPORT vp{vpX, vpY, vpW, vpH, 0.0f, 1.0f};
    ID3D11PixelShader* ps = useFxaa_.load(std::memory_order_relaxed)
                                ? presentFxaaPS_.Get()
                                : (useLanczos_.load(std::memory_order_relaxed)
                                       ? presentLanczosPS_.Get()
                                       : presentBilinearPS_.Get());
    DrawQuadLocked(fb->resource, fb->width, fb->height, 0.0f, 0.0f, static_cast<float>(fb->width),
                   static_cast<float>(fb->height), backView_.Get(), vp, ps, linearSampler_.Get(),
                   gammaSrv_.Get());
  }

  presentingFrame_ = true;
  return true;
}

void RenderD3D11::UpdateImGuiTextureLocked(ImTextureData* tex) {
  if (!tex) return;
  if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates) {
    uint64_t id = static_cast<uint64_t>(tex->GetTexID());
    if (id == 0 || tex->Status == ImTextureStatus_WantCreate) {
      id = nextImGuiTexId_++;
      tex->SetTexID(static_cast<ImTextureID>(id));
    }
    const uint32_t w = static_cast<uint32_t>(tex->Width);
    const uint32_t h = static_cast<uint32_t>(tex->Height);
    if (w == 0 || h == 0 || !tex->GetPixels()) return;

    std::vector<uint32_t> rgba(static_cast<size_t>(w) * h);
    const auto* src = static_cast<const uint8_t*>(tex->GetPixels());
    if (tex->Format == ImTextureFormat_Alpha8) {
      for (size_t i = 0; i < rgba.size(); ++i) {
        rgba[i] = 0x00FFFFFFu | (static_cast<uint32_t>(src[i]) << 24);
      }
    } else {
      std::memcpy(rgba.data(), src, rgba.size() * 4u);
    }

    ImGuiTexture& entry = imguiTextures_[id];
    if (!entry.texture || entry.width != w || entry.height != h) {
      entry.texture.Reset();
      entry.srv.Reset();
      D3D11_TEXTURE2D_DESC td{};
      td.Width = w;
      td.Height = h;
      td.MipLevels = 1;
      td.ArraySize = 1;
      td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      td.SampleDesc.Count = 1;
      td.Usage = D3D11_USAGE_DEFAULT;
      td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      const D3D11_SUBRESOURCE_DATA init{rgba.data(), w * 4u, 0};
      if (SUCCEEDED(device_->CreateTexture2D(&td, &init, &entry.texture))) {
        device_->CreateShaderResourceView(entry.texture.Get(), nullptr, &entry.srv);
        entry.width = w;
        entry.height = h;
      }
    } else {
      context_->UpdateSubresource(entry.texture.Get(), 0, nullptr, rgba.data(), w * 4u, 0);
    }
    tex->SetStatus(ImTextureStatus_OK);
  } else if (tex->Status == ImTextureStatus_WantDestroy) {
    const uint64_t id = static_cast<uint64_t>(tex->GetTexID());
    imguiTextures_.erase(id);
    tex->SetTexID(ImTextureID_Invalid);
    tex->SetStatus(ImTextureStatus_Destroyed);
  }
}

void RenderD3D11::SyncImGuiTexturesLocked(ImDrawData& drawData) {
  if (!drawData.Textures) return;
  for (ImTextureData* tex : *drawData.Textures) {
    if (tex && tex->Status != ImTextureStatus_OK && tex->Status != ImTextureStatus_Destroyed) {
      UpdateImGuiTextureLocked(tex);
    }
  }
}

void RenderD3D11::RenderOverlays(ImDrawData& drawData) {
  std::lock_guard lock(contextMutex_);
  if (!presentingFrame_ || !backView_ || !imguiVS_ || !imguiPS_) return;

  SyncImGuiTexturesLocked(drawData);
  if (drawData.TotalVtxCount <= 0 || drawData.DisplaySize.x <= 0.0f ||
      drawData.DisplaySize.y <= 0.0f) {
    return;
  }

  const uint32_t neededVtx = static_cast<uint32_t>(drawData.TotalVtxCount);
  const uint32_t neededIdx = static_cast<uint32_t>(drawData.TotalIdxCount);
  if (!imguiVB_ || imguiVBCount_ < neededVtx) {
    imguiVB_.Reset();
    imguiVBCount_ = neededVtx + 4096u;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = imguiVBCount_ * sizeof(ImDrawVert);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateBuffer(&bd, nullptr, &imguiVB_))) return;
  }
  if (!imguiIB_ || imguiIBCount_ < neededIdx) {
    imguiIB_.Reset();
    imguiIBCount_ = neededIdx + 8192u;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = imguiIBCount_ * sizeof(ImDrawIdx);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateBuffer(&bd, nullptr, &imguiIB_))) return;
  }

  D3D11_MAPPED_SUBRESOURCE vtxMap{};
  D3D11_MAPPED_SUBRESOURCE idxMap{};
  if (FAILED(context_->Map(imguiVB_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &vtxMap))) return;
  if (FAILED(context_->Map(imguiIB_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &idxMap))) {
    context_->Unmap(imguiVB_.Get(), 0);
    return;
  }
  auto* vtxDst = static_cast<ImDrawVert*>(vtxMap.pData);
  auto* idxDst = static_cast<ImDrawIdx*>(idxMap.pData);
  for (int n = 0; n < drawData.CmdListsCount; ++n) {
    const ImDrawList* list = drawData.CmdLists[n];
    std::memcpy(vtxDst, list->VtxBuffer.Data, list->VtxBuffer.Size * sizeof(ImDrawVert));
    std::memcpy(idxDst, list->IdxBuffer.Data, list->IdxBuffer.Size * sizeof(ImDrawIdx));
    vtxDst += list->VtxBuffer.Size;
    idxDst += list->IdxBuffer.Size;
  }
  context_->Unmap(imguiVB_.Get(), 0);
  context_->Unmap(imguiIB_.Get(), 0);

  D3D11_MAPPED_SUBRESOURCE cbMap{};
  if (SUCCEEDED(context_->Map(imguiCB_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &cbMap))) {
    const float L = drawData.DisplayPos.x;
    const float R = drawData.DisplayPos.x + drawData.DisplaySize.x;
    const float T = drawData.DisplayPos.y;
    const float B = drawData.DisplayPos.y + drawData.DisplaySize.y;
    const float mvp[4][4] = {
        {2.0f / (R - L), 0.0f, 0.0f, 0.0f},
        {0.0f, 2.0f / (T - B), 0.0f, 0.0f},
        {0.0f, 0.0f, 0.5f, 0.0f},
        {(R + L) / (L - R), (T + B) / (B - T), 0.5f, 1.0f},
    };
    std::memcpy(cbMap.pData, mvp, sizeof(mvp));
    context_->Unmap(imguiCB_.Get(), 0);
  }

  ID3D11RenderTargetView* targets[1] = {backView_.Get()};
  context_->OMSetRenderTargets(1, targets, nullptr);
  const D3D11_VIEWPORT vp{0.0f, 0.0f, drawData.DisplaySize.x, drawData.DisplaySize.y, 0.0f, 1.0f};
  context_->RSSetViewports(1, &vp);

  const UINT stride = sizeof(ImDrawVert);
  const UINT offset = 0;
  ID3D11Buffer* vbs[1] = {imguiVB_.Get()};
  context_->IASetInputLayout(imguiLayout_.Get());
  context_->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
  context_->IASetIndexBuffer(imguiIB_.Get(),
                             sizeof(ImDrawIdx) == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT,
                             0);
  context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context_->VSSetShader(imguiVS_.Get(), nullptr, 0);
  context_->GSSetShader(nullptr, nullptr, 0);
  context_->PSSetShader(imguiPS_.Get(), nullptr, 0);
  ID3D11Buffer* cbs[1] = {imguiCB_.Get()};
  context_->VSSetConstantBuffers(0, 1, cbs);
  ID3D11SamplerState* smps[1] = {linearSampler_.Get()};
  context_->PSSetSamplers(0, 1, smps);
  const float blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  context_->OMSetBlendState(imguiBlend_.Get(), blendFactor, 0xFFFFFFFFu);
  context_->OMSetDepthStencilState(imguiDepth_.Get(), 0);
  context_->RSSetState(imguiRasterizer_.Get());

  int vtxBase = 0;
  int idxBase = 0;
  const ImVec2 clipOff = drawData.DisplayPos;
  for (int n = 0; n < drawData.CmdListsCount; ++n) {
    const ImDrawList* list = drawData.CmdLists[n];
    for (int c = 0; c < list->CmdBuffer.Size; ++c) {
      const ImDrawCmd& cmd = list->CmdBuffer[c];
      if (cmd.UserCallback) continue;
      const D3D11_RECT clip{
          static_cast<LONG>(cmd.ClipRect.x - clipOff.x),
          static_cast<LONG>(cmd.ClipRect.y - clipOff.y),
          static_cast<LONG>(cmd.ClipRect.z - clipOff.x),
          static_cast<LONG>(cmd.ClipRect.w - clipOff.y),
      };
      if (clip.right <= clip.left || clip.bottom <= clip.top) continue;
      context_->RSSetScissorRects(1, &clip);
      const uint64_t tid = static_cast<uint64_t>(cmd.GetTexID());
      auto it = imguiTextures_.find(tid);
      ID3D11ShaderResourceView* srv = it != imguiTextures_.end() ? it->second.srv.Get() : nullptr;
      context_->PSSetShaderResources(0, 1, &srv);
      context_->DrawIndexed(cmd.ElemCount, static_cast<UINT>(idxBase + cmd.IdxOffset),
                            vtxBase + static_cast<INT>(cmd.VtxOffset));
    }
    vtxBase += list->VtxBuffer.Size;
    idxBase += list->IdxBuffer.Size;
  }

  ID3D11ShaderResourceView* noSrv[1] = {nullptr};
  context_->PSSetShaderResources(0, 1, noSrv);
  ID3D11RenderTargetView* noTarget[1] = {nullptr};
  context_->OMSetRenderTargets(1, noTarget, nullptr);
  ForgetBindings();
}

void RenderD3D11::RequestFrameCapture(const char* tag) {
  std::lock_guard lock(contextMutex_);
  if (tag && tag[0]) captureTag_ = tag;
}

void RenderD3D11::EndPresent() {
  std::lock_guard lock(contextMutex_);
  if (!presentingFrame_ || !swapChain_) return;

  if (!captureTag_.empty() && backBuffer_) {
    const char* dirEnv = std::getenv("BS1_CAPTURE_DIR");
    if (dirEnv && dirEnv[0]) {
      D3D11_TEXTURE2D_DESC desc{};
      backBuffer_->GetDesc(&desc);
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      desc.MiscFlags = 0;
      ComPtr<ID3D11Texture2D> staging;
      if (SUCCEEDED(device_->CreateTexture2D(&desc, nullptr, &staging))) {
        context_->CopyResource(staging.Get(), backBuffer_.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
          std::error_code ec;
          std::filesystem::create_directories(dirEnv, ec);
          const std::filesystem::path outPath =
              std::filesystem::path(dirEnv) / (captureTag_ + ".bmp");
          if (FILE* f = std::fopen(outPath.string().c_str(), "wb")) {
            const uint32_t w = desc.Width;
            const uint32_t h = desc.Height;
            const uint32_t imgBytes = w * h * 4u;
            uint8_t hdr[54] = {};
            hdr[0] = 'B';
            hdr[1] = 'M';
            const uint32_t fileSize = 54u + imgBytes;
            std::memcpy(hdr + 2, &fileSize, 4);
            const uint32_t off = 54u;
            std::memcpy(hdr + 10, &off, 4);
            const uint32_t dib = 40u;
            std::memcpy(hdr + 14, &dib, 4);
            const int32_t sw = static_cast<int32_t>(w);
            const int32_t sh = -static_cast<int32_t>(h);  // top-down
            std::memcpy(hdr + 18, &sw, 4);
            std::memcpy(hdr + 22, &sh, 4);
            const uint16_t planes = 1;
            const uint16_t bpp = 32;
            std::memcpy(hdr + 26, &planes, 2);
            std::memcpy(hdr + 28, &bpp, 2);
            std::memcpy(hdr + 34, &imgBytes, 4);
            std::fwrite(hdr, 1, 54, f);
            for (uint32_t y = 0; y < h; ++y) {
              std::fwrite(static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch,
                          1, size_t(w) * 4u, f);
            }
            std::fclose(f);
            REXLOG_INFO("custom-renderer: captured frame '{}' ({}x{}) -> {}", captureTag_, w, h,
                        outPath.string());
          }
          context_->Unmap(staging.Get(), 0);
        }
      }
    }
    captureTag_.clear();
  }

  const bool vsync = useVsync_.load(std::memory_order_relaxed);
  const UINT syncInterval = vsync ? 1u : 0u;
  const UINT flags = (!vsync && tearingSupported_) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
  swapChain_->Present(syncInterval, flags);
  presentingFrame_ = false;
}

}  // namespace bs1::renderer
