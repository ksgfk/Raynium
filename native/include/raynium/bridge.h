#pragma once

#include <stdint.h>

#if defined(RAYNIUM_RUNTIME_EXPORTS)
#define RAYNIUM_API __declspec(dllexport)
#else
#define RAYNIUM_API __declspec(dllimport)
#endif
#define RN_ABI_VERSION 7u

struct rn_runtime;
struct rn_preview;
struct rn_terrain;

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

struct rn_terrain_desc {
    uint32_t struct_size, width, height, mip_levels;
    uint32_t split_mips, reserved;
};

// All pointers are borrowed for one draw call. IDs are monotonic within an epoch.
struct rn_terrain_mesh {
    uint64_t id;
    const void* vertices;
    uint32_t vertex_count, reserved;
    uint32_t group_vertices[7];
};

struct rn_terrain_draw {
    uint64_t id;
    int32_t origin[3];
    float visibility;
    uint32_t group_mask, reserved;
};

struct rn_terrain_frame {
    uint32_t struct_size, flags; // bit 0: direct draws; bit 1: atlas changed; bit 2: RGSS; bit 3: profile
    uint64_t epoch, input_sequence;
    const rn_terrain_mesh* updates;
    const rn_terrain_draw* draws;
    const uint64_t* removed;
    uint32_t update_count, draw_count, removed_count, anisotropy;
    float projection[16], view[16];
    int32_t camera_block[3];
    float camera_fraction[3];
    float fog_color[4], fog_distances[4];
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

struct rn_preview_desc {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    uint32_t reserved;
};

// NT handles are borrowed from the preview until its destruction. All fields are
// fixed-width; native_* describe the exporting API, not the importing API.
struct rn_preview_info {
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

// This header requires C++; exported functions use C linkage for Java FFM.
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
// All previews must already have been successfully destroyed.
RAYNIUM_API void rn_destroy_runtime(rn_runtime* runtime) noexcept;

// One preview per runtime. Success returns complete resources; failure clears a
// valid output handle and leaves info unchanged. Runtime must outlive preview.
RAYNIUM_API rn_status rn_create_preview(rn_runtime* runtime, const rn_preview_desc* desc,
                                     rn_preview** out_preview, rn_preview_info* info, uint32_t info_capacity,
                                     char* error_message, uint32_t error_capacity) noexcept;

// sequence starts at 1 and increases by one. Consumer must submit release and
// signal consumed(sequence) after every successful draw before requesting another.
// Failure submits nothing. All calls run on the runtime's creating thread.
RAYNIUM_API rn_status rn_draw_preview(rn_preview* preview, uint64_t sequence,
                                   char* error_message, uint32_t error_capacity) noexcept;

// Caller first finishes all consumer GPU use and destroys imported objects.
// Failure (GPU timeout) leaves the preview live; never unload its DLL on failure.
// NULL succeeds. Success destroys the preview; its pointer must not be reused.
RAYNIUM_API rn_status rn_destroy_preview(rn_preview* preview,
                                      char* error_message, uint32_t error_capacity) noexcept;

// Two images for direct sharing; mip_levels + 1 for split-mip fallback (lightmap last).
// ready is signaled by native, starting at 1 after initialization/release; consumed
// is signaled by the host after writing. Each successful frame consumes input(n),
// then releases all input images and signals ready(n+1). No CPU pixel transfer.
RAYNIUM_API rn_status rn_create_terrain(rn_runtime* runtime, const rn_terrain_desc* desc,
    rn_terrain** output, rn_preview_info* images, uint32_t image_capacity,
    char* error_message, uint32_t error_capacity) noexcept;
RAYNIUM_API rn_status rn_draw_terrain(rn_preview* preview, rn_terrain* terrain, uint64_t sequence,
    const rn_terrain_frame* frame, char* error_message, uint32_t error_capacity) noexcept;
RAYNIUM_API rn_status rn_destroy_terrain(rn_terrain* terrain, char* error_message, uint32_t error_capacity) noexcept;

}  // extern "C"

#undef RAYNIUM_API
