#include <raynium/bridge.h>
#include <radray/types.h>
#include <raynium/build_info.h>
#include <radray/runtime/gpu_device.h>
#include <radray/runtime/gpu_env.h>
#include <raynium/renderer.h>
#include <raynium/exception.h>
#include <radray/logger.h>
#include <radray/profiler.h>
#include <radray/scope_guard.h>

#if defined(RADRAY_ENABLE_PROFILER) && (!defined(TRACY_DELAYED_INIT) || !defined(TRACY_MANUAL_LIFETIME))
#error "The unloadable bridge requires an explicitly managed Tracy lifetime"
#endif
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <utility>
#include <string_view>
#include <span>

#if defined(_DLL) || !defined(RADRAY_ENABLE_MIMALLOC)
#error "Raynium runtime requires static CRT and mimalloc"
#endif
#if defined(RADRAY_IS_DEBUG)
static_assert(_ITERATOR_DEBUG_LEVEL == 2, "Debug requires the /MTd Debug CRT and STL iterator checks");
#else
static_assert(_ITERATOR_DEBUG_LEVEL == 0, "Release requires the /MT release CRT");
#endif

static_assert(std::is_same_v<std::underlying_type_t<rn_status>, uint32_t>);
static_assert(std::is_same_v<std::underlying_type_t<rn_backend>, uint32_t>);
static_assert(std::is_same_v<std::underlying_type_t<rn_env_flags>, uint32_t>);

static_assert(sizeof(rn_abi_info) == 240);
static_assert(offsetof(rn_abi_info, build_id) == 16);
static_assert(offsetof(rn_abi_info, toolchain) == 176);
static_assert(sizeof(RAYNIUM_BUILD_ID) <= sizeof(rn_abi_info::build_id));
static_assert(sizeof(RAYNIUM_RADRAY_REVISION) <= sizeof(rn_abi_info::radray_commit));
static_assert(sizeof(RAYNIUM_CONFIGURATION) <= sizeof(rn_abi_info::configuration));
static_assert(sizeof(RAYNIUM_TOOLCHAIN) <= sizeof(rn_abi_info::toolchain));
static_assert(sizeof(rn_env_desc) == 12);
static_assert(alignof(rn_env_desc) == 4);
static_assert(sizeof(rn_runtime_desc) == 8);
static_assert(alignof(rn_runtime_desc) == 4);
static_assert(!std::is_default_constructible_v<radray::DeviceContext>);
static_assert(!std::is_default_constructible_v<radray::Environment>);
static_assert(sizeof(rn_swapchain_desc)==24 && offsetof(rn_swapchain_desc,hwnd)==16);
static_assert(sizeof(rn_present_result)==16 && offsetof(rn_present_result,acquire_timeouts)==8);
static_assert(sizeof(rn_device_info)==488 && offsetof(rn_device_info,name)==40 && offsetof(rn_device_info,driver)==360);
static_assert(sizeof(rn_uploaded_texture_desc)==24);
static_assert(sizeof(rn_texture_upload_desc)==48 && offsetof(rn_texture_upload_desc,pixels)==32 && offsetof(rn_texture_upload_desc,byte_size)==40);
static_assert(sizeof(rn_renderer_desc)==24 && offsetof(rn_renderer_desc,shader_pack)==8 && offsetof(rn_renderer_desc,shader_pack_size)==16);
static_assert(sizeof(rn_index_update)==24);
static_assert(sizeof(rn_mesh_update)==24);
static_assert(offsetof(rn_mesh_update,vertex_count)==16 && offsetof(rn_mesh_update,layer)==20);
static_assert(sizeof(rn_visible_item)==24);
static_assert(offsetof(rn_visible_item,mesh)==0 && offsetof(rn_visible_item,element_mask)==8);
static_assert(offsetof(rn_visible_item,sort_key)==12 && offsetof(rn_visible_item,first_instance)==16 && offsetof(rn_visible_item,instance_count)==20);
static_assert(sizeof(rn_render_frame_desc)==328);
static_assert(sizeof(rn_gui_vertex)==24 && sizeof(rn_gui_draw)==40);
static_assert(offsetof(rn_render_frame_desc,gui_vertices)==280 && offsetof(rn_render_frame_desc,gui_draws)==288);
static_assert(offsetof(rn_render_frame_desc,gui_vertex_count)==296 && offsetof(rn_render_frame_desc,clear_color)==312);
static_assert(offsetof(rn_render_frame_desc,projection)==96);
static_assert(offsetof(rn_render_frame_desc,indices)==24);
static_assert(offsetof(rn_render_frame_desc,index_count)==64);
static_assert(offsetof(rn_render_frame_desc,camera_block)==224);
static_assert(offsetof(rn_render_frame_desc,fog_distances)==264);
static_assert(sizeof(rn_instance)==16);
static_assert(!std::is_default_constructible_v<raynium::Renderer>);

