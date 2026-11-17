// HLE implementations for the measured kernel/XAM import set.
// Real semantics are implemented for boot-critical APIs; everything else
// returns NTSTATUS_SUCCESS (0) and is recorded via the missing-import path.
//
// Guest ABI: arguments in r3..r10, return value in r3. All handlers write r3.

#include "hle_imports.h"

#include "hle_import_names.h"
#include "gpu_layer.h"
#include "ppc_context.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace hle_imports {
namespace {

// ---- Guest memory helpers (big-endian, addressed from the runtime base) ----

inline uint8_t* gm(uint8_t* base, uint32_t addr) { return base + addr; }

uint32_t ld32(const uint8_t* base, uint32_t addr) {
    uint32_t be = 0;
    std::memcpy(&be, base + addr, 4);
    return __builtin_bswap32(be);
}
void st32(uint8_t* base, uint32_t addr, uint32_t value) {
    const uint32_t be = __builtin_bswap32(value);
    std::memcpy(base + addr, &be, 4);
}
uint64_t ld64(const uint8_t* base, uint32_t addr) {
    uint64_t be = 0;
    std::memcpy(&be, base + addr, 8);
    return __builtin_bswap64(be);
}
uint16_t ld16(const uint8_t* base, uint32_t addr) {
    uint16_t be = 0;
    std::memcpy(&be, base + addr, 2);
    return __builtin_bswap16(be);
}

std::string read_ascii(uint8_t* base, uint32_t addr, size_t max = 512) {
    std::string s;
    for (size_t i = 0; i < max; ++i) {
        const char c = static_cast<char>(*gm(base, addr + static_cast<uint32_t>(i)));
        if (c == '\0') break;
        s.push_back(c);
    }
    return s;
}

// ---- Simple guest heap (bump allocator above the game image) --------------

// High region, clear of the guest indirect-dispatch table (0x82CD0000..
// 0x83EF32B8) and all seeded state (0x86000000+), so HLE bump allocations
// never overlap guest state or dispatch slots.
uint32_t g_heap_next = 0x90000000u;

uint32_t alloc_guest(size_t size) {
    const uint32_t addr = g_heap_next;
    g_heap_next = static_cast<uint32_t>(g_heap_next + ((size + 15u) & ~size_t(15u)));
    return addr;
}


// ---- TLS slots ------------------------------------------------------------

std::array<uint32_t, 64> g_tls{};
uint32_t g_tls_next = 1;

// ---- Diagnostic -----------------------------------------------------------

uint64_t default_handler(PPCContext& ctx, uint8_t*) {
    // NTSTATUS_SUCCESS and most Xbox APIs use r3 = 0 for success.
    gpr_write(ctx, 3, 0);
    return 0;
}

// ---- Real handlers ---------------------------------------------------------

// Xbox RTL_CRITICAL_SECTION: LockCount at +0, RecursionCount at +4.
uint64_t cs_init(PPCContext& ctx, uint8_t* base) {
    const uint32_t addr = static_cast<uint32_t>(gpr_read(ctx, 3));
    if (addr) {
        st32(base, addr, 0);
        st32(base, addr + 4, 0);
    }
    gpr_write(ctx, 3, 0);
    return 0;
}
uint64_t cs_enter(PPCContext& ctx, uint8_t* base) {
    const uint32_t addr = static_cast<uint32_t>(gpr_read(ctx, 3));
    if (addr) {
        const uint32_t count = ld32(base, addr);
        st32(base, addr, count + 1);
        st32(base, addr + 4, ld32(base, addr + 4) + 1);
    }
    gpr_write(ctx, 3, 1);
    return 1;
}
uint64_t cs_leave(PPCContext& ctx, uint8_t* base) {
    const uint32_t addr = static_cast<uint32_t>(gpr_read(ctx, 3));
    if (addr) {
        const uint32_t count = ld32(base, addr);
        st32(base, addr, count ? count - 1 : 0);
        const uint32_t recursion = ld32(base, addr + 4);
        st32(base, addr + 4, recursion ? recursion - 1 : 0);
    }
    gpr_write(ctx, 3, 0);
    return 0;
}

// RtlInitAnsiString(dest, source): X_ANSI_STRING is {u16 len, u16 max, u32 buf}.
uint64_t init_ansi_string(PPCContext& ctx, uint8_t* base) {
    const uint32_t dst = static_cast<uint32_t>(gpr_read(ctx, 3));
    const uint32_t src = static_cast<uint32_t>(gpr_read(ctx, 4));
    if (dst) {
        uint32_t len = 0;
        while (src + len < 0x100100000u && *(base + src + len)) ++len;
        const uint16_t l = static_cast<uint16_t>(len);
        const uint16_t m = static_cast<uint16_t>(len + 1);
        const uint16_t lbe = __builtin_bswap16(l);
        const uint16_t mbe = __builtin_bswap16(m);
        const uint32_t pbe = __builtin_bswap32(src);
        std::memcpy(base + dst + 0, &lbe, 2);
        std::memcpy(base + dst + 2, &mbe, 2);
        std::memcpy(base + dst + 4, &pbe, 4);
    }
    gpr_write(ctx, 3, 0);
    return 0;
}

// RtlInitUnicodeString(dest, source): UTF-16 source, X_UNICODE_STRING layout.
uint64_t init_unicode_string(PPCContext& ctx, uint8_t* base) {
    const uint32_t dst = static_cast<uint32_t>(gpr_read(ctx, 3));
    const uint32_t src = static_cast<uint32_t>(gpr_read(ctx, 4));
    if (dst) {
        uint32_t bytes = 0;
        while (src + bytes + 1 < 0x100100000u && ld16(base, src + bytes)) bytes += 2;
        const uint16_t l = static_cast<uint16_t>(bytes);
        const uint16_t m = static_cast<uint16_t>(bytes + 2);
        const uint16_t lbe = __builtin_bswap16(l);
        const uint16_t mbe = __builtin_bswap16(m);
        const uint32_t pbe = __builtin_bswap32(src);
        std::memcpy(base + dst + 0, &lbe, 2);
        std::memcpy(base + dst + 2, &mbe, 2);
        std::memcpy(base + dst + 4, &pbe, 4);
    }
    gpr_write(ctx, 3, 0);
    return 0;
}

// RtlUnicodeToMultiByteN(dst, maxBytes, bytesWritten*, src, srcBytes)
uint64_t unicode_to_multibyte(PPCContext& ctx, uint8_t* base) {
    const uint32_t dst = static_cast<uint32_t>(gpr_read(ctx, 3));
    const uint32_t max_bytes = static_cast<uint32_t>(gpr_read(ctx, 4));
    const uint32_t written_ptr = static_cast<uint32_t>(gpr_read(ctx, 5));
    const uint32_t src = static_cast<uint32_t>(gpr_read(ctx, 6));
    const uint32_t src_bytes = static_cast<uint32_t>(gpr_read(ctx, 7));
    uint32_t out = 0;
    for (uint32_t i = 0; i + 1 < src_bytes && out + 1 < max_bytes &&
                        src + i + 1 < 0x100100000u; i += 2) {
        const uint16_t wc = ld16(base, src + i);
        if (!wc) break;
        base[dst + out++] = static_cast<char>(wc < 128 ? wc : '?');
    }
    if (dst + out < 0x100100000u) base[dst + out] = '\0';
    if (written_ptr) st32(base, written_ptr, out);
    gpr_write(ctx, 3, 0); // STATUS_SUCCESS
    return 0;
}

uint64_t dbg_print(PPCContext& ctx, uint8_t* base) {
    const auto msg = read_ascii(base, static_cast<uint32_t>(gpr_read(ctx, 3)));
    std::fprintf(stderr, "[guest] %s\n", msg.c_str());
    gpr_write(ctx, 3, 0);
    return 0;
}

uint64_t alloc_pool(PPCContext& ctx, uint8_t*) {
    const size_t size = static_cast<size_t>(gpr_read(ctx, 3));
    const uint32_t addr = alloc_guest(size);
    gpr_write(ctx, 3, addr);
    return addr;
}

uint64_t free_pool(PPCContext&, uint8_t*) {
    return 0; // bump allocator: frees are no-ops
}

uint64_t alloc_physical(PPCContext& ctx, uint8_t*) {
    const size_t size = static_cast<size_t>(gpr_read(ctx, 3));
    const uint32_t addr = alloc_guest(size);
    gpr_write(ctx, 3, addr);
    return addr;
}

uint64_t query_alloc_size(PPCContext& ctx, uint8_t*) {
    (void)ctx; // bump allocator does not track per-allocation sizes
    gpr_write(ctx, 3, 0);
    return 0;
}

// NtQueryVirtualMemory(BaseAddress, ..., HeapAllocationInfo*, Length, ...)
// The XDK heap-create (sub_821A0B00) checks that the returned base_address
// (offset 0) equals the queried address; our old no-op left it 0, so heap
// creation failed and the allocator global stayed null.
uint64_t query_virtual_memory(PPCContext& ctx, uint8_t* base) {
    const uint32_t queried = static_cast<uint32_t>(gpr_read(ctx, 3));
    const uint32_t out = static_cast<uint32_t>(gpr_read(ctx, 4));
    if (out) {
        st32(base, out + 0, queried);            // base_address
        st32(base, out + 4, queried);            // allocation_base
        st32(base, out + 8, 0x04);               // allocation_protect (RW)
        st32(base, out + 12, 0x10000000u);       // allocation_size (256 MB)
        st32(base, out + 16, 0x10000000u);       // region_size
        st32(base, out + 20, 0x1000);            // state = MEM_COMMIT
        st32(base, out + 24, 0x04);              // protect = RW
    }
    gpr_write(ctx, 3, 0); // STATUS_SUCCESS
    return 0;
}

uint64_t free_virtual_memory(PPCContext& ctx, uint8_t*) {
    gpr_write(ctx, 3, 0); // STATUS_SUCCESS (bump allocator: no-op free)
    return 0;
}

uint64_t alloc_virtual(PPCContext& ctx, uint8_t* base) {
    // NtAllocateVirtualMemory(BaseAddress*, 0, RegionSize*, ...)
    const uint32_t base_ptr = static_cast<uint32_t>(gpr_read(ctx, 3));
    const uint32_t size_ptr = static_cast<uint32_t>(gpr_read(ctx, 5));
    const size_t size = static_cast<size_t>(ld32(base, size_ptr));
    const uint32_t addr = alloc_guest(size);
    st32(base, base_ptr, addr);
    st32(base, size_ptr, static_cast<uint32_t>(size));
    gpr_write(ctx, 3, 0);
    return 0;
}

// ---- File system HLE backed by the extracted disc tree --------------------

// Map of open guest handles to host file paths. Host-only: the Android build
// uses the launcher-provided asset fd layer (gpu_layer/platform) instead.
struct FileSystem {
    std::map<std::string, std::string> index;   // lowercased name -> host path
    std::map<uint32_t, std::string> open;        // handle -> host path
    uint32_t next_handle = 0x1000;

