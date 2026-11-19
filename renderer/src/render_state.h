#pragma once

#include <cstdint>

struct ID3D11Buffer;
struct ID3D11DepthStencilView;
struct ID3D11PixelShader;
struct ID3D11RenderTargetView;
struct ID3D11SamplerState;
struct ID3D11ShaderResourceView;
struct ID3D11VertexShader;

namespace bs1::renderer {

class Pm4Processor;

namespace RenderState {

enum Register : uint32_t {
  SurfaceInfo = 0x2000,
  ColorInfo0 = 0x2001,
  DepthInfo = 0x2002,
  ColorInfo1 = 0x2003,
  ColorInfo2 = 0x2004,
  ColorInfo3 = 0x2005,
  WindowOffset = 0x2080,
  ScissorTopLeft = 0x2081,
  ScissorBottomRight = 0x2082,
  MaxVertexIndex = 0x2100,
  MinVertexIndex = 0x2101,
  IndexOffset = 0x2102,
  ResetIndex = 0x2103,
  ColorMask = 0x2104,
  BlendRed = 0x2105,
  StencilRefMaskBF = 0x210C,
  StencilRefMask = 0x210D,
  AlphaReference = 0x210E,
  ViewportXScale = 0x210F,
  ProgramControl = 0x2180,
  ContextMisc = 0x2181,
  DepthControl = 0x2200,
  BlendControl0 = 0x2201,
  ColorControl = 0x2202,
  ClipControl = 0x2204,
  SuScModeControl = 0x2205,
  VteControl = 0x2206,
  ModeControl = 0x2208,
  BlendControl1 = 0x2209,
  BlendControl2 = 0x220A,
  BlendControl3 = 0x220B,
  PointSize = 0x2280,
  VertexControl = 0x2302,
  CopyControl = 0x2318,
  CopyDestBase = 0x2319,
  CopyDestPitch = 0x231A,
  CopyDestInfo = 0x231B,
  CopyDepthClear = 0x231D,
  CopyColorClear = 0x231E,
  PolyOffsetFrontScale = 0x2380,
  VertexConstants = 0x4000,
  PixelConstants = 0x4400,
  FetchConstants = 0x4800,
  BoolConstants = 0x4900,
};

enum Primitive : uint32_t {
  Points = 1,
  Lines = 2,
  LineStrip = 3,
  Triangles = 4,
  TriangleFan = 5,
  TriangleStrip = 6,
  Rectangles = 8,
  LineLoop = 12,
  Quads = 13,
};

using Handle = uint32_t;

struct Snapshot {
  uint32_t primitive = 0;
  uint32_t sourceSelect = 0;
  uint32_t indexCount = 0;
  bool wideIndices = false;
  uint32_t indexBase = 0;

  uint32_t pitch = 0;
  uint32_t surfaceInfo = 0;
  uint32_t colorInfo[4] = {};
  uint32_t depthInfo = 0;
  uint32_t modeControl = 0;
  uint32_t colorMask = 0;
  uint32_t depthControl = 0;
  uint32_t stencilRefMask = 0;
  uint32_t stencilRefMaskBF = 0;

  uint32_t clipControl = 0;
  uint32_t suScModeControl = 0;
  float polyOffset[4] = {};
  uint32_t scissorTopLeft = 0;
  uint32_t scissorBottomRight = 0;
  uint32_t windowOffset = 0;
  int32_t indexOffset = 0;
  uint32_t minVertexIndex = 0;
  uint32_t maxVertexIndex = 0;
  uint32_t resetIndex = 0;
  uint32_t pointSize = 0;
  uint32_t vertexControl = 0;

  uint32_t blendControl[4] = {};
  float blendFactor[4] = {};

  uint32_t vteControl = 0;
  uint32_t colorControl = 0;
  uint32_t alphaReference = 0;
  uint32_t programControl = 0;
  uint32_t contextMisc = 0;
  float viewport[6] = {};

