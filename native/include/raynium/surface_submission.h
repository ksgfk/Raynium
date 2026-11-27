#pragma once
#include <raynium/bridge.h>
#include <radray/gpu/types.h>
#include <optional>

namespace raynium {
// A missing result is a terminal rendering failure. Completion sequence is tracked independently.
std::optional<rn_present_outcome> ResolveSurfaceOutcome(radray::gpu::SwapChainStatus status,
                                                        radray::gpu::SwapChainStatus acquiredStatus = radray::gpu::SwapChainStatus::Success) noexcept;
void CheckFailedQueueSubmission(bool deviceLost) noexcept;
}  // namespace raynium
