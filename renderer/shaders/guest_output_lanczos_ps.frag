#version 450

// 9-tap separable Lanczos2 guest-output upscale for VulkanPresenter.
// Matches ReXGlue's kGuestOutputBilinearPS descriptor & push-constant layout:
//   set = 0, binding = 0 : texture2D (guest frontbuffer)
//   set = 0, binding = 1 : sampler   (linear clamp-to-edge)
//   push_constant offset 16 : ivec2 output_offset
//   push_constant offset 24 : vec2  output_size_inv
// Input is gl_FragCoord.xy (swapchain pixel coordinates).

layout(set = 0, binding = 0) uniform texture2D xe_bilinear_source;
layout(set = 0, binding = 1) uniform sampler xe_bilinear_sampler;

layout(push_constant) uniform PushConsts {
  layout(offset = 16) ivec2 xe_bilinear_output_offset;
  layout(offset = 24) vec2 xe_bilinear_output_size_inv;
};

layout(location = 0) out vec4 out_color;

vec4 LanczosWeights(float f) {
  f = clamp(f, 1e-4, 1.0 - 1e-4);
  float pi_f = 3.14159265 * f;
  float half_pi_f = 1.57079633 * f;
  float sf = sin(pi_f);
  float sh = sin(half_pi_f);
  float ch = cos(half_pi_f);
  float a0 = f + 1.0;
  float a1 = f;
  float a2 = 1.0 - f;
  float a3 = 2.0 - f;
  return vec4(-sf * ch / (a0 * a0), sf * sh / (a1 * a1),
              sf * ch / (a2 * a2), -sf * sh / (a3 * a3));
}

void main() {
  ivec2 src_dim = textureSize(sampler2D(xe_bilinear_source, xe_bilinear_sampler), 0);
  vec2 src_size = vec2(src_dim);

  ivec2 pixel_coord = ivec2(gl_FragCoord.xy) - xe_bilinear_output_offset;
  vec2 uv = (vec2(pixel_coord) + 0.5) * xe_bilinear_output_size_inv;

  vec2 p = uv * src_size - 0.5;
  vec2 f = fract(p);
  vec2 base = floor(p);
  vec4 wx = LanczosWeights(f.x);
  vec4 wy = LanczosWeights(f.y);
  vec2 w0 = vec2(wx.x, wy.x);
  vec2 w1 = vec2(wx.y, wy.y);
  vec2 w2 = vec2(wx.z, wy.z);
  vec2 w3 = vec2(wx.w, wy.w);
  vec2 w12 = w1 + w2;
  vec2 inv_src = 1.0 / src_size;
  vec2 t0 = (base - 0.5) * inv_src;
  vec2 t12 = (base + 0.5 + w2 / w12) * inv_src;
  vec2 t3 = (base + 2.5) * inv_src;

  vec4 acc = vec4(0.0);
  acc += textureLod(sampler2D(xe_bilinear_source, xe_bilinear_sampler), vec2(t0.x, t0.y), 0.0) * (w0.x * w0.y);
  acc += textureLod(sampler2D(xe_bilinear_source, xe_bilinear_sampler), vec2(t12.x, t0.y), 0.0) * (w12.x * w0.y);
  acc += textureLod(sampler2D(xe_bilinear_source, xe_bilinear_sampler), vec2(t3.x, t0.y), 0.0) * (w3.x * w0.y);
  acc += textureLod(sampler2D(xe_bilinear_source, xe_bilinear_sampler), vec2(t0.x, t12.y), 0.0) * (w0.x * w12.y);
  acc += textureLod(sampler2D(xe_bilinear_source, xe_bilinear_sampler), vec2(t12.x, t12.y), 0.0) * (w12.x * w12.y);
  acc += textureLod(sampler2D(xe_bilinear_source, xe_bilinear_sampler), vec2(t3.x, t12.y), 0.0) * (w3.x * w12.y);
  acc += textureLod(sampler2D(xe_bilinear_source, xe_bilinear_sampler), vec2(t0.x, t3.y), 0.0) * (w0.x * w3.y);
  acc += textureLod(sampler2D(xe_bilinear_source, xe_bilinear_sampler), vec2(t12.x, t3.y), 0.0) * (w12.x * w3.y);
  acc += textureLod(sampler2D(xe_bilinear_source, xe_bilinear_sampler), vec2(t3.x, t3.y), 0.0) * (w3.x * w3.y);

  vec2 sum = w0 + w12 + w3;
  vec3 color = clamp(acc.rgb / (sum.x * sum.y), 0.0, 1.0);
  out_color = vec4(color, 1.0);
}
