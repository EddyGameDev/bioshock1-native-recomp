// Host-side first-boot harness for the recompiled BioShock XEX.
// Links the generated host objects with our runtime and attempts to call the
// guest entry point. This is the quickest way to iterate on boot failures
// before flashing an APK.

#include "hle_imports.h"
#include "hle_kernel.h"
#include "ppc_context.h"
#include "runtime_bridge.h"

#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {

constexpr uint32_t kImageBase = 0x82000000u;
constexpr uint32_t kStackBase = 0x82D00000u; // just past the 0xCD0000 image
constexpr size_t kStackSize = 0x20000u;
// Full 32-bit guest address space plus slack. ReXGlue's runtime maps this so
// wrapped guest addresses never fault due to the host window size.
constexpr size_t kGuestSize = 0x100100000ull;

#if defined(_WIN32)
LONG WINAPI boot_fault_filter(EXCEPTION_POINTERS* info) {
    const auto* rec = info->ExceptionRecord;
    const auto mod = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
    const auto addr = reinterpret_cast<uintptr_t>(rec->ExceptionAddress);
    std::fprintf(stderr,
                 "[boot] FAULT code=0x%08X addr=%p module=0x%llX rva=0x%llX accessed=%p\n",
                 rec->ExceptionCode, rec->ExceptionAddress,
                 (unsigned long long)mod, (unsigned long long)(addr - mod),
                 rec->NumberParameters >= 2 ? (void*)rec->ExceptionInformation[1] : nullptr);
    void* frames[40];
    const USHORT count = RtlCaptureStackBackTrace(1, 40, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        const auto f = reinterpret_cast<uintptr_t>(frames[i]);
        if (f >= mod) {
            std::fprintf(stderr, "[boot] frame %u rva=0x%llX\n", i,
                         (unsigned long long)(f - mod));
        }
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

LONG WINAPI boot_vector_handler(EXCEPTION_POINTERS* info) {
    const auto* rec = info->ExceptionRecord;
    const auto mod = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
    const auto addr = reinterpret_cast<uintptr_t>(rec->ExceptionAddress);
    char buf[256];
    const int n = std::snprintf(buf, sizeof(buf),
        "VECTOR code=0x%08X rva=0x%llX accessed=%p\n",
        rec->ExceptionCode, (unsigned long long)(addr - mod),
        rec->NumberParameters >= 2 ? (void*)rec->ExceptionInformation[1] : nullptr);
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), buf, n, &written, nullptr);
    void* frames[16];
    const USHORT count = RtlCaptureStackBackTrace(1, 16, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        const auto f = reinterpret_cast<uintptr_t>(frames[i]);
        const int m = std::snprintf(buf, sizeof(buf), "VECTOR frame %u rva=0x%llX\n",
                                    i, f >= mod ? (unsigned long long)(f - mod) : 0ull);
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), buf, m, &written, nullptr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

uint8_t* map_guest_memory() {
#if defined(_WIN32)
    void* p = VirtualAlloc(nullptr, kGuestSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) {
        std::fprintf(stderr, "VirtualAlloc(%zu) failed: %lu\n", kGuestSize, GetLastError());
        return nullptr;
    }
    return static_cast<uint8_t*>(p);
#else
    void* p = mmap(nullptr, kGuestSize, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
#endif
}

} // namespace

extern "C" void __imp__xstart(PPCContext& ctx, uint8_t* base);
// Debug wrappers were removed: non-tail host wrappers inflate the host stack
// during deep guest loops and caused artificial stack overflows.

// The Unreal config string-hash (sub_8227A278, 28-byte entries) sometimes gets
// a corrupt bucket-array pointer from a skipped constructor. If the bucket
// pointer is not a plausible guest address, give it a valid array pre-filled
// with -1 (empty-chain sentinel) so lookups terminate instead of cycling.
extern "C" void __imp__sub_8227A278(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8227A278(PPCContext& ctx, uint8_t* base) {
    static bool bucket_array_ready = false;
    constexpr uint32_t kBuckets = 0x86200000u;  // 256 * 4 bytes of -1
    if (!bucket_array_ready) {
        bucket_array_ready = true;
        for (uint32_t i = 0; i < 256; ++i) {
            const uint32_t be = __builtin_bswap32(0xFFFFFFFFu);
            std::memcpy(base + kBuckets + i * 4, &be, 4);
        }
    }
    const uint32_t table = static_cast<uint32_t>(gpr_read(ctx, 3));
    if (table && table < 0x100100000u) {
        uint32_t be = 0;
        std::memcpy(&be, base + table + 12, 4);
        const uint32_t buckets = __builtin_bswap32(be);
        if (buckets < 0x82000000u || buckets >= 0x100100000u) {
            const uint32_t fixed = __builtin_bswap32(kBuckets);
            std::memcpy(base + table + 12, &fixed, 4);
        }
    }
    __imp__sub_8227A278(ctx, base);
}

 // Trace constructor-dispatcher progress: sub_8219EE30.
extern "C" void __imp__sub_8219EE30(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8219EE30(PPCContext& ctx, uint8_t* base) {
    std::fprintf(stderr, "[ctors] sub_8219EE30 ENTER\n");
    std::fflush(stderr);
    __imp__sub_8219EE30(ctx, base);
    std::fprintf(stderr, "[ctors] sub_8219EE30 RETURN\n");
    std::fflush(stderr);
}

// Same empty-bucket repair for the 48-byte file-cache hash (sub_82170A50).
extern "C" void __imp__sub_82170A50(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82170A50(PPCContext& ctx, uint8_t* base) {
    static bool ready = false;
    constexpr uint32_t kBuckets = 0x86210000u;
    if (!ready) {
        ready = true;
        for (uint32_t i = 0; i < 256; ++i) {
            const uint32_t be = __builtin_bswap32(0xFFFFFFFFu);
            std::memcpy(base + kBuckets + i * 4, &be, 4);
        }
    }
    const uint32_t table = static_cast<uint32_t>(gpr_read(ctx, 3));
    if (table && table < 0x100100000u) {
        uint32_t be = 0;
        std::memcpy(&be, base + table + 12, 4);
        const uint32_t buckets = __builtin_bswap32(be);
        if (buckets < 0x82000000u || buckets >= 0x100100000u) {
            const uint32_t fixed = __builtin_bswap32(kBuckets);
            std::memcpy(base + table + 12, &fixed, 4);
        }
    }
    __imp__sub_82170A50(ctx, base);
}

struct BootArgs { PPCContext* ctx; uint8_t* base; };
static BootArgs g_boot_args{};
static HANDLE g_boot_thread = nullptr;
static std::vector<uintptr_t> g_rip_samples;
static std::thread g_sampler;

static void sample_guest_rip() {
    for (int i = 0; i < 1200; ++i) {
        Sleep(50);
        if (!g_boot_thread) break;
        SuspendThread(g_boot_thread);
        CONTEXT c{};
        c.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(g_boot_thread, &c)) {
            g_rip_samples.push_back(static_cast<uintptr_t>(c.Rip));
        }
        ResumeThread(g_boot_thread);
        if (WaitForSingleObject(g_boot_thread, 0) == WAIT_OBJECT_0) break;
    }
}

static BootArgs g_boot_args_unused{};
static DWORD WINAPI boot_thread(LPVOID) {
    __imp__xstart(*g_boot_args.ctx, g_boot_args.base);
    return 0;
}
extern const PPCFuncMapping kPPCFuncMappings[];

namespace {
constexpr uint64_t kCodeBase = 0x82160000ull;
constexpr uint64_t kImageSize = 0xCD0000ull;

// REX_LOOKUP_FUNC reads a host function pointer from this guest-memory table.
// The real rexkernel has a valid pointer at every slot; guest code sometimes
// reads an uninitialised vtable slot (0) and indexes the table out of range,
// which otherwise yields garbage (0xFFFFFFFF) and a hard fault. Fill the whole
// dispatch region with a safe no-op first, then overwrite the real entries.
static void dispatch_noop(PPCContext&, uint8_t*) {}

void install_dispatch_table(uint8_t* base) {
    // Dispatch region: slots indexed by (guest_addr - code_base)*2, 8-byte
    // host pointers, up to the highest mapped guest code address.
    constexpr uint64_t kMaxGuestCode = 0x82E40000ull;
    const size_t region = static_cast<size_t>((kMaxGuestCode - kCodeBase) * 2 + 8);
    void* noop = reinterpret_cast<void*>(&dispatch_noop);
    uint8_t* region_start = base + kImageBase + kImageSize;
    for (size_t o = 0; o < region; o += sizeof(void*)) {
        std::memcpy(region_start + o, &noop, sizeof(void*));
    }
    size_t count = 0;
    for (const PPCFuncMapping* m = kPPCFuncMappings; m && m->address; ++m) {
        const uint64_t offset = (static_cast<uint64_t>(m->address) - kCodeBase) * 2;
        uint8_t* slot = region_start + offset;
        void* host = reinterpret_cast<void*>(m->host);
        std::memcpy(slot, &host, sizeof(host));
        ++count;
    }
    std::printf("[boot] installed %zu indirect-dispatch entries (region %zu bytes)\n",
                count, region);
}
} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    SetUnhandledExceptionFilter(boot_fault_filter);
    AddVectoredExceptionHandler(1, boot_vector_handler);
#endif
    register_named_imports(hle::Kernel::instance());
    const char* image_path = argc > 1 ? argv[1] : "_work/default_unpacked";
    std::printf("[boot] opening decompressed image %s\n", image_path);

    std::ifstream in(image_path, std::ios::binary | std::ios::ate);
    if (!in) {
        std::fprintf(stderr, "[boot] cannot open image\n");
        return 1;
    }
    const auto image_size = static_cast<size_t>(in.tellg());
    in.seekg(0);
    std::vector<uint8_t> image(image_size);
    in.read(reinterpret_cast<char*>(image.data()), image_size);
    std::printf("[boot] image bytes: %zu\n", image_size);

    uint8_t* base = map_guest_memory();
    if (!base) return 2;
    std::printf("[boot] guest memory mapped at %p\n", (void*)base);
    bs_set_guest_base(base);

    std::memcpy(base + kImageBase, image.data(), image_size);
    std::printf("[boot] image loaded at guest 0x%08X\n", kImageBase);
    install_dispatch_table(base);

    // XDK startup reads an allocator-context pointer from this guest global
    // (sub_821A2538: lis r11,0x8219; lwz r3,-7428(r11)). Our minimal runtime
    // never initialises it, so seed it with a valid guest heap address.
    // Initialise the XDK allocator the way rexkernel does. sub_821A10B0 walks
    // 128 bins at ctx + (index + 48) * 8; an empty bin is a self-pointer.
    constexpr uint32_t kAllocatorGlobal = 0x82B2E2FCu;
    constexpr uint32_t kHeapCtx = 0x86000000u;
    auto put32 = [&](uint32_t addr, uint32_t value) {
        const uint32_t be = __builtin_bswap32(value);
        std::memcpy(base + addr, &be, 4);
    };
    // Guest heap-create path (allocations work); the real rexkernel also
    // pre-populates its pool table before guest code runs.
    put32(kAllocatorGlobal, 0u);
    std::printf("[boot] cleared allocator global for guest heap-create\n");


    // KeTlsGetValue(-1) returns 0x86300000 as scratch. Generated startup code
    // treats it as a small allocator structure: +32 = bump pointer,
    // +44 = limit, +20 = function pointer called when the limit is reached.
    constexpr uint32_t kTlsScratch = 0x86300000u;
    put32(kTlsScratch + 0, kTlsScratch + 0x100);
    put32(kTlsScratch + 20, kTlsScratch + 0x100);
    put32(kTlsScratch + 32, kTlsScratch + 0x200);
    put32(kTlsScratch + 44, kTlsScratch + 0xF00);
    std::printf("[boot] initialised TLS scratch at 0x%08X\n", kTlsScratch);

    // Two guest globals hold TLS slot indices. XDK startup normally stores
    // the KeTlsAlloc result (slot 1) into them; with zero/-1 the guest loops
    // on an invalid TLS block. Seed the allocated slot explicitly.
    put32(0x82ACC250u, 1u); // sub_82216E00: lwz r3,-15792(r30)
    put32(0x82A6F2FCu, 1u); // sub_82623480: lwz r3,-3400(r31)
    std::printf("[boot] seeded TLS slot globals -> 1\n");

    PPCContext ctx{};
    ctx.r1.u32 = kStackBase - 16;  // guest stack pointer (descending)
    ctx.msr = 0x200A000u;

    // r13 is the Xenon KPCR pointer. ReXGlue's kernel runtime allocates and
    // fills it; generated code reads r13+336 (flag) and r13+256 (thread-data
    // pointer). Seed a minimal KPCR so those reads are valid.
    constexpr uint32_t kKpcr = 0x86400000u;
    constexpr uint32_t kKpcrThreadData = 0x86401000u;
    ctx.r13.u32 = kKpcr;
    // X_KPCR.tls_ptr (SDK rex/system/xthread.h) is at offset 0. It points at
    // the current thread's TLS block; without it KeTlsGetValue has no state.
    uint32_t be_kpcr = __builtin_bswap32(0x86300000u);
    std::memcpy(base + kKpcr + 0, &be_kpcr, 4);
    be_kpcr = __builtin_bswap32(1u);
    std::memcpy(base + kKpcr + 336, &be_kpcr, 4);
    be_kpcr = __builtin_bswap32(kKpcrThreadData);
    std::memcpy(base + kKpcr + 256, &be_kpcr, 4);
    // argc = 0, argv = 0, envp = 0 on the guest stack.
    uint8_t* sp = base + ctx.r1.u32;
    std::memset(sp, 0, 64);

#if defined(_WIN32)
    // Run guest code on a thread with a large stack: generated C++ calls use
    // the host stack directly, and XDK startup can recurse deeply.
    g_boot_args = BootArgs{&ctx, base};
    HANDLE thread = CreateThread(nullptr, 256u * 1024u * 1024u, boot_thread,
                                 nullptr, 0, nullptr);
    g_boot_thread = thread;
    g_sampler = std::thread(sample_guest_rip);
    std::printf("[boot] calling guest entry 0x8219D698 on 256 MB stack thread\n");
    std::fflush(stdout);
    for (int i = 0; i < 300; ++i) {
        if (WaitForSingleObject(thread, 2000) == WAIT_OBJECT_0) break;
        if (i && i % 30 == 0) g_rip_samples.clear();
        std::fprintf(stderr, "[boot] heartbeat %ds: HLE calls=%llu\n",
                     (i + 1) * 2,
                     (unsigned long long)hle::Kernel::instance().hle_call_counter());
        if (i % 5 == 4) {
            const auto mod = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
            std::map<uintptr_t, int> rip_hist;
            for (auto rip : g_rip_samples) {
                const auto rva = rip - mod;
                if (rip >= mod && rva >= 0x300000ull && rva < 0x8000000ull) {
                    ++rip_hist[rva];
                }
            }
            std::vector<std::pair<int, uintptr_t>> rtop;
            for (auto& [rva, n] : rip_hist) rtop.push_back({n, rva});
            std::sort(rtop.begin(), rtop.end(), std::greater<>{});
            for (int k = 0; k < std::min(3, (int)rtop.size()); ++k) {
                std::fprintf(stderr, "  RIP rva=0x%llX count=%d\n",
                             (unsigned long long)rtop[k].second, rtop[k].first);
            }
            std::vector<std::pair<uint64_t, std::string>> top;
            for (const auto& [name, count] : hle::Kernel::instance().call_counts()) {
                top.push_back({count, name});
            }
            std::sort(top.begin(), top.end(), std::greater<>{});
            for (int k = 0; k < std::min(5, (int)top.size()); ++k) {
                std::fprintf(stderr, "  %s = %llu\n", top[k].second.c_str(),
                             (unsigned long long)top[k].first);
            }
        }
    }
    g_boot_thread = nullptr;
    if (g_sampler.joinable()) g_sampler.join();
    CloseHandle(thread);
#else
    __imp__xstart(ctx, base);
#endif
    std::printf("[boot] guest entry returned (r3=0x%llX)\n",
                (unsigned long long)ctx.r3.u64);
    {
        const auto mod = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
        std::map<uintptr_t, int> hist;
        for (auto rip : g_rip_samples) {
            if (rip >= mod) ++hist[rip - mod];
        }
        std::vector<std::pair<int, uintptr_t>> top;
        for (auto& [rva, n] : hist) top.push_back({n, rva});
        std::sort(top.begin(), top.end(), std::greater<>{});
        std::fprintf(stderr, "[boot] top sampled guest RIPs:\n");
        for (int i = 0; i < std::min(12, (int)top.size()); ++i) {
            std::fprintf(stderr, "  rva=0x%llX count=%d\n",
                         (unsigned long long)top[i].second, top[i].first);
        }
    }
    std::fprintf(stderr, "[boot] all HLE call counts:\n");
    for (const auto& [name, count] : hle::Kernel::instance().call_counts()) {
        std::fprintf(stderr, "  %-40s %llu\n", name.c_str(),
                     (unsigned long long)count);
    }
    std::fprintf(stderr, "[boot] last HLE calls:\n");
    for (const auto& name : hle::Kernel::instance().latest_calls()) {
        std::fprintf(stderr, "  %s\n", name.c_str());
    }
    return 0;
}
