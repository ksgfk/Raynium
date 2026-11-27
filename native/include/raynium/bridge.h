#pragma once

#include <stdint.h>

#if defined(RAYNIUM_RUNTIME_EXPORTS)
#define RAYNIUM_API __declspec(dllexport)
#else
#define RAYNIUM_API __declspec(dllimport)
#endif
#define RN_ABI_VERSION 27u

enum rn_submit_flags : uint32_t { RN_SUBMIT_FLAG_DEVICE_LOST = 1u, RN_SUBMIT_FLAG_TERMINAL_ERROR = 2u };
struct rn_wait_result {
    uint64_t consumed_sequence, submitted_sequence, completed_sequence, presented_frames;
    uint32_t surface_generation, surface_recovery_state, last_present_outcome, flags;
};

struct rn_runtime;
struct rn_renderer;
struct rn_swapchain;
struct rn_texture;
struct rn_buffer;
struct rn_pipeline;
struct rn_query_pool;

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


enum rn_frame_flags : uint32_t {
    RN_FRAME_RGSS = 1,
    RN_FRAME_IMPROVED_TRANSPARENCY = 4,
    RN_FRAME_WIREFRAME = 128,
    RN_FRAME_REUSE_VISIBLE = 256,
    RN_FRAME_REUSE_INSTANCES = 512,
    RN_FRAME_INSTANCE_POOL = 1024
};
// Zero selects auto. Forced direct and indirect are mutually exclusive.
enum rn_renderer_flags : uint32_t { RN_RENDERER_DIRECT = 1, RN_RENDERER_INDIRECT = 2 };
// Pack bytes are borrowed only during rn_create_renderer; no pointer is retained.
struct rn_renderer_desc { uint32_t struct_size, flags; const void* shader_pack; uint64_t shader_pack_size; };
enum rn_present_mode : uint32_t { RN_PRESENT_IMMEDIATE = 0, RN_PRESENT_FIFO = 1 };
struct rn_swapchain_desc { uint32_t struct_size, width, height; rn_present_mode present_mode; uint64_t hwnd; };
enum rn_present_outcome : uint32_t { RN_PRESENTED = 0, RN_PRESENT_SKIPPED = 1, RN_PRESENT_RECREATE = 2, RN_PRESENT_NONE = 3, RN_PRESENTED_SUBOPTIMAL = 4, RN_PRESENT_SURFACE_LOST = 5 };
// Opt-in measurement: completed GPU sequence/time, current wait/prepare/RDG/present wall times, all nanoseconds.
struct rn_frame_metrics {
    uint64_t terrain_upload_bytes{},texture_upload_bytes{},scene_upload_bytes{},gui_upload_bytes{};
    uint64_t terrain_records{},terrain_calls{},scene_calls{},gui_calls{},bind_calls{};
    uint64_t vertex_capacity{},index_capacity{},geometry_allocated{};
};
// Capture pixels are packed RGBA8, top row first. A successful non-null read consumes the capture.
struct rn_capture_info { uint32_t struct_size, ready, width, height; uint64_t byte_size; uint32_t row_pitch, reserved; };
enum rn_device_features : uint32_t {
    RN_DEVICE_WIREFRAME = 1u, RN_DEVICE_INDIRECT_DRAW = 2u, RN_DEVICE_INDIRECT_FIRST_INSTANCE = 4u,
    RN_DEVICE_TIMESTAMP_QUERIES = 8u, RN_DEVICE_PERSISTENT_MAPPING = 16u
};
struct rn_device_info {
    uint32_t struct_size; rn_backend backend;
    uint32_t device_type, vendor_id; // 0 unknown, 1 integrated, 2 discrete, 3 software.
    uint64_t uniform_offset_alignment;
    uint32_t max_texture_size, max_color_attachments;
    float max_anisotropy; uint32_t flags; // Bit 0: active renderer has the optional terrain wireframe pipelines.
    char name[256], vendor[64], driver[128];
    uint64_t max_buffer_size, max_uniform_buffer_range, max_storage_buffer_range;
    uint64_t storage_offset_alignment, texel_offset_alignment, buffer_copy_offset_alignment, texture_placement_alignment;
    uint32_t texture_pitch_alignment, max_texture_1d, max_texture_3d, max_array_layers;
    uint32_t max_vertex_bindings, max_push_constant_bytes, max_draw_indirect_count, timestamp_valid_bits;
    uint64_t timestamp_frequency;
    double timestamp_period_ns;
    uint64_t host_coherent_atom_size;
};
enum rn_pixel_format : uint32_t {
    RN_PIXEL_RGBA8 = 0, RN_PIXEL_R8 = 1, RN_PIXEL_RGBA16F = 2, RN_PIXEL_RGBA32F = 3,
    RN_PIXEL_DEPTH32F = 4, RN_PIXEL_R8_SNORM = 5, RN_PIXEL_RG8_UNORM = 6, RN_PIXEL_RG8_SNORM = 7,
    RN_PIXEL_RGBA8_SNORM = 8, RN_PIXEL_R16_UNORM = 9, RN_PIXEL_R16_SNORM = 10, RN_PIXEL_RG16_UNORM = 11,
    RN_PIXEL_RG16_SNORM = 12, RN_PIXEL_RGBA16_UNORM = 13, RN_PIXEL_RGBA16_SNORM = 14, RN_PIXEL_R8_UINT = 15,
    RN_PIXEL_R8_SINT = 16, RN_PIXEL_RG8_UINT = 17, RN_PIXEL_RG8_SINT = 18, RN_PIXEL_RGBA8_UINT = 19,
    RN_PIXEL_RGBA8_SINT = 20, RN_PIXEL_R16_UINT = 21, RN_PIXEL_R16_SINT = 22, RN_PIXEL_RG16_UINT = 23,
    RN_PIXEL_RG16_SINT = 24, RN_PIXEL_RGBA16_UINT = 25, RN_PIXEL_RGBA16_SINT = 26, RN_PIXEL_R32_UINT = 27,
    RN_PIXEL_R32_SINT = 28, RN_PIXEL_RG32_UINT = 29, RN_PIXEL_RG32_SINT = 30, RN_PIXEL_RGBA32_UINT = 31,
    RN_PIXEL_RGBA32_SINT = 32, RN_PIXEL_R16_FLOAT = 33, RN_PIXEL_RG16_FLOAT = 34, RN_PIXEL_R32_FLOAT = 35,
    RN_PIXEL_RG32_FLOAT = 36, RN_PIXEL_RGB10A2_UNORM = 37, RN_PIXEL_RGB10A2_UINT = 38, RN_PIXEL_RG11B10_FLOAT = 39,
    RN_PIXEL_D32_FLOAT_S8_UINT = 40, RN_PIXEL_D24_UNORM_S8_UINT = 41, RN_PIXEL_D16_UNORM = 42
 };
