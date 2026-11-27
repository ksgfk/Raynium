#include <raynium/surface_submission.h>

#include <radray/logger.h>

namespace raynium {
std::optional<rn_present_outcome> ResolveSurfaceOutcome(
    radray::gpu::SwapChainStatus status,
    radray::gpu::SwapChainStatus acquiredStatus) noexcept {
    switch (status) {
        case radray::gpu::SwapChainStatus::Success:
            return acquiredStatus == radray::gpu::SwapChainStatus::Suboptimal ? RN_PRESENTED_SUBOPTIMAL : RN_PRESENTED;
        case radray::gpu::SwapChainStatus::Suboptimal: return RN_PRESENTED_SUBOPTIMAL;
        case radray::gpu::SwapChainStatus::RequireRecreate: return RN_PRESENT_RECREATE;
        case radray::gpu::SwapChainStatus::RetryLater: return RN_PRESENT_SKIPPED;
        case radray::gpu::SwapChainStatus::SurfaceLost: return RN_PRESENT_SURFACE_LOST;
        case radray::gpu::SwapChainStatus::DeviceLost:
        case radray::gpu::SwapChainStatus::Error: return std::nullopt;
    }
    return std::nullopt;
}

void CheckFailedQueueSubmission(bool deviceLost) noexcept {
    if (!deviceLost) RADRAY_ABORT("Command tape rejected on a healthy device");
}
}  // namespace raynium
