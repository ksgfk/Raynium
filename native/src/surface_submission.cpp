#include <raynium/surface_submission.h>
#include <radray/logger.h>

namespace raynium {
std::optional<rn_present_outcome> ResolveSurfaceOutcome(radray::gpu::SwapChainStatus status,
    radray::gpu::SwapChainStatus acquiredStatus) noexcept {
    using radray::gpu::SwapChainStatus;
    switch(status) {
    case SwapChainStatus::Success: return acquiredStatus==SwapChainStatus::Suboptimal?RN_PRESENTED_SUBOPTIMAL:RN_PRESENTED;
    case SwapChainStatus::Suboptimal: return RN_PRESENTED_SUBOPTIMAL;
    case SwapChainStatus::RequireRecreate: return RN_PRESENT_RECREATE;
    case SwapChainStatus::RetryLater: return RN_PRESENT_SKIPPED;
    case SwapChainStatus::SurfaceLost: return RN_PRESENT_SURFACE_LOST;
    case SwapChainStatus::DeviceLost:
    case SwapChainStatus::Error: return std::nullopt;
    }
    return std::nullopt;
}
void CheckFailedQueueSubmission(bool deviceLost) noexcept {
    if(!deviceLost) RADRAY_ABORT("Command tape rejected on a healthy device");
}
}
