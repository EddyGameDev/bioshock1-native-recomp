// Android JNI entry point for the ReXGlue-compiled BioShock title.
//
// Supports BOTH:
//   - "custom" : standalone custom GLES 3.2 Xenos renderer (librexgpu-xenos-custom.so)
//                with true live, zero-restart internal resolution scaling
//                (360p / 540p / 720p / 900p / 1080p / 1440p) + Lanczos2 upscale
//   - "vulkan" : ReXGlue Vulkan Xenos renderer (librexgpu-xenos-vulkan.so)
//                with custom SPIR-V Lanczos2 upscale + persistent VkPipelineCache
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <rex/app/title_app.h>
#include <rex/cli.h>
#include <rex/driver_module.h>
#include <rex/driver_registration.h>
#include <rex/hid/input_driver.h>
#include <rex/hid/pad.h>
#include <rex/module.h>
#include <rex/result.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>

#include <algorithm>
#include <array>
#include <dlfcn.h>
#include <jni.h>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <android/log.h>
#include <cstdlib>
#define AJLOG(...) __android_log_print(ANDROID_LOG_INFO, "BS1-JNI", __VA_ARGS__)

#include "generated/default/bioshock1_init.h"

namespace {

constexpr u64 kTouchPadValue = 0x54554348;  // "TUCH"

std::mutex g_touch_mutex;
rex::hid::PadState g_touch_pad{};
u16 g_touch_latched_buttons = 0;

rex::Error NotTouchPad() {
  return rex::Error(rex::ErrorCategory::kNotFound, "not the touch pad");
}

class TouchInputDriver final : public rex::hid::IInputDriver {
 public:
  std::string_view GetName() const override { return "touch"; }
  [[nodiscard]] rex::Result<void> Initialize(rex::IDriverContext&) override { return {}; }
  void Shutdown() override {}
  u64 GetDeviceRevision() const override { return 1; }
  void EnumerateDevices(std::vector<rex::hid::PadDevice>& out) override {
    rex::hid::PadDevice device;
    device.id = rex::hid::PadDeviceID{kTouchPadValue};
    device.name = "Touch Controls";
    device.info.type = rex::hid::PadType::kX360;
    device.identity.xinput_slot = 0;
    out.push_back(std::move(device));
  }
  [[nodiscard]] rex::Result<rex::hid::PadState> GetDeviceState(rex::hid::PadDeviceID id) override {
    if (id.value != kTouchPadValue) return std::unexpected(NotTouchPad());
    std::lock_guard<std::mutex> lock(g_touch_mutex);
    rex::hid::PadState out = g_touch_pad;
    out.buttons |= g_touch_latched_buttons;
    g_touch_latched_buttons = 0;
    return out;
  }
  [[nodiscard]] rex::Result<rex::hid::PadCapabilities> GetDeviceCapabilities(
      rex::hid::PadDeviceID id) override {
    if (id.value != kTouchPadValue) return std::unexpected(NotTouchPad());
    rex::hid::PadCapabilities caps;
    caps.subtype = 0x01;
    caps.supported.buttons = 0xFFFF;
    caps.supported.left_trigger = 0xFF;
    caps.supported.right_trigger = 0xFF;
    caps.supported.thumb_lx = 0x7FFF;
    caps.supported.thumb_ly = 0x7FFF;
    caps.supported.thumb_rx = 0x7FFF;
    caps.supported.thumb_ry = 0x7FFF;
    caps.motors.left_motor = 0xFFFF;
    caps.motors.right_motor = 0xFFFF;
    return caps;
  }
  [[nodiscard]] rex::Result<void> SetDeviceVibration(rex::hid::PadDeviceID id,
                                                     const rex::hid::PadVibration&) override {
    if (id.value != kTouchPadValue) return std::unexpected(NotTouchPad());
    return {};
  }
};

std::unique_ptr<rex::IDriver> CreateTouchInput() {
  return std::make_unique<TouchInputDriver>();
}

constexpr std::array<rex::DriverRegistration, 1> kTouchDrivers = {{
    rex::DriverRegistration{rex::DriverType::kInput, "touch", {}, &CreateTouchInput},
}};

class TouchInputProvider final : public rex::IModule, public rex::IDriverModule {
 public:
  REX_MODULE_SERVES(::rex::IDriverModule)
  std::string_view GetName() const override { return "rexhid-touch"; }
  [[nodiscard]] rex::Result<void> Connect(rex::IInterfaceFactory&) override { return {}; }
  void Disconnect() override {}
  [[nodiscard]] rex::Result<void> Init() override { return {}; }
  void Shutdown() override {}
  rex::IInterface* QueryInterface(std::string_view name, u32 version) override {
    return rex::FindModuleInterface<rex::IDriverModule>(*this, name, version);
  }
  std::span<const rex::DriverRegistration> GetDrivers() const override { return kTouchDrivers; }
};

TouchInputProvider g_touch_input_provider;

class Bioshock1App : public rex::app::TitleApp {
 public:
  Bioshock1App() : TitleApp("bioshock1", kPPCImageConfig) {}
  rex::AppSysRequirements GetRequirements() const override {
    rex::AppSysRequirements needs;
    needs.graphics_family = rex::GraphicsFamily::kXenos;
    needs.audio = true;
    needs.input = false;
    needs.ui = true;
    return needs;
  }
  void OnRegisterModules(std::vector<rex::IModule*>& modules) override {
    modules.push_back(&g_touch_input_provider);
  }
};

std::unique_ptr<Bioshock1App> g_app;
bool g_launched = false;

using PresentCountFn = u64 (*)();
PresentCountFn g_get_present_count = nullptr;

PresentCountFn ResolvePresentCounter() {
  if (void* h = dlopen("librexgpu-xenos-custom.so", RTLD_NOLOAD | RTLD_NOW)) {
    if (auto* fn = reinterpret_cast<PresentCountFn>(dlsym(h, "rex_android_get_custom_present_count"))) {
      return fn;
    }
  }
  if (void* h = dlopen("librexgpu-xenos-vulkan.so", RTLD_NOLOAD | RTLD_NOW)) {
    if (auto* fn = reinterpret_cast<PresentCountFn>(dlsym(h, "rex_android_get_vk_present_count"))) {
      return fn;
    }
  }
  if (auto* fn = reinterpret_cast<PresentCountFn>(dlsym(RTLD_DEFAULT, "rex_android_get_custom_present_count"))) {
    return fn;
  }
  if (auto* fn = reinterpret_cast<PresentCountFn>(dlsym(RTLD_DEFAULT, "rex_android_get_vk_present_count"))) {
    return fn;
  }
  return nullptr;
}

int InitRuntime(const std::string& root, const std::string& driverMode,
                const std::string& internalRes, int vkScale, const std::string& upscaleMode,
                bool fxaa, bool perfMode) {
  const std::string driver = (driverMode == "vulkan") ? "vulkan" : "custom";
  const int scale = std::clamp(vkScale, 1, 2);
  const std::string upscale = (upscaleMode == "bilinear") ? "bilinear" : "lanczos";
  const std::string resPreset = internalRes.empty() ? "720p" : internalRes;

  AJLOG("nativeInit start root=%s driver=%s res=%s vkScale=%dx upscale=%s fxaa=%d perf=%d",
        root.c_str(), driver.c_str(), resPreset.c_str(), scale, upscale.c_str(),
        fxaa ? 1 : 0, perfMode ? 1 : 0);
  SDL_SetMainReady();
  g_app = std::make_unique<Bioshock1App>();

  const std::string files_dir = root.substr(0, root.find_last_of('/'));
  std::vector<std::string> args = {
      "--game_data_root=" + root,
      "--cache_root=" + files_dir + "/cache",
      "--user_data_root=" + files_dir + "/user",
      "--log.file_name=" + files_dir + "/bs1.log",
      "--gpu.driver=" + driver,
      "--runtime.protect_zero=false",
      "--hid.drivers=touch",
  };

  if (driver == "custom") {
    args.push_back("--gpu.custom.internal_resolution=" + resPreset);
    args.push_back("--gpu.custom.present_filter=" + upscale);
    args.push_back("--gpu.custom.anisotropy=2");
    args.push_back("--gpu.custom.letterbox=true");
    args.push_back("--gpu.custom.vsync=true");
    args.push_back("--apu.max_queued_frames=32");
  } else {
    args.push_back("--gpu.resolution_scale_x=" + std::to_string(scale));
    args.push_back("--gpu.resolution_scale_y=" + std::to_string(scale));
    args.push_back("--gpu.xenos.present_upscale=" + upscale);
    args.push_back(std::string("--gpu.xenos.swap_post_effect=") + (fxaa ? "fxaa" : "none"));
    args.push_back("--gpu.xenos.store_shaders=true");
    args.push_back("--gpu.xenos.async_shader_compilation=true");
    args.push_back("--gpu.vulkan.pipeline_creation_threads=4");
    args.push_back("--gpu.vulkan.async_skip_incomplete_frames=false");
    args.push_back("--gpu.vulkan.log_debug_messages=false");
    if (perfMode) {
      args.push_back("--gpu.xenos.native_2x_msaa=false");
      args.push_back("--gpu.xenos.gamma_render_target_as_unorm16=false");
      args.push_back("--gpu.xenos.readback_memexport=false");
      args.push_back("--gpu.vulkan.readback_memexport=false");
      args.push_back("--gpu.xenos.texture_cache_memory_limit_soft=256");
      args.push_back("--gpu.xenos.texture_cache_memory_limit_hard=512");
    }
  }

  const auto cli = rex::CLIArgList::Parse(args);
  AJLOG("calling Init (driver=%s)", driver.c_str());
  auto init_result = g_app->Init(cli);
  if (!init_result) {
    AJLOG("Init FAILED: %s", init_result.error().GetDescription().c_str());
    return 0;
  }
  AJLOG("Init OK, calling LaunchTitle");
  auto launch_result = g_app->LaunchTitle();
  if (!launch_result) {
    AJLOG("LaunchTitle FAILED: %s", launch_result.error().GetDescription().c_str());
    return 0;
  }
  g_get_present_count = ResolvePresentCounter();
  AJLOG("LaunchTitle OK (present_counter=%s)", g_get_present_count ? "linked" : "none");
  g_launched = true;
  return 1;
}

std::string JStringToStd(JNIEnv* env, jstring s, const char* fallback = "") {
  if (!s) return fallback;
  const char* chars = env->GetStringUTFChars(s, nullptr);
  if (!chars) return fallback;
  std::string out(chars);
  env->ReleaseStringUTFChars(s, chars);
  return out;
}

}  // namespace