struct rn_runtime {
    radray::unique_ptr<radray::DeviceContext> runtime;
    rn_renderer* renderer{};
};
struct rn_renderer {
    rn_runtime* owner;
    radray::unique_ptr<raynium::Renderer> renderer;
    rn_swapchain* swapchain{};
};
struct rn_swapchain { rn_renderer* owner; radray::unique_ptr<raynium::Swapchain> swapchain; };

namespace {

// Access is serialized by the caller. The environment outlives its active runtime.
radray::unique_ptr<radray::Environment> environment;
rn_runtime* activeRuntime = nullptr;

template<class Action> rn_status Invoke(std::string_view operation,Action&& action) noexcept {
    try {
        return action()?RN_STATUS_OK:RN_STATUS_FAILED;
    } catch(...) {
        raynium::AbortUnexpectedException(operation);
    }
}
bool Owner(rn_renderer* renderer) {
    if(!environment || !environment->IsOwnerThread() || !activeRuntime || !renderer || activeRuntime->renderer!=renderer) {
        RADRAY_ERR_LOG("Invalid renderer owner or thread"); return false;
    }
    return true;
}
bool SwapchainOwner(rn_swapchain* chain) {
    auto* renderer=activeRuntime?activeRuntime->renderer:nullptr;
    if(!Owner(renderer)) return false;
    if(!chain || renderer->swapchain!=chain) {
        RADRAY_ERR_LOG("Invalid swapchain owner"); return false;
    }
    return true;
}
}  // namespace

uint32_t rn_get_abi_version(void) noexcept {
    return RN_ABI_VERSION;
}

rn_status rn_query_abi(rn_abi_info* info,uint32_t capacity) noexcept {
    return Invoke("rn_query_abi",[&] {
        if(!info) { RADRAY_ERR_LOG("rn_query_abi: info is null"); return false; }
        if(capacity<sizeof(rn_abi_info)) {
            RADRAY_ERR_LOG("rn_query_abi: info capacity {} must be at least 240 bytes",capacity); return false;
        }
        *info={};
        info->struct_size=sizeof(rn_abi_info);
        info->abi_version=rn_get_abi_version();
        info->pointer_size=sizeof(void*);
        info->flags=6u | RAYNIUM_RADRAY_DIRTY;
        std::memcpy(info->build_id,RAYNIUM_BUILD_ID,sizeof(RAYNIUM_BUILD_ID));
        std::memcpy(info->radray_commit,RAYNIUM_RADRAY_REVISION,sizeof(RAYNIUM_RADRAY_REVISION));
        std::memcpy(info->configuration,RAYNIUM_CONFIGURATION,sizeof(RAYNIUM_CONFIGURATION));
        std::memcpy(info->platform,"windows-x86_64",sizeof("windows-x86_64"));
        std::memcpy(info->toolchain,RAYNIUM_TOOLCHAIN,sizeof(RAYNIUM_TOOLCHAIN));
        return true;
    });
}

rn_status rn_init_env(const rn_env_desc* desc) noexcept {
    return Invoke("rn_init_env",[&] {
        if(!desc || desc->struct_size!=sizeof(rn_env_desc)) {
            RADRAY_ERR_LOG("Invalid rn_env_desc; expected 12 bytes"); return false;
        }
        if(desc->flags & ~static_cast<uint32_t>(RN_ENV_VALIDATION)) {
            RADRAY_ERR_LOG("Invalid environment flags: {}",static_cast<uint32_t>(desc->flags)); return false;
        }
        radray::render::RenderBackend backend;
        switch(desc->backend) {
            case RN_BACKEND_D3D12: backend=radray::render::RenderBackend::D3D12; break;
            case RN_BACKEND_VULKAN: backend=radray::render::RenderBackend::Vulkan; break;
            default:
                RADRAY_ERR_LOG("Unknown RHI backend {}; expected 1 (D3D12) or 2 (Vulkan)",static_cast<uint32_t>(desc->backend)); return false;
        }
        if(environment) { RADRAY_ERR_LOG("Native environment is already initialized"); return false; }
#ifdef RADRAY_ENABLE_PROFILER
        tracy::StartupProfiler();
        auto stopFailedProfiler=radray::MakeScopeGuard([]() noexcept { if(!environment) tracy::ShutdownProfiler(); });
#endif
        RADRAY_PROFILE_SCOPE_N("Raynium.Environment.Create");
        environment=radray::Environment::Create(backend,(desc->flags & RN_ENV_VALIDATION)!=0,"Raynium").Release();
        return static_cast<bool>(environment);
    });
}

