#pragma once

#include <stdint.h>

#if defined(RAYNIUM_RUNTIME_EXPORTS)
#define RAYNIUM_API __declspec(dllexport)
#else
#define RAYNIUM_API __declspec(dllimport)
#endif
#define RN_ABI_VERSION 18u

struct rn_runtime;
struct rn_renderer;
struct rn_swapchain;

// Fixed-width unscoped enums preserve the Java FFM integer layout.
enum rn_status : uint32_t {
    RN_STATUS_OK = 0,
    RN_STATUS_FAILED = 1
};

enum rn_backend : uint32_t {
    RN_BACKEND_D3D12 = 1,
    RN_BACKEND_VULKAN = 2
};


enum rn_env_flags : uint32_t {
    RN_ENV_VALIDATION = 1
};


// Strings are UTF-8, zero-terminated and stored inline; no allocation crosses this ABI.
struct rn_abi_info {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t pointer_size;
    uint32_t flags; // bit 0: dirty RadRay; bit 1: static CRT; bit 2: mimalloc enabled.
    char build_id[72];
    char radray_commit[48];
    char configuration[16];
    char platform[24];
    char toolchain[64];
};

struct rn_env_desc {
    uint32_t struct_size;
    rn_backend backend;
    rn_env_flags flags;
};

struct rn_runtime_desc { uint32_t struct_size, reserved; };


enum rn_frame_flags : uint32_t { RN_FRAME_RGSS = 1, RN_FRAME_PROFILE = 2, RN_FRAME_IMPROVED_TRANSPARENCY = 4,
    RN_FRAME_FLIP_Y = 8, RN_FRAME_CAPTURE = 16, RN_FRAME_CAPTURE_PREVIOUS = 32, RN_FRAME_WORLD_ONLY = 64, RN_FRAME_WIREFRAME = 128, RN_FRAME_MEASURE = 256 }; // Reflect terrain clip Y and its front-face winding together.