extern "C" {

JNIEXPORT jint JNICALL
Java_com_bioshock1_recomp_NativeBridge_nativeInit(JNIEnv* env, jclass,
                                                  jstring gameRoot, jint, jint) {
  const std::string root = JStringToStd(env, gameRoot);
  if (root.empty()) return 0;
  return InitRuntime(root, "custom", "720p", 1, "lanczos", false, true);
}

JNIEXPORT jint JNICALL
Java_com_bioshock1_recomp_NativeBridge_nativeInitWithConfig(JNIEnv* env, jclass,
                                                            jstring gameRoot,
                                                            jint resScale,
                                                            jstring upscaleMode,
                                                            jboolean fxaa,
                                                            jboolean perfMode) {
  const std::string root = JStringToStd(env, gameRoot);
  if (root.empty()) return 0;
  const std::string upscale = JStringToStd(env, upscaleMode, "lanczos");
  return InitRuntime(root, "vulkan", "720p", static_cast<int>(resScale), upscale,
                     fxaa != JNI_FALSE, perfMode != JNI_FALSE);
}

JNIEXPORT jint JNICALL
Java_com_bioshock1_recomp_NativeBridge_nativeInitCustom(JNIEnv* env, jclass,
                                                        jstring gameRoot,
                                                        jstring driverMode,
                                                        jstring internalRes,
                                                        jint vkScale,
                                                        jstring upscaleMode,
                                                        jboolean fxaa,
                                                        jboolean perfMode) {
  const std::string root = JStringToStd(env, gameRoot);
  if (root.empty()) return 0;
  const std::string driver = JStringToStd(env, driverMode, "custom");
  const std::string res = JStringToStd(env, internalRes, "720p");
  const std::string upscale = JStringToStd(env, upscaleMode, "lanczos");
  return InitRuntime(root, driver, res, static_cast<int>(vkScale), upscale,
                     fxaa != JNI_FALSE, perfMode != JNI_FALSE);
}

JNIEXPORT jboolean JNICALL
Java_com_bioshock1_recomp_NativeBridge_nativeSetInternalResolution(JNIEnv* env, jclass,
                                                                   jstring presetStr) {
  if (!g_launched || !presetStr) return JNI_FALSE;
  const std::string preset = JStringToStd(env, presetStr, "720p");
  if (auto found = REX_SETTINGS().Find("gpu.custom.internal_resolution")) {
    auto res = REX_SETTINGS().SetText(rex::SettingLayer::kRuntime,
                                      "gpu.custom.internal_resolution", preset);
    AJLOG("live internal_resolution -> %s (%s)", preset.c_str(), res ? "ok" : "err");
    return res ? JNI_TRUE : JNI_FALSE;
  }
  return JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_bioshock1_recomp_NativeBridge_nativeSetUpscaleMode(JNIEnv* env, jclass,
                                                            jstring upscaleMode) {
  if (!g_launched || !upscaleMode) return JNI_FALSE;
  const std::string raw = JStringToStd(env, upscaleMode, "lanczos");
  const std::string mode = (raw == "bilinear") ? "bilinear" : "lanczos";
  if (REX_SETTINGS().Find("gpu.custom.present_filter")) {
    auto res = REX_SETTINGS().SetText(rex::SettingLayer::kRuntime,
                                      "gpu.custom.present_filter", mode);
    AJLOG("live custom present_filter -> %s (%s)", mode.c_str(), res ? "ok" : "err");
    return res ? JNI_TRUE : JNI_FALSE;
  }
  auto res = REX_SETTINGS().SetText(rex::SettingLayer::kRuntime,
                                    "gpu.xenos.present_upscale", mode);
  AJLOG("live vulkan present_upscale -> %s (%s)", mode.c_str(), res ? "ok" : "err");
  return res ? JNI_TRUE : JNI_FALSE;
}

// Export LOGS: tell all renderer modules where to mirror their log lines.
JNIEXPORT void JNICALL
Java_com_bioshock1_recomp_NativeBridge_nativeSetLogFile(JNIEnv* env, jclass, jstring path) {
  if (!path) return;
  const char* chars = env->GetStringUTFChars(path, nullptr);
  if (chars) {
    setenv("BS1_LOG_FILE", chars, 1);
    env->ReleaseStringUTFChars(path, chars);
  }
}

JNIEXPORT jlong JNICALL
Java_com_bioshock1_recomp_NativeBridge_nativeGetPresentCount(JNIEnv*, jclass) {
  if (!g_launched) return 0;
  if (!g_get_present_count) {
    g_get_present_count = ResolvePresentCounter();
  }
  return g_get_present_count ? static_cast<jlong>(g_get_present_count()) : 0;
}

JNIEXPORT void JNICALL
Java_com_bioshock1_recomp_NativeBridge_nativeRender(JNIEnv*, jclass) {
  if (g_launched) {
    if (!g_app->RunFrame()) g_launched = false;
  }
}

JNIEXPORT jint JNICALL
Java_com_bioshock1_recomp_NativeBridge_nativeFrame(JNIEnv*, jclass) {
  if (g_launched && g_app->RunFrame()) return 1;
  g_launched = false;
  return 0;
}

JNIEXPORT void JNICALL
Java_com_bioshock1_recomp_NativeBridge_nativeShutdown(JNIEnv*, jclass) {
  if (g_app) {
    g_app->Shutdown();
    g_app.reset();
  }
  g_launched = false;
  g_get_present_count = nullptr;
}

JNIEXPORT void JNICALL
Java_com_bioshock1_recomp_NativeBridge_nativeResize(JNIEnv*, jclass, jint w, jint h) {
  if (g_app) {
    g_app->NotifySurfaceResized(static_cast<u32>(w), static_cast<u32>(h));
    AJLOG("NotifySurfaceResized %dx%d", w, h);
  }
}

JNIEXPORT jstring JNICALL
Java_com_bioshock1_recomp_NativeBridge_nativeStatus(JNIEnv* env, jclass) {
  const char* s = g_launched ? "{\"running\":true}" : "{\"running\":false}";
  return env->NewStringUTF(s);
}

JNIEXPORT jint JNICALL
Java_com_bioshock1_recomp_NativeBridge_nativeHleImportCount(JNIEnv*, jclass) {
  return 1;
}

JNIEXPORT void JNICALL
Java_com_bioshock1_recomp_NativeBridge_nativeSetControls(JNIEnv* env, jclass, jfloatArray axes_array,
                                                         jbyteArray buttons_array) {
  rex::hid::PadState state;
  if (buttons_array) {
    const jsize count = env->GetArrayLength(buttons_array);
    jbyte* buttons = env->GetByteArrayElements(buttons_array, nullptr);
    for (jsize i = 0; i < count && i < 32; ++i) {
      if (!buttons[i]) continue;
      switch (i) {
        case 0: state.buttons |= rex::hid::kPadA; break;
        case 1: state.buttons |= rex::hid::kPadB; break;
        case 2: state.buttons |= rex::hid::kPadX; break;
        case 3: state.buttons |= rex::hid::kPadY; break;
        case 4: state.buttons |= rex::hid::kPadLeftShoulder; break;
        case 5: state.buttons |= rex::hid::kPadRightShoulder; break;
        case 6: state.left_trigger = 255; break;
        case 7: state.right_trigger = 255; break;
        case 8: state.buttons |= rex::hid::kPadBack; break;
        case 9: state.buttons |= rex::hid::kPadStart; break;
        case 10: state.buttons |= rex::hid::kPadLeftThumb; break;
        case 11: state.buttons |= rex::hid::kPadRightThumb; break;
        case 12: state.buttons |= rex::hid::kPadDPadUp; break;
        case 13: state.buttons |= rex::hid::kPadDPadDown; break;
        case 14: state.buttons |= rex::hid::kPadDPadLeft; break;
        case 15: state.buttons |= rex::hid::kPadDPadRight; break;
        default: break;
      }
    }
    env->ReleaseByteArrayElements(buttons_array, buttons, JNI_ABORT);
  }
  if (axes_array) {
    const jsize count = env->GetArrayLength(axes_array);
    jfloat* axes = env->GetFloatArrayElements(axes_array, nullptr);
    const auto to_i16 = [](float v) {
      return static_cast<i16>(std::clamp(v, -1.0f, 1.0f) * 32767.0f);
    };
    if (count > 0) state.thumb_lx = to_i16(axes[0]);
    if (count > 1) state.thumb_ly = to_i16(axes[1]);
    if (count > 2) state.thumb_rx = to_i16(axes[2]);
    if (count > 3) state.thumb_ry = to_i16(axes[3]);
    env->ReleaseFloatArrayElements(axes_array, axes, JNI_ABORT);
  }
  {
    std::lock_guard<std::mutex> lock(g_touch_mutex);
    g_touch_latched_buttons |= (state.buttons & ~g_touch_pad.buttons);
    g_touch_pad = state;
  }
}

JNIEXPORT jint JNICALL
Java_com_bioshock1_recomp_NativeBridge_nativeSubmitCmds(JNIEnv*, jclass, jbyteArray) {
  return 0;
}

}  // extern "C"
