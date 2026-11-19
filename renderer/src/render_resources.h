#pragma once

#include "render_state.h"
#include "render_table.h"

#include <d3d11_1.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include <rex/guest.h>
#include <rex/utility.h>

namespace bs1::renderer {

/**
 * Manages D3D11 render targets, resolved surfaces, untiled guest textures,
 * raw vertex buffers, static index buffers, and dynamic streaming rings.
 * Supports live scale and MSAA changes at frame boundaries with zero restart.
 */
class RenderResources final : public rex::IPhysicalWriteListener {
 public:
  using Handle = RenderState::Handle;

  struct ColorTarget {
    ID3D11Texture2D* texture = nullptr;
    ID3D11RenderTargetView* view = nullptr;
    ID3D11ShaderResourceView* resource = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pitch = 0;
    uint32_t rows = 0;
    uint32_t samples = 1;
    uint32_t msaa = 0;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
  };

  struct DepthTarget {
    ID3D11Texture2D* texture = nullptr;
    ID3D11DepthStencilView* view = nullptr;
    ID3D11ShaderResourceView* resource = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pitch = 0;
    uint32_t rows = 0;
    uint32_t samples = 1;
    uint32_t msaa = 0;
  };

  struct Resolved {
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* resource = nullptr;
    ID3D11RenderTargetView* view = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t guestWidth = 0;
    uint32_t guestHeight = 0;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    bool depth = false;
    bool rbSwapped = false;
  };

  void Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                  rex::IPhysicalMemory* memory, rex::IPhysicalWriteWatch* watch);
  void Shutdown();

  void RequestScale(float scale);
  float GetScale() const { return scale_; }
  void RequestMultisample(uint32_t samples);
  uint32_t GetSamples() const { return samples_; }

  /** Applies pending scale/MSAA changes and evicts stale resources. Returns true if targets rebuilt. */
  bool BeginFrame(uint64_t frame);

  void TargetSize(uint32_t surfaceInfo, uint32_t bytesPerSample, uint32_t& width, uint32_t& height,
                  float& scaleX, float& scaleY) const;
  bool EnsureRows(uint32_t surfaceInfo, uint32_t rows);

  Handle ColorTargetFor(uint32_t colorInfo, uint32_t surfaceInfo);
  Handle DepthTargetFor(uint32_t depthInfo, uint32_t surfaceInfo);
  Handle PrimaryColorTargetAt(uint32_t base, uint32_t minPitch, uint32_t cls) const;
  Handle PrimaryDepthTargetAt(uint32_t base, uint32_t minPitch, uint32_t cls) const;
  const ColorTarget* ColorTargetOf(Handle handle) const;
  const DepthTarget* DepthTargetOf(Handle handle) const;

  Resolved* ResolvedFor(uint32_t physical, uint32_t width, uint32_t height, DXGI_FORMAT format,
                        bool depth);
  const Resolved* ResolvedAt(uint32_t physical) const;
  /** Uploads a CPU-written frontbuffer from guest memory when no GPU resolve owns that address. */
  const Resolved* UploadCpuFrontBuffer(uint32_t physical, uint32_t width, uint32_t height);

  Handle WhiteTexture() const { return 1; }
  Handle TextureFor(const uint32_t fetch[6], uint32_t& width, uint32_t& height, bool& resolved);
  ID3D11ShaderResourceView* TextureView(Handle handle, bool cube, bool force2D) const;

  Handle VertexBufferFor(uint32_t physical, uint32_t bytes);
  ID3D11ShaderResourceView* VertexBufferView(Handle handle) const;

  uint32_t IndexAppend(const void* data, uint32_t bytes);
  uint8_t* IndexMap(uint32_t bytes, uint32_t& offset);
  void IndexUnmap();
  ID3D11Buffer* IndexRing() const { return indexRing_.buffer.Get(); }

  ID3D11Buffer* StaticIndicesFor(uint32_t physical, uint32_t count, bool wide,
                                 uint32_t resetIndex);

  uint8_t* ConstantMap(uint32_t bytes, uint32_t& offset);
  bool ConstantWouldWrap(uint32_t bytes) const;
  void ConstantUnmap();
  ID3D11Buffer* ConstantRing() const { return constantRing_.buffer.Get(); }

  const uint8_t* PhysicalPtr(uint32_t physical, size_t size = 4) const;

  rex::PhysicalRange OnPhysicalWrite(rex::PhysicalRange range, bool exact) override;

