#include "render_pipeline.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bs1::renderer {

namespace {

D3D11_BLEND BlendFactor(uint32_t factor, bool alpha) {
  switch (factor) {
    case 0:  return D3D11_BLEND_ZERO;
    case 1:  return D3D11_BLEND_ONE;
    case 4:  return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 5:  return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 6:  return D3D11_BLEND_SRC_ALPHA;
    case 7:  return D3D11_BLEND_INV_SRC_ALPHA;
    case 8:  return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 9:  return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 10: return D3D11_BLEND_DEST_ALPHA;
    case 11: return D3D11_BLEND_INV_DEST_ALPHA;
    case 12: return D3D11_BLEND_BLEND_FACTOR;
    case 13: return D3D11_BLEND_INV_BLEND_FACTOR;
    case 14: return D3D11_BLEND_BLEND_FACTOR;
    case 15: return D3D11_BLEND_INV_BLEND_FACTOR;
    case 16: return D3D11_BLEND_SRC_ALPHA_SAT;
    default: return D3D11_BLEND_ONE;
  }
}

D3D11_BLEND_OP BlendOperation(uint32_t op) {
  switch (op) {
    case 0: return D3D11_BLEND_OP_ADD;
    case 1: return D3D11_BLEND_OP_SUBTRACT;
    case 2: return D3D11_BLEND_OP_MIN;
    case 3: return D3D11_BLEND_OP_MAX;
    case 4: return D3D11_BLEND_OP_REV_SUBTRACT;
    default: return D3D11_BLEND_OP_ADD;
  }
}

D3D11_STENCIL_OP StencilOperation(uint32_t op) {
  switch (op) {
    case 0: return D3D11_STENCIL_OP_KEEP;
    case 1: return D3D11_STENCIL_OP_ZERO;
    case 2: return D3D11_STENCIL_OP_REPLACE;
    case 3: return D3D11_STENCIL_OP_INCR_SAT;
    case 4: return D3D11_STENCIL_OP_DECR_SAT;
    case 5: return D3D11_STENCIL_OP_INVERT;
    case 6: return D3D11_STENCIL_OP_INCR;
    case 7: return D3D11_STENCIL_OP_DECR;
    default: return D3D11_STENCIL_OP_KEEP;
  }
}

D3D11_TEXTURE_ADDRESS_MODE AddressMode(uint32_t clamp) {
  switch (clamp) {
    case 0: return D3D11_TEXTURE_ADDRESS_WRAP;
    case 1: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 2: return D3D11_TEXTURE_ADDRESS_CLAMP;
    case 3: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
    case 4: return D3D11_TEXTURE_ADDRESS_CLAMP;
    case 5: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
    default: return D3D11_TEXTURE_ADDRESS_BORDER;
  }
}

}  // namespace

void RenderPipeline::Initialize(ID3D11Device* device) {
  device_ = device;
  Shutdown();
  device_ = device;
}

void RenderPipeline::Shutdown() {
  blend_.Clear();
  depth_.Clear();
  rasterizer_.Clear();
  sampler_.Clear();
  device_ = nullptr;
}

void RenderPipeline::SetFiltering(Filter filter, uint32_t anisotropy) {
  anisotropy = std::clamp(anisotropy, 1u, 16u);
  if (filter == filter_ && anisotropy == anisotropy_) return;
  filter_ = filter;
  anisotropy_ = anisotropy;
  sampler_.Clear();
}

Microsoft::WRL::ComPtr<ID3D11BlendState> RenderPipeline::MakeBlend(const uint32_t control[4],
                                                                   uint32_t colorMask) {
  D3D11_BLEND_DESC desc{};
  desc.IndependentBlendEnable = TRUE;
  for (int i = 0; i < 4; ++i) {
    const uint32_t c = control[i];
    D3D11_RENDER_TARGET_BLEND_DESC& target = desc.RenderTarget[i];
    const uint32_t srcColor = c & 0x1F;
    const uint32_t colorOp = (c >> 5) & 7;
    const uint32_t dstColor = (c >> 8) & 0x1F;
    const uint32_t srcAlpha = (c >> 16) & 0x1F;
    const uint32_t alphaOp = (c >> 21) & 7;
    const uint32_t dstAlpha = (c >> 24) & 0x1F;
    const bool trivial = srcColor == 1 && dstColor == 0 && colorOp == 0 && srcAlpha == 1 &&
                         dstAlpha == 0 && alphaOp == 0;
    target.BlendEnable = trivial ? FALSE : TRUE;
    target.SrcBlend = BlendFactor(srcColor, false);
    target.DestBlend = BlendFactor(dstColor, false);
    target.BlendOp = BlendOperation(colorOp);
    target.SrcBlendAlpha = BlendFactor(srcAlpha, true);
    target.DestBlendAlpha = BlendFactor(dstAlpha, true);
    target.BlendOpAlpha = BlendOperation(alphaOp);
    target.RenderTargetWriteMask = static_cast<UINT8>((colorMask >> (i * 4)) & 0xF);
  }
  Microsoft::WRL::ComPtr<ID3D11BlendState> state;
  if (device_) device_->CreateBlendState(&desc, &state);
  return state;
}

