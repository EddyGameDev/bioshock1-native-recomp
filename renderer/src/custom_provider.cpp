#if defined(__ANDROID__)
#include "gles3/android_custom_driver.h"
#else
#include "custom_driver.h"
#endif

#include <array>
#include <memory>

#include <rex/driver.h>
#include <rex/driver_module.h>
#include <rex/driver_registration.h>
#include <rex/gpu/graphics_driver.h>
#include <rex/gpu/graphics_system.h>
#include <rex/memory/host.h>

namespace bs1::renderer {

namespace {

std::unique_ptr<rex::IDriver> CreateCustomDriver() {
  return std::make_unique<CustomXenosGraphicsDriver>();
}

constexpr std::array<rex::InterfaceType, 1> kRequired = {rex::gpu::IGraphicsSystem::kInterface};

constexpr std::array<rex::DriverRegistration, 1> kDrivers = {
    rex::DriverRegistration{rex::DriverType::kGraphics, "custom",
                            rex::GraphicsFamily::kXenos, &CreateCustomDriver}};

}  // namespace

class CustomXenosProvider final : public rex::IModule, public rex::IDriverModule {
 public:
  REX_MODULE_SERVES(::rex::IDriverModule)

  std::string_view GetName() const override { return "rexgpu-xenos-custom"; }

  std::span<const rex::InterfaceType> GetRequiredInterfaces() const override {
    return kRequired;
  }

  [[nodiscard]] rex::Result<void> Connect(rex::IInterfaceFactory& /*factory*/) override {
    rex::memory::SetTagName(rex::gpu::kGPU, "GPU");
    return {};
  }

  void Disconnect() override {}
  [[nodiscard]] rex::Result<void> Init() override { return {}; }
  void Shutdown() override {}

  rex::IInterface* QueryInterface(std::string_view name, rex::u32 version) override {
    return rex::FindModuleInterface<rex::IDriverModule>(*this, name, version);
  }

  std::span<const rex::DriverRegistration> GetDrivers() const override { return kDrivers; }
};

}  // namespace bs1::renderer

REX_DECLARE_MODULE(bs1::renderer::CustomXenosProvider)
