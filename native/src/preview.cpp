#include <raynium/preview.h>
#include <raynium/uv_bytecode.h>
#include <raynium/terrain.h>
#include <algorithm>
#include <limits>
#include <new>

namespace raynium {
using namespace radray::render;
namespace {
template<class T>
bool Take(ExternalResult<T> result, T& output, radray::string& error) {
    if (auto* failure = std::get_if<ExternalError>(&result)) {
        error = failure->Message;
        return false;
    }
    output = std::get<T>(std::move(result));
    return true;
}
void Barrier(CommandBuffer& command, Texture* texture, TextureStates before, TextureStates after,
             ExternalOwnershipTransfer transfer) {
    const ResourceBarrierDescriptor barrier = BarrierTextureDescriptor{
        .Target = texture, .Before = before, .After = after, .ExternalTransfer = transfer};
    command.ResourceBarrier({&barrier, 1});
}
}

Preview::Preview(RayniumRuntime& runtime, Resources&& resources, uint32_t width, uint32_t height) noexcept
    : _runtime(runtime), _resources(std::move(resources)), _width(width), _height(height) {}

bool Preview::Create(RayniumRuntime& runtime, uint32_t width, uint32_t height,
                     radray::unique_ptr<Preview>& output, rn_preview_info& info, radray::string& error) noexcept {
    error.clear();
    if (output || width == 0 || height == 0 || width > 16384 || height > 16384) {
        error = "Preview dimensions must be 1..16384 and output must be empty";
        return false;
    }
    auto& device = runtime.GetDevice();
    auto extension = device.GetExternalResourceExtension();
    if (!extension) { error = "External resources unavailable"; return false; }
    const bool vulkan = device.GetBackend() == RenderBackend::Vulkan;
    const ExternalTextureDescriptor external{vulkan ? ExternalMemoryHandleType::OpaqueWin32 : ExternalMemoryHandleType::D3D12Resource};
    const auto fenceType = vulkan ? ExternalFenceHandleType::OpaqueWin32 : ExternalFenceHandleType::D3D12Fence;
    const TextureDescriptor desc{.Dim = TextureDimension::Dim2D, .Width = width, .Height = height,
        .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::RGBA8_UNORM,
        .Memory = MemoryType::Device, .Usage = TextureUse::RenderTarget | TextureUse::CopySource};
    const auto textureSupport = extension->QueryTextureSupport(desc, external);
    const auto fenceSupport = extension->QueryFenceSupport(fenceType);
    if (!textureSupport.CanCreateExportable || !fenceSupport.CanCreateExportable) {
        error = !textureSupport.CanCreateExportable ? textureSupport.Reason.Message : fenceSupport.Reason.Message;
        return false;
    }
    Resources resources;
    if (!Take(extension->CreateExportableTexture(desc, external), resources.texture, error) ||
        !Take(extension->CreateExportableFence({fenceType, 0}), resources.ready, error) ||
        !Take(extension->CreateExportableFence({fenceType, 0}), resources.consumed, error) ||
        !Take(extension->ExportTexture(resources.texture.get()), resources.textureExport, error) ||
        !Take(extension->ExportFence(resources.ready.get()), resources.readyExport, error) ||
        !Take(extension->ExportFence(resources.consumed.get()), resources.consumedExport, error)) return false;
    const auto category = vulkan ? ShaderBlobCategory::SPIRV : ShaderBlobCategory::DXIL;
    resources.vs = device.CreateShader({vulkan ? UvVsSpirv() : UvVsDxil(), category, ShaderStage::Vertex}).Unwrap();
    resources.ps = device.CreateShader({vulkan ? UvPsSpirv() : UvPsDxil(), category, ShaderStage::Pixel}).Unwrap();
    const PushConstantDescriptor push{0, 8, ShaderStage::Pixel,
        PushConstantDescriptor::D3D12Location{0, 0}, PushConstantDescriptor::VulkanLocation{0}};
    resources.layout = device.CreatePipelineLayout({{}, {&push, 1}}).Unwrap();
    resources.view = device.CreateTextureView({resources.texture.get(), TextureDimension::Dim2D,
        desc.Format, {0, 1, 0, 1}, TextureViewUsage::RenderTarget}).Unwrap();
    auto depthDesc=desc; depthDesc.Format=TextureFormat::D32_FLOAT; depthDesc.Usage=TextureUse::DepthStencilWrite;
    resources.depth=device.CreateTexture(depthDesc).Unwrap();
    if (!resources.depth) { error="Cannot create preview depth"; return false; }
    resources.depthView=device.CreateTextureView({resources.depth.get(),TextureDimension::Dim2D,TextureFormat::D32_FLOAT,{0,1,0,1},TextureViewUsage::DepthWrite}).Unwrap();
    for (auto& slot:resources.slots) {
        slot.allocator=device.CreateCommandAllocator(&runtime.GetQueue()).Unwrap();
        if (!slot.allocator) { error="Cannot create preview command allocator"; return false; }
        slot.command=device.CreateCommandBuffer(slot.allocator.get()).Unwrap();
        if (!slot.command) { error="Cannot create preview command buffer"; return false; }
    }
    if (!resources.vs || !resources.ps || !resources.layout || !resources.view || !resources.depthView) {
        error = "Cannot create UV shader, layout, view or command allocator";
        return false;
    }
    auto primitive = PrimitiveState::Default();
    primitive.Cull = CullMode::None;
    const auto color = ColorTargetState::Default(desc.Format);
    resources.pipeline = device.CreateGraphicsPipelineState({.PipelineLayout = resources.layout.get(),
        .VS = ShaderEntry{resources.vs.get(), "VSMain"}, .PS = ShaderEntry{resources.ps.get(), "PSMain"}, .Primitive = primitive,
        .MultiSample = {1, std::numeric_limits<uint64_t>::max(), false}, .ColorTargets = {&color, 1}}).Unwrap();
    if (!resources.pipeline) { error = "Cannot create UV pipeline or command buffer"; return false; }
    const auto& exported = resources.textureExport.Info;
    rn_preview_info result{};
    result.struct_size = sizeof(result);
    result.handle_type = vulkan ? RN_SHARED_OPAQUE_WIN32 : RN_SHARED_D3D12;
    result.width = width;
    result.height = height;
    result.memory_handle = resources.textureExport.Handle.Borrow().Value;
    result.ready_handle = resources.readyExport.Handle.Borrow().Value;
    result.consumed_handle = resources.consumedExport.Handle.Borrow().Value;
    result.allocation_size = exported.Allocation.Size;
    result.allocation_offset = exported.Allocation.Offset;
    result.dedicated = exported.Allocation.Dedicated ? 1 : 0;
    result.node_mask = exported.Device.NodeMask;
    std::copy(exported.Device.Luid.begin(), exported.Device.Luid.end(), result.luid);
    result.memory_type_index = std::numeric_limits<uint32_t>::max();
    if (exported.VulkanMemory) {
        result.memory_type_index = exported.VulkanMemory->MemoryTypeIndex;
        const auto& identity = exported.VulkanMemory->Device;
        std::copy(identity.DeviceUuid.begin(), identity.DeviceUuid.end(), result.device_uuid);
        std::copy(identity.DriverUuid.begin(), identity.DriverUuid.end(), result.driver_uuid);
    }
    result.native_format = exported.NativeFormat;
    result.native_usage = exported.NativeUsage;
    result.native_flags = exported.NativeFlags;
    result.native_tiling = exported.NativeTiling;
    radray::unique_ptr<Preview> preview{new (std::nothrow) Preview(runtime, std::move(resources), width, height)};
    if (!preview) { error = "Cannot allocate preview"; return false; }
    info = result;
    output = std::move(preview);
    return true;
}

bool Preview::Finish(radray::string& error) noexcept {
    if (_submitted && !_resources.ready->WaitFor(_submitted, 5000)) {
        error = "Preview GPU completion timed out; resources remain live";
        return false;
    }
    return true;
}

bool Preview::Draw(uint64_t sequence, radray::string& error) noexcept {
    return DrawInternal(sequence,nullptr,nullptr,error);
}
bool Preview::DrawTerrain(uint64_t sequence, Terrain& terrain, const rn_terrain_frame& frame, radray::string& error) noexcept {
    if (!terrain.Validate(frame,error)) return false;
    return DrawInternal(sequence,&terrain,&frame,error);
}
bool Preview::DrawInternal(uint64_t sequence, Terrain* terrain, const rn_terrain_frame* frame, radray::string& error) noexcept {
    error.clear();
    if (!sequence || sequence == std::numeric_limits<uint64_t>::max() || sequence != _submitted + 1) {
        error="Preview sequence must start at 1 and increase by one"; return false;
    }
    if (sequence>2 && !_resources.ready->WaitFor(sequence-2,5000)) {
        error="Preview command slot completion timed out; resources remain live"; return false;
    }
    auto& r=_resources; const uint32_t index=uint32_t((sequence-1)%2); auto& slot=r.slots[index];
    slot.allocator->Reset(); slot.command->Begin();
    if (terrain && !terrain->Prepare(*slot.command,index,*frame,error)) { slot.command->End(); return false; }
    Barrier(*slot.command,r.texture.get(),_submitted ? TextureState::Common : TextureState::Undefined,
        TextureState::RenderTarget,_submitted ? ExternalOwnershipTransfer::Acquire : ExternalOwnershipTransfer::None);
    if (terrain) Barrier(*slot.command,r.depth.get(),_depthInitialized ? TextureState::DepthWrite : TextureState::Undefined,
        TextureState::DepthWrite,ExternalOwnershipTransfer::None);
    RenderPassColorAttachment color{r.view.get(),LoadAction::Clear,StoreAction::Store};
    if (terrain) std::copy_n(frame->fog_color,4,color.Clear.Value.begin());
    RenderPassBeginDescriptor begin{.Colors={&color,1},.Width=_width,.Height=_height};
    if (terrain) begin.DepthStencil=RenderPassDepthStencilAttachment{.View=r.depthView.get(),.DepthLoad=LoadAction::Clear,
        .DepthStore=StoreAction::Store,.Clear={0.0f,0}};
    auto pass=slot.command->BeginRenderPass(begin).Unwrap();
    if (!pass) { slot.command->End(); error="Cannot begin preview render pass"; return false; }
    pass->SetViewport({0,0,float(_width),float(_height),0,1}); pass->SetScissor({0,0,_width,_height});
    bool pushed=true;
    if (terrain) terrain->Render(*pass,index,(frame->flags&1)!=0);
    else {
        pass->BindGraphicsPipelineState(r.pipeline.get()); const float size[]{float(_width),float(_height)};
        pushed=pass->SetPushConstants(0,0,std::as_bytes(std::span{size})); if(pushed) pass->Draw(3,1,0,0);
    }
    slot.command->EndRenderPass(std::move(pass));
    Barrier(*slot.command,r.texture.get(),TextureState::RenderTarget,TextureState::Common,ExternalOwnershipTransfer::Release);
    if (terrain) terrain->Release(*slot.command,index);
    slot.command->End();
    if (!pushed) { error="Cannot set UV constants"; return false; }
    CommandBuffer* commands[]{slot.command.get()};
    Fence* signals[]{r.ready.get(),terrain ? terrain->Ready() : nullptr};
    uint64_t signalValues[]{sequence,terrain ? frame->input_sequence+1 : 0};
    Fence* waits[2]{}; uint64_t waitValues[2]{}; size_t waitCount=0;
    if (_submitted) { waits[waitCount]=r.consumed.get(); waitValues[waitCount++]=_submitted; }
    if (terrain) { waits[waitCount]=terrain->Available(); waitValues[waitCount++]=frame->input_sequence; }
    _runtime.GetQueue().Submit({.CmdBuffers=commands,.SignalFences={signals,terrain ? 2u : 1u},
        .SignalValues={signalValues,terrain ? 2u : 1u},.WaitFences={waits,waitCount},.WaitValues={waitValues,waitCount}});
    _submitted=sequence; if(terrain) { _depthInitialized=true; terrain->Submitted(frame->input_sequence); }
    return true;
}
}
