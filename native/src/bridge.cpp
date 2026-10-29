#include <raynium/bridge.h>
#include <raynium/build_info.h>
#include <raynium/runtime.h>
#include <raynium/environment.h>
#include <raynium/preview.h>
#include <raynium/terrain.h>
#include <bit>
#include <radray/logger.h>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

#if defined(_DLL) || !defined(RADRAY_ENABLE_MIMALLOC)
#error "Raynium runtime requires static CRT and mimalloc"
#endif

static_assert(std::is_same_v<std::underlying_type_t<rn_status>, uint32_t>);
static_assert(std::is_same_v<std::underlying_type_t<rn_backend>, uint32_t>);
static_assert(std::is_same_v<std::underlying_type_t<rn_env_flags>, uint32_t>);
static_assert(std::is_same_v<std::underlying_type_t<rn_runtime_flags>, uint32_t>);
static_assert(std::is_same_v<std::underlying_type_t<rn_shared_handle_type>, uint32_t>);

static_assert(sizeof(rn_abi_info) == 240);
static_assert(offsetof(rn_abi_info, build_id) == 16);
static_assert(offsetof(rn_abi_info, toolchain) == 176);
static_assert(sizeof(RAYNIUM_BUILD_ID) <= sizeof(rn_abi_info::build_id));
static_assert(sizeof(RAYNIUM_RADRAY_REVISION) <= sizeof(rn_abi_info::radray_commit));
static_assert(sizeof(RAYNIUM_CONFIGURATION) <= sizeof(rn_abi_info::configuration));
static_assert(sizeof(RAYNIUM_TOOLCHAIN) <= sizeof(rn_abi_info::toolchain));
static_assert(sizeof(rn_env_desc) == 12);
static_assert(alignof(rn_env_desc) == 4);
static_assert(sizeof(rn_runtime_desc) == 20);
static_assert(alignof(rn_runtime_desc) == 4);
static_assert(offsetof(rn_runtime_desc, luid) == 12);
static_assert(!std::is_default_constructible_v<raynium::RayniumRuntime>);
static_assert(!std::is_default_constructible_v<raynium::RayniumEnvironment>);
static_assert(sizeof(rn_preview_desc) == 16);
static_assert(sizeof(rn_preview_info) == 128);
static_assert(offsetof(rn_preview_info, memory_handle) == 16);
static_assert(offsetof(rn_preview_info, luid) == 56);
static_assert(offsetof(rn_preview_info, native_format) == 96);
static_assert(offsetof(rn_preview_info, allocation_offset) == 112);

struct rn_runtime {
    radray::unique_ptr<raynium::RayniumRuntime> runtime;
    rn_preview* preview{nullptr};
    rn_terrain* terrain{nullptr};
};

struct rn_terrain {
    rn_runtime* owner;
    radray::unique_ptr<raynium::Terrain> terrain;
};

struct rn_preview {
    rn_runtime* owner;
    radray::unique_ptr<raynium::Preview> preview;
};

