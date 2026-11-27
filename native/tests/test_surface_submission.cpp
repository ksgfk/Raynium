#include <gtest/gtest.h>
#include <raynium/surface_submission.h>

namespace raynium {
TEST(SurfaceSubmission, AcquireAndPresentKeepRetryRecreateAndSurfaceLossDistinct) {
    using Status = radray::gpu::SwapChainStatus;
    for (auto acquired : {Status::Success, Status::Suboptimal}) {
        EXPECT_EQ(ResolveSurfaceOutcome(Status::Success, acquired), acquired == Status::Suboptimal ? RN_PRESENTED_SUBOPTIMAL : RN_PRESENTED);
        EXPECT_EQ(ResolveSurfaceOutcome(Status::Suboptimal, acquired), RN_PRESENTED_SUBOPTIMAL);
        EXPECT_EQ(ResolveSurfaceOutcome(Status::RetryLater, acquired), RN_PRESENT_SKIPPED);
        EXPECT_EQ(ResolveSurfaceOutcome(Status::RequireRecreate, acquired), RN_PRESENT_RECREATE);
        EXPECT_EQ(ResolveSurfaceOutcome(Status::SurfaceLost, acquired), RN_PRESENT_SURFACE_LOST);
        EXPECT_FALSE(ResolveSurfaceOutcome(Status::DeviceLost, acquired));
        EXPECT_FALSE(ResolveSurfaceOutcome(Status::Error, acquired));
    }
}

TEST(SurfaceSubmissionDeathTest, HealthyQueueFailureAbortsBeforeDiscard) {
    EXPECT_DEATH(CheckFailedQueueSubmission(false), "");
    CheckFailedQueueSubmission(true);
}
}  // namespace raynium
