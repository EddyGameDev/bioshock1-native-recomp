#pragma once

#include "render_state.h"

#include <d3d11_1.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

struct ImDrawData;
struct ImTextureData;

namespace bs1::renderer {

class Pm4Processor;
class RenderPipeline;
class RenderResources;

/**
 * Owns the D3D11 device, DXGI swap chain, state-tracked draw/resolve execution,
 * Lanczos2/bilinear/FXAA + DC_LUT present pipeline, and ImGui 1.92 overlay pass.
 */
class RenderD3D11 {
 public:
  bool Initialize(RenderPipeline* pipeline, RenderResources* resources, Pm4Processor* pm4);
  void Shutdown();

  ID3D11Device* Device() const { return device_.Get(); }
  ID3D11DeviceContext* Context() const { return context_.Get(); }
  std::mutex& ContextMutex() { return contextMutex_; }

  void SetSurface(HWND hwnd, uint32_t width, uint32_t height);
  void ResizeSurface(uint32_t width, uint32_t height);

  void SetPresentOptions(bool lanczos, bool fxaa, bool letterbox, bool vsync);
  void ForgetBindings();

  void ExecuteDraw(const RenderState::DrawCommand& command);
  void ExecuteResolve(const RenderState::ResolveCommand& command);
  void OnSwap(uint32_t frontbufferAddr, uint32_t guestWidth, uint32_t guestHeight);

  bool BeginPresent();
  void RenderOverlays(ImDrawData& drawData);
  void EndPresent();

  uint64_t SwapCount() const { return swapCount_.load(std::memory_order_relaxed); }
  uint32_t FrontWidth() const { return frontGuestW_; }
  uint32_t FrontHeight() const { return frontGuestH_; }

  /** Optional diagnostic: saves the next presented backbuffer as a BMP file (when BS1_CAPTURE_DIR is set). */
  void RequestFrameCapture(const char* tag);

 private:
  struct BoundState {
    bool vsSet = false;
    bool psSet = false;
    bool gsSet = false;
    bool targetsSet = false;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11GeometryShader* gs = nullptr;
    RenderState::Handle blend = 0;
    float blendFactor[4] = {};
    RenderState::Handle depth = 0;
    uint32_t stencilRef = ~0u;
    RenderState::Handle rasterizer = 0;
    uint32_t topology = 0;
    ID3D11RenderTargetView* targets[4] = {};
    uint32_t targetCount = 0;
    ID3D11DepthStencilView* depthView = nullptr;
    uint32_t targetWidth = 0;
    uint32_t targetHeight = 0;
    int32_t scissor[4] = {};
    bool indexSet = false;
    bool indices32 = false;
    ID3D11Buffer* indexBuffer = nullptr;
    ID3D11ShaderResourceView* psSrv[32] = {};
    ID3D11SamplerState* psSamplers[32] = {};
    ID3D11ShaderResourceView* vsSrv[128] = {};
    ID3D11SamplerState* vsSamplers[16] = {};
  };

  struct ImGuiTexture {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    uint32_t width = 0;
    uint32_t height = 0;
  };

  bool BuildInternalShaders();
  bool EnsureSwapChainLocked();
  void UpdateGammaLutLocked();
  void UnbindResourceLocked(ID3D11ShaderResourceView* resource);
  void DrawQuadLocked(ID3D11ShaderResourceView* source, uint32_t sourceWidth, uint32_t sourceHeight,
                      float x0, float y0, float width, float height,
                      ID3D11RenderTargetView* target, const D3D11_VIEWPORT& viewport,
                      ID3D11PixelShader* program, ID3D11SamplerState* sampler,
                      ID3D11ShaderResourceView* second = nullptr, float colorScale = 1.0f,
                      float clampMode = 0.0f);
  void SyncImGuiTexturesLocked(ImDrawData& drawData);
  void UpdateImGuiTextureLocked(ImTextureData* tex);

  RenderPipeline* pipeline_ = nullptr;
  RenderResources* resources_ = nullptr;
  Pm4Processor* pm4_ = nullptr;

  std::mutex contextMutex_;
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1_;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> backView_;

  HWND hwnd_ = nullptr;
  HWND swapChainHwnd_ = nullptr;
  uint32_t clientWidth_ = 1280;
  uint32_t clientHeight_ = 720;
  uint32_t swapWidth_ = 0;
  uint32_t swapHeight_ = 0;
  bool tearingSupported_ = false;

  // Constant buffers for guest draw stages.
  Microsoft::WRL::ComPtr<ID3D11Buffer> vsFloatCB_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> psFloatCB_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> boolCB_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> drawCB_;

  // Built-in geometry/fullscreen/resolve/present shaders.
  Microsoft::WRL::ComPtr<ID3D11GeometryShader> rectangleGS_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> quadVS_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> copyPS_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> presentBilinearPS_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> presentLanczosPS_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> presentFxaaPS_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> copyColorMS_[4];
  Microsoft::WRL::ComPtr<ID3D11PixelShader> copyDepthMS_[4];
  Microsoft::WRL::ComPtr<ID3D11SamplerState> pointSampler_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> linearSampler_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> quadCB_;
  Microsoft::WRL::ComPtr<ID3D11Texture1D> gammaTex_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> gammaSrv_;
  uint64_t gammaVersion_ = ~0ULL;

  // ImGui overlay resources.
  Microsoft::WRL::ComPtr<ID3D11VertexShader> imguiVS_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> imguiPS_;
  Microsoft::WRL::ComPtr<ID3D11InputLayout> imguiLayout_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> imguiCB_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> imguiVB_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> imguiIB_;
  Microsoft::WRL::ComPtr<ID3D11BlendState> imguiBlend_;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> imguiDepth_;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> imguiRasterizer_;
  uint32_t imguiVBCount_ = 0;
  uint32_t imguiIBCount_ = 0;
  std::unordered_map<uint64_t, ImGuiTexture> imguiTextures_;
  uint64_t nextImGuiTexId_ = 1;

  // Present options (atomic so live setting changes reflect cleanly).
  std::atomic<bool> useLanczos_{true};
  std::atomic<bool> useFxaa_{false};
  std::atomic<bool> useLetterbox_{true};
  std::atomic<bool> useVsync_{true};

  // Frontbuffer tracked across OnSwap -> BeginPresent.
  uint32_t frontbufferAddr_ = 0;
  uint32_t frontGuestW_ = 1280;
  uint32_t frontGuestH_ = 720;
  std::atomic<uint64_t> swapCount_{0};
  bool presentingFrame_ = false;
  std::string captureTag_;

  BoundState bound_{};
};

}  // namespace bs1::renderer
