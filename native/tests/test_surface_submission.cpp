#include <raynium/surface_submission.h>
#include <gtest/gtest.h>

namespace raynium {
using namespace radray::gpu;

TEST(SurfaceSubmission, AcquireAndPresentKeepRetryRecreateAndSurfaceLossDistinct) {
    using S=SwapChainStatus;
    for(auto acquired:{S::Success,S::Suboptimal}) {
        EXPECT_EQ(ResolveSurfaceOutcome(S::Success,acquired),acquired==S::Suboptimal?RN_PRESENTED_SUBOPTIMAL:RN_PRESENTED);
        EXPECT_EQ(ResolveSurfaceOutcome(S::Suboptimal,acquired),RN_PRESENTED_SUBOPTIMAL);
        EXPECT_EQ(ResolveSurfaceOutcome(S::RetryLater,acquired),RN_PRESENT_SKIPPED);
        EXPECT_EQ(ResolveSurfaceOutcome(S::RequireRecreate,acquired),RN_PRESENT_RECREATE);
        EXPECT_EQ(ResolveSurfaceOutcome(S::SurfaceLost,acquired),RN_PRESENT_SURFACE_LOST);
        EXPECT_FALSE(ResolveSurfaceOutcome(S::DeviceLost,acquired));
        EXPECT_FALSE(ResolveSurfaceOutcome(S::Error,acquired));
    }
}
TEST(SurfaceSubmissionDeathTest, HealthyQueueFailureAbortsBeforeDiscard) {
    EXPECT_DEATH(CheckFailedQueueSubmission(false),"");
    CheckFailedQueueSubmission(true);
}
}
