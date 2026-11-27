#include <raynium/bridge.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include <radray/logger.h>
#include <radray/profiler.h>
#include <radray/runtime/gpu_device.h>
#include <radray/runtime/gpu_env.h>
#include <radray/scope_guard.h>
#include <radray/types.h>
#include <raynium/build_info.h>
#include <raynium/device_info.h>
#include <raynium/exception.h>
#include <raynium/renderer.h>
#include <raynium/renderer_frontend.h>

#if defined(RADRAY_ENABLE_PROFILER) && (!defined(TRACY_DELAYED_INIT) || !defined(TRACY_MANUAL_LIFETIME))
#error "The unloadable bridge requires an explicitly managed Tracy lifetime"
#endif

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
static_assert(sizeof(rn_swapchain_desc) == 24 && offsetof(rn_swapchain_desc, hwnd) == 16);
static_assert(sizeof(rn_wait_result) == 48 && offsetof(rn_wait_result, submitted_sequence) == 8 && offsetof(rn_wait_result, completed_sequence) == 16 &&
              offsetof(rn_wait_result, presented_frames) == 24 && offsetof(rn_wait_result, surface_generation) == 32 &&
              offsetof(rn_wait_result, surface_recovery_state) == 36 && offsetof(rn_wait_result, last_present_outcome) == 40 && offsetof(rn_wait_result, flags) == 44);
static_assert(sizeof(rn_batch_telemetry) == 152 && offsetof(rn_batch_telemetry, flags) == 8 && offsetof(rn_batch_telemetry, reserved) == 12 &&
              offsetof(rn_batch_telemetry, gpu_ns) == 16 && offsetof(rn_batch_telemetry, wait_ns) == 24 && offsetof(rn_batch_telemetry, prepare_ns) == 32 &&
              offsetof(rn_batch_telemetry, rdg_ns) == 40 && offsetof(rn_batch_telemetry, present_ns) == 48 && offsetof(rn_batch_telemetry, metrics) == 56);
static_assert(sizeof(rn_capture_info) == 32 && offsetof(rn_capture_info, byte_size) == 16);
static_assert(sizeof(rn_device_info) == 600 && offsetof(rn_device_info, host_coherent_atom_size) == 592 && offsetof(rn_device_info, max_buffer_size) == 488 &&
              offsetof(rn_device_info, timestamp_period_ns) == 584 && offsetof(rn_device_info, name) == 40 && offsetof(rn_device_info, driver) == 360);
static_assert(sizeof(rn_texture_desc) == 32 && offsetof(rn_texture_desc, array_layers) == 20);
static_assert(sizeof(rn_texture_upload_desc) == 56 && offsetof(rn_texture_upload_desc, pixels) == 40 && offsetof(rn_texture_upload_desc, byte_size) == 48 && offsetof(rn_texture_upload_desc, texture) == 8);
static_assert(sizeof(rn_renderer_desc) == 24 && offsetof(rn_renderer_desc, shader_pack) == 8 && offsetof(rn_renderer_desc, shader_pack_size) == 16);
static_assert(sizeof(rn_terrain_mesh_layout) == 72 && alignof(rn_terrain_mesh_layout) == 8);
static_assert(offsetof(rn_terrain_mesh_layout, flags) == 28 && offsetof(rn_terrain_mesh_layout, minimum) == 32 &&
              offsetof(rn_terrain_mesh_layout, maximum) == 44 && offsetof(rn_terrain_mesh_layout, original_quad_groups) == 56 &&
              offsetof(rn_terrain_mesh_layout, original_quad_groups_bytes) == 64 && offsetof(rn_terrain_mesh_layout, reserved) == 68);
