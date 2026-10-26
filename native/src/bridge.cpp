#include <raynium/bridge.h>
#include <raynium/build_info.h>
#include <radray/memory.h>
#include <radray/render/rhi.h>
#include <mimalloc.h>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>

#if defined(_DLL) || !defined(RADRAY_ENABLE_MIMALLOC)
#error "Raynium bridge requires static CRT and mimalloc"
#endif

static_assert(sizeof(rn_abi_info) == 240);
static_assert(offsetof(rn_abi_info, build_id) == 16);
static_assert(offsetof(rn_abi_info, toolchain) == 176);
static_assert(sizeof(RAYNIUM_BUILD_ID) <= sizeof(rn_abi_info::build_id));
static_assert(sizeof(RAYNIUM_RADRAY_REVISION) <= sizeof(rn_abi_info::radray_commit));
static_assert(sizeof(RAYNIUM_CONFIGURATION) <= sizeof(rn_abi_info::configuration));
static_assert(sizeof(RAYNIUM_TOOLCHAIN) <= sizeof(rn_abi_info::toolchain));

uint32_t rn_get_abi_version(void) noexcept {
    return 1;
}

uint32_t rn_query_abi(rn_abi_info* info, uint32_t capacity) noexcept {
    if (info == nullptr) return RN_STATUS_INVALID_ARGUMENT;
    if (capacity < sizeof(rn_abi_info)) return RN_STATUS_BUFFER_TOO_SMALL;
    *info = {};
    info->struct_size = sizeof(rn_abi_info);
    info->abi_version = rn_get_abi_version();
    info->pointer_size = sizeof(void*);
    info->flags = 6u | RAYNIUM_RADRAY_DIRTY;
    std::memcpy(info->build_id, RAYNIUM_BUILD_ID, sizeof(RAYNIUM_BUILD_ID));
    std::memcpy(info->radray_commit, RAYNIUM_RADRAY_REVISION, sizeof(RAYNIUM_RADRAY_REVISION));
    std::memcpy(info->configuration, RAYNIUM_CONFIGURATION, sizeof(RAYNIUM_CONFIGURATION));
    std::memcpy(info->platform, "windows-x86_64", sizeof("windows-x86_64"));
    std::memcpy(info->toolchain, RAYNIUM_TOOLCHAIN, sizeof(RAYNIUM_TOOLCHAIN));
    return RN_STATUS_OK;
}

uint32_t rn_probe_vertex_stride(void) noexcept {
    return radray::render::GetVertexFormatSizeInBytes(radray::render::VertexFormat::FLOAT32X3);
}

uint32_t rn_probe_allocator(void) noexcept {
    uint32_t result = 0;
    auto* crt = std::malloc(64);
    if (crt != nullptr && mi_any_heap_contains(crt)) result |= 1;
    std::free(crt);
    auto* aligned = radray::AlignedAlloc(64, 128);
    if (aligned != nullptr && mi_any_heap_contains(aligned)) result |= 2;
    radray::AlignedFree(aligned);
    auto* object = new (std::nothrow) std::byte[64];
    if (object != nullptr && mi_any_heap_contains(object)) result |= 4;
    delete[] object;
    return result;
}
