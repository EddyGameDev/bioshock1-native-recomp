// bioshock1 - ReXGlue Recompiled Project

#include <rex/app/title_app.h>
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
  // F1 cycles the internal draw resolution (1x -> 2x -> 3x -> 1x). The scale is
  // read when the GPU command processor is created at title launch, so a title
  // restart is required for it to take effect. This gives a real A/B comparison
  // of the guest's internal rendering resolution at the cost of a quick reload.
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
    auto found = REX_SETTINGS().Find("gpu.xenos.present_upscale");
    std::string current = "lanczos";
    if (found) current = (*found)->GetText();
    const char* next = current == "lanczos" ? "bilinear" : "lanczos";
    (void)REX_SETTINGS().SetText(rex::SettingLayer::kRuntime, "gpu.xenos.present_upscale", next);
    std::printf("[BS1] upscale -> %s (live, no restart)\n", next);
    std::fflush(stdout);
  }

  void CycleResolution() {
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
