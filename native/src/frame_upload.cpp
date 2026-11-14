#include <raynium/frame_upload.h>
#include <radray/logger.h>

namespace raynium {
bool BindFrameConstants(radray::render::ShaderParameterSet& parameters,radray::RDGBufferSlice& bound,radray::RDGBufferSlice slice) noexcept {
    if(!slice.Buffer || !slice.Size) return false;
    if(bound.Buffer==slice.Buffer && bound.Offset==slice.Offset && bound.Size==slice.Size) return true;
    parameters.ResetBindingsForReuse(); bound={};
    if(!parameters.Set(0,0,radray::render::ShaderBufferBinding{slice.Buffer.Get(),{slice.Offset,slice.Size}}) || !parameters.FlushWrites()) return false;
    bound=slice; return true;
}
radray::render::VertexBufferView FrameVertexView(radray::RDGBufferSlice slice) noexcept {
    return {slice.Buffer.Get(),slice.Offset,slice.Size};
}
radray::render::IndexBufferView FrameIndexView(radray::RDGBufferSlice slice) noexcept {
    if(slice.Offset>UINT32_MAX) RADRAY_ABORT("Index slice offset cannot be represented: {}",slice.Offset);
    return {slice.Buffer.Get(),uint32_t(slice.Offset),4};
}
uint64_t FrameIndirectOffset(radray::RDGBufferSlice slice,uint32_t record) noexcept {
    constexpr auto stride=sizeof(radray::render::DrawIndexedIndirectArguments);
    static_assert(stride==20);
    const auto offset=uint64_t(record)*stride;
    if(!slice.Buffer || offset>slice.Size || slice.Size-offset<stride || slice.Offset>UINT64_MAX-offset)
        RADRAY_ABORT("Invalid indirect slice record: record={} size={}",record,slice.Size);
    return slice.Offset+offset;
}
}
