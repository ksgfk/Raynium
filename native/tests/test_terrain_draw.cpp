#include <raynium/terrain_draw.h>
#include <raynium/mesh_pass.h>
#include <raynium/terrain_lookup.h>
#include <gtest/gtest.h>
#include <radray/profiler.h>
#include <limits>
#include <algorithm>
#include <tuple>

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
                if(!bindings.Acquire(*item.geometry,*item.material,stateId,error)) return false;
                auto& payload=lists.visible[layer];
                lists.order[layer].push_back({(uint64_t(stateId)<<32)|item.depth,uint32_t(payload.size())});
                payload.push_back({item.geometry->ranges.data(),item.mask&item.geometry->validMask,item.geometry->mergeMask,item.first,item.instances,stateId});
            }
        }
        return true;
    }
    void Build(bool oit=false) {
        ASSERT_TRUE(Prepare())<<error.c_str();
        ASSERT_TRUE(BuildTerrainDraws(lists,bindings.States(),oit,error))<<error.c_str();
    }
    struct Source { const TerrainDrawTemplate* geometry; const MeshMaterialBinding* material; uint32_t depth,mask,first,instances; };
    std::array<std::vector<Source>,3> input;
    TerrainBindingStates bindings;
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
    ASSERT_TRUE(Prepare()); EXPECT_FALSE(BuildTerrainDraws(lists,bindings.States(),false,error)); EXPECT_NE(error.find("overflow"),radray::string::npos);
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
    ASSERT_TRUE(BuildTerrainDraws(lists,bindings.States(),false,error)); ASSERT_EQ(lists.arguments.size(),2);
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
    std::vector<uint32_t> ids;
    for(uint32_t n=0;n<5000;++n) {
        auto view=geometry; view.vertices.Size+=n;
        uint32_t id=0; ASSERT_TRUE(bindings.Acquire(view,material,id,error)); ids.push_back(id);
    }
    auto sorted=ids; std::sort(sorted.begin(),sorted.end());
    EXPECT_EQ(std::unique(sorted.begin(),sorted.end()),sorted.end());
    EXPECT_EQ(bindings.Count(),5000);
    for(uint32_t n=0;n<5000;++n) {
        auto view=geometry; view.vertices.Size+=n;
        uint32_t id=0; ASSERT_TRUE(bindings.Acquire(view,material,id,error)); EXPECT_EQ(id,ids[n]);
        bindings.Release(id); bindings.Release(id);
    }
    EXPECT_EQ(bindings.Count(),0);
}
TEST_F(TerrainDrawTest, BindingReferencesPreserveLiveStateAndReuseReleasedIDs) {
    TerrainBindingStates limited{2}; uint32_t a=0,again=0,b=0,c=0;
    ASSERT_TRUE(limited.Acquire(geometry,material,a,error));
    ASSERT_TRUE(limited.Acquire(geometry,material,again,error)); EXPECT_EQ(a,again);
    auto second=geometry; second.indices.Offset++;
    auto third=geometry; third.indices.Offset+=2;
    ASSERT_TRUE(limited.Acquire(second,material,b,error));
    limited.Release(a); EXPECT_FALSE(limited.Acquire(third,material,c,error));
    EXPECT_NE(error.find("exhausted"),radray::string::npos);
    limited.Release(again); ASSERT_TRUE(limited.Acquire(third,material,c,error)); EXPECT_EQ(a,c);
    EXPECT_EQ(limited.States()[b].indices.Offset,second.indices.Offset);
    limited.Release(b); limited.Release(c); EXPECT_EQ(limited.Count(),0);
}
TEST_F(TerrainDrawTest, BindingIdentityIncludesEveryViewFieldAndMaterialVersion) {
    uint32_t base=0; ASSERT_TRUE(bindings.Acquire(geometry,material,base,error));
    std::vector<TerrainDrawTemplate> variants(6,geometry);
    variants[0].vertices.Target=reinterpret_cast<radray::render::Buffer*>(uintptr_t(1));
    variants[1].vertices.Offset++; variants[2].vertices.Size++;
    variants[3].indices.Target=reinterpret_cast<radray::render::Buffer*>(uintptr_t(1));
    variants[4].indices.Offset++; variants[5].indices.Stride=2;
    std::vector<uint32_t> ids{base};
    for(const auto& view:variants) { uint32_t id=0; ASSERT_TRUE(bindings.Acquire(view,material,id,error)); ids.push_back(id); }
    MeshMaterialBinding other; other.sortKey=material.sortKey; other.parameters=radray::make_unique<Parameters>();
    uint32_t otherId=0; ASSERT_TRUE(bindings.Acquire(geometry,other,otherId,error)); ids.push_back(otherId);
    material.sortKey=UINT64_MAX;
    uint32_t version=0; ASSERT_TRUE(bindings.Acquire(geometry,material,version,error)); ids.push_back(version);
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
        std::vector<Quad> expected,actual;
        for(const auto& sorted:reference) {
            const auto& source=input[2][sorted.payloadIndex];
            for(uint32_t bit=0;bit<32;++bit) if(source.mask&(1u<<bit))
                expected.emplace_back(geometry.ranges[bit].first,geometry.ranges[bit].vertex,source.first,source.instances);
        }
        ASSERT_TRUE(BuildTerrainDraws(lists,bindings.States(),oit,error));
        for(const auto& arg:lists.arguments) for(uint32_t offset=0;offset<arg.IndexCount;offset+=6)
            actual.emplace_back(arg.FirstIndex+offset,arg.VertexOffset,arg.FirstInstance,arg.InstanceCount);
        EXPECT_EQ(actual,expected);
    }
}
TEST(TerrainIndirect, SmallNativeLimitSplitsAllRecordsWithoutLosingOffset) {
    uint32_t offset=7,remaining=8;
    std::vector<uint32_t> offsets,counts;
    while(remaining) { auto count=TerrainIndirectChunk(remaining,3); offsets.push_back(offset); counts.push_back(count); offset+=count; remaining-=count; }
    EXPECT_EQ(offsets,(std::vector<uint32_t>{7,10,13})); EXPECT_EQ(counts,(std::vector<uint32_t>{3,3,2}));
    EXPECT_EQ(TerrainIndirectChunk(8,0),0); EXPECT_EQ(TerrainIndirectChunk(8,1),1);
}
TEST(TerrainPrimitiveLookup, RehashEraseAndDenseIndexChangesMatchReference) {
    TerrainPrimitiveIndex actual; std::unordered_map<uint64_t,uint32_t> expected;
    uint64_t random=0x91e10da5c79e7b1dULL;
    for(uint32_t step=0;step<100000;++step) {
        random^=random<<13; random^=random>>7; random^=random<<17;
        const auto id=(random%12000)*0x100000001ULL+1;
        switch(random%4) {
        case 0: actual.Erase(id); expected.erase(id); break;
        case 1:
            if(expected.contains(id)) { actual.Set(id,step); expected[id]=step; }
            break;
        default: actual.Insert(id,step); expected[id]=step; break;
        }
        const auto found=expected.find(id);
        ASSERT_EQ(actual.Find(id),found==expected.end()?TerrainPrimitiveIndex::Missing:found->second);
        ASSERT_EQ(actual.Size(),expected.size());
    }
    actual.Insert(UINT64_MAX,17); EXPECT_EQ(actual.Find(UINT64_MAX),17); actual.Erase(UINT64_MAX);
    EXPECT_EQ(actual.Find(0),TerrainPrimitiveIndex::Missing);
    for(const auto& [id,index]:expected) EXPECT_EQ(actual.Find(id),index);
    for(const auto& [id,index]:expected) actual.Erase(id);
    EXPECT_EQ(actual.Size(),0);
    for(const auto& [id,index]:expected) EXPECT_EQ(actual.Find(id),TerrainPrimitiveIndex::Missing);
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
