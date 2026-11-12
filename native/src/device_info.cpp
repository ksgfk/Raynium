#include <raynium/render_resources.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace raynium {
void QueryDeviceInfo(radray::DeviceContext& runtime,rn_device_info& info) {
    using namespace radray::render;
    auto& device=runtime.GetDevice(); const auto& caps=device.GetCapabilities();
    info={}; info.struct_size=sizeof(info);
    info.backend=device.GetBackend()==RenderBackend::Vulkan?RN_BACKEND_VULKAN:RN_BACKEND_D3D12;
    info.vendor_id=caps.Detail.VendorId;
    info.device_type=caps.Detail.IsSoftware?3:caps.Detail.IsUMA?1:2;
    info.uniform_offset_alignment=std::max(uint64_t(1),caps.Limits.CBufferOffsetAlignment);
    info.max_texture_size=caps.Limits.MaxTexture2DDimension;
    info.max_color_attachments=caps.Limits.MaxColorAttachments;
    info.max_anisotropy=std::max(1.f,caps.Limits.MaxSamplerAnisotropy);
    std::snprintf(info.name,sizeof(info.name),"%s",caps.Detail.GpuName.c_str());
    const char* vendor=caps.Detail.VendorId==0x10de?"NVIDIA":caps.Detail.VendorId==0x1002?"AMD":caps.Detail.VendorId==0x8086?"Intel":nullptr;
    if(vendor) std::snprintf(info.vendor,sizeof(info.vendor),"%s",vendor);
    else std::snprintf(info.vendor,sizeof(info.vendor),"PCI vendor 0x%04x",caps.Detail.VendorId);
    std::snprintf(info.driver,sizeof(info.driver),"%s",caps.Detail.DriverVersion.empty()?"unavailable":caps.Detail.DriverVersion.c_str());
}
}