namespace {

// Access is serialized by the caller. The environment outlives its active runtime.
radray::unique_ptr<raynium::RayniumEnvironment> environment;
rn_runtime* activeRuntime = nullptr;

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

rn_status rn_query_abi(rn_abi_info* info, uint32_t capacity,
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

rn_status rn_init_env(const rn_env_desc* desc, char* error_message, uint32_t error_capacity) noexcept {
    WriteError(error_message, error_capacity, {});
    if (error_message == nullptr && error_capacity != 0) return RN_STATUS_FAILED;
    if (!desc || desc->struct_size != sizeof(rn_env_desc)) {
        WriteError(error_message, error_capacity, "Invalid rn_env_desc; expected 12 bytes");
        return RN_STATUS_FAILED;
    }
    if (desc->flags & ~static_cast<uint32_t>(RN_ENV_VALIDATION)) {
        WriteError(error_message, error_capacity, "Invalid environment flags");
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
    if (environment) {
        WriteError(error_message, error_capacity, "Native environment is already initialized");
        return RN_STATUS_FAILED;
    }
    std::string_view failure;
    if (!raynium::RayniumEnvironment::Create(backend, (desc->flags & RN_ENV_VALIDATION) != 0,
                                            environment, failure)) {
        WriteError(error_message, error_capacity, failure);
        return RN_STATUS_FAILED;
    }
    return RN_STATUS_OK;
}

rn_status rn_destroy_env(char* error_message, uint32_t error_capacity) noexcept {
    WriteError(error_message, error_capacity, {});
    if (!error_message && error_capacity) return RN_STATUS_FAILED;
    if (!environment) return RN_STATUS_OK;
    if (!environment->IsOwnerThread()) {
        WriteError(error_message, error_capacity, "Native environment belongs to another thread");
        return RN_STATUS_FAILED;
    }
    if (activeRuntime) {
        WriteError(error_message, error_capacity, "Destroy the active runtime before destroying the environment");
        return RN_STATUS_FAILED;
    }
    environment.reset();
    return RN_STATUS_OK;
}

rn_status rn_create_runtime(const rn_runtime_desc* desc, rn_runtime** out_runtime,
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
        WriteError(error_message, error_capacity, "Invalid rn_runtime_desc struct_size; expected 20 bytes");
        return RN_STATUS_FAILED;
    }
    radray::unique_ptr<raynium::RayniumRuntime> runtime;
    if ((desc->flags & ~static_cast<uint32_t>(RN_RUNTIME_MATCH_GPU)) != 0 ||
        ((desc->flags & RN_RUNTIME_MATCH_GPU) && desc->node_mask != 1)) {
        WriteError(error_message, error_capacity, "Invalid runtime flags or host node mask (expected 1)");
        return RN_STATUS_FAILED;
    }
    if (!environment || !environment->IsOwnerThread()) {
        WriteError(error_message, error_capacity, "Native environment is not initialized on this thread");
        return RN_STATUS_FAILED;
    }
    if (activeRuntime) {
        WriteError(error_message, error_capacity, "Native environment already has an active runtime");
        return RN_STATUS_FAILED;
    }
    std::optional<radray::render::ExternalDeviceIdentity> host;
    if (desc->flags & RN_RUNTIME_MATCH_GPU) {
        host.emplace();
        host->NodeMask = desc->node_mask;
        std::copy_n(desc->luid, 8, host->Luid.begin());
    }
    std::string_view failure;
    if (!raynium::RayniumRuntime::Create(*environment, runtime, failure, host)) {
        WriteError(error_message, error_capacity, failure);
        return RN_STATUS_FAILED;
    }
    radray::unique_ptr<rn_runtime> owned{new (std::nothrow) rn_runtime{std::move(runtime)}};
    if (!owned) {
        WriteError(error_message, error_capacity, "Cannot allocate runtime handle");
        return RN_STATUS_FAILED;
    }
    *out_runtime = owned.release();
    activeRuntime = *out_runtime;
    return RN_STATUS_OK;
}

void rn_destroy_runtime(rn_runtime* runtime) noexcept {
    if (!runtime) return;
    if (runtime != activeRuntime || !environment || !environment->IsOwnerThread() || runtime->preview || runtime->terrain) {
        RADRAY_ERR_LOG("Cannot destroy runtime: wrong owner/thread or live preview");
        return;
    }
    delete runtime;
    activeRuntime = nullptr;
}

rn_status rn_create_preview(rn_runtime* runtime, const rn_preview_desc* desc,
                          rn_preview** out_preview, rn_preview_info* info, uint32_t info_capacity,
                          char* error_message, uint32_t error_capacity) noexcept {
    if (out_preview) *out_preview = nullptr;
    WriteError(error_message, error_capacity, {});
    if (!error_message && error_capacity) return RN_STATUS_FAILED;
    if (!runtime || !desc || !out_preview || !info || info_capacity < sizeof(*info)) {
        WriteError(error_message, error_capacity, "Invalid preview pointer or info capacity (expected 128 bytes)");
        return RN_STATUS_FAILED;
    }
    if (desc->struct_size != sizeof(*desc) || desc->reserved || runtime->preview) {
        WriteError(error_message, error_capacity, "Invalid preview descriptor or runtime already has a preview");
        return RN_STATUS_FAILED;
    }
    radray::unique_ptr<raynium::Preview> preview;
    rn_preview_info result{};
    radray::string failure;
    if (!raynium::Preview::Create(*runtime->runtime, desc->width, desc->height, preview, result, failure)) {
        WriteError(error_message, error_capacity, failure);
        return RN_STATUS_FAILED;
    }
    radray::unique_ptr<rn_preview> owned{new (std::nothrow) rn_preview{runtime, std::move(preview)}};
    if (!owned) {
        WriteError(error_message, error_capacity, "Cannot allocate preview handle");
        return RN_STATUS_FAILED;
    }
    *info = result;
    *out_preview = owned.release();
    runtime->preview = *out_preview;
    return RN_STATUS_OK;
}

rn_status rn_draw_preview(rn_preview* preview, uint64_t sequence,
                        char* error_message, uint32_t error_capacity) noexcept {
    WriteError(error_message, error_capacity, {});
    if (!error_message && error_capacity) return RN_STATUS_FAILED;
    if (!preview) {
        WriteError(error_message, error_capacity, "Preview is null");
        return RN_STATUS_FAILED;
    }
    radray::string failure;
    if (!preview->preview->Draw(sequence, failure)) {
        WriteError(error_message, error_capacity, failure);
        return RN_STATUS_FAILED;
    }
    return RN_STATUS_OK;
}

rn_status rn_destroy_preview(rn_preview* preview, char* error_message, uint32_t error_capacity) noexcept {
    WriteError(error_message, error_capacity, {});
    if (!error_message && error_capacity) return RN_STATUS_FAILED;
    if (!preview) return RN_STATUS_OK;
    radray::string failure;
    if (!preview->preview->Finish(failure)) {
        WriteError(error_message, error_capacity, failure);
        return RN_STATUS_FAILED;
    }
    preview->owner->preview = nullptr;
    delete preview;
    return RN_STATUS_OK;
}

static_assert(sizeof(rn_terrain_desc)==24);
static_assert(sizeof(rn_terrain_mesh)==56);
static_assert(sizeof(rn_terrain_draw)==32);
static_assert(sizeof(rn_terrain_frame)==248);
static_assert(offsetof(rn_terrain_frame,projection)==64);

rn_status rn_create_terrain(rn_runtime* runtime, const rn_terrain_desc* desc, rn_terrain** output,
    rn_preview_info* images, uint32_t capacity, char* error, uint32_t error_capacity) noexcept {
    if(output) *output=nullptr;
    WriteError(error,error_capacity,{});
    if(!error && error_capacity) return RN_STATUS_FAILED;
    if(runtime!=activeRuntime || !runtime || !environment || !environment->IsOwnerThread() || runtime->terrain ||
       !desc || !output || !images || desc->struct_size!=sizeof(*desc) || desc->reserved || desc->split_mips>1 ||
       !desc->width || !desc->height || desc->width>16384 || desc->height>16384 || !desc->mip_levels ||
       desc->mip_levels>uint32_t(std::bit_width(std::max(desc->width,desc->height))) ||
       capacity<(desc->split_mips ? desc->mip_levels+1 : 2)) {
        WriteError(error,error_capacity,"Invalid terrain descriptor, owner, or image capacity"); return RN_STATUS_FAILED;
    }
    radray::unique_ptr<rn_terrain> owned{new(std::nothrow) rn_terrain{runtime,{}}};
    if(!owned) { WriteError(error,error_capacity,"Cannot allocate terrain handle"); return RN_STATUS_FAILED; }
    rn_preview_info result[16]{}; radray::string failure;
    if(!raynium::Terrain::Create(*runtime->runtime,*desc,owned->terrain,result,failure)) {
        WriteError(error,error_capacity,failure); return RN_STATUS_FAILED;
    }
    std::copy_n(result,desc->split_mips ? desc->mip_levels+1 : 2,images);
    *output=owned.release(); runtime->terrain=*output; return RN_STATUS_OK;
}
rn_status rn_draw_terrain(rn_preview* preview, rn_terrain* terrain, uint64_t sequence,
    const rn_terrain_frame* frame, char* error, uint32_t error_capacity) noexcept {
    WriteError(error,error_capacity,{});
    if(!error && error_capacity) return RN_STATUS_FAILED;
    if(!preview || !terrain || !frame || preview->owner!=terrain->owner ||
       !environment || !environment->IsOwnerThread()) {
        WriteError(error,error_capacity,"Invalid terrain draw owner or frame"); return RN_STATUS_FAILED;
    }
    radray::string failure;
    if(!preview->preview->DrawTerrain(sequence,*terrain->terrain,*frame,failure)) {
        WriteError(error,error_capacity,failure); return RN_STATUS_FAILED;
    }
    return RN_STATUS_OK;
}
rn_status rn_destroy_terrain(rn_terrain* terrain, char* error, uint32_t error_capacity) noexcept {
    WriteError(error,error_capacity,{});
    if(!error && error_capacity) return RN_STATUS_FAILED;
    if(!terrain) return RN_STATUS_OK;
    if(!environment || !environment->IsOwnerThread()) { WriteError(error,error_capacity,"Wrong terrain thread"); return RN_STATUS_FAILED; }
    radray::string failure;
    if(!terrain->terrain->Finish(failure)) { WriteError(error,error_capacity,failure); return RN_STATUS_FAILED; }
    terrain->owner->terrain=nullptr; delete terrain; return RN_STATUS_OK;
}