  uint64_t vertexProgram = 0;
  uint64_t pixelProgram = 0;
};

struct ResolveSnapshot {
  uint32_t control = 0;
  uint32_t destBase = 0;
  uint32_t destInfo = 0;
  uint32_t destPitch = 0;
  uint32_t pitch = 0;
  uint32_t surfaceInfo = 0;
  uint32_t scissorTopLeft = 0;
  uint32_t scissorBottomRight = 0;
  uint32_t colorInfo[4] = {};
  uint32_t depthInfo = 0;
  uint32_t colorClearValue = 0;
  uint32_t depthClearValue = 0;
};

struct DrawConstants {
  float viewportScale[4];
  float viewportOffset[4];
  float targetSize[4];
  uint32_t flags[4];
  float textureSize[32][4];
  uint32_t textureAdjustment[32][4];
  float pixelGen[4];
  float colorExpBias[4];
};

struct DrawCommand {
  Handle vertexShader = 0;
  Handle pixelShader = 0;
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  bool psNoDepth = false;
  bool rectangles = false;

  Handle blend = 0;
  Handle depth = 0;
  Handle rasterizer = 0;
  float blendFactor[4] = {};
  uint32_t stencilReference = 0;
  uint32_t topology = 0;

  ID3D11RenderTargetView* targets[4] = {};
  ID3D11ShaderResourceView* targetResources[4] = {};
  uint32_t targetCount = 0;
  ID3D11DepthStencilView* depthView = nullptr;
  ID3D11ShaderResourceView* depthResource = nullptr;
  uint32_t targetWidth = 0;
  uint32_t targetHeight = 0;
  int32_t scissor[4] = {};

  struct Texture {
    uint8_t stage = 0;        // 0 = pixel shader, 1 = vertex shader
    uint8_t slot = 0;         // fetch slot 0..31 -> register(t{slot})
    uint8_t samplerSlot = 0;  // sampler slot -> register(s{samplerSlot})
    ID3D11ShaderResourceView* view = nullptr;
    ID3D11SamplerState* sampler = nullptr;
  };
  static constexpr uint32_t MaxTextures = 64;
  Texture textures[MaxTextures] = {};
  uint32_t textureCount = 0;

  struct Buffer {
    uint8_t slot = 0;
    ID3D11ShaderResourceView* view = nullptr;
  };
  static constexpr uint32_t MaxBuffers = 32;
  Buffer buffers[MaxBuffers] = {};
  uint32_t bufferCount = 0;

  bool ring = false;
  uint32_t floatAt[2] = {};
  uint32_t floatCount[2] = {};
  uint32_t drawAt = 0;
  uint32_t drawBytes = 0;
  const uint32_t* floatFile[2] = {};
  bool floatChanged[2] = {};
  const DrawConstants* drawConstants = nullptr;
  bool drawChanged = false;
  const uint32_t* bools = nullptr;
  bool boolsChanged = false;

  bool indexed = false;
  bool indices32 = false;
  uint32_t indexCount = 0;
  uint32_t indexRingOffset = 0;
  ID3D11Buffer* indexBuffer = nullptr;
  int32_t baseVertex = 0;

  bool bindingsLost = false;
};

struct ResolveCommand {
  bool copy = false;
  ID3D11ShaderResourceView* source = nullptr;
  uint32_t sourceWidth = 0;
  uint32_t sourceHeight = 0;
  uint32_t sourceSamples = 1;
  bool fromDepth = false;
  float colorScale = 1.0f;
  float clampMode = 0.0f;
  ID3D11RenderTargetView* destination = nullptr;
  ID3D11ShaderResourceView* destinationResource = nullptr;
  float x0 = 0.0f;
  float y0 = 0.0f;
  uint32_t width = 0;
  uint32_t height = 0;

  bool colorClear = false;
  ID3D11RenderTargetView* clearTarget = nullptr;
  float clearColor[4] = {};
  int32_t clearRect[4] = {};
  bool depthClear = false;
  ID3D11DepthStencilView* clearDepth = nullptr;
  float clearDepthValue = 1.0f;
  uint8_t clearStencilValue = 0;

  uint32_t guestWidth = 0;
  uint32_t guestHeight = 0;
  uint32_t destinationBase = 0;

  bool bindingsLost = false;
};

void ReadSnapshot(const Pm4Processor& pm4, uint32_t initiator, uint32_t indexBase,
                  uint64_t vsHash, uint64_t psHash, Snapshot& out);

void ReadResolveSnapshot(const Pm4Processor& pm4, ResolveSnapshot& out);

void ReadTextureFetch(const Pm4Processor& pm4, uint32_t slot, uint32_t words[6]);
void ReadVertexFetch(const Pm4Processor& pm4, uint32_t slot, uint32_t& word0, uint32_t& word1);

}  // namespace RenderState
}  // namespace bs1::renderer
