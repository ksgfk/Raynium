#pragma once

#include <stdint.h>

#if defined(RAYNIUM_RUNTIME_EXPORTS)
#define RAYNIUM_API __declspec(dllexport)
#else
#define RAYNIUM_API __declspec(dllimport)
#endif
#define RN_ABI_VERSION 17u

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
    RN_FRAME_FLIP_Y = 8 }; // Reflect terrain clip Y and its front-face winding together.
// Zero selects auto. Forced direct and indirect are mutually exclusive.
enum rn_renderer_flags : uint32_t { RN_RENDERER_DIRECT = 1, RN_RENDERER_INDIRECT = 2 };
// Pack bytes are borrowed only during rn_create_renderer; no pointer is retained.
struct rn_renderer_desc { uint32_t struct_size, flags; const void* shader_pack; uint64_t shader_pack_size; };
enum rn_present_mode : uint32_t { RN_PRESENT_IMMEDIATE = 0, RN_PRESENT_FIFO = 1 };
struct rn_swapchain_desc { uint32_t struct_size, width, height; rn_present_mode present_mode; uint64_t hwnd; };
enum rn_present_outcome : uint32_t { RN_PRESENTED = 0, RN_PRESENT_SKIPPED = 1, RN_PRESENT_RECREATE = 2 };
struct rn_present_result { uint32_t struct_size; rn_present_outcome outcome; uint64_t acquire_timeouts; };
struct rn_device_info {
    uint32_t struct_size; rn_backend backend;
    uint32_t device_type, vendor_id; // 0 unknown, 1 integrated, 2 discrete, 3 software.
    uint64_t uniform_offset_alignment;
    uint32_t max_texture_size, max_color_attachments;
    float max_anisotropy; uint32_t reserved;
    char name[256], vendor[64], driver[128];
};
enum rn_pixel_format : uint32_t { RN_PIXEL_RGBA8 = 0, RN_PIXEL_R8 = 1 };
struct rn_uploaded_texture_desc { uint32_t struct_size, id, width, height, mip_levels; rn_pixel_format format; };
struct rn_texture_upload_desc {
    uint32_t struct_size, id, mip_level, x, y, width, height, row_pitch;
    const void* pixels; uint64_t byte_size;
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
    RN_GUI_OPAQUE, RN_GUI_NAUSEA, RN_GUI_VIGNETTE, RN_GUI_CROSSHAIR, RN_GUI_LOGO, RN_GUI_TEXT, RN_GUI_TEXT_GRAYSCALE
};
// Sampler bits: repeat S/T = 1/2, linear min/mag/mip = 4/8/16. Scissor is in logical GUI units.
struct rn_gui_draw { rn_gui_pipeline pipeline; uint32_t texture, sampler_bits, first_vertex, vertex_count; int32_t scissor[4]; uint32_t reserved; };
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
// Uploaded IDs >=16 are never reused during this renderer's lifetime. Upload copies bytes during the call.
RAYNIUM_API rn_status rn_create_texture(rn_renderer*, const rn_uploaded_texture_desc*) noexcept;
RAYNIUM_API rn_status rn_upload_texture(rn_renderer*, const rn_texture_upload_desc*) noexcept;
RAYNIUM_API rn_status rn_destroy_texture(rn_renderer*, uint32_t id) noexcept;
}
