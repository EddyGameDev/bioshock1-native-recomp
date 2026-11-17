# Third-party dependencies

The large upstream toolchains and SDK sources are **not stored in this git
repository**. They are fetched at build time, because together they are several
gigabytes and include their own git histories and build outputs.

| Dependency | Upstream | Local path | Fetched by |
|---|---|---|---|
| ReXGlue SDK v0.11.0 (host tooling) | https://github.com/rexglue/rexglue-sdk | `tools-bin/rexglue-sdk-0.11.0/` | `scripts/install-rexglue.ps1` |
| ReXGlue SDK source (runtime; patched) | https://github.com/rexglue/rexglue-sdk | `third_party/rexglue-git/` | clone upstream, then apply [`../patches/PATCHES.md`](../patches/PATCHES.md) |
| glslang (SPIR-V generation) | https://github.com/KhronosGroup/glslang | `third_party/rexglue-git/thirdparty/glslang/` | SDK submodule |
| SDL3 | https://github.com/libsdl-org/SDL | `third_party/rexglue-git/thirdparty/sdl3/` | SDK submodule |
| Codegen/recompiler tools | various (see `docs/02-TOOLCHAIN.md`) | `tools-bin/` | `scripts/install-toolchain.ps1` |

Android builds use **NDK r30** (`arm64-v8a`, API 28, `c++_shared`) and the
**Vulkan** backend; D3D12 is Windows-only. The SDK source tree must be present
and patched for the Android title screen to render — see the patches folder.