    static std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    void build_index() {
        if (!index.empty()) return;
        namespace fs = std::filesystem;
        const char* env = std::getenv("BS1_GAME_ROOT");
        const fs::path root = env ? fs::path(env) : fs::path("_game/root");
        std::error_code ec;
        for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const fs::path p = it->path();
            index.emplace(lower(p.filename().string()), p.generic_string());
            index.emplace(lower(fs::relative(p, root).generic_string()), p.generic_string());
        }
        std::fprintf(stderr, "[file] indexed %zu files from %s\n", index.size(),
                     root.string().c_str());
    }

    std::string resolve(const std::string& guest_name) {
        build_index();
        std::string n = guest_name;
        // Strip Xbox device prefix (d:\, D:\, \\??\, etc.) and normalise.
        size_t colon = n.find(':');
        if (colon != std::string::npos && colon < 4) n = n.substr(colon + 1);
        while (!n.empty() && (n[0] == '\\' || n[0] == '/')) n.erase(n.begin());
        std::replace(n.begin(), n.end(), '\\', '/');
        n = lower(n);
        auto it = index.find(n);
        if (it != index.end()) return it->second;
        // Try basename only.
        const size_t slash = n.find_last_of('/');
        if (slash != std::string::npos) {
            it = index.find(n.substr(slash + 1));
            if (it != index.end()) return it->second;
        }
        return {};
    }
};