rn_status rn_destroy_env(void) noexcept {
    return Invoke("rn_destroy_env",[&] {
        if(!environment) return true;
        if(!environment->IsOwnerThread()) { RADRAY_ERR_LOG("Native environment belongs to another thread"); return false; }
        if(activeRuntime) { RADRAY_ERR_LOG("Destroy the active runtime before destroying the environment"); return false; }
#ifdef RADRAY_ENABLE_PROFILER
        // Join profiler workers before FFM unloads the DLL under the Windows loader lock.
        auto stopProfiler=radray::MakeScopeGuard([]() noexcept { tracy::ShutdownProfiler(); });
#endif
        RADRAY_PROFILE_SCOPE_N("Raynium.Environment.Destroy");
        environment.reset(); return true;
    });
}

rn_status rn_create_runtime(const rn_runtime_desc* desc,rn_runtime** out_runtime) noexcept {
    if(out_runtime) *out_runtime=nullptr;
    return Invoke("rn_create_runtime",[&] {
        if(!desc) { RADRAY_ERR_LOG("rn_create_runtime: desc is null"); return false; }
        if(!out_runtime) { RADRAY_ERR_LOG("rn_create_runtime: out_runtime is null"); return false; }
        if(desc->struct_size!=sizeof(rn_runtime_desc)) {
            RADRAY_ERR_LOG("Invalid rn_runtime_desc struct_size {}; expected 8 bytes",desc->struct_size); return false;
        }
        if(desc->reserved) { RADRAY_ERR_LOG("Invalid runtime reserved field: {}",desc->reserved); return false; }
        if(!environment || !environment->IsOwnerThread()) {
            RADRAY_ERR_LOG("Native environment is not initialized on this thread"); return false;
        }
        if(activeRuntime) { RADRAY_ERR_LOG("Native environment already has an active runtime"); return false; }
        RADRAY_PROFILE_SCOPE_N("Raynium.Runtime.Create");
        auto runtime=radray::DeviceContext::Create(*environment).Release();
        if(!runtime) return false;
        auto owned=radray::unique_ptr<rn_runtime>(new rn_runtime{std::move(runtime)});
        *out_runtime=owned.release(); activeRuntime=*out_runtime; return true;
    });
}

void rn_destroy_runtime(rn_runtime* runtime) noexcept {
    Invoke("rn_destroy_runtime",[&] {
        if(!runtime) return true;
        if(runtime!=activeRuntime || !environment || !environment->IsOwnerThread() || runtime->renderer)
            RADRAY_ABORT("Cannot destroy runtime: wrong owner/thread or live renderer");
        RADRAY_PROFILE_SCOPE_N("Raynium.Runtime.Destroy");
        delete runtime; activeRuntime=nullptr; return true;
    });
}

