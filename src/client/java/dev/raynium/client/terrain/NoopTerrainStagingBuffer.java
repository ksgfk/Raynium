package dev.raynium.client.terrain;

import com.mojang.blaze3d.vertex.StagingBuffer;
import com.mojang.renderpearl.api.buffers.GpuBuffer;
import com.mojang.renderpearl.api.commands.CommandEncoder;
import java.nio.ByteBuffer;

/** Zero-allocation no-op StagingBuffer used by SectionRenderDispatcher when retained terrain is active. */
public final class NoopTerrainStagingBuffer extends StagingBuffer {
    private final BufferHandle handle = new BufferHandle(0, 0) {
        @Override
        public void close() {}
    };

    @Override
    public BufferHandle tryAppend(ByteBuffer buffer) {
        return handle;
    }

    @Override
    protected ByteBuffer getWriteBuffer() {
        throw new UnsupportedOperationException("Retained terrain staging buffer is disabled");
    }

    @Override
    protected void copyTo(CommandEncoder encoder, GpuBuffer dstBuffer, long dstOffset, long stagingBufferOffset, long copySize) {}

    @Override
    public void close() {}
}