// Zero selects auto. Forced direct and indirect are mutually exclusive.
enum rn_renderer_flags : uint32_t { RN_RENDERER_DIRECT = 1, RN_RENDERER_INDIRECT = 2 };
// Pack bytes are borrowed only during rn_create_renderer; no pointer is retained.
struct rn_renderer_desc { uint32_t struct_size, flags; const void* shader_pack; uint64_t shader_pack_size; };
enum rn_present_mode : uint32_t { RN_PRESENT_IMMEDIATE = 0, RN_PRESENT_FIFO = 1 };
struct rn_swapchain_desc { uint32_t struct_size, width, height; rn_present_mode present_mode; uint64_t hwnd; };
enum rn_present_outcome : uint32_t { RN_PRESENTED = 0, RN_PRESENT_SKIPPED = 1, RN_PRESENT_RECREATE = 2 };
// Opt-in measurement: completed GPU sequence/time, current wait/prepare/RDG/present wall times, all nanoseconds.
struct rn_frame_metrics {
    uint64_t terrain_upload_bytes{},texture_upload_bytes{},scene_upload_bytes{},gui_upload_bytes{};
    uint64_t terrain_records{},terrain_calls{},scene_calls{},gui_calls{},bind_calls{};
    uint64_t vertex_capacity{},index_capacity{},geometry_allocated{};
};
struct rn_present_result { uint32_t struct_size; rn_present_outcome outcome; uint64_t acquire_timeouts; uint64_t gpu_sequence,gpu_ns,wait_ns,prepare_ns,rdg_ns,present_ns; rn_frame_metrics metrics; };
// Capture pixels are packed RGBA8, top row first. A successful non-null read consumes the capture.
struct rn_capture_info { uint32_t struct_size, ready, width, height; uint64_t byte_size; uint32_t row_pitch, reserved; };
// Sources: final frame=0, last submitted output=1, world before outline/post/GUI=2, texture subresource=3, Tracy thumbnail=4.
struct rn_capture_request { uint32_t struct_size,source; uint64_t id; uint32_t texture,mip,layer,reserved; };
struct rn_device_info {
    uint32_t struct_size; rn_backend backend;
    uint32_t device_type, vendor_id; // 0 unknown, 1 integrated, 2 discrete, 3 software.
    uint64_t uniform_offset_alignment;
    uint32_t max_texture_size, max_color_attachments;
    float max_anisotropy; uint32_t flags; // Bit 0: active renderer has the optional terrain wireframe pipelines.
    char name[256], vendor[64], driver[128];
};
enum rn_pixel_format : uint32_t { RN_PIXEL_RGBA8 = 0, RN_PIXEL_R8 = 1, RN_PIXEL_RGBA16F = 2, RN_PIXEL_RGBA32F = 3, RN_PIXEL_DEPTH32F = 4 };
enum rn_texture_flags : uint32_t { RN_TEXTURE_CUBE = 1, RN_TEXTURE_ATTACHMENT = 2, RN_TEXTURE_READBACK = 4 };
struct rn_uploaded_texture_desc { uint32_t struct_size, id, width, height, mip_levels; rn_pixel_format format; uint32_t array_layers, flags; };
struct rn_texture_upload_desc {
    uint32_t struct_size, id, mip_level, x, y, width, height, row_pitch;
    const void* pixels; uint64_t byte_size;
    uint32_t array_layer, reserved;
};
// IDs are nonzero and scoped to the frame epoch. Input pointers are borrowed for one call.
enum rn_material_mode : uint32_t { RN_MATERIAL_OPAQUE = 0, RN_MATERIAL_CUTOUT = 1, RN_MATERIAL_TRANSLUCENT = 2 };
struct rn_index_update { uint64_t mesh_id; const uint32_t* indices; uint32_t count, reserved; };
struct rn_mesh_update { uint64_t id; const void* vertices; uint32_t vertex_count; rn_material_mode layer; };
struct rn_instance { int32_t origin[3]; float visibility; };
// UINT32_MAX requests all groups; explicit masks intersect native conservative face culling.
struct rn_visible_item { uint64_t mesh; uint32_t element_mask, sort_key, first_instance, instance_count; };
struct rn_gui_vertex { float position[3], uv[2]; uint32_t color; };
enum rn_gui_pipeline : uint32_t {
    RN_GUI_COLOR, RN_GUI_INVERT, RN_GUI_HIGHLIGHT, RN_GUI_TEXTURED, RN_GUI_PREMULTIPLIED,
    RN_GUI_OPAQUE, RN_GUI_NAUSEA, RN_GUI_VIGNETTE, RN_GUI_CROSSHAIR, RN_GUI_LOGO, RN_GUI_TEXT, RN_GUI_TEXT_GRAYSCALE,
    RN_GUI_SCENE // Zero vertices; texture identifies the ordered RN_SCENE_GUI draw.
};
// Sampler bits: repeat S/T = 1/2, linear min/mag/mip = 4/8/16. Scissor is in logical GUI units.
// mip_count=0 selects all remaining levels. LOD is relative to the sampling view.
struct rn_texture_sampling { uint32_t base_mip, mip_count; float max_lod; uint32_t max_anisotropy; };
struct rn_gui_draw { rn_gui_pipeline pipeline; uint32_t texture, sampler_bits, first_vertex, vertex_count; int32_t scissor[4]; uint32_t reserved; rn_texture_sampling sampling; };
enum rn_scene_phase : uint32_t {
    RN_SCENE_SKY, RN_SCENE_SOLID, RN_SCENE_TRANSLUCENT, RN_SCENE_AFTER_TERRAIN,
    RN_SCENE_SEE_THROUGH, RN_SCENE_ALWAYS_ON_TOP, RN_SCENE_HAND, RN_SCENE_PANORAMA,
    RN_SCENE_OIT_BOUNDS, RN_SCENE_OIT_TRANSMITTANCE, RN_SCENE_OIT_ACCUMULATE,
    RN_SCENE_WATER_MASK,
    RN_SCENE_CLOUD_BOUNDS, RN_SCENE_CLOUD_TRANSMITTANCE, RN_SCENE_CLOUD_ACCUMULATE,
    RN_SCENE_OFFSCREEN, RN_SCENE_OUTLINE, RN_SCENE_GUI,
    RN_SCENE_PHASE_COUNT
};
// Blob bytes are borrowed through synchronous Prepare only. UINT32_MAX denotes no geometry stream.
struct rn_scene_blob { const void* bytes; uint64_t size; };
// kind 0: constant blob; kind 1: uploaded texture; kind 2: graph texture (1 depth bounds, 2/3 coefficients); kind 3: persistent R8_SINT texel buffer.
struct rn_scene_binding { uint32_t kind, binding, resource, sampler_bits; uint64_t offset, size; rn_texture_sampling sampling; };
// A cleared temporary color/depth tile is rendered, then copied into a persistent sampling texture.
struct rn_scene_target { uint32_t texture,x,y,width,height,first_draw,draw_count,reserved; };
// Built-in chain IDs: blur=1, creeper=2, outline=3, invert=4, spider=5. Phase 0 world, 1 GUI.
struct rn_scene_effect { uint32_t chain,phase,globals_blob,reserved; };
struct rn_scene_draw {
    uint32_t pipeline, phase, vertices, indices, vertex_count, index_count, index_stride;
    uint32_t first_binding, binding_count, instance_count, instances, reserved;
    uint64_t vertex_offset, index_offset, instance_offset;
    int32_t scissor[4];
};
struct rn_render_frame_desc {
    uint32_t struct_size, flags;
    uint64_t epoch, output_sequence;
    const rn_index_update* indices;
    const rn_mesh_update* meshes;
    const uint64_t* removed_meshes;
    const rn_visible_item* visible;
    const rn_instance* instances;
    uint32_t index_count, mesh_count, removed_mesh_count, visible_count, instance_count;
    uint32_t terrain_albedo_texture, terrain_light_texture, terrain_anisotropy;
    float projection[16], view[16];
    int32_t camera_block[3];
    float camera_fraction[3], fog_color[4], fog_distances[4];
    const rn_gui_vertex* gui_vertices;
    const rn_gui_draw* gui_draws;
    uint32_t gui_vertex_count, gui_draw_count;
    float gui_width, gui_height, clear_color[4];
    const rn_scene_blob* scene_blobs;
    const rn_scene_binding* scene_bindings;
    const rn_scene_draw* scene_draws;
    uint32_t scene_blob_count, scene_binding_count, scene_draw_count, scene_reserved;
    const rn_scene_target* scene_targets;
    uint32_t scene_target_count, scene_target_reserved;
    const rn_scene_effect* scene_effects;
    uint32_t scene_effect_count, gui_blur_split;
    // Both zero uses the swapchain extent; nonzero renders offscreen without acquiring/presenting.
    uint32_t render_width,render_height;
};