static FileSystem& fs() {
    static FileSystem instance;
    return instance;
}

static std::string ansi_name(uint8_t* base, uint32_t name_str) {
    if (!name_str) return {};
    const uint32_t nlen = ld32(base, name_str + 0) & 0xFFFF;
    const uint32_t nptr = ld32(base, name_str + 4);
    std::string name;
    for (uint32_t i = 0; i < nlen && nptr + i < 0x100100000u; ++i) {
        name.push_back(static_cast<char>(*(base + nptr + i)));
    }
    // Some XDK ANSI_STRING lengths include the terminating null.
    while (!name.empty() && name.back() == '\0') name.pop_back();
    return name;
}

uint64_t create_file(PPCContext& ctx, uint8_t* base) {
    // NtCreateFile(Handle*, DesiredAccess, ObjectAttributes, IoStatusBlock, ...)
    const uint32_t handle_ptr = static_cast<uint32_t>(gpr_read(ctx, 3));
    const uint32_t objattr = static_cast<uint32_t>(gpr_read(ctx, 5));
    uint32_t name_str = objattr ? ld32(base, objattr + 4) : 0;
    const std::string guest_name = ansi_name(base, name_str);
    std::string path = fs().resolve(guest_name);
    // A bare device root (d:\) is a directory handle, not a file.
    if (path.empty() && (guest_name == "d:\\" || guest_name == "D:\\" ||
                         guest_name == "\\" || guest_name == "d:")) {
        path = "_game/root";
    }

    const uint32_t handle = fs().next_handle++;
    fs().open[handle] = path;
    st32(base, handle_ptr, handle);
    // X_IO_STATUS_BLOCK.status @ +0
    const uint32_t iosb = static_cast<uint32_t>(gpr_read(ctx, 6));
    if (iosb) st32(base, iosb, 0);
    gpr_write(ctx, 3, 0); // STATUS_SUCCESS
    if (!guest_name.empty()) {
        std::fprintf(stderr, "[file] open '%s' -> %s\n", guest_name.c_str(),
                     path.empty() ? "(not found)" : path.c_str());
    }
    return 0;
}

