#pragma once
#include <raynium/bridge.h>
#include <radray/gpu/types.h>
#include <optional>

namespace raynium {
// A missing result is a terminal rendering failure. Completion sequence is tracked independently.
std::optional<rn_present_outcome> ResolveSurfaceOutcome(radray::gpu::GpuBackend backend,
    radray::gpu::SwapChainStatus status,int64_t nativeCode,int64_t acquiredCode=0) noexcept;
void CheckFailedQueueSubmission(bool deviceLost) noexcept;
}
