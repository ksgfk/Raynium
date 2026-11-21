#include <raynium/terrain_draw.h>
#include <radray/types.h>
#include <radray/logger.h>
#include <Eigen/LU>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>

namespace raynium {
namespace {
float TerrainCoordinate(std::span<const radray::byte> bytes,size_t vertex,uint32_t axis) noexcept {
    float value; std::memcpy(&value,bytes.data()+vertex*28+axis*4,4); return value;
}
uint32_t TerrainQuadGroup(std::span<const radray::byte> bytes,size_t first) noexcept {
    for(size_t v=0;v<4;++v) for(uint32_t a=0;a<3;++a)
        if(!std::isfinite(TerrainCoordinate(bytes,first+v,a))) return 6;
    for(uint32_t a=0;a<3;++a) {
        const auto plane=TerrainCoordinate(bytes,first,a);
        if(plane!=TerrainCoordinate(bytes,first+1,a) || plane!=TerrainCoordinate(bytes,first+2,a) || plane!=TerrainCoordinate(bytes,first+3,a)) continue;
        const uint32_t b=(a+1)%3,c=(a+2)%3;
        const double b0=TerrainCoordinate(bytes,first,b),c0=TerrainCoordinate(bytes,first,c);
        const double b1=double(TerrainCoordinate(bytes,first+1,b))-b0,c1=double(TerrainCoordinate(bytes,first+1,c))-c0;
        const double b2=double(TerrainCoordinate(bytes,first+2,b))-b0,c2=double(TerrainCoordinate(bytes,first+2,c))-c0;
        const double b3=double(TerrainCoordinate(bytes,first+3,b))-b0,c3=double(TerrainCoordinate(bytes,first+3,c))-c0;
        const double n=b1*c2-c1*b2,m=b2*c3-c2*b3;
        if(n>0 && m>0) return a*2;
        if(n<0 && m<0) return a*2+1;
    }
    return 6;
}
}
bool SelectTerrainSubmission(uint32_t flags,const radray::gpu::GpuDeviceCapabilities& caps,bool& indirect) {
    indirect=false;
    if(flags>RN_RENDERER_INDIRECT) { RADRAY_ERR_LOG("Unknown or mutually exclusive terrain submission flags: {}",flags); return false; }
    const std::string_view reason=!caps.Features.IndirectDraw?"indirect draws unsupported":
        !caps.Features.IndirectFirstInstance?"nonzero indirect firstInstance unsupported":
        caps.Limits.MaxDrawIndirectCount<=1?"native multi-record indirect unsupported":"";
    indirect=flags!=RN_RENDERER_DIRECT && reason.empty();
    if(flags==RN_RENDERER_INDIRECT && !indirect) {
        RADRAY_ERR_LOG("Terrain indirect submission unavailable: {}",reason); return false;
    }
    RADRAY_INFO_LOG("Terrain submission requested={} actual={} indirect={} first_instance={} max_native_records={} fallback={}",
        flags==RN_RENDERER_DIRECT?"direct":flags==RN_RENDERER_INDIRECT?"indirect":"auto",indirect?"indirect":"direct",
        caps.Features.IndirectDraw,caps.Features.IndirectFirstInstance,caps.Limits.MaxDrawIndirectCount,
        flags==0 && !indirect?reason:std::string_view{"none"});
    return true;
}
bool AnalyzeTerrainMesh(std::span<const radray::byte> bytes,rn_material_mode layer,TerrainMeshLayout& layout,
    TerrainMeshUpload& upload) {
    if(layer>RN_MATERIAL_TRANSLUCENT || bytes.size()%112 || bytes.size()/28>1048576) {
        RADRAY_ERR_LOG("Invalid terrain quad stream or layer"); return false;
    }
    layout={}; layout.vertexCount=uint32_t(bytes.size()/28); layout.layer=layer; layout.finiteBounds=!bytes.empty();
    layout.minimum.fill(std::numeric_limits<double>::infinity()); layout.maximum.fill(-std::numeric_limits<double>::infinity());
    radray::array<uint32_t,7> counts{};
    upload.source=bytes; upload.groups.resize(bytes.size()/112); upload.offsets={};
    auto& groups=upload.groups; auto& offsets=upload.offsets;
    for(size_t q=0;q<groups.size();++q) {
        for(size_t v=0;v<4;++v) for(uint32_t a=0;a<3;++a) {
            const double x=TerrainCoordinate(bytes,q*4+v,a);
            if(!std::isfinite(x)) layout.finiteBounds=false;
            layout.minimum[a]=std::min(layout.minimum[a],x); layout.maximum[a]=std::max(layout.maximum[a],x);
        }
        groups[q]=uint8_t(layer==RN_MATERIAL_TRANSLUCENT?6:TerrainQuadGroup(bytes,q*4)); counts[groups[q]]+=4;
    }
    uint32_t first=0;
    for(uint32_t g=0;g<7;++g) {
        offsets[g]=first/6*112;
        if(counts[g]) { layout.groupBits[g]=1u<<layout.ranges.size(); layout.ranges.push_back({first,counts[g]/4*6,0}); }
        first+=counts[g]/4*6;
    }
    if(layer!=RN_MATERIAL_TRANSLUCENT && layout.ranges.size()>1) {
        layout.originalQuadGroups.resize((groups.size()*3+7)/8,0);
        for(size_t q=0;q<groups.size();++q) {
            const size_t bit=q*3,byte=bit/8; const auto shift=uint32_t(bit%8);
            layout.originalQuadGroups[byte]|=uint8_t(groups[q]<<shift);
            if(shift>5) layout.originalQuadGroups[byte+1]|=uint8_t(groups[q]>>(8-shift));
        }
    }
    return true;
}
bool BuildPreanalyzedTerrainMeshLayout(const rn_terrain_mesh_layout& source,uint32_t vertexCount,rn_material_mode layer,
    TerrainMeshLayout& layout) {
    if(layer>RN_MATERIAL_TRANSLUCENT || vertexCount%4 || vertexCount>1048576 || source.reserved ||
       (source.flags&~(RN_TERRAIN_LAYOUT_FINITE_BOUNDS|RN_TERRAIN_LAYOUT_WIREFRAME_GROUPS))) {
        RADRAY_ERR_LOG("Invalid preanalyzed terrain mesh layout header"); return false;
    }
    const uint64_t expectedQuads=vertexCount/4;
    uint64_t totalQuads=0;
    for(uint32_t g=0;g<7;++g) {
        if(layer==RN_MATERIAL_TRANSLUCENT && g<6 && source.group_quad_counts[g]!=0) {
            RADRAY_ERR_LOG("Preanalyzed translucent mesh cannot use directional groups"); return false;
        }
        totalQuads+=source.group_quad_counts[g];
    }
    if(totalQuads!=expectedQuads) {
        RADRAY_ERR_LOG("Preanalyzed terrain quad count mismatch: sum={} expected={}",totalQuads,expectedQuads); return false;
    }
    layout={};
    layout.vertexCount=vertexCount;
    layout.layer=layer;
    layout.minimum.fill(std::numeric_limits<double>::infinity());
    layout.maximum.fill(-std::numeric_limits<double>::infinity());
    const bool finiteFlag=(source.flags&RN_TERRAIN_LAYOUT_FINITE_BOUNDS)!=0;
    if(finiteFlag) {
        if(!vertexCount) {
            RADRAY_ERR_LOG("Preanalyzed empty terrain mesh cannot declare finite bounds"); return false;
        }
        for(uint32_t a=0;a<3;++a) {
            if(!std::isfinite(source.minimum[a]) || !std::isfinite(source.maximum[a]) || source.minimum[a]>source.maximum[a]) {
                RADRAY_ERR_LOG("Preanalyzed terrain mesh bounds are invalid"); return false;
            }
            layout.minimum[a]=source.minimum[a];
            layout.maximum[a]=source.maximum[a];
        }
        layout.finiteBounds=true;
    }
    uint32_t first=0;
    for(uint32_t g=0;g<7;++g) {
        const uint32_t quads=source.group_quad_counts[g];
        if(quads) {
            layout.groupBits[g]=1u<<layout.ranges.size();
            layout.ranges.push_back({first,quads*6,0});
        }
        first+=quads*6;
    }
    const bool needsWireframeGroups=layer!=RN_MATERIAL_TRANSLUCENT && layout.ranges.size()>1;
    const bool hasWireframeFlag=(source.flags&RN_TERRAIN_LAYOUT_WIREFRAME_GROUPS)!=0;
    if(needsWireframeGroups) {
        const uint64_t expectedBytes=(totalQuads*3+7)/8;
        if(!hasWireframeFlag || !source.original_quad_groups || source.original_quad_groups_bytes!=expectedBytes) {
            RADRAY_ERR_LOG("Preanalyzed multi-group terrain mesh missing valid wireframe groups"); return false;
        }
        layout.originalQuadGroups.assign(source.original_quad_groups,source.original_quad_groups+expectedBytes);
    } else if(hasWireframeFlag || source.original_quad_groups || source.original_quad_groups_bytes) {
        RADRAY_ERR_LOG("Unexpected wireframe group payload on single-group or translucent terrain mesh"); return false;
    }
    return true;
}
bool TerrainMeshUpload::Write(std::span<radray::byte> destination) const noexcept {
    if(destination.size()!=source.size() || groups.size()*112!=source.size()) return false;
    auto positions=offsets;
    for(size_t q=0;q<groups.size();++q) {
        const auto group=groups[q];
        if(group>=positions.size() || uint64_t(positions[group])+112>destination.size()) return false;
        auto& offset=positions[group];
        std::memcpy(destination.data()+offset,source.data()+q*112,112); offset+=112;
    }
    return true;
}
bool WriteQuadIndices(std::span<radray::byte> destination,uint32_t vertexCount) noexcept {
    if(vertexCount%4 || destination.size()!=uint64_t(vertexCount/4)*24) return false;
    for(uint32_t q=0;q<vertexCount/4;++q) {
        const uint32_t v=q*4; const radray::array<uint32_t,6> quad{v,v+1,v+2,v+2,v+3,v};
        std::memcpy(destination.data()+size_t(q)*24,quad.data(),24);
    }
    return true;
}
bool WriteWireframeIndices(std::span<radray::byte> destination,const TerrainMeshLayout& layout,uint32_t mask) noexcept {
    if(layout.ranges.size()>7 || layout.vertexCount%4 || layout.layer==RN_MATERIAL_TRANSLUCENT ||
       (mask&~((1u<<layout.ranges.size())-1))) return false;
    uint64_t count=0;
    for(size_t n=0;n<layout.ranges.size();++n) if(mask&(1u<<n)) count+=layout.ranges[n].count;
    if(destination.size()!=count*4) return false;
    if(!count) return true;
    if(layout.ranges.size()<=1) return WriteQuadIndices(destination,layout.vertexCount);
    const size_t quads=layout.vertexCount/4;
    if(layout.originalQuadGroups.size()!=(quads*3+7)/8) return false;
    radray::array<uint32_t,7> positions{};
    for(uint32_t g=0;g<7;++g) if(layout.groupBits[g]) {
        const auto bit=layout.groupBits[g]; const auto range=std::countr_zero(bit);
        if((bit&(bit-1)) || size_t(range)>=layout.ranges.size() || layout.ranges[range].first%6) return false;
        positions[g]=layout.ranges[range].first/6;
    }
    size_t output=0;
    for(size_t q=0;q<quads;++q) {
        const size_t bit=q*3,byte=bit/8; const auto shift=uint32_t(bit%8);
        uint32_t packed=layout.originalQuadGroups[byte];
        if(shift>5) packed|=uint32_t(layout.originalQuadGroups[byte+1])<<8;
        const auto group=(packed>>shift)&7u;
        if(group>=7 || !layout.groupBits[group]) return false;
        if(positions[group]>=quads) return false;
        const auto v=positions[group]++*4;
        if(!(layout.groupBits[group]&mask)) continue;
        if(output+24>destination.size()) return false;
        const radray::array<uint32_t,6> quad{v,v+1,v+2,v+2,v+3,v};
        std::memcpy(destination.data()+output,quad.data(),24); output+=24;
    }
    return output==destination.size();
}
bool WriteLineIndices(std::span<radray::byte> destination,uint32_t vertexCount) noexcept {
    if(vertexCount%4 || destination.size()!=uint64_t(vertexCount/4)*24) return false;
    for(uint32_t q=0;q<vertexCount/4;++q) {
        const uint32_t v=q*4; const radray::array<uint32_t,6> line{v,v+1,v+2,v+3,v+2,v+1};
        std::memcpy(destination.data()+size_t(q)*24,line.data(),24);
    }
    return true;
}
TerrainEye ComputeTerrainEye(const rn_terrain_view_desc& frame) noexcept {
    const Eigen::Matrix4f matrix=Eigen::Map<const Eigen::Matrix4f>(frame.projection)*Eigen::Map<const Eigen::Matrix4f>(frame.view);
    if(!matrix.allFinite()) return {};
    const Eigen::FullPivLU<Eigen::Matrix4f> inverse(matrix);
    if(!inverse.isInvertible()) return {};
    const Eigen::Vector4f point=inverse.solve(Eigen::Vector4f(0,0,1,0));
    if(!point.allFinite() || std::abs(point.w())<=1e-8f) return {};
    const auto eye=(point/point.w()).eval();
    return {{{eye.x(),eye.y(),eye.z()}},true};
}
uint32_t TerrainVisibleMask(const TerrainMeshLayout& layout,const TerrainEye& eye,const rn_terrain_view_desc& frame,
    std::span<const rn_instance> instances) noexcept {
    const uint32_t full=(1u<<layout.ranges.size())-1;
    if(layout.layer==RN_MATERIAL_TRANSLUCENT || !layout.finiteBounds || !eye.valid) return full;
    uint32_t mask=layout.groupBits[6];
    for(const auto& instance:instances) {
        radray::array<double,3> position;
        for(uint32_t a=0;a<3;++a) {
            position[a]=double(int64_t(frame.camera_block[a])-int64_t(instance.origin[a]))+frame.camera_fraction[a]+eye.offset[a];
            if(!std::isfinite(position[a])) return full;
            if(position[a]>=layout.minimum[a]-0.001) mask|=layout.groupBits[a*2];
            if(position[a]<=layout.maximum[a]+0.001) mask|=layout.groupBits[a*2+1];
        }
        if(mask==full) break;
    }
    return mask;
}
}