uint64_t open_file(PPCContext& ctx, uint8_t* base) {
    // NtOpenFile(Handle*, DesiredAccess, ObjectAttributes, IoStatusBlock, ...)
    // Same layout as NtCreateFile for the arguments we use.
    return create_file(ctx, base);
}

uint64_t read_file(PPCContext& ctx, uint8_t* base) {
    // NtReadFile(FileHandle, Event, Apc, ApcCtx, IoStatusBlock, Buffer,
    //            Length, ByteOffset, Key): r3=handle, r7=iosb, r8=buf, r9=len.
    const uint32_t handle = static_cast<uint32_t>(gpr_read(ctx, 3));
    const uint32_t iosb = static_cast<uint32_t>(gpr_read(ctx, 7));
    const uint32_t buffer = static_cast<uint32_t>(gpr_read(ctx, 8));
    const uint32_t length = static_cast<uint32_t>(gpr_read(ctx, 9));
    uint32_t read = 0;
    auto it = fs().open.find(handle);
    if (it != fs().open.end() && !it->second.empty()) {
        std::ifstream f(it->second, std::ios::binary);
        if (f.is_open()) {
            const uint32_t cap = std::min<uint32_t>(length, 0x10000000u - buffer);
            std::vector<char> tmp(cap);
            f.read(tmp.data(), cap);
            read = static_cast<uint32_t>(f.gcount());
            if (buffer + read <= 0x100100000u) {
                std::memcpy(base + buffer, tmp.data(), read);
            }
        }
    }
    if (iosb) {
        st32(base, iosb + 0, 0);       // status = STATUS_SUCCESS
        st32(base, iosb + 4, read);    // information = bytes read
    }
    gpr_write(ctx, 3, 0);
    return 0;
}

