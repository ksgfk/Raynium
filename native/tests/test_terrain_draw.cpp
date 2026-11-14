#include <raynium/terrain_draw.h>
#include <radray/types.h>
#include <raynium/mesh_pass.h>
#include <cstring>
#include <cmath>
#include <gtest/gtest.h>
#include <radray/profiler.h>
#include <limits>
#include <algorithm>
#include <tuple>
#include <span>
#include <utility>

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
        geometry.ranges.resize(32); geometry.fullMask=geometry.validMask=UINT32_MAX; geometry.mergeMask=0x7fffffff;
        for(uint32_t n=0;n<32;++n) geometry.ranges[n]={11+n*6,6,37};
    }
    void Add(uint32_t mask,uint32_t depth=0,uint32_t first=7,uint32_t instances=1,uint32_t layer=0) {
        input[layer].push_back({&geometry,&material,depth,mask,first,instances});
    }
    bool Prepare() {
        for(uint32_t layer=0;layer<3;++layer) {
            lists.visible[layer].clear(); lists.order[layer].clear();
            for(const auto& item:input[layer]) {
                uint32_t stateId=0;
                if(!bindings.Acquire(*item.geometry,*item.material,stateId)) return false;
                auto& payload=lists.visible[layer];
                lists.order[layer].push_back({(uint64_t(stateId)<<32)|item.depth,uint32_t(payload.size())});
                payload.push_back({item.geometry->ranges.data(),item.mask&item.geometry->validMask,item.geometry->mergeMask,item.first,item.instances,stateId});
            }
        }
        return true;
    }
    void Build(bool oit=false) {
        ASSERT_TRUE(Prepare());
        ASSERT_TRUE(BuildTerrainDraws(lists,bindings.States(),oit));
    }
    struct Source { const TerrainDrawTemplate* geometry; const MeshMaterialBinding* material; uint32_t depth,mask,first,instances; };
    radray::array<radray::vector<Source>,3> input;
    TerrainBindingStates bindings;
    MeshMaterialBinding material;
    TerrainDrawTemplate geometry;
    TerrainFrameLists lists;
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
    Add(3,9,1,1,2); Add(3,2,2,1,2); input[2][1].material=&other;
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
    ASSERT_TRUE(Prepare()); EXPECT_FALSE(BuildTerrainDraws(lists,bindings.States(),false));
}
TEST_F(TerrainDrawTest, FullViewsAndMaterialGroupsControlAdjacentBatches) {
    auto second=geometry; second.vertices.Size++;
    Add(1,0,7,1,2); Add(1,1,7,1,2); input[2][1].geometry=&second; Add(1,2,7,1,2);
    Build(); ASSERT_EQ(lists.batches.size(),3);
    EXPECT_EQ(lists.batches[0].first,0); EXPECT_EQ(lists.batches[1].first,1); EXPECT_EQ(lists.batches[2].first,2);
}
TEST_F(TerrainDrawTest, IndexOffsetStrideAndMaterialParameterIdentitySplitBatches) {
    auto offset=geometry,stride=geometry; offset.indices.Offset=16; stride.indices.Stride=2;
    MeshMaterialBinding other; other.parameters=radray::make_unique<Parameters>(); other.sortKey=2;
    Add(1,0,7,1,2); Add(1,1,7,1,2); Add(1,2,7,1,2); Add(1,3,7,1,2);
    input[2][1].geometry=&offset; input[2][2].geometry=&stride; input[2][3].material=&other;
    Build(); ASSERT_EQ(lists.batches.size(),4);
    EXPECT_EQ(lists.batches[1].indices.Offset,16); EXPECT_EQ(lists.batches[2].indices.Stride,2);
    EXPECT_EQ(lists.batches[3].material,&other);
}
TEST_F(TerrainDrawTest, RadixSortKeepsUnsignedDepthOrderAndStableTies) {
    radray::vector<std::pair<uint32_t,uint32_t>> expected;
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
        if(n%2) input[2].back().material=&other;
    }
    Build(true); ASSERT_EQ(lists.arguments.size(),128);
    for(uint32_t i=0;i<64;++i) EXPECT_EQ(lists.arguments[i].FirstInstance,126-i*2);
    for(uint32_t i=0;i<64;++i) EXPECT_EQ(lists.arguments[64+i].FirstInstance,127-i*2);
}
TEST_F(TerrainDrawTest, EqualStateKeyCollisionsRetainOrderAndStillSplitFullViews) {
    auto second=geometry; second.vertices.Size++;
    Add(1,0,3); Add(1,0,4); input[0][1].geometry=&second;
    ASSERT_TRUE(Prepare()); for(auto& item:lists.order[0]) item.sortKey=0;
    ASSERT_TRUE(BuildTerrainDraws(lists,bindings.States(),false)); ASSERT_EQ(lists.arguments.size(),2);
    EXPECT_EQ(lists.arguments[0].FirstInstance,3); EXPECT_EQ(lists.arguments[1].FirstInstance,4);
    EXPECT_EQ(lists.batches.size(),2);
}
TEST_F(TerrainDrawTest, ExactStatesGroupInterleavedIndexViewsForOit) {
    auto other=geometry; other.indices.Offset+=4;
    for(uint32_t n=0;n<128;++n) {
        Add(1,127-n,n,1,2); if(n%2) input[2].back().geometry=&other;
    }
    Build(true); ASSERT_EQ(lists.batches.size(),2);
    EXPECT_EQ(lists.batches[0].count,64); EXPECT_EQ(lists.batches[1].count,64);
    for(uint32_t i=0;i<64;++i) EXPECT_EQ(lists.arguments[i].FirstInstance,126-i*2);
    for(uint32_t i=0;i<64;++i) EXPECT_EQ(lists.arguments[64+i].FirstInstance,127-i*2);
}
TEST_F(TerrainDrawTest, DenseStatesDistinguishMoreThanOldVertexBucketCount) {
    radray::vector<uint32_t> ids;
    for(uint32_t n=0;n<5000;++n) {
        auto view=geometry; view.vertices.Size+=n;
        uint32_t id=0; ASSERT_TRUE(bindings.Acquire(view,material,id)); ids.push_back(id);
    }
    auto sorted=ids; std::sort(sorted.begin(),sorted.end());
    EXPECT_EQ(std::unique(sorted.begin(),sorted.end()),sorted.end());
    EXPECT_EQ(bindings.Count(),5000);
    for(uint32_t n=0;n<5000;++n) {
        auto view=geometry; view.vertices.Size+=n;
        uint32_t id=0; ASSERT_TRUE(bindings.Acquire(view,material,id)); EXPECT_EQ(id,ids[n]);
        bindings.Release(id); bindings.Release(id);
    }
    EXPECT_EQ(bindings.Count(),0);
}
TEST_F(TerrainDrawTest, BindingReferencesPreserveLiveStateAndReuseReleasedIDs) {
    TerrainBindingStates limited{2}; uint32_t a=0,again=0,b=0,c=0;
    ASSERT_TRUE(limited.Acquire(geometry,material,a));
    ASSERT_TRUE(limited.Acquire(geometry,material,again)); EXPECT_EQ(a,again);
    auto second=geometry; second.indices.Offset++;
    auto third=geometry; third.indices.Offset+=2;
    ASSERT_TRUE(limited.Acquire(second,material,b));
    limited.Release(a); EXPECT_FALSE(limited.Acquire(third,material,c));
    limited.Release(again); ASSERT_TRUE(limited.Acquire(third,material,c)); EXPECT_EQ(a,c);
    EXPECT_EQ(limited.States()[b].indices.Offset,second.indices.Offset);
    limited.Release(b); limited.Release(c); EXPECT_EQ(limited.Count(),0);
}
TEST_F(TerrainDrawTest, BindingIdentityIncludesEveryViewFieldAndMaterialVersion) {
    uint32_t base=0; ASSERT_TRUE(bindings.Acquire(geometry,material,base));
    radray::vector<TerrainDrawTemplate> variants(6,geometry);
    variants[0].vertices.Target=reinterpret_cast<radray::render::Buffer*>(uintptr_t(1));
    variants[1].vertices.Offset++; variants[2].vertices.Size++;
    variants[3].indices.Target=reinterpret_cast<radray::render::Buffer*>(uintptr_t(1));
    variants[4].indices.Offset++; variants[5].indices.Stride=2;
    radray::vector<uint32_t> ids{base};
    for(const auto& view:variants) { uint32_t id=0; ASSERT_TRUE(bindings.Acquire(view,material,id)); ids.push_back(id); }
    MeshMaterialBinding other; other.sortKey=material.sortKey; other.parameters=radray::make_unique<Parameters>();
    uint32_t otherId=0; ASSERT_TRUE(bindings.Acquire(geometry,other,otherId)); ids.push_back(otherId);
    material.sortKey=UINT64_MAX;
    uint32_t version=0; ASSERT_TRUE(bindings.Acquire(geometry,material,version)); ids.push_back(version);
    std::sort(ids.begin(),ids.end()); EXPECT_EQ(std::unique(ids.begin(),ids.end()),ids.end());
}
TEST_F(TerrainDrawTest, SortedPayloadsMatchIndependentExpandedDrawReference) {
    using Quad=std::tuple<uint32_t,int32_t,uint32_t,uint32_t>;
    for(bool oit:{false,true}) {
        input={}; lists={}; bindings=TerrainBindingStates{};
        for(uint32_t n=0;n<513;++n) {
            const auto bits=(n*2654435761u)|1u;
            Add(bits,(n%41)*2246822519u,n,n%5==0?3u:1u,2);
        }
        ASSERT_TRUE(Prepare());
        auto reference=lists.order[2];
        std::stable_sort(reference.begin(),reference.end(),[oit](const auto& a,const auto& b) {
            const auto key=[oit](uint64_t k) { return oit?k:(k<<32)|(k>>32); }; return key(a.sortKey)<key(b.sortKey);
        });
        radray::vector<Quad> expected,actual;
        for(const auto& sorted:reference) {
            const auto& source=input[2][sorted.payloadIndex];
            for(uint32_t bit=0;bit<32;++bit) if(source.mask&(1u<<bit))
                expected.emplace_back(geometry.ranges[bit].first,geometry.ranges[bit].vertex,source.first,source.instances);
        }
        ASSERT_TRUE(BuildTerrainDraws(lists,bindings.States(),oit));
        for(const auto& arg:lists.arguments) for(uint32_t offset=0;offset<arg.IndexCount;offset+=6)
            actual.emplace_back(arg.FirstIndex+offset,arg.VertexOffset,arg.FirstInstance,arg.InstanceCount);
        EXPECT_EQ(actual,expected);
    }
}
TEST(TerrainIndirect, SmallNativeLimitSplitsAllRecordsWithoutLosingOffset) {
    uint32_t offset=7,remaining=8;
    radray::vector<uint32_t> offsets,counts;
    while(remaining) { auto count=TerrainIndirectChunk(remaining,3); offsets.push_back(offset); counts.push_back(count); offset+=count; remaining-=count; }
    EXPECT_EQ(offsets,(radray::vector<uint32_t>{7,10,13})); EXPECT_EQ(counts,(radray::vector<uint32_t>{3,3,2}));
    EXPECT_EQ(TerrainIndirectChunk(8,0),0); EXPECT_EQ(TerrainIndirectChunk(8,1),1);
}
TEST(TerrainSubmission, AutoRequiresEveryCapabilityAndForcedModesRespectFailures) {
    radray::render::RenderDeviceCapabilities caps;
    bool indirect=true;
    EXPECT_TRUE(SelectTerrainSubmission(0,caps,indirect)); EXPECT_FALSE(indirect);
    caps.Features.IndirectDraw=true; caps.Features.IndirectFirstInstance=true;
    for(auto limit:{0u,1u}) {
        caps.Limits.MaxDrawIndirectCount=limit;
        EXPECT_TRUE(SelectTerrainSubmission(0,caps,indirect)); EXPECT_FALSE(indirect);
        EXPECT_FALSE(SelectTerrainSubmission(RN_RENDERER_INDIRECT,caps,indirect));
    }
    caps.Limits.MaxDrawIndirectCount=4;
    EXPECT_TRUE(SelectTerrainSubmission(0,caps,indirect)); EXPECT_TRUE(indirect);
    EXPECT_TRUE(SelectTerrainSubmission(RN_RENDERER_DIRECT,caps,indirect)); EXPECT_FALSE(indirect);
    caps.Features.IndirectFirstInstance=false;
    EXPECT_FALSE(SelectTerrainSubmission(RN_RENDERER_INDIRECT,caps,indirect));
    EXPECT_FALSE(SelectTerrainSubmission(3,caps,indirect)); EXPECT_FALSE(SelectTerrainSubmission(4,caps,indirect));
}
}
}
namespace raynium {
namespace {
bool AnalyzeAndWrite(std::span<const radray::byte> source,rn_material_mode layer,TerrainMeshLayout& layout,radray::vector<radray::byte>& bytes) {
    std::pmr::monotonic_buffer_resource memory;
    TerrainMeshUpload upload{memory};
    if(!AnalyzeTerrainMesh(source,layer,layout,upload)) return false;
    bytes.resize(source.size()); return upload.Write(bytes);
}
radray::vector<radray::byte> Quad(uint32_t group,float plane=0) {
    float points[4][3]{};
    if(group<6) {
        const uint32_t a=group/2,b=(a+1)%3,c=(a+2)%3;
        for(auto& point:points) point[a]=plane;
        points[1][b]=1; points[2][b]=1; points[2][c]=1; points[3][c]=1;
        if(group&1) for(uint32_t i=0;i<3;i++) std::swap(points[1][i],points[3][i]);
    }
    radray::vector<radray::byte> bytes(112);
    for(uint32_t v=0;v<4;++v) {
        std::memcpy(bytes.data()+v*28,points[v],12);
        const uint32_t color=0x12340000+group; std::memcpy(bytes.data()+v*28+12,&color,4);
    }
    return bytes;
}
TEST(TerrainMeshAnalysis, ReordersSevenGroupsWithoutChangingVertexBytesOrWithinGroupOrder) {
    radray::vector<radray::byte> source(13); for(int g=6;g>=0;--g) { auto q=Quad(g); source.insert(source.end(),q.begin(),q.end()); }
    auto q=Quad(0,2); source.insert(source.end(),q.begin(),q.end());
    TerrainMeshLayout layout; radray::vector<radray::byte> bytes;
    ASSERT_TRUE(AnalyzeAndWrite(std::span{source}.subspan(13),RN_MATERIAL_OPAQUE,layout,bytes));
    ASSERT_EQ(layout.ranges.size(),7); EXPECT_EQ(layout.ranges[0].count,12);
    EXPECT_EQ(layout.groupBits,(radray::array<uint32_t,7>{1,2,4,8,16,32,64}));
    radray::vector<radray::byte> expected;
    for(uint32_t g=0;g<7;g++) { auto first=Quad(g); expected.insert(expected.end(),first.begin(),first.end()); if(g==0) expected.insert(expected.end(),q.begin(),q.end()); }
    EXPECT_EQ(bytes,expected);
    rn_render_frame_desc frame{}; rn_instance instance{};
    frame.camera_fraction[0]=frame.camera_fraction[1]=frame.camera_fraction[2]=-2;
    EXPECT_EQ(TerrainVisibleMask(layout,{{},true},frame,{&instance,1}),0b1101010u);
    frame.camera_fraction[0]=3; frame.camera_fraction[1]=frame.camera_fraction[2]=2;
    EXPECT_EQ(TerrainVisibleMask(layout,{{},true},frame,{&instance,1}),0b1010101u);
}
TEST(TerrainMeshAnalysis, OutlyingBoundsSparseGroupsAndEveryInstanceRemainConservative) {
    auto source=Quad(0,32); TerrainMeshLayout layout; radray::vector<radray::byte> bytes;
    ASSERT_TRUE(AnalyzeAndWrite(source,RN_MATERIAL_CUTOUT,layout,bytes));
    EXPECT_EQ(layout.minimum[0],32); EXPECT_EQ(layout.maximum[0],32);
    rn_render_frame_desc frame{}; const rn_instance instances[]{{{-31,0,0},1},{{-33,0,0},1}};
    EXPECT_EQ(TerrainVisibleMask(layout,{{},true},frame,{instances,1}),0u);
    EXPECT_EQ(TerrainVisibleMask(layout,{{},true},frame,instances),1u);
    frame.camera_block[0]=INT32_MAX; rn_instance far{{INT32_MAX-32,0,0},1};
    EXPECT_EQ(TerrainVisibleMask(layout,{{},true},frame,{&far,1}),1u);
    frame.camera_fraction[0]=std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(TerrainVisibleMask(layout,{{},true},frame,{&far,1}),1u);
    source=Quad(5); auto second=Quad(2); source.insert(source.end(),second.begin(),second.end());
    ASSERT_TRUE(AnalyzeAndWrite(source,RN_MATERIAL_OPAQUE,layout,bytes));
    EXPECT_EQ(layout.groupBits[2],1); EXPECT_EQ(layout.groupBits[5],2);
    frame={}; frame.camera_fraction[1]=-2; frame.camera_fraction[2]=2; rn_instance origin{};
    EXPECT_EQ(TerrainVisibleMask(layout,{{},true},frame,{&origin,1}),0u);
}
TEST(TerrainMeshAnalysis, NonplanarInconsistentDegenerateAndNonfiniteQuadsStayGeneral) {
    auto source=Quad(0); float value=.1f; std::memcpy(source.data()+56,&value,4);
    TerrainMeshLayout layout; radray::vector<radray::byte> bytes;
    ASSERT_TRUE(AnalyzeAndWrite(source,RN_MATERIAL_OPAQUE,layout,bytes)); EXPECT_EQ(layout.groupBits[6],1);
    source=Quad(0); value=2; std::memcpy(source.data()+84+4,&value,4);
    ASSERT_TRUE(AnalyzeAndWrite(source,RN_MATERIAL_OPAQUE,layout,bytes)); EXPECT_EQ(layout.groupBits[6],1);
    source=Quad(6); ASSERT_TRUE(AnalyzeAndWrite(source,RN_MATERIAL_OPAQUE,layout,bytes)); EXPECT_EQ(layout.groupBits[6],1);
    source=Quad(0); value=std::numeric_limits<float>::quiet_NaN(); std::memcpy(source.data(),&value,4);
    ASSERT_TRUE(AnalyzeAndWrite(source,RN_MATERIAL_OPAQUE,layout,bytes)); EXPECT_FALSE(layout.finiteBounds);
    rn_render_frame_desc frame{}; rn_instance instance{};
    EXPECT_EQ(TerrainVisibleMask(layout,{{},true},frame,{&instance,1}),1u);
    EXPECT_TRUE(AnalyzeAndWrite({},RN_MATERIAL_OPAQUE,layout,bytes)); EXPECT_TRUE(layout.ranges.empty());
    EXPECT_FALSE(AnalyzeAndWrite(std::span{source}.first(111),RN_MATERIAL_OPAQUE,layout,bytes));
}
TEST(TerrainMeshAnalysis, TranslucencyPreservesCompilerOrder) {
    auto source=Quad(5); auto q=Quad(0); source.insert(source.end(),q.begin(),q.end());
    TerrainMeshLayout layout; radray::vector<radray::byte> bytes;
    ASSERT_TRUE(AnalyzeAndWrite(source,RN_MATERIAL_TRANSLUCENT,layout,bytes));
    EXPECT_EQ(bytes,source); ASSERT_EQ(layout.ranges.size(),1); EXPECT_EQ(layout.ranges[0].count,12);
}
TEST(TerrainMeshAnalysis, InverseProjectionEyeIncludesTranslationAndRejectsInvalidMatrices) {
    rn_render_frame_desc frame{};
    for(uint32_t i=0;i<4;i++) frame.projection[i*5]=frame.view[i*5]=1;
    EXPECT_FALSE(ComputeTerrainEye(frame).valid);
    frame.projection[10]=frame.projection[15]=0; frame.projection[11]=-1; frame.projection[14]=1; frame.projection[12]=-2;
    auto eye=ComputeTerrainEye(frame); ASSERT_TRUE(eye.valid); EXPECT_NEAR(eye.offset[0],2,1e-6);
    frame.view[12]=-3; eye=ComputeTerrainEye(frame); ASSERT_TRUE(eye.valid); EXPECT_NEAR(eye.offset[0],5,1e-6);
    frame.projection[0]=std::numeric_limits<float>::infinity(); EXPECT_FALSE(ComputeTerrainEye(frame).valid);
    frame={}; EXPECT_FALSE(ComputeTerrainEye(frame).valid);
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
