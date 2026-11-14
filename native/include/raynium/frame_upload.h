#pragma once
#include <radray/types.h>
#include <radray/runtime/rdg.h>

namespace raynium {
// Cached bindings may outlive a frame only when their upload page remains owned by its slot.
bool BindFrameConstants(radray::render::ShaderParameterSet&,radray::RDGBufferSlice&,radray::RDGBufferSlice) noexcept;
radray::render::VertexBufferView FrameVertexView(radray::RDGBufferSlice) noexcept;
radray::render::IndexBufferView FrameIndexView(radray::RDGBufferSlice) noexcept;
uint64_t FrameIndirectOffset(radray::RDGBufferSlice,uint32_t record) noexcept;
}