extern "C" {

RAYNIUM_API uint32_t rn_get_abi_version(void) noexcept;

// Status-returning calls return only OK/FAILED; failures are logged where detected.
// Internal invariant violations and unexpected C++ exceptions log and abort.
// Query failure leaves info unchanged.
RAYNIUM_API rn_status rn_query_abi(rn_abi_info* info, uint32_t capacity) noexcept;

// One environment per loaded DLL. Duplicate initialization fails without changing it.
// Creates no GPU device. D3D12 validation requires initialization before host graphics.
// All lifecycle calls are serialized on the environment's initializing thread.
RAYNIUM_API rn_status rn_init_env(const rn_env_desc* desc) noexcept;

// Fails and preserves the environment while a runtime is live. Uninitialized is OK.
// Success releases the factory/instance, but does not disable the D3D12 debug layer.
// Destroy the environment before unloading this DLL.
RAYNIUM_API rn_status rn_destroy_env(void) noexcept;

// Requires an initialized environment; at most one runtime may be active.
// Creation failure preserves the environment; runtime destruction allows recreation.
// Inputs are borrowed only for this call; buffers must not overlap. struct_size must
// equal sizeof(rn_runtime_desc). A valid out_runtime is set to NULL on failure.
// No exception may cross this boundary.
RAYNIUM_API rn_status rn_create_runtime(const rn_runtime_desc* desc, rn_runtime** out_runtime) noexcept;

// NULL is a no-op. A live pointer must be destroyed exactly once, before DLL unload.
// All renderers must already have been successfully destroyed.
// Wrong owner/thread or a live renderer logs and aborts.
RAYNIUM_API void rn_destroy_runtime(rn_runtime* runtime) noexcept;


// Calls are serialized on the runtime owner thread. Failed creation clears the output handle.
RAYNIUM_API rn_status rn_create_renderer(rn_runtime*, const rn_renderer_desc*, rn_renderer**) noexcept;
RAYNIUM_API rn_status rn_destroy_renderer(rn_renderer*) noexcept;
// Query failure leaves info unchanged. The buffer capacity is in bytes.
RAYNIUM_API rn_status rn_query_device_info(rn_runtime*, rn_device_info*, uint32_t) noexcept;
RAYNIUM_API rn_status rn_create_swapchain(rn_renderer*, const rn_swapchain_desc*, rn_swapchain**) noexcept;
RAYNIUM_API rn_status rn_resize_swapchain(rn_swapchain*, const rn_swapchain_desc*) noexcept;
RAYNIUM_API rn_status rn_destroy_swapchain(rn_swapchain*) noexcept;
// OK consumes residency updates even when presentation was skipped. Failure leaves result unchanged;
// close the renderer after failure. No acquired frame remains outstanding on any returned status.
RAYNIUM_API rn_status rn_present_frame(rn_renderer*, rn_swapchain*, const rn_render_frame_desc*, rn_present_result*) noexcept;
struct rn_scene_buffer_desc { uint32_t struct_size, id; uint64_t size; };
struct rn_scene_buffer_upload_desc { uint32_t struct_size, id; uint64_t offset, size; const void* bytes; };
// Scene buffer IDs increase in [1,0x7ffffffe]; geometry references encode 0x80000000|id.
// Upload copies bytes before returning. In-flight owners survive registry removal.
RAYNIUM_API rn_status rn_create_scene_buffer(rn_renderer*, const rn_scene_buffer_desc*) noexcept;
RAYNIUM_API rn_status rn_upload_scene_buffer(rn_renderer*, const rn_scene_buffer_upload_desc*) noexcept;
RAYNIUM_API rn_status rn_destroy_scene_buffer(rn_renderer*, uint32_t id) noexcept;
// Uploaded IDs >=16 are never reused during this renderer's lifetime. Upload copies bytes during the call.
RAYNIUM_API rn_status rn_create_texture(rn_renderer*, const rn_uploaded_texture_desc*) noexcept;
RAYNIUM_API rn_status rn_upload_texture(rn_renderer*, const rn_texture_upload_desc*) noexcept;
RAYNIUM_API rn_status rn_destroy_texture(rn_renderer*, uint32_t id) noexcept;
RAYNIUM_API rn_status rn_poll_capture(rn_renderer*, uint64_t sequence, rn_capture_info*, void* pixels, uint64_t capacity) noexcept;
// Copies the request and retains its source owner immediately; pending uploads execute before readback.
RAYNIUM_API rn_status rn_request_capture(rn_renderer*,const rn_capture_request*) noexcept;
}
