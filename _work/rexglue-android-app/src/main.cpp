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

#include <jni.h>
#include <string>
#include <vector>
#include <memory>
#include <android/log.h>
#define AJLOG(...) __android_log_print(ANDROID_LOG_INFO, "BS1-JNI", __VA_ARGS__)

#include "generated/default/bioshock1_init.h"

namespace {
class Bioshock1App : public rex::app::TitleApp {
 public:
  Bioshock1App() : TitleApp("bioshock1", kPPCImageConfig) {}
  // Android boot-test: audio/input drivers need extra Java setup; the title
  // screen only needs the graphics + UI (surface) path. Disable audio/input
  // requirements so Init can proceed to the guest loop and present a frame.
  rex::AppSysRequirements GetRequirements() const override {
    rex::AppSysRequirements needs;
    needs.graphics_family = rex::GraphicsFamily::kXenos;
    needs.audio = false;
    needs.input = false;
    needs.ui = true;
    return needs;
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
Java_com_bioshock1_recomp_NativeBridge_nativeSetControls(JNIEnv*, jclass,
                                                         jfloatArray, jbyteArray) {
  // TODO: bridge touch -> XInput HLE once the guest loop is confirmed.
}

JNIEXPORT jint JNICALL
Java_com_bioshock1_recomp_NativeBridge_nativeSubmitCmds(JNIEnv*, jclass, jbyteArray) {
  return 0;  // commands now flow through the real GPU plugin
}

}  // extern "C"
