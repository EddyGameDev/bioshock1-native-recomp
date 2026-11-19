// bioshock1 - ReXGlue Recompiled Project

#include <rex/app/title_app.h>
#include <rex/logging/log_category.h>
#include <rex/settings_registry.h>
#include <rex/setting.h>
#include <rex/ui/ui_system.h>

#include <cstdio>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "generated/default/bioshock1_init.h"

class Bioshock1App : public rex::app::TitleApp {
 public:
  Bioshock1App() : TitleApp("bioshock1", kPPCImageConfig) {}

 protected:
  rex::Result<void> OnConfigure(rex::SettingsRegistry& settings) override {
#if defined(_WIN32)
    // Default Windows to our custom D3D11 Xenos renderer with live resolution control.
    // CLI/config (--gpu.driver=d3d12) still overrides kTitle cleanly.
    return settings.SetText(rex::SettingLayer::kTitle, "gpu.driver", "custom");
#else
    (void)settings;
    return {};
#endif
  }

  // F1 cycles the internal draw resolution LIVE with zero restart under the
  // custom D3D11 renderer (360p -> 720p -> 1080p -> 1440p -> 2160p -> 360p).
  void OnRun() override {
    if (rex::app::IsShutdownRequested()) {
      return;
    }

    bool previous_down = false;
    bool previous_down_f2 = false;
    while (RunFrame()) {
#if defined(_WIN32)
      const bool down = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
      if (down && !previous_down) {
        CycleResolution();
      }
      previous_down = down;

      // F2 toggles the guest-output upscale filter live, with no restart.
      const bool down_f2 = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
      if (down_f2 && !previous_down_f2) {
        ToggleUpscale();
      }
      previous_down_f2 = down_f2;
#endif
    }
  }

 private:
  void ToggleUpscale() {
    if (auto found = REX_SETTINGS().Find("gpu.custom.present_filter")) {
      const std::string current = (*found)->GetText();
      const char* next = current == "lanczos" ? "bilinear" : "lanczos";
      (void)REX_SETTINGS().SetText(rex::SettingLayer::kRuntime, "gpu.custom.present_filter", next);
      REXLOG_INFO("[BS1] present_filter -> {} (live, no restart)", next);
      return;
    }
    auto found = REX_SETTINGS().Find("gpu.xenos.present_upscale");
    std::string current = "lanczos";
    if (found) current = (*found)->GetText();
    const char* next = current == "lanczos" ? "bilinear" : "lanczos";
    (void)REX_SETTINGS().SetText(rex::SettingLayer::kRuntime, "gpu.xenos.present_upscale", next);
    std::printf("[BS1] upscale -> %s (live, no restart)\n", next);
    std::fflush(stdout);
  }

  void CycleResolution() {
    if (auto custom = REX_SETTINGS().Find("gpu.custom.internal_resolution")) {
      static constexpr const char* kPresets[] = {"360p", "720p", "1080p", "1440p", "2160p"};
      const std::string current = (*custom)->GetText();
      int idx = 2;  // default 1080p
      for (int i = 0; i < 5; ++i) {
        if (current == kPresets[i]) {
          idx = i;
          break;
        }
      }
      const char* next = kPresets[(idx + 1) % 5];
      (void)REX_SETTINGS().SetText(rex::SettingLayer::kRuntime, "gpu.custom.internal_resolution",
                                   next);
      REXLOG_INFO("[BS1] internal_resolution -> {} (live, no restart)", next);
      return;
    }

    int current = 1;
    auto found = REX_SETTINGS().Find("gpu.resolution_scale_x");
    if (found) {
      try {
        current = std::stoi((*found)->GetText());
      } catch (...) {
        current = 1;
      }
    }
    int next = current >= 3 ? 1 : current + 1;
    const std::string text = std::to_string(next);
    (void)REX_SETTINGS().SetText(rex::SettingLayer::kRuntime, "gpu.resolution_scale_x", text);
    (void)REX_SETTINGS().SetText(rex::SettingLayer::kRuntime, "gpu.resolution_scale_y", text);
    std::printf("[BS1] resolution scale -> %dx, restarting title...\n", next);
    RestartTitle();
  }
};

REX_TITLE_APP(Bioshock1App)