// NtQueryDirectoryFile(FileHandle, Event, Apc, ApcCtx, IoStatusBlock,
//                      FileInformation, Length, FileInformationClass,
//                      ReturnSingleEntry, FileName, RestartScan)
//   r3=handle, r7=iosb, r8=buffer, r9=length, r10=info class.
// Enumerates the top-level entries of the extracted disc tree using the SDK's
// X_FILE_DIRECTORY_INFORMATION layout (rex/system/xfile.h).
uint64_t query_directory(PPCContext& ctx, uint8_t* base) {
    static std::map<uint32_t, size_t> dir_pos;
    const uint32_t handle = static_cast<uint32_t>(gpr_read(ctx, 3));
    const uint32_t iosb = static_cast<uint32_t>(gpr_read(ctx, 7));
    const uint32_t buffer = static_cast<uint32_t>(gpr_read(ctx, 8));
    const uint32_t length = static_cast<uint32_t>(gpr_read(ctx, 9));
    uint32_t written = 0;

    auto it = fs().open.find(handle);
    std::string dir_path;
    if (it != fs().open.end()) dir_path = it->second;
    if (dir_path.empty()) {
        // d:\ (device root) resolves to the game root directory.
        dir_path = "_game/root";
    }
    namespace fsns = std::filesystem;
    std::error_code ec;
    std::vector<fsns::path> entries;
    if (fsns::is_directory(dir_path, ec)) {
        for (fsns::directory_iterator it2(dir_path, ec), end; it2 != end; it2.increment(ec)) {
            entries.push_back(it2->path());
        }
    }
    size_t& pos = dir_pos[handle];
    const uint32_t HDR = 0x40;  // X_FILE_DIRECTORY_INFORMATION header size
    for (; pos < entries.size(); ++pos) {
        const auto& p = entries[pos];
        const std::string name = p.filename().string();
        const uint32_t entry_size = HDR + static_cast<uint32_t>(name.size() * 2 + 2);
        if (written + entry_size > length) break;
        const uint32_t p0 = buffer + written;
        if (p0 + entry_size > 0x100100000u) break;
        st32(base, p0 + 0x0, entry_size);                       // next_entry_offset
        st32(base, p0 + 0x4, static_cast<uint32_t>(pos));       // file_index
        st32(base, p0 + 0x38, 0x10);                            // attributes = ARCHIVE
        st32(base, p0 + 0x3C, static_cast<uint32_t>(name.size() * 2)); // name length (bytes)
        for (size_t i = 0; i < name.size(); ++i) {
            const uint16_t be = __builtin_bswap16(static_cast<uint16_t>(name[i]));
            std::memcpy(base + p0 + HDR + i * 2, &be, 2);
        }
        const uint16_t nul = 0;
        std::memcpy(base + p0 + HDR + name.size() * 2, &nul, 2);
        written += entry_size;
        ++pos;
    }
    if (iosb) {
        st32(base, iosb + 0, 0);       // STATUS_SUCCESS
        st32(base, iosb + 4, written); // information = bytes written
    }
    gpr_write(ctx, 3, 0);
    return 0;
}

uint64_t close_handle(PPCContext& ctx, uint8_t*) {
    const uint32_t handle = static_cast<uint32_t>(gpr_read(ctx, 3));
    fs().open.erase(handle);
    gpr_write(ctx, 3, 0);
    return 0;
}

uint64_t write_file(PPCContext& ctx, uint8_t* base) {
    const uint32_t iosb = static_cast<uint32_t>(gpr_read(ctx, 4));
    if (iosb) st32(base, iosb + 8, 0);
    gpr_write(ctx, 3, 0);
    return 0;
}

uint64_t current_process_type(PPCContext& ctx, uint8_t*) {
    gpr_write(ctx, 3, 2); // kXboxProcessTitle
    return 2;
}

