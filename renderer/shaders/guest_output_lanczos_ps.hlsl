// Lanczos2 upscale of the guest output, matching ReXGlue's guest-output paint
// root signature exactly (so it drops into the existing pipeline):
//   t0, space0 : Texture2D source (the 1280x720 guest frontbuffer)
//   b0, space0 : 32-bit constants - c0.xy = int2 output_offset
//                                    c0.zw = float2 output_size_inv
//   s0, space0 : linear clamp sampler
// Input is SV_Position (pixel coords in the swapchain), as in ReXGlue's
// guest_output_bilinear shader. The UV is reconstructed identically, then a
// 4x4 Lanczos2 tap gives a much sharper 1.5x (1280->1920) upscale than bilinear.

Texture2D<float4> xe_bilinear_source : register(t0, space0);
SamplerState xe_bilinear_sampler : register(s0, space0);

cbuffer push_consts_xe : register(b0, space0) {
  int2 xe_bilinear_output_offset;   // c0.xy
  float2 xe_bilinear_output_size_inv; // c0.zw
};

// The source texture size is derived from the sampled texture itself, so no
// extra constant is needed. Passing it via GetDimensions keeps the root
// signature identical to the bilinear/dither shaders.

float4 LanczosWeights(float f) {
  f = clamp(f, 1e-4, 1.0 - 1e-4);
  float sf, cf, sh, ch;
  sincos(3.14159265 * f, sf, cf);
  sincos(1.57079633 * f, sh, ch);
  const float a0 = f + 1.0, a1 = f, a2 = 1.0 - f, a3 = 2.0 - f;
  return float4(-sf * ch / (a0 * a0), sf * sh / (a1 * a1),
                sf * ch / (a2 * a2), -sf * sh / (a3 * a3));
}

float4 main(in float4 in_pixel_coord : SV_Position) : SV_Target {
  uint src_w, src_h;
  xe_bilinear_source.GetDimensions(src_w, src_h);
  const float2 src_size = float2(src_w, src_h);

  // Reconstruct the guest UV exactly as the bilinear shader does.
  const int2 pixel_coord =
      int2(in_pixel_coord.xy) - xe_bilinear_output_offset;
  const float2 uv = (float2(pixel_coord) + 0.5f) * xe_bilinear_output_size_inv;

  // Lanczos2 separable upsample (9 bilinear taps), from Edge of Time's blit.
  const float2 p = uv * src_size - 0.5;
  const float2 f = frac(p);
  const float2 base = floor(p);
  const float4 wx = LanczosWeights(f.x), wy = LanczosWeights(f.y);
  const float2 w0 = float2(wx.x, wy.x);
  const float2 w1 = float2(wx.y, wy.y);
  const float2 w2 = float2(wx.z, wy.z);
  const float2 w3 = float2(wx.w, wy.w);
  const float2 w12 = w1 + w2;
  const float2 t0 = (base - 0.5) / src_size;
  const float2 t12 = (base + 0.5 + w2 / w12) / src_size;
  const float2 t3 = (base + 2.5) / src_size;

  float4 acc = 0.0;
  acc += xe_bilinear_source.SampleLevel(xe_bilinear_sampler, float2(t0.x, t0.y), 0) * (w0.x * w0.y);
  acc += xe_bilinear_source.SampleLevel(xe_bilinear_sampler, float2(t12.x, t0.y), 0) * (w12.x * w0.y);
  acc += xe_bilinear_source.SampleLevel(xe_bilinear_sampler, float2(t3.x, t0.y), 0) * (w3.x * w0.y);
  acc += xe_bilinear_source.SampleLevel(xe_bilinear_sampler, float2(t0.x, t12.y), 0) * (w0.x * w12.y);
  acc += xe_bilinear_source.SampleLevel(xe_bilinear_sampler, float2(t12.x, t12.y), 0) * (w12.x * w12.y);
  acc += xe_bilinear_source.SampleLevel(xe_bilinear_sampler, float2(t3.x, t12.y), 0) * (w3.x * w12.y);
  acc += xe_bilinear_source.SampleLevel(xe_bilinear_sampler, float2(t0.x, t3.y), 0) * (w0.x * w3.y);
  acc += xe_bilinear_source.SampleLevel(xe_bilinear_sampler, float2(t12.x, t3.y), 0) * (w12.x * w3.y);
  acc += xe_bilinear_source.SampleLevel(xe_bilinear_sampler, float2(t3.x, t3.y), 0) * (w3.x * w3.y);
  const float2 sum = w0 + w12 + w3;
  float4 color = acc / (sum.x * sum.y);
  color.a = 1.0f;
  return color;
}
