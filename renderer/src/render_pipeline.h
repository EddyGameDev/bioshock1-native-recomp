#pragma once

#include "render_state.h"
#include "render_table.h"

#include <d3d11_1.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace bs1::renderer {

/**
 * Caches D3D11 pipeline state objects (blend, depth-stencil, rasterizer, sampler)
 * keyed by Xenos register words.
 */
class RenderPipeline {
 public:
  using Handle = RenderState::Handle;

  enum class Filter { Native, Bilinear, Trilinear, Anisotropic };

  void Initialize(ID3D11Device* device);
  void Shutdown();

  void SetFiltering(Filter filter, uint32_t anisotropy);
  Filter GetFiltering() const { return filter_; }
  uint32_t GetAnisotropy() const { return anisotropy_; }

  Handle Blend(const uint32_t control[4], uint32_t colorMask);
  Handle Depth(uint32_t depthControl, uint32_t stencilRefMask);
  Handle Rasterizer(uint32_t suScModeControl, bool scissor, bool cullNone, bool depthClip,
                    float slopeScaledBias, int32_t depthBias);
  Handle Sampler(const uint32_t fetch[6]);

  ID3D11BlendState* BlendObject(Handle handle) const { return blend_.Get(handle); }
  ID3D11DepthStencilState* DepthObject(Handle handle) const { return depth_.Get(handle); }
  ID3D11RasterizerState* RasterizerObject(Handle handle) const { return rasterizer_.Get(handle); }
  ID3D11SamplerState* SamplerObject(Handle handle) const { return sampler_.Get(handle); }

  static bool BlendTakesConstantAlpha(const uint32_t control[4]);

 private:
  template <int KeyWords, typename Object>
  struct StateTable {
    KeyTable<KeyWords> keys;
    std::vector<Microsoft::WRL::ComPtr<Object>> objects;

    StateTable() { objects.emplace_back(); }

    uint32_t Find(const uint32_t* key) const { return keys.Find(key); }

    uint32_t Insert(const uint32_t* key, Microsoft::WRL::ComPtr<Object> obj) {
      const uint32_t handle = static_cast<uint32_t>(objects.size());
      objects.push_back(std::move(obj));
      keys.Insert(key, handle);
      return handle;
    }

    Object* Get(uint32_t handle) const {
      return handle < objects.size() ? objects[handle].Get() : nullptr;
    }

    void Clear() {
      keys.Clear();
      objects.clear();
      objects.emplace_back();
    }
  };

  Microsoft::WRL::ComPtr<ID3D11BlendState> MakeBlend(const uint32_t control[4],
                                                     uint32_t colorMask);
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> MakeDepth(uint32_t depthControl,
                                                            uint32_t stencilRefMask);
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> MakeRasterizer(uint32_t key, float slopeScaledBias,
                                                               int32_t depthBias);
  Microsoft::WRL::ComPtr<ID3D11SamplerState> MakeSampler(uint32_t key);

  ID3D11Device* device_ = nullptr;
  Filter filter_ = Filter::Anisotropic;
  uint32_t anisotropy_ = 8;

  StateTable<5, ID3D11BlendState> blend_;
  StateTable<2, ID3D11DepthStencilState> depth_;
  StateTable<3, ID3D11RasterizerState> rasterizer_;
  StateTable<2, ID3D11SamplerState> sampler_;
};

}  // namespace bs1::renderer