uint64_t query_perf_frequency(PPCContext& ctx, uint8_t* base) {
    const uint32_t ptr = static_cast<uint32_t>(gpr_read(ctx, 3));
    if (ptr) {
        // Xenon timebase runs at the 32 kHz / 1 GHz family; 3.579545 MHz is
        // the conventional XBOX value used by games for timing.
        const uint64_t value = 3579545ull;
        const uint64_t be = __builtin_bswap64(value);
        std::memcpy(base + ptr, &be, 8);
    }
    gpr_write(ctx, 3, 0);
    return 0;
}

uint64_t query_system_time(PPCContext& ctx, uint8_t* base) {
    const uint32_t ptr = static_cast<uint32_t>(gpr_read(ctx, 3));
    if (ptr) {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        const uint64_t value = static_cast<uint64_t>(now) * 10ull; // 100ns units
        const uint64_t be = __builtin_bswap64(value);
        std::memcpy(base + ptr, &be, 8);
    }
    gpr_write(ctx, 3, 0);
    return 0;
}

uint64_t tls_alloc(PPCContext& ctx, uint8_t*) {
    const uint32_t slot = g_tls_next++;
    gpr_write(ctx, 3, slot);
    return slot;
}

uint64_t tls_get(PPCContext& ctx, uint8_t* base) {
    // X_KPCR.tls_ptr lives at offset 0 of the KPCR pointed to by r13
    // (SDK rex/system/xthread.h). Keep all TLS state in guest memory so the
    // value changes as the guest calls KeTlsSetValue.
    const uint32_t kpcr = static_cast<uint32_t>(gpr_read(ctx, 13));
    const uint32_t block = kpcr ? ld32(base, kpcr) : 0;
    const auto slot = static_cast<size_t>(gpr_read(ctx, 3));
    // XDK uses -1 (TLS_OUT_OF_INDEXES) as an uninitialised sentinel. Games
    // often dereference the returned pointer; hand back valid guest scratch
    // memory instead of null so early startup can proceed.
    constexpr uint32_t kTlsScratch = 0x86300000u;
    uint32_t result = kTlsScratch;
    if (block && slot < 1024) {
        const uint32_t stored = ld32(base, block + static_cast<uint32_t>(slot) * 4);
        result = stored ? stored : kTlsScratch;
    } else if (slot < g_tls.size() && g_tls[slot]) {
        result = g_tls[slot];
    }
    gpr_write(ctx, 3, result);
    return result;
}

uint64_t tls_set(PPCContext& ctx, uint8_t* base) {
    const auto slot = static_cast<size_t>(gpr_read(ctx, 3));
    const uint32_t value = static_cast<uint32_t>(gpr_read(ctx, 4));
    const uint32_t kpcr = static_cast<uint32_t>(gpr_read(ctx, 13));
    const uint32_t block = kpcr ? ld32(base, kpcr) : 0;
    if (block && slot < 1024) {
        st32(base, block + static_cast<uint32_t>(slot) * 4, value);
    }
    if (slot < g_tls.size()) g_tls[slot] = value;
    gpr_write(ctx, 3, 0);
    return 0;
}

uint64_t compare_memory(PPCContext& ctx, uint8_t* base) {
    const uint8_t* a = gm(base, static_cast<uint32_t>(gpr_read(ctx, 3)));
    const uint8_t* b = gm(base, static_cast<uint32_t>(gpr_read(ctx, 4)));
    const size_t n = static_cast<size_t>(gpr_read(ctx, 5));
    size_t i = 0;
    while (i < n && a[i] == b[i]) ++i;
    gpr_write(ctx, 3, n - i);
    return n - i;
}

uint64_t fill_memory_ulong(PPCContext& ctx, uint8_t* base) {
    const uint32_t dst = static_cast<uint32_t>(gpr_read(ctx, 3));
    const uint32_t value = static_cast<uint32_t>(gpr_read(ctx, 4));
    const size_t count = static_cast<size_t>(gpr_read(ctx, 5));
    for (size_t i = 0; i < count; ++i) st32(base, dst + static_cast<uint32_t>(i * 4), value);
    gpr_write(ctx, 3, 0);
    return 0;
}

uint64_t status_to_dos_error(PPCContext& ctx, uint8_t*) {
    const uint32_t status = static_cast<uint32_t>(gpr_read(ctx, 3));
    gpr_write(ctx, 3, status);
    return status;
}

