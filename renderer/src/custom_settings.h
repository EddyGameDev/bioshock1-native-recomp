#pragma once

#include <algorithm>
#include <cctype>
#include <string>

#include <rex/setting.h>
#include <rex/types.h>

namespace bs1::renderer {

/**
 * Live settings for the custom D3D11 Xenos renderer (scope: gpu.custom).
 * Every setting is kLive so internal resolution, filtering, MSAA, and post-FX
 * take effect on the very next frame with zero restart.
 */
class CustomRendererSettings final : public rex::SettingOwner {
 public:
  CustomRendererSettings() : rex::SettingOwner("gpu.custom") {}

  REX_SETTING(std::string, internal_resolution, "1080p",
              "Target internal render height (360p, 720p, 900p, 1080p, 1440p, 2160p, or scale)",
              rex::Allowed("scale", "360p", "720p", "900p", "1080p", "1440p", "2160p"));

  REX_SETTING(i32, resolution_scale_pct, 200,
              "Internal resolution scale percentage when internal_resolution=scale (50-400%)",
              rex::Range{50, 400});

  REX_SETTING(std::string, present_filter, "lanczos",
              "Present upscale/downsample filter (lanczos or bilinear)",
              rex::Allowed("lanczos", "bilinear"));

  REX_SETTING(i32, anisotropy, 8,
              "Anisotropic texture filtering (1, 2, 4, 8, 16)",
              rex::Range{1, 16});

  REX_SETTING(i32, msaa, 1,
              "Internal MSAA sample count for main render targets (1, 2, 4)",
              rex::Range{1, 4});

  REX_SETTING(bool, fxaa, false, "Enable FXAA post-process on present");
  REX_SETTING(bool, letterbox, true, "Preserve title aspect ratio when presenting");
  REX_SETTING(bool, vsync, true, "Wait for vertical blank when presenting");
};

inline u32 ParseTargetHeight(const std::string& preset) {
  std::string lower;
  lower.reserve(preset.size());
  for (unsigned char c : preset) {
    lower.push_back(static_cast<char>(std::tolower(c)));
  }
  if (lower == "360p") return 360;
  if (lower == "720p") return 720;
  if (lower == "900p") return 900;
  if (lower == "1080p") return 1080;
  if (lower == "1440p") return 1440;
  if (lower == "2160p" || lower == "4k") return 2160;
  return 0;  // "scale"
}

inline f32 ComputeEffectiveScale(const CustomRendererSettings& settings, u32 base_guest_height) {
  const u32 target_h = ParseTargetHeight(settings.internal_resolution.Get());
  if (target_h > 0 && base_guest_height > 0) {
    const f32 scale = static_cast<f32>(target_h) / static_cast<f32>(base_guest_height);
    return std::clamp(scale, 0.25f, 8.0f);
  }
  const f32 pct = static_cast<f32>(std::clamp(settings.resolution_scale_pct.Get(), 50, 400));
  return pct / 100.0f;
}

}  // namespace bs1::renderer