enum rn_texture_flags : uint32_t { RN_TEXTURE_CUBE = 1, RN_TEXTURE_ATTACHMENT = 2, RN_TEXTURE_READBACK = 4 };
struct rn_texture_desc { uint32_t struct_size, width, height, mip_levels; rn_pixel_format format; uint32_t array_layers, flags, reserved; };
struct rn_texture_upload_desc {
    uint32_t struct_size, mip_level;
    rn_texture* texture;
    uint32_t x, y, width, height, row_pitch, array_layer;
    const void* pixels; uint64_t byte_size;
};
// IDs are nonzero and scoped to the frame epoch.
enum rn_material_mode : uint32_t { RN_MATERIAL_OPAQUE = 0, RN_MATERIAL_CUTOUT = 1, RN_MATERIAL_TRANSLUCENT = 2 };
enum rn_terrain_mesh_layout_flags : uint32_t {
    RN_TERRAIN_LAYOUT_FINITE_BOUNDS = 1u,
    RN_TERRAIN_LAYOUT_WIREFRAME_GROUPS = 2u
};
struct rn_terrain_mesh_layout {
    uint32_t group_quad_counts[7];
    uint32_t flags;
    float minimum[3];
    float maximum[3];
    const uint8_t* original_quad_groups;
    uint32_t original_quad_groups_bytes;
    uint32_t reserved;
};
struct rn_index_update { uint64_t mesh_id; const uint32_t* indices; uint32_t count, reserved; };
struct rn_mesh_update { uint64_t id; const void* vertices; uint32_t vertex_count; rn_material_mode layer; const rn_terrain_mesh_layout* layout; };
struct rn_instance { int32_t origin[3]; uint32_t fade_start_ms; };
// UINT32_MAX requests all groups; explicit masks intersect native conservative face culling.
struct rn_visible_item { uint64_t mesh; uint32_t element_mask, sort_key, first_instance, instance_count; };
struct rn_texture_sampling { uint32_t base_mip, mip_count; float max_lod; uint32_t max_anisotropy; };
struct rn_terrain_view_desc {
    uint32_t struct_size, flags;
    uint64_t epoch, output_sequence;
    const rn_index_update* indices;
    const rn_mesh_update* meshes;
    const uint64_t* removed_meshes;
    const rn_visible_item* visible;
    const rn_instance* instances;
    uint32_t index_count, mesh_count, removed_mesh_count, visible_count, instance_count;
    uint32_t terrain_anisotropy;
    rn_texture *terrain_albedo_texture, *terrain_light_texture;
    float projection[16], view[16];
    int32_t camera_block[3];
    float camera_fraction[3], fog_color[4], fog_distances[4];
    uint64_t visible_version, origins_version, instances_version;
    // In pool mode instances[i] replaces instance_slots[i]. A zero base resets the view's table.
    const uint32_t* instance_slots;
    uint32_t instance_capacity, instance_reserved;
    uint64_t instance_base_version;
    uint32_t fade_now_ms,fade_duration_ms;
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
RAYNIUM_API rn_status rn_destroy_runtime(rn_runtime* runtime) noexcept;


// Calls are serialized on the runtime owner thread. Failed creation clears the output handle.
RAYNIUM_API rn_status rn_create_renderer(rn_runtime*, const rn_renderer_desc*, rn_renderer**) noexcept;
RAYNIUM_API rn_status rn_destroy_renderer(rn_renderer*) noexcept;
// Query failure leaves info unchanged. The buffer capacity is in bytes.
RAYNIUM_API rn_status rn_query_device_info(rn_runtime*, rn_device_info*, uint32_t) noexcept;
RAYNIUM_API rn_status rn_create_swapchain(rn_renderer*, const rn_swapchain_desc*, rn_swapchain**) noexcept;
RAYNIUM_API rn_status rn_resize_swapchain(rn_swapchain*, const rn_swapchain_desc*) noexcept;
RAYNIUM_API rn_status rn_destroy_swapchain(rn_swapchain*) noexcept;
enum rn_buffer_usage : uint32_t {
    RN_BUFFER_MAP_READ=1, RN_BUFFER_MAP_WRITE=2, RN_BUFFER_CLIENT_STORAGE=4, RN_BUFFER_COPY_DST=8,
    RN_BUFFER_COPY_SRC=16, RN_BUFFER_VERTEX=32, RN_BUFFER_INDEX=64, RN_BUFFER_UNIFORM=128, RN_BUFFER_TEXEL=256, RN_BUFFER_INDIRECT=512,
    RN_BUFFER_FENCE_ROTATED=1024
};
struct rn_buffer_desc { uint32_t struct_size,usage; uint64_t size; const void* initial; };
struct rn_buffer_result { rn_buffer* buffer; void* mapped; };
struct rn_mapped_range { rn_buffer* buffer; uint64_t offset,length; };
struct rn_pipeline_desc {
    uint32_t struct_size,word_count;
    const char* program;
    const uint32_t* words;
    const void* shader_pack;
    uint64_t shader_pack_size;
};
RAYNIUM_API rn_status rn_register_pipeline(rn_renderer*,const rn_pipeline_desc*,rn_pipeline**) noexcept;
RAYNIUM_API rn_status rn_destroy_pipeline(rn_renderer*,rn_pipeline*) noexcept;
RAYNIUM_API rn_status rn_create_buffer(rn_renderer*,const rn_buffer_desc*,rn_buffer_result*) noexcept;
RAYNIUM_API rn_status rn_destroy_buffer(rn_renderer*,rn_buffer* buffer) noexcept;
struct rn_timestamp_write { rn_query_pool* pool; uint32_t slot, reserved; };
struct rn_batch_desc {
    uint32_t struct_size, flags;
    rn_swapchain* surface;
    const void* commands;
    uint64_t command_bytes;
    const void* uploads;
    uint64_t upload_bytes;
    const void* instance_uploads;
    uint64_t instance_upload_bytes;
    const rn_mapped_range* dirty_ranges;
    uint32_t dirty_count,reserved;
    const rn_terrain_view_desc* views;
    uint32_t view_count,view_reserved;
    const uint64_t* captures;
    uint32_t capture_count,capture_reserved;
    const rn_timestamp_write* queries;
    uint32_t query_count,query_reserved;
};
enum rn_batch_flags : uint32_t { RN_BATCH_MEASURE=1, RN_BATCH_BORROW_POOLED=2 };
struct rn_submit_result {
    uint64_t sequence;
    rn_wait_result progress;
};
enum rn_telemetry_flags : uint32_t { RN_TELEMETRY_HAS_GPU_NS=1 };
struct rn_batch_telemetry {
    uint64_t sequence;
    uint32_t flags, reserved;
    uint64_t gpu_ns,wait_ns,prepare_ns,rdg_ns,present_ns;
    rn_frame_metrics metrics;
};
RAYNIUM_API rn_status rn_flush_renderer(rn_renderer*, rn_wait_result*) noexcept;
RAYNIUM_API rn_status rn_drain_telemetry(rn_renderer*, rn_batch_telemetry*, uint32_t capacity, uint32_t* out_count) noexcept;
// Command records are eight-byte aligned. Batch, view and dirty-range descriptors are copied on submission.
// Payloads are copied by default. With RN_BATCH_BORROW_POOLED, keep commands, uploads, instance uploads,
// and all arrays and bytes referenced by views alive and unchanged until consumed_sequence reaches the returned sequence.
RAYNIUM_API rn_status rn_submit_frame(rn_renderer*, const rn_batch_desc*, rn_submit_result*) noexcept;
struct rn_query_sample { uint64_t sequence,ticks; }; // Zero sequence denotes an unavailable slot.
RAYNIUM_API rn_status rn_create_query_pool(rn_renderer*,uint32_t count,rn_query_pool** pool) noexcept;
RAYNIUM_API rn_status rn_destroy_query_pool(rn_renderer*,rn_query_pool* pool) noexcept;
RAYNIUM_API rn_status rn_sync_query_pool(rn_renderer*,rn_query_pool* pool,uint32_t first,uint32_t count,rn_query_sample*,rn_wait_result*) noexcept;
// Resource handles belong to the creating renderer. Keep resources referenced by commands alive until consumed.
RAYNIUM_API rn_status rn_create_texture(rn_renderer*, const rn_texture_desc*, rn_texture**) noexcept;
RAYNIUM_API rn_status rn_upload_texture(rn_renderer*, const rn_texture_upload_desc*) noexcept;
RAYNIUM_API rn_status rn_destroy_texture(rn_renderer*, rn_texture* texture) noexcept;
// User mesh IDs start at 16. Vertex bytes are copied during the call (24 B/vertex, quad-indexed).
RAYNIUM_API rn_status rn_upsert_model_mesh(rn_renderer*, int32_t mesh_id, const void* vertex_data, uint32_t vertex_bytes, uint32_t vertex_count, uint32_t index_count, uint32_t* out_page_id) noexcept;
RAYNIUM_API rn_status rn_remove_model_mesh(rn_renderer*, int32_t mesh_id) noexcept;
RAYNIUM_API rn_status rn_wait_submit(rn_renderer*, uint64_t sequence, int64_t timeout_ns, rn_wait_result* result) noexcept;
RAYNIUM_API rn_status rn_poll_capture(rn_renderer*, uint64_t sequence, rn_capture_info*, void* pixels, uint64_t capacity) noexcept;
}