Microsoft::WRL::ComPtr<ID3D11DepthStencilState> RenderPipeline::MakeDepth(uint32_t depthControl,
                                                                          uint32_t stencilRefMask) {
  D3D11_DEPTH_STENCIL_DESC desc{};
  desc.DepthEnable = (depthControl >> 1) & 1;
  desc.DepthWriteMask =
      ((depthControl >> 2) & 1) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
  desc.DepthFunc = static_cast<D3D11_COMPARISON_FUNC>(((depthControl >> 4) & 7) + 1);
  desc.StencilEnable = depthControl & 1;
  desc.StencilReadMask = static_cast<UINT8>((stencilRefMask >> 8) & 0xFF);
  desc.StencilWriteMask = static_cast<UINT8>((stencilRefMask >> 16) & 0xFF);
  desc.FrontFace.StencilFunc = static_cast<D3D11_COMPARISON_FUNC>(((depthControl >> 8) & 7) + 1);
  desc.FrontFace.StencilFailOp = StencilOperation((depthControl >> 11) & 7);
  desc.FrontFace.StencilPassOp = StencilOperation((depthControl >> 14) & 7);
  desc.FrontFace.StencilDepthFailOp = StencilOperation((depthControl >> 17) & 7);
  if ((depthControl >> 7) & 1) {
    desc.BackFace.StencilFunc = static_cast<D3D11_COMPARISON_FUNC>(((depthControl >> 20) & 7) + 1);
    desc.BackFace.StencilFailOp = StencilOperation((depthControl >> 23) & 7);
    desc.BackFace.StencilPassOp = StencilOperation((depthControl >> 26) & 7);
    desc.BackFace.StencilDepthFailOp = StencilOperation((depthControl >> 29) & 7);
  } else {
    desc.BackFace = desc.FrontFace;
  }
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> state;
  if (device_) device_->CreateDepthStencilState(&desc, &state);
  return state;
}

Microsoft::WRL::ComPtr<ID3D11RasterizerState> RenderPipeline::MakeRasterizer(uint32_t key,
                                                                             float slopeScaledBias,
                                                                             int32_t depthBias) {
  D3D11_RASTERIZER_DESC desc{};
  desc.FillMode = D3D11_FILL_SOLID;
  const bool cullFront = (key & 1) != 0;
  const bool cullBack = (key & 2) != 0;
  desc.CullMode = (key & 16) ? D3D11_CULL_NONE
                : cullFront && cullBack ? D3D11_CULL_BACK
                : cullFront ? D3D11_CULL_FRONT
                : cullBack  ? D3D11_CULL_BACK
                            : D3D11_CULL_NONE;
  desc.FrontCounterClockwise = ((key >> 2) & 1) == 0 ? TRUE : FALSE;
  desc.DepthClipEnable = (key & 32) ? TRUE : FALSE;
  desc.ScissorEnable = (key & 8) ? TRUE : FALSE;
  desc.MultisampleEnable = TRUE;

  desc.DepthBias = depthBias;
  desc.SlopeScaledDepthBias = slopeScaledBias;
  desc.DepthBiasClamp = 0.0f;

  Microsoft::WRL::ComPtr<ID3D11RasterizerState> state;
  if (device_) device_->CreateRasterizerState(&desc, &state);
  return state;
}

