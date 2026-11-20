#include <raynium/surface_submission.h>
#include <gtest/gtest.h>

namespace raynium {
using namespace radray::gpu;

TEST(SurfaceSubmission, AcquireAndPresentKeepRetryRecreateAndSurfaceLossDistinct) {
    constexpr int64_t suboptimal=1000001003,surfaceLost=-1000000000;
    for(auto backend:{GpuBackend::Vulkan,GpuBackend::D3D12}) {
        EXPECT_EQ(ResolveSurfaceOutcome(backend,SwapChainStatus::RetryLater,0),RN_PRESENT_SKIPPED);
        EXPECT_EQ(ResolveSurfaceOutcome(backend,SwapChainStatus::RequireRecreate,0,suboptimal),RN_PRESENT_RECREATE);
        EXPECT_EQ(ResolveSurfaceOutcome(backend,SwapChainStatus::Success,0),RN_PRESENTED);
        EXPECT_FALSE(ResolveSurfaceOutcome(backend,SwapChainStatus::Error,-1));
    }
    EXPECT_EQ(ResolveSurfaceOutcome(GpuBackend::Vulkan,SwapChainStatus::Success,0,suboptimal),RN_PRESENTED_SUBOPTIMAL);
    EXPECT_EQ(ResolveSurfaceOutcome(GpuBackend::Vulkan,SwapChainStatus::Success,suboptimal),RN_PRESENTED_SUBOPTIMAL);
    EXPECT_EQ(ResolveSurfaceOutcome(GpuBackend::Vulkan,SwapChainStatus::Error,surfaceLost),RN_PRESENT_SURFACE_LOST);
    EXPECT_FALSE(ResolveSurfaceOutcome(GpuBackend::D3D12,SwapChainStatus::Error,surfaceLost));
    EXPECT_EQ(ResolveSurfaceOutcome(GpuBackend::D3D12,SwapChainStatus::Success,suboptimal),RN_PRESENTED);
}
TEST(SurfaceSubmissionDeathTest, HealthyQueueFailureAbortsBeforeDiscard) {
    EXPECT_DEATH(CheckFailedQueueSubmission(false),"");
    CheckFailedQueueSubmission(true);
}
}