rn_status rn_create_renderer(rn_runtime* runtime,const rn_renderer_desc* desc,rn_renderer** output) noexcept {
    if(output) *output=nullptr;
    return Invoke("rn_create_renderer",[&] {
        if(!runtime || runtime!=activeRuntime || !environment || !environment->IsOwnerThread() || runtime->renderer ||
           !desc || desc->struct_size!=sizeof(*desc) || !desc->shader_pack ||
           !desc->shader_pack_size || desc->shader_pack_size>raynium::ShaderPackMaxSize || !output) {
            RADRAY_ERR_LOG("Invalid renderer descriptor or owner/shader pack"); return false;
        }
        if(desc->flags>RN_RENDERER_INDIRECT) {
            RADRAY_ERR_LOG("Unknown or mutually exclusive terrain submission flags: {}",desc->flags); return false;
        }
        auto owned=radray::unique_ptr<rn_renderer>(new rn_renderer{runtime,{},{}});
        const std::span shaders{static_cast<const radray::byte*>(desc->shader_pack),size_t(desc->shader_pack_size)};
        if(!raynium::Renderer::Create(*runtime->runtime,shaders,desc->flags,owned->renderer)) return false;
        runtime->renderer=owned.release(); *output=runtime->renderer; return true;
    });
}
rn_status rn_destroy_renderer(rn_renderer* renderer) noexcept {
    return Invoke("rn_destroy_renderer",[&] {
        if(!renderer) return true;
        if(!Owner(renderer)) return false;
        if(renderer->swapchain) { RADRAY_ERR_LOG("Renderer still has live output resources"); return false; }
        renderer->renderer->Finish();
        renderer->owner->renderer=nullptr; delete renderer; return true;
    });
}
rn_status rn_query_device_info(rn_runtime* runtime,rn_device_info* info,uint32_t capacity) noexcept {
    return Invoke("rn_query_device_info",[&] {
        if(!runtime || runtime!=activeRuntime || !environment || !environment->IsOwnerThread() || !info || capacity<sizeof(*info)) {
            RADRAY_ERR_LOG("Invalid device info owner, thread or capacity: {}",capacity); return false;
        }
        rn_device_info value{};
        raynium::QueryDeviceInfo(*runtime->runtime,value);
        *info=value; return true;
    });
}
rn_status rn_create_swapchain(rn_renderer* renderer,const rn_swapchain_desc* desc,rn_swapchain** output) noexcept {
    if(output) *output=nullptr;
    return Invoke("rn_create_swapchain",[&] {
        if(!Owner(renderer)) return false;
        if(!desc || !output || renderer->swapchain) { RADRAY_ERR_LOG("Invalid swapchain output or existing swapchain"); return false; }
        if(!renderer->renderer->PrepareOutput(radray::render::TextureFormat::BGRA8_UNORM)) return false;
        auto owned=radray::unique_ptr<rn_swapchain>(new rn_swapchain{renderer,{}});
        if(!raynium::Swapchain::Create(renderer->renderer->Runtime(),*desc,owned->swapchain)) return false;
        renderer->swapchain=owned.release(); *output=renderer->swapchain; return true;
    });
}
rn_status rn_resize_swapchain(rn_swapchain* chain,const rn_swapchain_desc* desc) noexcept {
    return Invoke("rn_resize_swapchain",[&] {
        if(!SwapchainOwner(chain)) return false;
        if(!desc) { RADRAY_ERR_LOG("Swapchain resize descriptor is null"); return false; }
        chain->owner->renderer->Finish();
        chain->owner->renderer->Runtime().GetQueue().Wait();
        return chain->swapchain->Resize(*desc);
    });
}
rn_status rn_destroy_swapchain(rn_swapchain* chain) noexcept {
    return Invoke("rn_destroy_swapchain",[&] {
        if(!chain) return true;
        if(!SwapchainOwner(chain)) return false;
        chain->owner->renderer->Finish();
        // Present is enqueued after the frame completion signal on the same queue.
        chain->owner->renderer->Runtime().GetQueue().Wait();
        chain->owner->swapchain=nullptr; delete chain; return true;
    });
}
rn_status rn_present_frame(rn_renderer* renderer,rn_swapchain* chain,const rn_render_frame_desc* frame,rn_present_result* result) noexcept {
    return Invoke("rn_present_frame",[&] {
        if(!Owner(renderer)) return false;
        if(!chain || chain!=renderer->swapchain || !frame || !result || result->struct_size!=sizeof(*result)) {
            RADRAY_ERR_LOG("Invalid present frame, result layout or swapchain owner"); return false;
        }
        rn_present_result value{};
        if(!renderer->renderer->Present(*chain->swapchain,*frame,value)) return false;
        *result=value; return true;
    });
}
rn_status rn_create_texture(rn_renderer* renderer,const rn_uploaded_texture_desc* desc) noexcept {
    return Invoke("rn_create_texture",[&] {
        if(!Owner(renderer)) return false;
        if(!desc) { RADRAY_ERR_LOG("Texture descriptor is null"); return false; }
        return renderer->renderer->CreateTexture(*desc);
    });
}
rn_status rn_upload_texture(rn_renderer* renderer,const rn_texture_upload_desc* desc) noexcept {
    return Invoke("rn_upload_texture",[&] {
        if(!Owner(renderer)) return false;
        if(!desc) { RADRAY_ERR_LOG("Upload descriptor is null"); return false; }
        return renderer->renderer->UploadTexture(*desc);
    });
}
rn_status rn_destroy_texture(rn_renderer* renderer,uint32_t id) noexcept {
    return Invoke("rn_destroy_texture",[&] { return Owner(renderer) && renderer->renderer->DestroyTexture(id); });
}
