#include <raynium/render_resources.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace raynium {
void QueryDeviceInfo(radray::DeviceContext& runtime,rn_device_info& info) {
    using namespace radray::gpu;
    auto& device=runtime.GetDevice(); const auto& caps=device.GetCapabilities();
    info={}; info.struct_size=sizeof(info);
    info.backend=device.GetBackend()==GpuBackend::Vulkan?RN_BACKEND_VULKAN:RN_BACKEND_D3D12;
    info.vendor_id=caps.Detail.VendorId;
    info.device_type=caps.Detail.IsSoftware?3:caps.Detail.IsUMA?1:2;
    info.uniform_offset_alignment=std::max(uint64_t(1),caps.Limits.CBufferOffsetAlignment);
    info.max_texture_size=caps.Limits.MaxTexture2DDimension;
    info.max_color_attachments=caps.Limits.MaxColorAttachments;
    info.max_buffer_size=caps.Limits.MaxBufferSize;
    info.max_uniform_buffer_range=caps.Limits.MaxUniformBufferRange;
    info.max_storage_buffer_range=caps.Limits.MaxStorageBufferRange;
    info.storage_offset_alignment=caps.Limits.StorageBufferOffsetAlignment;
    info.texel_offset_alignment=caps.Limits.TexelBufferOffsetAlignment;
    info.buffer_copy_offset_alignment=caps.Detail.BufferCopyOffsetAlignment;
    info.texture_placement_alignment=caps.Detail.TextureDataPlacementAlignment;
    info.texture_pitch_alignment=caps.Detail.TextureDataPitchAlignment;
    info.max_texture_1d=caps.Limits.MaxTexture1DDimension;
    info.max_texture_3d=caps.Limits.MaxTexture3DDimension;
    info.max_array_layers=caps.Limits.MaxTextureArrayLayers;
    info.max_vertex_bindings=caps.Detail.MaxVertexInputBindings;
    info.max_push_constant_bytes=caps.Limits.MaxPushConstantBytes;
    info.max_draw_indirect_count=caps.Limits.MaxDrawIndirectCount;
    info.host_coherent_atom_size=std::max<uint64_t>(1,caps.Limits.HostCoherentAtomSize);
    info.flags=RN_DEVICE_PERSISTENT_MAPPING;
    if(caps.Features.IndirectDraw) info.flags|=RN_DEVICE_INDIRECT_DRAW;
    if(caps.Features.IndirectFirstInstance) info.flags|=RN_DEVICE_INDIRECT_FIRST_INSTANCE;
    if(caps.Features.TimestampQueries && runtime.GetQueue().GetTimestampValidBits()) {
        auto pool=device.CreateQueryPool({QueryType::Timestamp,1,"Device timestamp calibration"});
        if(pool) {
            const auto calibration=pool->GetTimestampCalibration(&runtime.GetQueue());
            if(calibration.TickPeriodNs>0) {
                info.timestamp_valid_bits=runtime.GetQueue().GetTimestampValidBits();
                info.timestamp_frequency=calibration.FrequencyHz;
                info.timestamp_period_ns=calibration.TickPeriodNs;
                info.flags|=RN_DEVICE_TIMESTAMP_QUERIES;
            }
        }
    }
    info.max_anisotropy=std::max(1.f,caps.Limits.MaxSamplerAnisotropy);
    std::snprintf(info.name,sizeof(info.name),"%s",caps.Detail.GpuName.c_str());
    const char* vendor=caps.Detail.VendorId==0x10de?"NVIDIA":caps.Detail.VendorId==0x1002?"AMD":caps.Detail.VendorId==0x8086?"Intel":nullptr;
    if(vendor) std::snprintf(info.vendor,sizeof(info.vendor),"%s",vendor);
    else std::snprintf(info.vendor,sizeof(info.vendor),"PCI vendor 0x%04x",caps.Detail.VendorId);
    std::snprintf(info.driver,sizeof(info.driver),"%s",caps.Detail.DriverVersion.empty()?"unavailable":caps.Detail.DriverVersion.c_str());
}
}