Microsoft::WRL::ComPtr<ID3D11SamplerState> RenderPipeline::MakeSampler(uint32_t key) {
  const uint32_t clampX = key & 7;
  const uint32_t clampY = (key >> 3) & 7;
  const uint32_t clampZ = (key >> 6) & 7;
  const uint32_t mag = (key >> 9) & 3;
  const uint32_t min = (key >> 11) & 3;
  const uint32_t mip = (key >> 13) & 3;
  const uint32_t aniso = (key >> 15) & 7;
  const uint32_t border = (key >> 18) & 3;

  D3D11_SAMPLER_DESC desc{};
  bool linearMag = mag == 1;
  bool linearMin = min == 1;
  bool linearMip = mip == 1;
  const bool noMips = mip == 2;
  uint32_t anisotropy = (aniso >= 1 && aniso <= 4) ? (1u << aniso) : 1;
  switch (filter_) {
    case Filter::Native:
      break;
    case Filter::Bilinear:
      linearMag = linearMin = true;
      anisotropy = 1;
      break;
    case Filter::Trilinear:
      linearMag = linearMin = true;
      linearMip = !noMips;
      anisotropy = 1;
      break;
    case Filter::Anisotropic:
      linearMag = linearMin = true;
      linearMip = !noMips;
      anisotropy = noMips ? 1 : anisotropy_;
      break;
  }
  if (anisotropy > 1) {
    desc.Filter = D3D11_FILTER_ANISOTROPIC;
    desc.MaxAnisotropy = anisotropy;
  } else {
    desc.Filter = static_cast<D3D11_FILTER>((linearMin ? 0x10 : 0) | (linearMag ? 0x4 : 0) |
                                            (linearMip ? 0x1 : 0));
    desc.MaxAnisotropy = 1;
  }
  desc.AddressU = AddressMode(clampX);
  desc.AddressV = AddressMode(clampY);
  desc.AddressW = AddressMode(clampZ);
  desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
  desc.MinLOD = 0.0f;
  desc.MaxLOD = noMips ? 0.0f : D3D11_FLOAT32_MAX;
  const float white = (border == 1) ? 1.0f : 0.0f;
  for (float& f : desc.BorderColor) f = white;

  Microsoft::WRL::ComPtr<ID3D11SamplerState> state;
  if (device_) device_->CreateSamplerState(&desc, &state);
  return state;
}

RenderPipeline::Handle RenderPipeline::Blend(const uint32_t control[4], uint32_t colorMask) {
  const uint32_t key[5] = {control[0], control[1], control[2], control[3], colorMask};
  if (const uint32_t handle = blend_.Find(key)) return handle;
  return blend_.Insert(key, MakeBlend(control, colorMask));
}

RenderPipeline::Handle RenderPipeline::Depth(uint32_t depthControl, uint32_t stencilRefMask) {
  const uint32_t key[2] = {depthControl, stencilRefMask & 0x00FFFF00u};
  if (const uint32_t handle = depth_.Find(key)) return handle;
  return depth_.Insert(key, MakeDepth(depthControl, stencilRefMask));
}

RenderPipeline::Handle RenderPipeline::Rasterizer(uint32_t suScModeControl, bool scissor,
                                                  bool cullNone, bool depthClip,
                                                  float slopeScaledBias, int32_t depthBias) {
  uint32_t slopeBits = 0;
  std::memcpy(&slopeBits, &slopeScaledBias, 4);
  const uint32_t key[3] = {
      (suScModeControl & 7u) | (scissor ? 8u : 0u) | (cullNone ? 16u : 0u) | (depthClip ? 32u : 0u),
      slopeBits, static_cast<uint32_t>(depthBias)};
  if (const uint32_t handle = rasterizer_.Find(key)) return handle;
  return rasterizer_.Insert(key, MakeRasterizer(key[0], slopeScaledBias, depthBias));
}

RenderPipeline::Handle RenderPipeline::Sampler(const uint32_t fetch[6]) {
  const uint32_t clampX = (fetch[0] >> 10) & 7;
  const uint32_t clampY = (fetch[0] >> 13) & 7;
  const uint32_t clampZ = (fetch[0] >> 16) & 7;
  uint32_t mag = (fetch[3] >> 19) & 3;
  uint32_t min = (fetch[3] >> 21) & 3;
  uint32_t mip = (fetch[3] >> 23) & 3;
  const uint32_t aniso = (fetch[3] >> 25) & 7;
  const uint32_t border = fetch[5] & 3;
  if (mag == 3) mag = 1;
  if (min == 3) min = 1;
  if (mip == 3) mip = 1;
  const uint32_t key[2] = {clampX | (clampY << 3) | (clampZ << 6) | (mag << 9) | (min << 11) |
                               (mip << 13) | (aniso << 15) | (border << 18),
                           0};
  if (const uint32_t handle = sampler_.Find(key)) return handle;
  return sampler_.Insert(key, MakeSampler(key[0]));
}

bool RenderPipeline::BlendTakesConstantAlpha(const uint32_t control[4]) {
  for (int i = 0; i < 4; ++i) {
    const uint32_t word = control[i];
    const uint32_t factors[4] = {word & 0x1F, (word >> 8) & 0x1F, (word >> 16) & 0x1F,
                                 (word >> 24) & 0x1F};
    for (uint32_t factor : factors) {
      if (factor == 14 || factor == 15) return true;
    }
  }
  return false;
}

}  // namespace bs1::renderer
