#pragma once
#include <raynium/bridge.h>
#include <radray/types.h>
#include <radray/viewport.h>
#include <cstring>
#include <span>

namespace raynium::wire {
struct Pass {
    uint32_t opcode, size, colors, depth;
    radray::Rect area;
    bool Valid(size_t length) const {
        return depth<=1 && length==sizeof(Pass)+(uint64_t(colors)+depth)*40 &&
               area.X>=0 && area.Y>=0 && area.Width && area.Height;
    }
};
struct Draw {
    uint32_t opcode,size; rn_pipeline* pipeline; uint32_t kind,count,instances,first,firstInstance;
    int32_t base;
    radray::Rect scissor;
    uint32_t bindings,push;
    bool Valid(size_t length) const {
        return kind<=4 && bindings<=256 && !(push%4) &&
               length==sizeof(Draw)+uint64_t(bindings)*48+((uint64_t(push)+7)&~uint64_t(7));
    }
};
struct Geometry {
    uint32_t opcode,size; rn_pipeline* pipeline; uint32_t bindings,runCount,instanceCount,instanceStride,push,reserved;
    uint64_t uploadOffset;
    radray::Rect scissor;
    bool Valid(size_t length) const {
        return !reserved && instanceStride && runCount && instanceCount && bindings<=256 && !(push%4) &&
               length==sizeof(Geometry)+uint64_t(bindings)*48+uint64_t(runCount)*16+((uint64_t(push)+7)&~uint64_t(7));
    }
};
struct TerrainMaterial { rn_texture* id; uint32_t flags,reserved; rn_texture_sampling sampling; };
struct Terrain {
    uint32_t opcode,size,view,stage;
    radray::Rect scissor;
    rn_texture* ids[3]; uint32_t mips[3],reserved;
    TerrainMaterial materials[2];
};
static_assert(sizeof(Pass)==32 && sizeof(Draw)==64 && sizeof(Geometry)==64 && sizeof(Terrain)==136);

template<class T>
bool ReadRasterHeader(std::span<const radray::byte> bytes, T& header) {
    if (bytes.size()<sizeof(T)) return false;
    std::memcpy(&header,bytes.data(),sizeof(T));
    return header.Valid(bytes.size());
}
}
