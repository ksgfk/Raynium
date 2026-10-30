#pragma once

#include <stdint.h>

#if defined(RAYNIUM_RUNTIME_EXPORTS)
#define RAYNIUM_API __declspec(dllexport)
#else
#define RAYNIUM_API __declspec(dllimport)
#endif
#define RN_ABI_VERSION 8u

struct rn_runtime;
struct rn_renderer;
struct rn_render_target;
struct rn_shared_textures;

// Fixed-width unscoped enums preserve the Java FFM integer layout.
enum rn_status : uint32_t {
    RN_STATUS_OK = 0,
    RN_STATUS_FAILED = 1
};

enum rn_backend : uint32_t {
    RN_BACKEND_D3D12 = 1,
    RN_BACKEND_VULKAN = 2
};

enum rn_runtime_flags : uint32_t {
    RN_RUNTIME_MATCH_GPU = 1
};

enum rn_env_flags : uint32_t {
    RN_ENV_VALIDATION = 1
};

enum rn_shared_handle_type : uint32_t {
    RN_SHARED_D3D12 = 1,
    RN_SHARED_OPAQUE_WIN32 = 2
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

struct rn_runtime_desc {
    uint32_t struct_size;
    rn_runtime_flags flags;
    uint32_t node_mask;
    uint8_t luid[8];
};

struct rn_shared_image_info {
    uint32_t struct_size;
    rn_shared_handle_type handle_type;
    uint32_t width;
    uint32_t height;
    uint64_t memory_handle;
    uint64_t ready_handle;
    uint64_t consumed_handle;
    uint64_t allocation_size;
    uint32_t memory_type_index;
    uint32_t node_mask;
    uint8_t luid[8];
    uint8_t device_uuid[16];
    uint8_t driver_uuid[16];
    uint32_t native_format;
    uint32_t native_usage;
    uint32_t native_flags;
    uint32_t native_tiling;
    uint64_t allocation_offset;
    uint32_t dedicated;
    uint32_t reserved;
};

enum rn_shader_program : uint32_t { RN_PROGRAM_UV = 1, RN_PROGRAM_TEXTURED = 2 };
enum rn_vertex_layout : uint32_t { RN_LAYOUT_PROCEDURAL = 0, RN_LAYOUT_POSITION_COLOR_UV_LIGHT = 1 };
enum rn_frame_flags : uint32_t { RN_FRAME_DIRECT = 1, RN_FRAME_RGSS = 4, RN_FRAME_PROFILE = 8 };
struct rn_renderer_desc { uint32_t struct_size, flags; };
struct rn_render_target_desc { uint32_t struct_size, width, height, reserved; };
struct rn_texture_desc { uint32_t width, height, mip_levels, split_mips; };
struct rn_shared_textures_desc { uint32_t struct_size, texture_count; const rn_texture_desc* textures; };
// Handles are borrowed from their owner; texture_index/mip_level map physical exports to logical textures.
struct rn_shared_image_export { rn_shared_image_info image; uint32_t texture_index, mip_level, mip_count, reserved; };
// IDs are nonzero and scoped to the frame epoch. Input pointers are borrowed for one call.
struct rn_index_update { uint64_t id; const uint32_t* indices; uint32_t count, reserved; };
struct rn_submesh { uint32_t first, count; int32_t base_vertex; uint32_t reserved; };
struct rn_mesh_update {
    uint64_t id;
    const void* vertices;
    uint64_t index_buffer;
    const rn_submesh* submeshes;
    uint32_t vertex_count, vertex_stride, layout, submesh_count;
};
// Texture indices address logical textures in the attached shared_textures set.
struct rn_material_update { uint64_t id; uint32_t program, albedo_texture, light_texture, anisotropy; };
struct rn_primitive_update { uint64_t id, mesh, material; };
struct rn_instance { int32_t origin[3]; float visibility; };
struct rn_visible_item { uint64_t primitive; uint32_t submesh, depth_key, first_instance, instance_count; };
struct rn_render_frame_desc {
    uint32_t struct_size, flags;
    uint64_t epoch, output_sequence, input_sequence;
    const rn_index_update* indices;
    const rn_mesh_update* meshes;
    const rn_material_update* materials;
    const rn_primitive_update* primitives;
    const uint64_t* removed_meshes;
    const uint64_t* removed_primitives;
    const uint64_t* removed_materials;
    const uint64_t* removed_indices;
    const rn_visible_item* visible;
    const rn_instance* instances;
    uint32_t index_count, mesh_count, material_count, primitive_count;
    uint32_t removed_mesh_count, removed_primitive_count, removed_material_count, removed_index_count;
    uint32_t visible_count, instance_count, changed_textures, reserved;
    float projection[16], view[16];
    int32_t camera_block[3];
    float camera_fraction[3], fog_color[4], fog_distances[4];
};

extern "C" {

RAYNIUM_API uint32_t rn_get_abi_version(void) noexcept;

// All status-returning calls return only OK/FAILED; diagnostics describe the reason.
// The caller owns non-overlapping info/diagnostic buffers. Failure leaves info unchanged.
// Diagnostics are truncated and NUL-terminated when capacity > 0; NULL requires zero capacity.
RAYNIUM_API rn_status rn_query_abi(rn_abi_info* info, uint32_t capacity,
                                char* error_message, uint32_t error_capacity) noexcept;

// One environment per loaded DLL. Duplicate initialization fails without changing it.
// Creates no GPU device. D3D12 validation requires initialization before host graphics.
// All lifecycle calls are serialized on the environment's initializing thread.
RAYNIUM_API rn_status rn_init_env(const rn_env_desc* desc, char* error_message,
                                uint32_t error_capacity) noexcept;

// Fails and preserves the environment while a runtime is live. Uninitialized is OK.
// Success releases the factory/instance, but does not disable the D3D12 debug layer.
// Destroy the environment before unloading this DLL.
RAYNIUM_API rn_status rn_destroy_env(char* error_message, uint32_t error_capacity) noexcept;

// Requires an initialized environment; at most one runtime may be active.
// Creation failure preserves the environment; runtime destruction allows recreation.
// Inputs are borrowed only for this call; buffers must not overlap. struct_size must
// equal sizeof(rn_runtime_desc). A valid out_runtime is set to NULL on failure.
// Diagnostics are UTF-8, truncated and NUL-terminated when capacity > 0. A NULL
// error_message requires zero capacity. No exception may cross this boundary.
RAYNIUM_API rn_status rn_create_runtime(const rn_runtime_desc* desc, rn_runtime** out_runtime,
                                     char* error_message, uint32_t error_capacity) noexcept;

// NULL is a no-op. A live pointer must be destroyed exactly once, before DLL unload.
// All renderers must already have been successfully destroyed.
RAYNIUM_API void rn_destroy_runtime(rn_runtime* runtime) noexcept;


// Calls are serialized on the runtime owner thread. Failed creation clears the output handle.
RAYNIUM_API rn_status rn_create_renderer(rn_runtime*, const rn_renderer_desc*, rn_renderer**, char*, uint32_t) noexcept;
RAYNIUM_API rn_status rn_destroy_renderer(rn_renderer*, char*, uint32_t) noexcept;
RAYNIUM_API rn_status rn_create_render_target(rn_renderer*, const rn_render_target_desc*, rn_render_target**,
    rn_shared_image_info*, uint32_t info_capacity, char*, uint32_t) noexcept;
RAYNIUM_API rn_status rn_destroy_render_target(rn_render_target*, char*, uint32_t) noexcept;
// export_capacity counts records, not bytes. Creation releases shared inputs at sequence 1.
RAYNIUM_API rn_status rn_create_shared_textures(rn_renderer*, const rn_shared_textures_desc*, rn_shared_textures**,
    rn_shared_image_export*, uint32_t export_capacity, char*, uint32_t) noexcept;
RAYNIUM_API rn_status rn_destroy_shared_textures(rn_shared_textures*, char*, uint32_t) noexcept;
// Successful submissions alone advance sequences and publish scene updates. Failed waits preserve live owners.
RAYNIUM_API rn_status rn_render_frame(rn_renderer*, rn_render_target*, const rn_render_frame_desc*, char*, uint32_t) noexcept;
}
