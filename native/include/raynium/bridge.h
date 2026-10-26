#pragma once

#include <stdint.h>

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
#define RN_STATUS_INVALID_ARGUMENT 1u
#define RN_STATUS_BUFFER_TOO_SMALL 2u

#if defined(RAYNIUM_BRIDGE_EXPORTS)
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

// The caller owns info. On failure no output bytes are modified.
RAYNIUM_API uint32_t rn_query_abi(rn_abi_info* info, uint32_t capacity) RAYNIUM_NOEXCEPT;

// Returns RadRay's FLOAT32X3 vertex size in bytes without creating a GPU device.
RAYNIUM_API uint32_t rn_probe_vertex_stride(void) RAYNIUM_NOEXCEPT;

// Bits: CRT malloc=1, RadRay aligned allocation=2, C++ new=4; success is 7.
RAYNIUM_API uint32_t rn_probe_allocator(void) RAYNIUM_NOEXCEPT;

#if defined(__cplusplus)
}
#endif

#undef RAYNIUM_NOEXCEPT
#undef RAYNIUM_API
