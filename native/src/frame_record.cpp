#include <raynium/frame_record.h>
#include <raynium/mesh_pass.h>
#include <raynium/exception.h>
#include <radray/logger.h>

namespace raynium {
using namespace radray::render;
bool RecordTerrainDraws(const TerrainRecord& data,GraphicsCommandEncoder& encoder) noexcept try {
    const auto& draws=data.lists->layers[data.layer];
    if(draws.recordCount && (!data.instances.Size || (data.useIndirect && !data.indirect.Size)))
        RADRAY_ABORT("Nonempty terrain draws have empty upload slices");
    if(data.instances.Size) {
        const VertexBufferBinding instances{1,FrameVertexView(data.instances)}; encoder.BindVertexBuffers({&instances,1});
    }
    const TerrainDrawBatch* previous=nullptr;
    for(const auto& batch:std::span{data.lists->batches}.subspan(draws.firstBatch,draws.batchCount)) {
        if(!previous || previous->material->parameters.get()!=batch.material->parameters.get()) encoder.BindShaderParameterSet(1,batch.material->parameters.get());
        if(!previous || previous->vertices.Target!=batch.vertices.Target || previous->vertices.Offset!=batch.vertices.Offset || previous->vertices.Size!=batch.vertices.Size) {
            const VertexBufferBinding vertices{0,batch.vertices}; encoder.BindVertexBuffers({&vertices,1});
        }
        if(!previous || previous->indices.Target!=batch.indices.Target || previous->indices.Offset!=batch.indices.Offset || previous->indices.Stride!=batch.indices.Stride)
            encoder.BindIndexBuffer(batch.indices);
        if(data.useIndirect) {
            for(uint32_t offset=0;offset<batch.count;) {
                const auto chunk=TerrainIndirectChunk(batch.count-offset,data.nativeLimit);
                if(!chunk) RADRAY_ABORT("Invalid native indirect limit");
                encoder.DrawIndexedIndirect(data.indirect.Buffer.Get(),FrameIndirectOffset(data.indirect,batch.first+offset),chunk); offset+=chunk;
            }
        } else for(const auto& argument:std::span{data.lists->arguments}.subspan(batch.first,batch.count))
            encoder.DrawIndexed(argument.IndexCount,argument.InstanceCount,argument.FirstIndex,argument.VertexOffset,argument.FirstInstance);
        previous=&batch;
    }
    return encoder.IsValid();
} catch(...) { AbortUnexpectedException("RecordTerrainDraws"); }

bool RecordGuiDraws(const GuiFrameSlot& frame,std::span<GraphicsPipelineState* const> pipelines,
    radray::RDGBufferSlice vertexSlice,radray::RDGBufferSlice indexSlice,GraphicsCommandEncoder& encoder) noexcept try {
    if(!vertexSlice.Size || !indexSlice.Size) return true;
    const VertexBufferBinding vertices{0,FrameVertexView(vertexSlice)};
    for(const auto& draw:frame.draws) {
        if(!draw.desc.vertex_count || !draw.scissor.Width || !draw.scissor.Height) continue;
        encoder.SetScissor(draw.scissor);
        encoder.BindGraphicsPipelineState(pipelines[draw.desc.pipeline]);
        encoder.BindVertexBuffers({&vertices,1}); encoder.BindIndexBuffer(FrameIndexView(indexSlice));
        encoder.BindShaderParameterSet(0,frame.passParameters[draw.desc.pipeline==RN_GUI_TEXT_GRAYSCALE?1:0].get());
        if(draw.parameters) encoder.BindShaderParameterSet(1,draw.parameters);
        encoder.DrawIndexed(draw.desc.vertex_count/4*6,1,draw.desc.first_vertex/4*6,0,0);
    }
    return encoder.IsValid();
} catch(...) { AbortUnexpectedException("RecordGuiDraws"); }
}
