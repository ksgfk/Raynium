#include <raynium/terrain_draw.h>
#include <raynium/mesh_pass.h>
#include <gtest/gtest.h>
#include <radray/profiler.h>
#include <limits>
#include <algorithm>

namespace raynium {
namespace {
class Parameters final : public radray::render::ShaderParameterSet {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    bool Set(uint32_t,uint32_t,radray::render::ShaderParameterValue) noexcept override { return true; }
    bool FlushWrites() noexcept override { return true; }
    void ResetBindingsForReuse() noexcept override {}
};
class TerrainDrawTest : public testing::Test {
protected:
    TerrainDrawTest() {
        material.parameters=radray::make_unique<Parameters>(); material.sortKey=1;
        geometry.vertices={nullptr,28,4096}; geometry.indices={nullptr,12,4};
        geometry.fullMask=geometry.validMask=UINT32_MAX; geometry.mergeMask=0x7fffffff;
        for(uint32_t n=0;n<32;++n) geometry.ranges[n]={11+n*6,6,37};
    }
    void Add(uint32_t mask,uint32_t depth=0,uint32_t first=7,uint32_t instances=1,uint32_t layer=0) {
        lists.visible[layer].push_back({&geometry,&material,(uint64_t(TerrainStateSortKey(geometry,material))<<32)|depth,mask,first,instances});
    }
    void Build(bool oit=false) {
        for(auto& layer:lists.visible) for(auto& item:layer)
            item.sortKey=(uint64_t(TerrainStateSortKey(*item.geometry,*item.material))<<32)|uint32_t(item.sortKey);
        ASSERT_TRUE(BuildTerrainDraws(lists,oit,error))<<error.c_str();
    }
    MeshMaterialBinding material;
    TerrainDrawTemplate geometry;
    TerrainFrameLists lists;
    radray::string error;
};
TEST_F(TerrainDrawTest, NoncontiguousMaskAndHighestBitKeepFaceOrderAndOffsets) {
    Add(0x80000005); Build(); ASSERT_EQ(lists.arguments.size(),3);
    EXPECT_EQ(lists.arguments[0].FirstIndex,11); EXPECT_EQ(lists.arguments[1].FirstIndex,23);
    EXPECT_EQ(lists.arguments[2].FirstIndex,197); EXPECT_EQ(lists.arguments[2].VertexOffset,37);
    EXPECT_EQ(lists.arguments[2].FirstInstance,7); EXPECT_EQ(lists.batches[0].vertices.Offset,28);
    EXPECT_EQ(lists.batches[0].indices.Offset,12); EXPECT_EQ(lists.layers[0].selectedGroups,3);
}
TEST_F(TerrainDrawTest, FullMaskMergesCompatibleSingleInstanceGroups) {
    Add(UINT32_MAX); Build(); ASSERT_EQ(lists.arguments.size(),1);
    EXPECT_EQ(lists.arguments[0].IndexCount,192); EXPECT_EQ(lists.layers[0].selectedGroups,32);
    EXPECT_EQ(lists.layers[0].indicesInstances,192);
}
TEST_F(TerrainDrawTest, DuplicateItemsAndEqualSortKeysRetainInputOrder) {
    Add(1,4,9); Add(1,4,8); Add(1,4,9); Build(); ASSERT_EQ(lists.arguments.size(),3);
    EXPECT_EQ(lists.arguments[0].FirstInstance,9); EXPECT_EQ(lists.arguments[1].FirstInstance,8);
    EXPECT_EQ(lists.arguments[2].FirstInstance,9); ASSERT_EQ(lists.batches.size(),1);
    EXPECT_EQ(lists.batches[0].count,3);
}
TEST_F(TerrainDrawTest, MultipleInstancesKeepSeparateGroups) {
    Add(3,0,5,4); Build(); ASSERT_EQ(lists.arguments.size(),2);
    EXPECT_EQ(lists.arguments[1].InstanceCount,4); EXPECT_EQ(lists.arguments[1].FirstInstance,5);
    EXPECT_EQ(lists.layers[0].indicesInstances,48);
}
TEST_F(TerrainDrawTest, OrdinaryTransparencyUsesDepthBeforeStateAndDoesNotMerge) {
    MeshMaterialBinding other; other.parameters=radray::make_unique<Parameters>(); other.sortKey=2;
    Add(3,9,1,1,2); Add(3,2,2,1,2); lists.visible[2][1].material=&other;
    Build(); ASSERT_EQ(lists.arguments.size(),4); EXPECT_EQ(lists.arguments[0].FirstInstance,2);
    EXPECT_EQ(lists.arguments[2].FirstInstance,1);
}
TEST_F(TerrainDrawTest, OitUsesOneStateSortedTransparentArgumentRange) {
    Add(3,9,1,1,2); Add(3,2,2,1,2); Build(true);
    ASSERT_EQ(lists.arguments.size(),2); EXPECT_EQ(lists.arguments[0].FirstInstance,2);
    EXPECT_EQ(lists.arguments[0].IndexCount,12); EXPECT_EQ(lists.layers[2].recordCount,2);
}
TEST_F(TerrainDrawTest, MergeMaskAndVertexBoundariesBreakRuns) {
    geometry.mergeMask=1; geometry.ranges[2].vertex=40;
    Add(7); Build(); ASSERT_EQ(lists.arguments.size(),2);
    EXPECT_EQ(lists.arguments[0].IndexCount,12); EXPECT_EQ(lists.arguments[1].VertexOffset,40);
}
TEST_F(TerrainDrawTest, EmptyGroupsAreSkippedWithoutBridgingUnselectedGroups) {
    geometry.validMask&=~2u; geometry.mergeMask=0;
    Add(7); Build(); ASSERT_EQ(lists.arguments.size(),2); EXPECT_EQ(lists.layers[0].selectedGroups,2);
}
TEST_F(TerrainDrawTest, IndexCountOverflowSplitsInsteadOfWrapping) {
    geometry.ranges[0].count=UINT32_MAX-2; geometry.ranges[1].count=6;
    Add(3); Build(); ASSERT_EQ(lists.arguments.size(),2);
    EXPECT_EQ(lists.arguments[0].IndexCount,UINT32_MAX-2); EXPECT_EQ(lists.arguments[1].IndexCount,6);
}
TEST_F(TerrainDrawTest, SubmittedIndexTotalOverflowIsRejected) {
    geometry.ranges[0].count=UINT32_MAX; Add(1,0,0,UINT32_MAX); Add(1,0,0,UINT32_MAX);
    EXPECT_FALSE(BuildTerrainDraws(lists,false,error)); EXPECT_NE(error.find("overflow"),radray::string::npos);
}
TEST_F(TerrainDrawTest, FullViewsAndMaterialGroupsControlAdjacentBatches) {
    auto second=geometry; second.vertices.Size++;
    Add(1,0,7,1,2); Add(1,1,7,1,2); lists.visible[2][1].geometry=&second; Add(1,2,7,1,2);
    Build(); ASSERT_EQ(lists.batches.size(),3);
    EXPECT_EQ(lists.batches[0].first,0); EXPECT_EQ(lists.batches[1].first,1); EXPECT_EQ(lists.batches[2].first,2);
}
TEST_F(TerrainDrawTest, IndexOffsetStrideAndMaterialParameterIdentitySplitBatches) {
    auto offset=geometry,stride=geometry; offset.indices.Offset=16; stride.indices.Stride=2;
    MeshMaterialBinding other; other.parameters=radray::make_unique<Parameters>(); other.sortKey=2;
    Add(1,0,7,1,2); Add(1,1,7,1,2); Add(1,2,7,1,2); Add(1,3,7,1,2);
    lists.visible[2][1].geometry=&offset; lists.visible[2][2].geometry=&stride; lists.visible[2][3].material=&other;
    Build(); ASSERT_EQ(lists.batches.size(),4);
    EXPECT_EQ(lists.batches[1].indices.Offset,16); EXPECT_EQ(lists.batches[2].indices.Stride,2);
    EXPECT_EQ(lists.batches[3].material,&other);
}
TEST_F(TerrainDrawTest, RadixSortKeepsUnsignedDepthOrderAndStableTies) {
    std::vector<std::pair<uint32_t,uint32_t>> expected;
    for(uint32_t n=0;n<1024;++n) {
        const uint32_t depth=(n%257)*2654435761u;
        Add(1,depth,n,1,2); expected.emplace_back(depth,n);
    }
    std::stable_sort(expected.begin(),expected.end(),[](const auto& a,const auto& b) { return a.first<b.first; });
    Build(); ASSERT_EQ(lists.arguments.size(),expected.size());
    for(size_t i=0;i<expected.size();++i) EXPECT_EQ(lists.arguments[i].FirstInstance,expected[i].second);
}
TEST_F(TerrainDrawTest, RadixSortPrioritizesStateThenDepthForOpaqueAndOit) {
    MeshMaterialBinding other; other.sortKey=1u<<19; other.parameters=radray::make_unique<Parameters>();
    for(uint32_t n=0;n<128;++n) {
        Add(1,127-n,n,1,2);
        if(n%2) lists.visible[2].back().material=&other;
    }
    Build(true); ASSERT_EQ(lists.arguments.size(),128);
    for(uint32_t i=0;i<64;++i) EXPECT_EQ(lists.arguments[i].FirstInstance,126-i*2);
    for(uint32_t i=0;i<64;++i) EXPECT_EQ(lists.arguments[64+i].FirstInstance,127-i*2);
}
TEST_F(TerrainDrawTest, EqualStateKeyCollisionsRetainOrderAndStillSplitFullViews) {
    auto second=geometry; second.vertices.Size++;
    Add(1,0,3); Add(1,0,4); lists.visible[0][1].geometry=&second;
    ASSERT_TRUE(BuildTerrainDraws(lists,false,error)); ASSERT_EQ(lists.arguments.size(),2);
    EXPECT_EQ(lists.arguments[0].FirstInstance,3); EXPECT_EQ(lists.arguments[1].FirstInstance,4);
    EXPECT_EQ(lists.batches.size(),2);
}
TEST(TerrainIndirect, SmallNativeLimitSplitsAllRecordsWithoutLosingOffset) {
    uint32_t offset=7,remaining=8;
    std::vector<uint32_t> offsets,counts;
    while(remaining) { auto count=TerrainIndirectChunk(remaining,3); offsets.push_back(offset); counts.push_back(count); offset+=count; remaining-=count; }
    EXPECT_EQ(offsets,(std::vector<uint32_t>{7,10,13})); EXPECT_EQ(counts,(std::vector<uint32_t>{3,3,2}));
    EXPECT_EQ(TerrainIndirectChunk(8,0),0); EXPECT_EQ(TerrainIndirectChunk(8,1),1);
}
TEST(TerrainSubmission, AutoRequiresEveryCapabilityAndForcedModesRespectFailures) {
    radray::render::RenderDeviceCapabilities caps;
    bool indirect=true; radray::string reason;
    EXPECT_TRUE(SelectTerrainSubmission(0,caps,indirect,reason)); EXPECT_FALSE(indirect); EXPECT_FALSE(reason.empty());
    caps.Features.IndirectDraw=true; caps.Features.IndirectFirstInstance=true;
    for(auto limit:{0u,1u}) {
        caps.Limits.MaxDrawIndirectCount=limit;
        EXPECT_TRUE(SelectTerrainSubmission(0,caps,indirect,reason)); EXPECT_FALSE(indirect);
        EXPECT_FALSE(SelectTerrainSubmission(RN_RENDERER_INDIRECT,caps,indirect,reason));
    }
    caps.Limits.MaxDrawIndirectCount=4;
    EXPECT_TRUE(SelectTerrainSubmission(0,caps,indirect,reason)); EXPECT_TRUE(indirect); EXPECT_TRUE(reason.empty());
    EXPECT_TRUE(SelectTerrainSubmission(RN_RENDERER_DIRECT,caps,indirect,reason)); EXPECT_FALSE(indirect);
    caps.Features.IndirectFirstInstance=false;
    EXPECT_FALSE(SelectTerrainSubmission(RN_RENDERER_INDIRECT,caps,indirect,reason)); EXPECT_NE(reason.find("firstInstance"),radray::string::npos);
    EXPECT_FALSE(SelectTerrainSubmission(3,caps,indirect,reason)); EXPECT_FALSE(SelectTerrainSubmission(4,caps,indirect,reason));
}
}
}
int main(int argc,char** argv) {
#ifdef RADRAY_ENABLE_PROFILER
    tracy::StartupProfiler();
#endif
    testing::InitGoogleTest(&argc,argv);
    const int result=RUN_ALL_TESTS();
#ifdef RADRAY_ENABLE_PROFILER
    tracy::ShutdownProfiler();
#endif
    return result;
}
