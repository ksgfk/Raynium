#pragma once

#include <stdint.h>

#define RN_ABI_VERSION 3u

// Strings are UTF-8, zero-terminated and stored inline; no allocation crosses this ABI.
typedef struct rn_abi_info {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t pointer_size;
    uint32_t flags; // bit 0: dirty RadRay; bit 1: static CRT; bit 2: mimalloc enabled.
    char build_id[72];
    char radray_commit[48];
    char configuration[16];
    char platform[24];
    char toolchain[64];
} rn_abi_info;

#define RN_STATUS_OK 0u
#define RN_STATUS_FAILED 1u

#define RN_BACKEND_D3D12 1u
#define RN_BACKEND_VULKAN 2u

typedef struct rn_runtime rn_runtime;

typedef struct rn_runtime_desc {
    uint32_t struct_size;
    uint32_t backend;
} rn_runtime_desc;

#if defined(RAYNIUM_RUNTIME_EXPORTS)
#define RAYNIUM_API __declspec(dllexport)
#else
#define RAYNIUM_API __declspec(dllimport)
#endif

#if defined(__cplusplus)
#define RAYNIUM_NOEXCEPT noexcept
extern "C" {
#else
#define RAYNIUM_NOEXCEPT
#endif

RAYNIUM_API uint32_t rn_get_abi_version(void) RAYNIUM_NOEXCEPT;

// All status-returning calls return only OK/FAILED; diagnostics describe the reason.
// The caller owns non-overlapping info/diagnostic buffers. Failure leaves info unchanged.
// Diagnostics are truncated and NUL-terminated when capacity > 0; NULL requires zero capacity.
RAYNIUM_API uint32_t rn_query_abi(rn_abi_info* info, uint32_t capacity,
                                char* error_message, uint32_t error_capacity) RAYNIUM_NOEXCEPT;

// One active runtime per client. All calls are serialized on its creating thread.
// Inputs are borrowed only for this call; buffers must not overlap. struct_size must
// equal sizeof(rn_runtime_desc). A valid out_runtime is set to NULL on failure.
// Diagnostics are UTF-8, truncated and NUL-terminated when capacity > 0. A NULL
// error_message requires zero capacity. No exception may cross this boundary.
RAYNIUM_API uint32_t rn_create_runtime(const rn_runtime_desc* desc, rn_runtime** out_runtime,
                                     char* error_message, uint32_t error_capacity) RAYNIUM_NOEXCEPT;

// NULL is a no-op. A live pointer must be destroyed exactly once, before DLL unload.
RAYNIUM_API void rn_destroy_runtime(rn_runtime* runtime) RAYNIUM_NOEXCEPT;

#if defined(__cplusplus)
}
#endif

#undef RAYNIUM_NOEXCEPT
#undef RAYNIUM_API
