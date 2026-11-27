#include <raynium/immediate_pipeline.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <optional>
#include <utility>

#include <radray/logger.h>

namespace raynium {
radray::gpu::GraphicsPipelineState* ImmediatePipeline::Resolve(
    radray::gpu::Device& device,
    radray::gpu::TextureFormat depthFormat) {
    if (auto found = _variants.find(depthFormat); found != _variants.end()) return found->second.get();
    std::optional<radray::gpu::DepthStencilState> depth;
    if (depthFormat != radray::gpu::TextureFormat::UNKNOWN) {
        depth = State.Depth.value_or(radray::gpu::DepthStencilState{.DepthCompare = radray::gpu::CompareFunction::Always});
        depth->Format = depthFormat;
    } else if (State.Depth) {
        return nullptr;
    }
    auto pso = device.CreateGraphicsPipelineState({
                                                      .PipelineLayout = Program->Layout.get(),
                                                      .VS = radray::gpu::ShaderEntry{Program->Vs.get(), Program->Program->Entry(radray::shader::ShaderStage::Vertex)},
                                                      .PS = radray::gpu::ShaderEntry{Program->Ps.get(), Program->Program->Entry(radray::shader::ShaderStage::Pixel)},
                                                      .VertexInput = {State.Vertices, State.Attributes},
                                                      .Primitive = State.Primitive,
                                                      .DepthStencil = depth,
                                                      .MultiSample = radray::gpu::MultiSampleState::Default(),
                                                      .ColorTargets = State.Colors,
                                                      .Cache = _cache,
                                                  })
                   .Release();
    if (!pso) return nullptr;
    auto* result = pso.get();
    _variants.emplace(depthFormat, std::move(pso));
    return result;
}

radray::unique_ptr<ImmediatePipeline> ImmediatePipeline::Create(
    radray::gpu::Device& device,
    RenderProgramLibrary& resources,
    const rn_pipeline_desc& desc,
    radray::Nullable<radray::gpu::PipelineCache*> cache) {
    if (desc.struct_size != sizeof(desc) || !desc.program || !desc.words || desc.word_count < 9) return {};
    const auto* w = desc.words;
    const uint32_t topology = w[0];
    const uint32_t flags = w[1];
    const uint32_t compare = w[2];
    const uint32_t bindings = w[6];
    const uint32_t attributes = w[7];
    const uint32_t colors = w[8];
    if (topology > 3 || flags > 3 || compare > 8 || w[3] > 1 ||
        bindings > device.GetCapabilities().Limits.MaxVertexInputBindings ||
        attributes > 32 || colors > device.GetCapabilities().Limits.MaxColorAttachments ||
        desc.word_count != 9ull + 3ull * bindings + 4ull * attributes + 9ull * colors) {
        return {};
    }
    const auto bias = std::bit_cast<float>(w[4]);
    const auto slope = std::bit_cast<float>(w[5]);
    if (!std::isfinite(bias) || double(bias) < INT32_MIN || double(bias) > INT32_MAX || !std::isfinite(slope)) return {};
    auto item = radray::make_unique<ImmediatePipeline>();
    if (desc.shader_pack_size > kShaderPackMaxSize || (desc.shader_pack_size && !desc.shader_pack)) return {};
    item->Program = resources.GetProgram(
        device,
        desc.program,
        {static_cast<const radray::byte*>(desc.shader_pack), size_t(desc.shader_pack_size)});
    if (!item->Program) return {};
    item->_cache = cache;
    auto& state = item->State;
    constexpr radray::gpu::PrimitiveTopology kModes[]{
        radray::gpu::PrimitiveTopology::TriangleList,
        radray::gpu::PrimitiveTopology::LineList,
        radray::gpu::PrimitiveTopology::LineStrip,
        radray::gpu::PrimitiveTopology::TriangleStrip,
    };
    state.Primitive.Topology = kModes[topology];
    state.Primitive.StripIndexFormat.reset();
    state.Primitive.UnclippedDepth = false;
    state.Primitive.FaceClockwise = radray::gpu::FrontFace::CW;
    state.Primitive.Cull = (flags & 1) ? radray::gpu::CullMode::Back : radray::gpu::CullMode::None;
    state.Primitive.Poly = (flags & 2) ? radray::gpu::PolygonMode::Line : radray::gpu::PolygonMode::Fill;
    if (compare < 8) {
        state.Depth = radray::gpu::DepthStencilState{
            .DepthCompare = radray::gpu::CompareFunction(compare),
            .DepthBias = {int32_t(bias), slope, 0},
            .DepthTestEnable = true,
            .DepthWriteEnable = w[3] != 0,
        };
    }
    w += 9;
    for (uint32_t i = 0; i < bindings; ++i, w += 3) {
        if (w[0] >= device.GetCapabilities().Limits.MaxVertexInputBindings || !w[1] || w[2] > 1) return {};
        state.Vertices.push_back({w[0], w[1], w[2] ? radray::gpu::VertexStepMode::Instance : radray::gpu::VertexStepMode::Vertex});
    }
    auto inputs = item->Program->Program->Artifact().VertexInputs();
    for (uint32_t i = 0; i < attributes; ++i, w += 4) {
        if (!w[3] || w[3] > uint32_t(radray::gpu::VertexFormat::FLOAT32X4)) return {};
        auto found = std::find_if(inputs.begin(), inputs.end(), [&](const auto& a) { return a.SemanticIndex == w[1]; });
        if (found != inputs.end()) {
            state.Attributes.push_back({w[0], w[2], "TEXCOORD", w[1], radray::gpu::VertexFormat(w[3]), found->Location});
        }
    }
    if (state.Attributes.size() != inputs.size()) {
        RADRAY_ERR_LOG("Incomplete vertex interface for {}: supplied={} required={}", desc.program, state.Attributes.size(), inputs.size());
        return {};
    }
    for (uint32_t i = 0; i < colors; ++i, w += 9) {
        auto format = ToTextureFormat(rn_pixel_format(w[0]));
        if (format == radray::gpu::TextureFormat::UNKNOWN || radray::gpu::IsDepthStencilFormat(format) || w[1] > 15 || w[2] > 1) return {};
        auto target = radray::gpu::ColorTargetState::Default(format);
        target.WriteMask = radray::gpu::ColorWrite(w[1]);
        if (w[2]) {
            if (w[3] > 12 || w[4] > 12 || w[5] > 4 || w[6] > 12 || w[7] > 12 || w[8] > 4) return {};
            target.Blend = radray::gpu::BlendState{
                {radray::gpu::BlendFactor(w[3]), radray::gpu::BlendFactor(w[4]), radray::gpu::BlendOperation(w[5])},
                {radray::gpu::BlendFactor(w[6]), radray::gpu::BlendFactor(w[7]), radray::gpu::BlendOperation(w[8])},
            };
        }
        state.Colors.push_back(target);
    }
    return item;
}
}  // namespace raynium
