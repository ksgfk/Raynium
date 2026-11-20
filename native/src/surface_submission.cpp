#include <raynium/surface_submission.h>
#include <radray/logger.h>

namespace raynium {
std::optional<rn_present_outcome> ResolveSurfaceOutcome(radray::gpu::GpuBackend backend,
    radray::gpu::SwapChainStatus status,int64_t nativeCode,int64_t acquiredCode) noexcept {
    using namespace radray::gpu;
    constexpr int64_t surfaceLost=-1000000000,suboptimal=1000001003;
    const bool vulkan=backend==GpuBackend::Vulkan;
    switch(status) {
    case SwapChainStatus::Success:
        return vulkan && (nativeCode==suboptimal || acquiredCode==suboptimal)?RN_PRESENTED_SUBOPTIMAL:RN_PRESENTED;
    case SwapChainStatus::RequireRecreate: return RN_PRESENT_RECREATE;
    case SwapChainStatus::RetryLater: return RN_PRESENT_SKIPPED;
    case SwapChainStatus::Error:
        if(vulkan && nativeCode==surfaceLost) return RN_PRESENT_SURFACE_LOST;
        return std::nullopt;
    }
    return std::nullopt;
}
void CheckFailedQueueSubmission(bool deviceLost) noexcept {
    if(!deviceLost) RADRAY_ABORT("Command tape rejected on a healthy device");
}
}
