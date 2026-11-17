// Android JNI entry point for the ReXGlue-compiled BioShock title.
//
// Creates a TitleApp, initializes it with the game_root passed from Kotlin,
// launches the title and then pumps RunFrame from JNI. The real ReXGlue runtime
// (kernel + Vulkan GPU backend) is linked from the SDK .so files shipped in the
// APK; generated PPC->C++ code is linked into this library.
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

#include <algorithm>
#include <array>
#include <jni.h>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <android/log.h>
#define AJLOG(...) __android_log_print(ANDROID_LOG_INFO, "BS1-JNI", __VA_ARGS__)

#include "generated/default/bioshock1_init.h"

namespace {

// ---- Virtual gamepad fed by the on-screen touch controls --------------------
// The guest reads XamInputGetState -> IInputSystem::GetState -> IInputDriver::
// GetDeviceState. The SDL3 input driver has no gamepad on Android, so we provide
// a tiny always-connected driver whose state comes from nativeSetControls().
constexpr u64 kTouchPadValue = 0x54554348;  // "TUCH"

std::mutex g_touch_mutex;
rex::hid::PadState g_touch_pad{};

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
    return g_touch_pad;
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
    needs.audio = false;
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
}

extern "C" {

JNIEXPORT jint JNICALL
Java_com_bioshock1_recomp_NativeBridge_nativeInit(JNIEnv* env, jclass,
                                                  jstring gameRoot,
                                                  jint, jint) {
  if (!gameRoot) return 0;
  const char* chars = env->GetStringUTFChars(gameRoot, nullptr);
  if (!chars) return 0;
  const std::string root(chars);
  env->ReleaseStringUTFChars(gameRoot, chars);

  AJLOG("nativeInit start root=%s", root.c_str());
  // Our entry point is not SDL_main, so SDL never got SDL_SetMainReady(). The
  // SDL3 Android JNI bootstrap's checkJNIReady() only calls it when all Java
  // callbacks are present; call it explicitly so SDL_InitSubSystem works.
  SDL_SetMainReady();
  g_app = std::make_unique<Bioshock1App>();
  const std::string arg = "--game_data_root=" + root;
  std::vector<std::string> args = {arg};
  // The guest reads low guest memory (~0x10) during config-init; the runtime's
  // default protect_zero makes the first 64 KiB fault, so disable it here.
  args.push_back("--runtime.protect_zero=false");
  // Dump generated SPIR-V shaders so they can be validated (Adreno driver rejects them).
  const std::string files_dir = root.substr(0, root.find_last_of('/'));
  args.push_back("--gpu.xenos.dump_shaders=" + files_dir + "/shaders");
  // Use the virtual touch gamepad as the input source (SDL3 has no Android gamepad here).
  args.push_back("--hid.drivers=touch");
  const auto cli = rex::CLIArgList::Parse(args);
  AJLOG("calling Init");
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
  AJLOG("LaunchTitle OK");
  g_launched = true;
  return 1;
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
}

JNIEXPORT void JNICALL
Java_com_bioshock1_recomp_NativeBridge_nativeResize(JNIEnv*, jclass, jint w, jint h) {
  // On Android the SDL pixel-size-changed event isn't reaching the UI resize
  // handler in this headless-bootstrap setup, leaving the presenter's surface
  // size at 0 and paint mode disconnected (black screen). Notify the graphics
  // system directly so the present thread connects the surface and presents.
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
  return 1;  // proof the library loaded; real count lives in the runtime
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
        case 9: state.buttons |= rex::hid::kPadStart; break;
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
    env->ReleaseFloatArrayElements(axes_array, axes, JNI_ABORT);
  }
  {
    std::lock_guard<std::mutex> lock(g_touch_mutex);
    g_touch_pad = state;
  }
}

JNIEXPORT jint JNICALL
Java_com_bioshock1_recomp_NativeBridge_nativeSubmitCmds(JNIEnv*, jclass, jbyteArray) {
  return 0;  // commands now flow through the real GPU plugin
}

}  // extern "C"
