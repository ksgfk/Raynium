#pragma once
#include <radray/types.h>
#include <radray/runtime/rdg.h>
#include <radray/runtime/transient_parameter_pool.h>

namespace raynium {
radray::TransientParameterBinding FrameConstantBinding(uint32_t binding,radray::RDGBufferSlice slice) noexcept;
radray::gpu::VertexBufferView FrameVertexView(radray::RDGBufferSlice) noexcept;
radray::gpu::IndexBufferView FrameIndexView(radray::RDGBufferSlice) noexcept;
uint64_t FrameIndirectOffset(radray::RDGBufferSlice,uint32_t record) noexcept;
}