 private:
  struct ColorEntry {
    ColorTarget target;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> view;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> resource;
    uint32_t key[2] = {};
    uint32_t cls = 0;
  };
  struct DepthEntry {
    DepthTarget target;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> view;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> resource;
    uint32_t key[2] = {};
    uint32_t cls = 0;
  };
  struct ResolvedEntry {
    Resolved surface;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> resource;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> view;
    uint32_t physical = 0;
  };
  struct TextureEntry {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> resource;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> face;
    bool cube = false;
    uint32_t key[4] = {};
    uint32_t physStart = 0;
    uint32_t physEnd = 0;
    uint64_t fingerprint = 0;
    uint64_t usedFrame = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t bytes = 0;
    uint32_t uploadSeq = 0;
    uint32_t rewriteStreak = 0;
    uint64_t volatileUntilFrame = 0;
    bool watchArmed = false;
    bool live = false;
  };
  struct BufferEntry {
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> resource;
    uint32_t key[2] = {};
    uint32_t physStart = 0;
    uint32_t physEnd = 0;
    uint64_t fingerprint = 0;
    uint64_t usedFrame = 0;
    uint32_t bytes = 0;
    uint32_t seen = 0;
    uint32_t uploadSeq = 0;
    uint32_t rewriteStreak = 0;
    uint64_t volatileUntilFrame = 0;
    bool watchArmed = false;
    bool live = false;
  };
  struct IndexEntry {
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    uint32_t key[4] = {};
    uint32_t physStart = 0;
    uint32_t physEnd = 0;
    uint64_t fingerprint = 0;
    uint64_t usedFrame = 0;
    uint32_t stable = 0;
    uint32_t uploadSeq = 0;
    uint32_t rewriteStreak = 0;
    uint64_t volatileUntilFrame = 0;
    bool watchArmed = false;
    bool live = false;
  };
  struct Ring {
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    uint32_t bytes = 0;
    uint32_t offset = 0;
    bool Make(ID3D11Device* dev, uint32_t size, UINT bind);
    uint8_t* Map(ID3D11DeviceContext* ctx, uint32_t size, uint32_t alignment, uint32_t& outOffset);
    void Unmap(ID3D11DeviceContext* ctx);
    uint32_t Append(ID3D11DeviceContext* ctx, const void* data, uint32_t size, uint32_t alignment);
    void Reset() {
      buffer.Reset();
      bytes = 0;
      offset = 0;
    }
  };

  uint32_t SupportedSamples(uint32_t wanted) const;
  uint32_t TargetRows(uint32_t pitch, uint32_t cls, uint32_t bytesPerSample) const;
  void HostSize(uint32_t pitch, uint32_t rows, uint32_t cls, uint32_t& width,
                uint32_t& height) const;
  bool UploadTexture(TextureEntry& entry, const uint32_t fetch[6]);
  void ArmWatch(uint32_t start, uint32_t size);

  ID3D11Device* device_ = nullptr;
  ID3D11DeviceContext* context_ = nullptr;
  rex::IPhysicalMemory* memory_ = nullptr;
  rex::IPhysicalWriteWatch* watch_ = nullptr;
  rex::ScopeGuard watchGuard_;
  const uint8_t* physBase_ = nullptr;
  size_t physSize_ = 0;

  uint64_t frame_ = 0;
  float scale_ = 1.5f;
  float requestedScale_ = 1.5f;
  uint32_t samples_ = 1;
  uint32_t requestedSamples_ = 1;
  uint32_t rowsAsked_[2][513] = {};

  std::deque<ColorEntry> colorTargets_;
  std::deque<DepthEntry> depthTargets_;
  KeyTable<2> colorKeys_;
  KeyTable<2> depthKeys_;

  std::deque<ResolvedEntry> resolved_;
  KeyTable<1> resolvedKeys_;

  std::deque<TextureEntry> textures_;
  KeyTable<4> textureKeys_;
  std::vector<uint32_t> freeTextures_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> whiteCube_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> whiteCubeView_;

  std::deque<BufferEntry> buffers_;
  KeyTable<2> bufferKeys_;
  std::vector<uint32_t> freeBuffers_;

  std::deque<IndexEntry> indexEntries_;
  KeyTable<4> indexKeys_;
  std::vector<uint32_t> freeIndexEntries_;
  std::vector<uint8_t> indexScratch_;
  std::vector<uint8_t> texScratch_;
  std::vector<D3D11_SUBRESOURCE_DATA> subresourceScratch_;

  Ring indexRing_;
  Ring constantRing_;

  bool RangeDirtiedSince(uint32_t start, uint32_t end, uint32_t afterSeq) const;
  bool NeedsRefresh(uint32_t physStart, uint32_t physEnd, uint32_t& uploadSeq,
                    uint64_t prevUsedFrame, uint64_t volatileUntilFrame, bool& watchArmed,
                    bool& watchHitOut) const;
  void FinishRefresh(uint32_t addr, uint32_t size, bool changed, bool watchHit,
                     uint64_t prevUsedFrame, uint32_t& rewriteStreak,
                     uint64_t& volatileUntilFrame, bool& watchArmed);

  static constexpr uint32_t kPhysPages = 512u * 1024u / 4u;   // 131072 x 4KB
  static constexpr uint32_t kPhysChunks = 512u * 1024u / 64u; // 8192 x 64KB
  std::atomic<uint32_t> writeSeq_{1};
  std::unique_ptr<std::atomic<uint32_t>[]> pageSeq_;
  std::unique_ptr<std::atomic<uint32_t>[]> chunkSeq_;
};

}  // namespace bs1::renderer