static_assert(sizeof(rn_index_update) == 24);
static_assert(sizeof(rn_mesh_update) == 32);
static_assert(offsetof(rn_mesh_update, vertex_count) == 16 && offsetof(rn_mesh_update, layer) == 20 && offsetof(rn_mesh_update, layout) == 24);
static_assert(sizeof(rn_visible_item) == 24);
static_assert(offsetof(rn_visible_item, mesh) == 0 && offsetof(rn_visible_item, element_mask) == 8);
static_assert(offsetof(rn_visible_item, sort_key) == 12 && offsetof(rn_visible_item, first_instance) == 16 && offsetof(rn_visible_item, instance_count) == 20);
static_assert(sizeof(rn_terrain_view_desc) == 344 && sizeof(rn_texture_sampling) == 16);
static_assert(offsetof(rn_terrain_view_desc, projection) == 104);
static_assert(offsetof(rn_terrain_view_desc, indices) == 24);
static_assert(offsetof(rn_terrain_view_desc, index_count) == 64);
static_assert(offsetof(rn_terrain_view_desc, camera_block) == 232);
static_assert(offsetof(rn_terrain_view_desc, fog_distances) == 272);
static_assert(offsetof(rn_terrain_view_desc, visible_version) == 288);
static_assert(offsetof(rn_terrain_view_desc, origins_version) == 296);
static_assert(offsetof(rn_terrain_view_desc, instances_version) == 304);
static_assert(offsetof(rn_terrain_view_desc, instance_slots) == 312 && offsetof(rn_terrain_view_desc, instance_capacity) == 320 &&
              offsetof(rn_terrain_view_desc, instance_base_version) == 328 &&
              offsetof(rn_terrain_view_desc, fade_now_ms) == 336 && offsetof(rn_terrain_view_desc, fade_duration_ms) == 340);
static_assert(sizeof(rn_instance) == 16 && offsetof(rn_instance, fade_start_ms) == 12);
static_assert(!std::is_default_constructible_v<raynium::Renderer>);

struct rn_runtime {
    radray::unique_ptr<radray::DeviceContext> Runtime;
    rn_renderer* Renderer{};
    rn_device_info Info{};
};

struct rn_renderer {
    rn_runtime* Owner{};
    radray::unique_ptr<raynium::Renderer> Renderer;
    rn_swapchain* Swapchain{};
    radray::unique_ptr<raynium::RendererFrontend> Frames;
    bool Wireframe{};
};

struct rn_swapchain {
    rn_renderer* Owner{};
    uint64_t Hwnd{};
    radray::unique_ptr<raynium::Swapchain> Swapchain;
};

namespace {
std::atomic_flag claimed = ATOMIC_FLAG_INIT;
thread_local raynium::RenderChannel* frontend = nullptr;
radray::unique_ptr<radray::Environment> environment;
rn_runtime* activeRuntime = nullptr;

template <class Action>
rn_status Invoke(std::string_view operation, Action&& action) noexcept {
    try {
        return action() ? RN_STATUS_OK : RN_STATUS_FAILED;
    } catch (...) {
        raynium::AbortUnexpectedException(operation);
    }
}

bool Owner(rn_renderer* renderer) {
    if (!environment || !activeRuntime || !renderer || activeRuntime->Renderer != renderer) {
        RADRAY_ERR_LOG("Invalid renderer owner or thread");
        return false;
    }
    return true;
}

template <class Action>
rn_status Barrier(std::string_view operation, Action&& action) noexcept {
    return Invoke(operation, [&] {
        if (!frontend) {
            RADRAY_ERR_LOG("{}: no render session on calling thread", operation);
            return false;
        }
        return frontend->Barrier(std::forward<Action>(action));
    });
}

bool FrontOwner(rn_renderer* renderer) {
    return frontend && activeRuntime && renderer && activeRuntime->Renderer == renderer;
}

bool SwapchainOwner(rn_swapchain* chain) {
    auto* renderer = activeRuntime ? activeRuntime->Renderer : nullptr;
    if (!Owner(renderer)) return false;
    if (!chain || renderer->Swapchain != chain) {
        RADRAY_ERR_LOG("Invalid swapchain owner");
        return false;
    }
    return true;
}
}  // namespace

uint32_t rn_get_abi_version(void) noexcept {
    return RN_ABI_VERSION;
}

