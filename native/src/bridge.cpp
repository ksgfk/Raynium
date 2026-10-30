#include <raynium/bridge.h>
#include <raynium/build_info.h>
#include <raynium/runtime.h>
#include <raynium/environment.h>
#include <raynium/renderer.h>
#include <exception>
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
static_assert(sizeof(rn_render_target_desc)==16);
static_assert(sizeof(rn_shared_image_info)==128);
static_assert(sizeof(rn_shared_image_export)==144);
static_assert(sizeof(rn_index_update)==24);
static_assert(sizeof(rn_mesh_update)==48);
static_assert(sizeof(rn_material_update)==24);
static_assert(sizeof(rn_primitive_update)==24);
static_assert(sizeof(rn_visible_item)==24);
static_assert(sizeof(rn_render_frame_desc)==344);
static_assert(offsetof(rn_render_frame_desc,projection)==160);
static_assert(offsetof(rn_render_frame_desc,indices)==32);
static_assert(offsetof(rn_render_frame_desc,index_count)==112);
static_assert(offsetof(rn_render_frame_desc,camera_block)==288);
static_assert(offsetof(rn_render_frame_desc,fog_distances)==328);
static_assert(sizeof(rn_instance)==16 && sizeof(rn_submesh)==16);
static_assert(sizeof(rn_shared_textures_desc)==16 && sizeof(rn_texture_desc)==16);
static_assert(!std::is_default_constructible_v<raynium::Renderer>);
static_assert(!std::is_default_constructible_v<raynium::RenderTarget>);
static_assert(!std::is_default_constructible_v<raynium::SharedTextures>);

