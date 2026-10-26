#include <raynium/bridge.h>
#include <raynium/build_info.h>
#include <raynium/runtime.h>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

#if defined(_DLL) || !defined(RADRAY_ENABLE_MIMALLOC)
#error "Raynium runtime requires static CRT and mimalloc"
#endif

static_assert(sizeof(rn_abi_info) == 240);
static_assert(offsetof(rn_abi_info, build_id) == 16);
static_assert(offsetof(rn_abi_info, toolchain) == 176);
static_assert(sizeof(RAYNIUM_BUILD_ID) <= sizeof(rn_abi_info::build_id));
static_assert(sizeof(RAYNIUM_RADRAY_REVISION) <= sizeof(rn_abi_info::radray_commit));
static_assert(sizeof(RAYNIUM_CONFIGURATION) <= sizeof(rn_abi_info::configuration));
static_assert(sizeof(RAYNIUM_TOOLCHAIN) <= sizeof(rn_abi_info::toolchain));
static_assert(sizeof(rn_runtime_desc) == 8);
static_assert(alignof(rn_runtime_desc) == 4);
static_assert(offsetof(rn_runtime_desc, backend) == 4);
static_assert(!std::is_default_constructible_v<raynium::RayniumRuntime>);

struct rn_runtime {
    radray::unique_ptr<raynium::RayniumRuntime> runtime;
};

namespace {

void WriteError(char* output, uint32_t capacity, std::string_view message) noexcept {
    if (output == nullptr || capacity == 0) return;
    const size_t count = std::min(message.size(), static_cast<size_t>(capacity - 1));
    if (count != 0) std::memcpy(output, message.data(), count);
    output[count] = '\0';
}

}  // namespace

uint32_t rn_get_abi_version(void) noexcept {
    return RN_ABI_VERSION;
}

uint32_t rn_query_abi(rn_abi_info* info, uint32_t capacity,
                      char* error_message, uint32_t error_capacity) noexcept {
    WriteError(error_message, error_capacity, {});
    if (error_message == nullptr && error_capacity != 0) return RN_STATUS_FAILED;
    if (info == nullptr) {
        WriteError(error_message, error_capacity, "rn_query_abi: info is null");
        return RN_STATUS_FAILED;
    }
    if (capacity < sizeof(rn_abi_info)) {
        WriteError(error_message, error_capacity, "rn_query_abi: info capacity must be at least 240 bytes");
        return RN_STATUS_FAILED;
    }
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

uint32_t rn_create_runtime(const rn_runtime_desc* desc, rn_runtime** out_runtime,
                           char* error_message, uint32_t error_capacity) noexcept {
    if (out_runtime != nullptr) *out_runtime = nullptr;
    WriteError(error_message, error_capacity, {});
    if (error_message == nullptr && error_capacity != 0) return RN_STATUS_FAILED;
    if (desc == nullptr) {
        WriteError(error_message, error_capacity, "rn_create_runtime: desc is null");
        return RN_STATUS_FAILED;
    }
    if (out_runtime == nullptr) {
        WriteError(error_message, error_capacity, "rn_create_runtime: out_runtime is null");
        return RN_STATUS_FAILED;
    }
    if (desc->struct_size != sizeof(rn_runtime_desc)) {
        WriteError(error_message, error_capacity, "Invalid rn_runtime_desc struct_size; expected 8 bytes");
        return RN_STATUS_FAILED;
    }
    radray::render::RenderBackend backend;
    switch (desc->backend) {
        case RN_BACKEND_D3D12: backend = radray::render::RenderBackend::D3D12; break;
        case RN_BACKEND_VULKAN: backend = radray::render::RenderBackend::Vulkan; break;
        default:
            WriteError(error_message, error_capacity, "Unknown RHI backend; expected 1 (D3D12) or 2 (Vulkan)");
            return RN_STATUS_FAILED;
    }
    radray::unique_ptr<raynium::RayniumRuntime> runtime;
    std::string_view failure;
    if (!raynium::RayniumRuntime::Create(backend, runtime, failure)) {
        WriteError(error_message, error_capacity, failure);
        return RN_STATUS_FAILED;
    }
    radray::unique_ptr<rn_runtime> owned{new (std::nothrow) rn_runtime{std::move(runtime)}};
    if (!owned) {
        WriteError(error_message, error_capacity, "Cannot allocate runtime handle");
        return RN_STATUS_FAILED;
    }
    *out_runtime = owned.release();
    return RN_STATUS_OK;
}

void rn_destroy_runtime(rn_runtime* runtime) noexcept {
    delete runtime;
}