rn_status rn_query_abi(rn_abi_info* info, uint32_t capacity) noexcept {
    return Invoke("rn_query_abi", [&] {
        if (!info) {
            RADRAY_ERR_LOG("rn_query_abi: info is null");
            return false;
        }
        if (capacity < sizeof(rn_abi_info)) {
            RADRAY_ERR_LOG("rn_query_abi: info capacity {} must be at least 240 bytes", capacity);
            return false;
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
        return true;
    });
}

rn_status rn_init_env(const rn_env_desc* desc) noexcept {
    return Invoke("rn_init_env", [&] {
        if (!desc || desc->struct_size != sizeof(rn_env_desc)) {
            RADRAY_ERR_LOG("Invalid rn_env_desc; expected 12 bytes");
            return false;
        }
        if (desc->flags & ~static_cast<uint32_t>(RN_ENV_VALIDATION)) {
            RADRAY_ERR_LOG("Invalid environment flags: {}", static_cast<uint32_t>(desc->flags));
            return false;
        }
        radray::gpu::GpuBackend backend;
        switch (desc->backend) {
            case RN_BACKEND_D3D12: backend = radray::gpu::GpuBackend::D3D12; break;
            case RN_BACKEND_VULKAN: backend = radray::gpu::GpuBackend::Vulkan; break;
            default:
                RADRAY_ERR_LOG("Unknown RHI backend {}; expected 1 (D3D12) or 2 (Vulkan)", static_cast<uint32_t>(desc->backend));
                return false;
        }
        if (claimed.test_and_set()) {
            RADRAY_ERR_LOG("Native environment is already initialized");
            return false;
        }
        auto releaseClaim = radray::MakeScopeGuard([]() noexcept {
            if (!frontend) claimed.clear();
        });
#ifdef RADRAY_ENABLE_PROFILER
        tracy::StartupProfiler();
        auto stopFailedProfiler = radray::MakeScopeGuard([]() noexcept {
            if (!frontend) tracy::ShutdownProfiler();
        });
#endif
        auto session = raynium::RenderChannel::Create();
        if (!session) return false;
        const bool started = session->Barrier([&] {
            RADRAY_PROFILE_THREAD("Raynium Render");
            RADRAY_PROFILE_SCOPE_N("Raynium.Environment.Create");
            environment = radray::Environment::Create(backend, (desc->flags & RN_ENV_VALIDATION) != 0, "Raynium").Release();
            return static_cast<bool>(environment);
        });
        if (started) frontend = session.release();
        return started;
    });
}

rn_status rn_destroy_env(void) noexcept {
    return Invoke("rn_destroy_env", [&] {
        if (!frontend) {
            if (!claimed.test()) return true;
            RADRAY_ERR_LOG("Native environment belongs to another thread");
            return false;
        }
        if (activeRuntime) {
            RADRAY_ERR_LOG("Destroy the active runtime before destroying the environment");
            return false;
        }
        radray::unique_ptr<raynium::RenderChannel> session(std::exchange(frontend, nullptr));
        session->Barrier([] {
            RADRAY_PROFILE_SCOPE_N("Raynium.Environment.Destroy");
            environment.reset();
            return true;
        });
        session.reset();
#ifdef RADRAY_ENABLE_PROFILER
        tracy::ShutdownProfiler();
#endif
        claimed.clear();
        return true;
    });
}

rn_status rn_create_runtime(const rn_runtime_desc* desc, rn_runtime** out_runtime) noexcept {
    if (out_runtime) *out_runtime = nullptr;
    return Barrier("rn_create_runtime", [&] {
        if (!desc) {
            RADRAY_ERR_LOG("rn_create_runtime: desc is null");
            return false;
        }
        if (!out_runtime) {
            RADRAY_ERR_LOG("rn_create_runtime: out_runtime is null");
            return false;
        }
        if (desc->struct_size != sizeof(rn_runtime_desc)) {
            RADRAY_ERR_LOG("Invalid rn_runtime_desc struct_size {}; expected 8 bytes", desc->struct_size);
            return false;
        }
        if (desc->reserved) {
            RADRAY_ERR_LOG("Invalid runtime reserved field: {}", desc->reserved);
            return false;
        }
        if (!environment) {
            RADRAY_ERR_LOG("Native environment is not initialized on this thread");
            return false;
        }
        if (activeRuntime) {
            RADRAY_ERR_LOG("Native environment already has an active runtime");
            return false;
        }
        RADRAY_PROFILE_SCOPE_N("Raynium.Runtime.Create");
        auto runtime = radray::DeviceContext::Create(*environment).Release();
        if (!runtime) return false;
        auto owned = radray::unique_ptr<rn_runtime>(new rn_runtime{std::move(runtime)});
        raynium::QueryDeviceInfo(*owned->Runtime, owned->Info);
        *out_runtime = owned.release();
        activeRuntime = *out_runtime;
        return true;
    });
}

rn_status rn_destroy_runtime(rn_runtime* runtime) noexcept {
    if (!runtime) return RN_STATUS_OK;
    return Barrier("rn_destroy_runtime", [&] {
        if (runtime != activeRuntime || !environment || runtime->Renderer) {
            RADRAY_ERR_LOG("Cannot destroy runtime: wrong owner/thread or live renderer");
            return false;
        }
        RADRAY_PROFILE_SCOPE_N("Raynium.Runtime.Destroy");
        delete runtime;
        activeRuntime = nullptr;
        return true;
    });
}

rn_status rn_create_renderer(rn_runtime* runtime, const rn_renderer_desc* desc, rn_renderer** output) noexcept {
    if (output) *output = nullptr;
    auto* session = frontend;
    return Barrier("rn_create_renderer", [&] {
        if (!runtime || runtime != activeRuntime || !environment || runtime->Renderer ||
            !desc || desc->struct_size != sizeof(*desc) || !desc->shader_pack ||
            !desc->shader_pack_size || desc->shader_pack_size > raynium::kShaderPackMaxSize || !output) {
            RADRAY_ERR_LOG("Invalid renderer descriptor or owner/shader pack");
            return false;
        }
        if (desc->flags > RN_RENDERER_INDIRECT) {
            RADRAY_ERR_LOG("Unknown or mutually exclusive terrain submission flags: {}", desc->flags);
            return false;
        }
        auto owned = radray::unique_ptr<rn_renderer>(new rn_renderer{runtime, {}, {}});
        const std::span shaders{static_cast<const radray::byte*>(desc->shader_pack), size_t(desc->shader_pack_size)};
        if (!raynium::Renderer::Create(*runtime->Runtime, shaders, desc->flags, owned->Renderer)) return false;
        owned->Frames = radray::make_unique<raynium::RendererFrontend>(owned.get(), *session, *owned->Renderer);
        owned->Wireframe = owned->Renderer->SupportsWireframe();
        runtime->Renderer = owned.release();
        *output = runtime->Renderer;
        return true;
    });
}

rn_status rn_destroy_renderer(rn_renderer* renderer) noexcept {
    if (!renderer) return RN_STATUS_OK;
    return Barrier("rn_destroy_renderer", [&] {
        if (!Owner(renderer)) return false;
        if (renderer->Swapchain) {
            RADRAY_ERR_LOG("Renderer still has live output resources");
            return false;
        }
        renderer->Renderer->Finish();
        renderer->Owner->Renderer = nullptr;
        delete renderer;
        return true;
    });
}

rn_status rn_query_device_info(rn_runtime* runtime, rn_device_info* info, uint32_t capacity) noexcept {
    return Invoke("rn_query_device_info", [&] {
        if (!frontend || !runtime || runtime != activeRuntime || !info || capacity < sizeof(*info)) {
            RADRAY_ERR_LOG("Invalid device info owner, thread or capacity: {}", capacity);
            return false;
        }
        auto value = runtime->Info;
        if (runtime->Renderer && runtime->Renderer->Wireframe) value.flags |= RN_DEVICE_WIREFRAME;
        *info = value;
        return true;
    });
}

rn_status rn_create_swapchain(rn_renderer* renderer, const rn_swapchain_desc* desc, rn_swapchain** output) noexcept {
    if (output) *output = nullptr;
    return Barrier("rn_create_swapchain", [&] {
        if (!Owner(renderer)) return false;
        if (!renderer->Frames->Open() || !desc || !output || renderer->Swapchain) {
            RADRAY_ERR_LOG("Invalid swapchain output or existing swapchain");
            return false;
        }
        auto owned = radray::unique_ptr<rn_swapchain>(new rn_swapchain{renderer, desc->hwnd, {}});
        if (!raynium::Swapchain::Create(renderer->Renderer->Runtime(), *desc, owned->Swapchain)) {
            if (renderer->Renderer->IsTerminal()) renderer->Frames->DeviceFailure();
            return false;
        }
        renderer->Frames->SurfaceConfigured();
        renderer->Swapchain = owned.release();
        *output = renderer->Swapchain;
        return true;
    });
}

rn_status rn_resize_swapchain(rn_swapchain* chain, const rn_swapchain_desc* desc) noexcept {
    return Barrier("rn_resize_swapchain", [&] {
        if (!SwapchainOwner(chain)) return false;
        if (!chain->Owner->Frames->Open()) return false;
        if (!desc) {
            RADRAY_ERR_LOG("Swapchain resize descriptor is null");
            return false;
        }
        chain->Owner->Renderer->Finish();
        chain->Owner->Renderer->Runtime().GetQueue().Wait();
        auto& frames = *chain->Owner->Frames;
        if (chain->Owner->Renderer->IsTerminal()) {
            frames.DeviceFailure();
            return false;
        }
        if (desc->hwnd != chain->Hwnd || !raynium::ValidSwapchainDesc(*desc)) return false;
        if (!chain->Swapchain->Resize(*desc)) {
            if (chain->Owner->Renderer->IsTerminal())
                frames.DeviceFailure();
            else
                frames.SurfaceLost();
            return false;
        }
        frames.SurfaceConfigured();
        return true;
    });
}

rn_status rn_destroy_swapchain(rn_swapchain* chain) noexcept {
    if (!chain) return RN_STATUS_OK;
    return Barrier("rn_destroy_swapchain", [&] {
        if (!SwapchainOwner(chain)) return false;
        chain->Owner->Renderer->Finish();
        // Present is enqueued after the frame completion signal on the same queue.
        chain->Owner->Renderer->Runtime().GetQueue().Wait();
        chain->Owner->Swapchain = nullptr;
        delete chain;
        return true;
    });
}

rn_status rn_create_texture(rn_renderer* renderer, const rn_texture_desc* desc, rn_texture** out) noexcept {
    if (out) *out = nullptr;
    return Invoke("rn_create_texture", [&] {
        return FrontOwner(renderer) && desc && out && renderer->Frames->CreateTexture(*desc, *out);
    });
}

rn_status rn_upload_texture(rn_renderer* renderer, const rn_texture_upload_desc* desc) noexcept {
    return Invoke("rn_upload_texture", [&] {
        return FrontOwner(renderer) && desc && renderer->Frames->UploadTexture(*desc);
    });
}

rn_status rn_destroy_texture(rn_renderer* renderer, rn_texture* id) noexcept {
    return Invoke("rn_destroy_texture", [&] {
        return FrontOwner(renderer) && renderer->Frames->RemoveTexture(id);
    });
}

rn_status rn_upsert_model_mesh(
    rn_renderer* renderer,
    int32_t mesh_id,
    const void* vertex_data,
    uint32_t vertex_bytes,
    uint32_t vertex_count,
    uint32_t index_count,
    uint32_t* out_page_id) noexcept {
    using Registry = raynium::StaticGeometryRegistry;
    if (!FrontOwner(renderer) || !out_page_id || !vertex_data || mesh_id < Registry::kFirstUserId || !vertex_count ||
        vertex_count % 4 || vertex_count > Registry::kMaxModelVertices ||
        vertex_bytes != uint64_t(vertex_count) * Registry::kModelVertexStride ||
        index_count != (vertex_count / 4) * 6) {
        RADRAY_ERR_LOG("Invalid rigid model mesh upsert: id={} bytes={} vertices={} indices={}", mesh_id, vertex_bytes, vertex_count, index_count);
        return RN_STATUS_FAILED;
    }
    return Barrier("rn_upsert_model_mesh", [&] {
        if (!renderer->Frames->Open()) return false;
        renderer->Frames->ApplyModelRemovals();
        if (renderer->Renderer->UpsertModelMesh(mesh_id, {static_cast<const radray::byte*>(vertex_data), vertex_bytes}, vertex_count, index_count, *out_page_id)) {
            return true;
        }
        if (renderer->Renderer->IsTerminal()) renderer->Frames->DeviceFailure();
        return false;
    });
}

rn_status rn_remove_model_mesh(rn_renderer* renderer, int32_t mesh_id) noexcept {
    return Invoke("rn_remove_model_mesh", [&] {
        if (!FrontOwner(renderer) || mesh_id < 16) return false;
        renderer->Frames->RemoveModelMesh(mesh_id);
        return true;
    });
}

rn_status rn_create_query_pool(rn_renderer* renderer, uint32_t count, rn_query_pool** id) noexcept {
    if (id) *id = nullptr;
    return Invoke("rn_create_query_pool", [&] {
        return FrontOwner(renderer) && id && renderer->Frames->CreateQueryPool(count, *id);
    });
}

rn_status rn_destroy_query_pool(rn_renderer* renderer, rn_query_pool* id) noexcept {
    return Invoke("rn_destroy_query_pool", [&] {
        return FrontOwner(renderer) && renderer->Frames->RemoveQueryPool(id);
    });
}

rn_status rn_sync_query_pool(
    rn_renderer* renderer,
    rn_query_pool* id,
    uint32_t first,
    uint32_t count,
    rn_query_sample* samples,
    rn_wait_result* result) noexcept {
    return Invoke("rn_sync_query_pool", [&] {
        if (!result) return false;
        *result = {};
        return FrontOwner(renderer) && (!count || samples) && renderer->Frames->Query(id, first, {samples, count}, *result);
    });
}

rn_status rn_register_pipeline(rn_renderer* renderer, const rn_pipeline_desc* desc, rn_pipeline** id) noexcept {
    if (id) *id = nullptr;
    return Invoke("rn_register_pipeline", [&] {
        return FrontOwner(renderer) && desc && id && renderer->Frames->RegisterPipeline(*desc, *id);
    });
}

rn_status rn_destroy_pipeline(rn_renderer* renderer, rn_pipeline* id) noexcept {
    return Invoke("rn_destroy_pipeline", [&] {
        return FrontOwner(renderer) && renderer->Frames->RemovePipeline(id);
    });
}

rn_status rn_create_buffer(rn_renderer* renderer, const rn_buffer_desc* desc, rn_buffer_result* result) noexcept {
    return Invoke("rn_create_buffer", [&] {
        if (!FrontOwner(renderer) || !desc || !result) return false;
        *result = {};
        return renderer->Frames->CreateBuffer(*desc, *result);
    });
}

rn_status rn_destroy_buffer(rn_renderer* renderer, rn_buffer* id) noexcept {
    return Invoke("rn_destroy_buffer", [&] {
        return FrontOwner(renderer) && renderer->Frames->RemoveBuffer(id);
    });
}

static_assert(sizeof(rn_buffer_desc) == 24 && sizeof(rn_buffer_result) == 16 && sizeof(rn_mapped_range) == 24);
static_assert(sizeof(rn_batch_desc) == 128 && sizeof(rn_submit_result) == 56 && offsetof(rn_submit_result, progress) == 8);
static_assert(offsetof(rn_batch_desc, instance_uploads) == 48 && offsetof(rn_batch_desc, dirty_ranges) == 64 && offsetof(rn_batch_desc, views) == 80);

rn_status rn_submit_frame(rn_renderer* renderer, const rn_batch_desc* batch, rn_submit_result* result) noexcept {
    return Invoke("rn_submit_frame", [&] {
        if (!result) return false;
        *result = {};
        if (!FrontOwner(renderer)) return false;
        if (!batch || batch->struct_size != sizeof(*batch) || (batch->surface && batch->surface != renderer->Swapchain)) {
            renderer->Frames->Snapshot(result->progress);
            return false;
        }
        return renderer->Frames->Submit(*batch, batch->surface ? batch->surface->Swapchain.get() : nullptr, *result);
    });
}

rn_status rn_flush_renderer(rn_renderer* renderer, rn_wait_result* result) noexcept {
    return Invoke("rn_flush_renderer", [&] {
        return result && FrontOwner(renderer) && renderer->Frames->Flush(*result);
    });
}

rn_status rn_drain_telemetry(rn_renderer* renderer, rn_batch_telemetry* reports, uint32_t capacity, uint32_t* count) noexcept {
    return Invoke("rn_drain_telemetry", [&] {
        if (!count || (capacity && !reports) || !FrontOwner(renderer)) return false;
        *count = renderer->Frames->Drain({reports, capacity});
        return true;
    });
}

rn_status rn_wait_submit(rn_renderer* renderer, uint64_t sequence, int64_t timeout_ns, rn_wait_result* result) noexcept {
    return Invoke("rn_wait_submit", [&] {
        if (!result) {
            RADRAY_ERR_LOG("Missing submission wait result");
            return false;
        }
        *result = {};
        return FrontOwner(renderer) && renderer->Frames->Wait(sequence, timeout_ns, *result);
    });
}

rn_status rn_poll_capture(rn_renderer* renderer, uint64_t sequence, rn_capture_info* info, void* pixels, uint64_t capacity) noexcept {
    return Invoke("rn_poll_capture", [&] {
        if (!FrontOwner(renderer)) return false;
        if (!info || info->struct_size != sizeof(*info) || info->reserved || (!pixels && capacity)) {
            RADRAY_ERR_LOG("Invalid capture result layout or destination");
            return false;
        }
        rn_capture_info result{};
        if (!renderer->Frames->PollCapture(sequence, result, pixels, capacity)) return false;
        *info = result;
        return true;
    });
}