struct rn_runtime {
    radray::unique_ptr<raynium::RayniumRuntime> runtime;
    rn_renderer* renderer{};
};
struct rn_renderer {
    rn_runtime* owner;
    radray::unique_ptr<raynium::Renderer> renderer;
    rn_render_target* target{};
    rn_shared_textures* textures{};
};
struct rn_render_target { rn_renderer* owner; radray::unique_ptr<raynium::RenderTarget> target; };
struct rn_shared_textures { rn_renderer* owner; radray::unique_ptr<raynium::SharedTextures> textures; };

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
    environment=raynium::RayniumEnvironment::Create(backend, (desc->flags & RN_ENV_VALIDATION) != 0, failure, "Raynium").Release();
    if (!environment) {
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
    runtime=raynium::RayniumRuntime::Create(*environment, failure, host).Release();
    if (!runtime) {
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
    if (runtime != activeRuntime || !environment || !environment->IsOwnerThread() || runtime->renderer) {
        RADRAY_ERR_LOG("Cannot destroy runtime: wrong owner/thread or live renderer");
        return;
    }
    delete runtime;
    activeRuntime = nullptr;
}

namespace {
template<class Action> rn_status Invoke(char* error,uint32_t capacity,Action&& action) noexcept {
    WriteError(error,capacity,{});
    if(!error && capacity) return RN_STATUS_FAILED;
    try {
        radray::string failure;
        if(action(failure)) return RN_STATUS_OK;
        WriteError(error,capacity,failure); return RN_STATUS_FAILED;
    } catch(const std::exception& e) { WriteError(error,capacity,e.what()); }
      catch(...) { WriteError(error,capacity,"Unexpected renderer failure"); }
    return RN_STATUS_FAILED;
}
bool Owner(rn_renderer* renderer,radray::string& error) {
    if(!environment || !environment->IsOwnerThread() || !activeRuntime || !renderer || activeRuntime->renderer!=renderer) {
        error="Invalid renderer owner or thread"; return false;
    }
    return true;
}
}
rn_status rn_create_renderer(rn_runtime* runtime,const rn_renderer_desc* desc,rn_renderer** output,char* error,uint32_t capacity) noexcept {
    if(output) *output=nullptr;
    return Invoke(error,capacity,[&](radray::string& failure) {
        if(!runtime || runtime!=activeRuntime || !environment || !environment->IsOwnerThread() || runtime->renderer ||
           !desc || desc->struct_size!=sizeof(*desc) || desc->flags || !output) { failure="Invalid renderer descriptor or owner"; return false; }
        auto owned=radray::unique_ptr<rn_renderer>(new rn_renderer{runtime,{},{},{}});
        if(!raynium::Renderer::Create(*runtime->runtime,owned->renderer,failure)) return false;
        runtime->renderer=owned.release(); *output=runtime->renderer; return true;
    });
}
rn_status rn_destroy_renderer(rn_renderer* renderer,char* error,uint32_t capacity) noexcept {
    return Invoke(error,capacity,[&](radray::string& failure) {
        if(!renderer) return true;
        if(!Owner(renderer,failure)) return false;
        if(renderer->target || renderer->textures) { failure="Renderer still has live shared resources"; return false; }
        if(!renderer->renderer->Finish(failure)) return false;
        renderer->owner->renderer=nullptr; delete renderer; return true;
    });
}
rn_status rn_create_render_target(rn_renderer* renderer,const rn_render_target_desc* desc,rn_render_target** output,
    rn_shared_image_info* info,uint32_t infoCapacity,char* error,uint32_t capacity) noexcept {
    if(output) *output=nullptr;
    return Invoke(error,capacity,[&](radray::string& failure) {
        if(!Owner(renderer,failure)) return false;
        if(!desc || !output || !info || infoCapacity<sizeof(*info) || renderer->target) { failure="Invalid output target arguments"; return false; }
        auto owned=radray::unique_ptr<rn_render_target>(new rn_render_target{renderer,{}});
        if(!raynium::RenderTarget::Create(renderer->renderer->Runtime(),*desc,owned->target,failure)) return false;
        *info=owned->target->Info(); renderer->target=owned.release(); *output=renderer->target; return true;
    });
}
rn_status rn_destroy_render_target(rn_render_target* target,char* error,uint32_t capacity) noexcept {
    return Invoke(error,capacity,[&](radray::string& failure) {
        if(!target) return true;
        if(!Owner(target->owner,failure)) return false;
        if(!target->target->Finish(failure) || !target->owner->renderer->Finish(failure)) return false;
        target->owner->target=nullptr; delete target; return true;
    });
}
rn_status rn_create_shared_textures(rn_renderer* renderer,const rn_shared_textures_desc* desc,rn_shared_textures** output,
    rn_shared_image_export* images,uint32_t imageCapacity,char* error,uint32_t capacity) noexcept {
    if(output) *output=nullptr;
    return Invoke(error,capacity,[&](radray::string& failure) {
        if(!Owner(renderer,failure)) return false;
        if(!desc || desc->struct_size!=sizeof(*desc) || !desc->textures || !desc->texture_count || desc->texture_count>16 ||
           !output || !images || renderer->textures) { failure="Invalid shared texture arguments"; return false; }
        uint64_t count=0;
        for(uint32_t i=0;i<desc->texture_count;++i) count+=desc->textures[i].split_mips ? desc->textures[i].mip_levels : 1;
        if(count>imageCapacity) { failure="Shared image export capacity is too small"; return false; }
        if(!renderer->renderer->Finish(failure)) return false;
        auto owned=radray::unique_ptr<rn_shared_textures>(new rn_shared_textures{renderer,{}});
        if(!raynium::SharedTextures::Create(renderer->renderer->Runtime(),*desc,owned->textures,failure)) return false;
        owned->textures->DescribeExports(images);
        renderer->renderer->AttachTextures(owned->textures.get());
        renderer->textures=owned.release(); *output=renderer->textures; return true;
    });
}
rn_status rn_destroy_shared_textures(rn_shared_textures* textures,char* error,uint32_t capacity) noexcept {
    return Invoke(error,capacity,[&](radray::string& failure) {
        if(!textures) return true;
        if(!Owner(textures->owner,failure)) return false;
        if(!textures->textures->Finish(failure) || !textures->owner->renderer->Finish(failure)) return false;
        textures->owner->renderer->AttachTextures(nullptr);
        textures->owner->textures=nullptr; delete textures; return true;
    });
}
rn_status rn_render_frame(rn_renderer* renderer,rn_render_target* target,const rn_render_frame_desc* frame,char* error,uint32_t capacity) noexcept {
    return Invoke(error,capacity,[&](radray::string& failure) {
        if(!Owner(renderer,failure)) return false;
        if(!target || target!=renderer->target || !frame) { failure="Invalid frame or output owner"; return false; }
        return renderer->renderer->Render(*target->target,*frame,failure);
    });
}