uint64_t vd_swap(PPCContext& ctx, uint8_t*) {
    gpu::present();
    gpr_write(ctx, 3, 0);
    return 0;
}

uint64_t xinput_state(PPCContext& ctx, uint8_t* base) {
    // No physical controller attached in this environment; return "no device"
    // and zero the guest state structure so games do not read garbage.
    const uint32_t state = static_cast<uint32_t>(gpr_read(ctx, 4));
    if (state) std::memset(base + state, 0, 64);
    gpr_write(ctx, 3, 1); // ERROR_DEVICE_NOT_CONNECTED
    return 1;
}

uint64_t xinput_set_state(PPCContext& ctx, uint8_t*) {
    gpr_write(ctx, 3, 1);
    return 1;
}

uint64_t get_game_region(PPCContext& ctx, uint8_t*) {
    gpr_write(ctx, 3, 0xFF); // all regions (World build)
    return 0xFF;
}

uint64_t get_language(PPCContext& ctx, uint8_t*) {
    gpr_write(ctx, 3, 1); // English
    return 1;
}

uint64_t check_privilege(PPCContext& ctx, uint8_t*) {
    gpr_write(ctx, 3, 1);
    return 1;
}

} // namespace

// ---- Handler table ---------------------------------------------------------

using Handler = uint64_t (*)(PPCContext&, uint8_t*);

struct Entry { const char* name; Handler handler; };

const Entry kHandlers[] = {
    {"RtlUnicodeToMultiByteN", unicode_to_multibyte},
    {"RtlMultiByteToUnicodeN", default_handler},
    {"RtlInitAnsiString", init_ansi_string},
    {"RtlInitUnicodeString", init_unicode_string},
    {"RtlInitializeCriticalSection", cs_init},
    {"RtlInitializeCriticalSectionAndSpinCount", cs_init},
    {"RtlEnterCriticalSection", cs_enter},
    {"RtlLeaveCriticalSection", cs_leave},
    {"DbgPrint", dbg_print},
    {"ExAllocatePool", alloc_pool},
    {"ExFreePool", free_pool},
    {"MmAllocatePhysicalMemoryEx", alloc_physical},
    {"MmFreePhysicalMemory", free_pool},
    {"MmQueryAllocationSize", query_alloc_size},
    {"NtAllocateVirtualMemory", alloc_virtual},
    {"NtQueryVirtualMemory", query_virtual_memory},
    {"NtFreeVirtualMemory", free_virtual_memory},
    {"NtCreateFile", create_file},
    {"NtOpenFile", open_file},
    {"NtClose", close_handle},
    {"NtQueryDirectoryFile", query_directory},
    {"NtReadFile", read_file},
    {"NtWriteFile", write_file},
    {"KeGetCurrentProcessType", current_process_type},
    {"KeQueryPerformanceFrequency", query_perf_frequency},
    {"KeQuerySystemTime", query_system_time},
    {"KeTlsAlloc", tls_alloc},
    {"KeTlsGetValue", tls_get},
    {"KeTlsSetValue", tls_set},
    {"RtlCompareMemory", compare_memory},
    {"RtlCompareMemoryUlong", compare_memory},
    {"RtlFillMemoryUlong", fill_memory_ulong},
    {"RtlNtStatusToDosError", status_to_dos_error},
    {"VdSwap", vd_swap},
    {"XInputGetState", xinput_state},
    {"XInputSetState", xinput_set_state},
    {"XamInputGetState", xinput_state},
    {"XamInputSetState", xinput_set_state},
    {"XGetGameRegion", get_game_region},
    {"XGetLanguage", get_language},
    {"XexCheckExecutablePrivilege", check_privilege},
};

Handler handler_for(const std::string& name) {
    for (const auto& e : kHandlers) {
        if (name == e.name) return e.handler;
    }
    return default_handler;
}

} // namespace hle_imports

void register_named_imports(hle::Kernel& kernel) {
    for (int i = 0; i < kHLEImportCount; ++i) {
        kernel.register_named(kHLEImportNames[i], hle_imports::handler_for(kHLEImportNames[i]));
    }
}
