package dev.raynium.client.renderpearl;

import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_FLOAT;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;
import static java.lang.foreign.ValueLayout.JAVA_SHORT;

import com.mojang.renderpearl.api.buffers.GpuBuffer;
import com.mojang.renderpearl.api.buffers.GpuBufferSlice;
import com.mojang.renderpearl.api.commands.GpuQueryPool;
import com.mojang.renderpearl.api.commands.RenderPassDescriptor;
import com.mojang.renderpearl.api.pipeline.IndexType;
import com.mojang.renderpearl.api.pipeline.PrimitiveTopology;
import com.mojang.renderpearl.api.textures.GpuTextureView;
import com.mojang.renderpearl.backend.api.BackendRenderPipeline;
import com.mojang.renderpearl.backend.api.RenderPassBackend;
import com.mojang.renderpearl.util.TextureViewAndSampler;
import dev.raynium.bridge.NativeBuffer;
import dev.raynium.bridge.NativeTexture;
import java.lang.foreign.MemorySegment;
import java.nio.BufferUnderflowException;
import java.nio.ByteBuffer;
import java.nio.IntBuffer;
import java.util.Arrays;
import java.util.List;
import java.util.Objects;
import java.util.function.Supplier;
import org.lwjgl.PointerBuffer;

final class RayniumRenderPassBackend implements RenderPassBackend {
    private static final int INITIAL_UNIFORMS = 16;
    private CommandTape tape;
    private int areaX, areaY, areaW, areaH, scissorX, scissorY, scissorW, scissorH;
    private RayniumPipeline pipeline;
    private Object[] uniforms = new Object[INITIAL_UNIFORMS];
    private int uniformCount;
    private final GpuBufferSlice[] vertices = new GpuBufferSlice[16];
    private GpuBuffer indexBuffer;
    private IndexType indexType;
    private byte[] push = new byte[0];
    private int pushLength;
    private int vertexMask;
    boolean isOpen() {
        return tape != null;
    }
    void close() {
        Arrays.fill(uniforms, 0, uniformCount, null);
        for (int mask = vertexMask; mask != 0; mask &= mask - 1)
            vertices[Integer.numberOfTrailingZeros(mask)] = null;
        if (uniforms.length > 128)
            uniforms = new Object[INITIAL_UNIFORMS];
        if (push.length > 1024)
            push = new byte[0];
        uniformCount = vertexMask = pushLength = 0;
        pipeline = null;
        indexBuffer = null;
        indexType = null;
        tape = null;
    }
    RayniumRenderPassBackend open(CommandTape tape, RenderPassDescriptor descriptor) {
        if (isOpen())
            throw new IllegalStateException("Render pass already open");
        this.tape = tape;
        var area = descriptor.renderArea();
        areaX = area.x();
        areaY = area.y();
        areaW = area.width();
        areaH = area.height();
        disableScissor();
        return this;
    }
    @Override
    public void setPipeline(BackendRenderPipeline value) {
        if (!(value instanceof RayniumPipeline selected) || selected.isClosed())
            throw new IllegalArgumentException("Invalid render pipeline");
        pipeline = selected;
        int count = selected.routes.size();
        if (uniformCount > 0)
            Arrays.fill(uniforms, 0, uniformCount, null);
        if (uniforms.length < count)
            uniforms = new Object[Math.max(count, uniforms.length * 2)];
        uniformCount = count;
        pushLength = 0;
    }
    @Override
    public void setUniform(int index, Object value) {
        Objects.checkIndex(index, uniformCount);
        uniforms[index] = value;
    }
    @Override
    public void pushConstants(ByteBuffer value) {
        int size = pipeline.pushBytes;
        if (value.remaining() < size)
            throw new BufferUnderflowException();
        if (push.length < size)
            push = new byte[size];
        if (size > 0)
            value.get(value.position(), push, 0, size);
        pushLength = size;
    }
    @Override
    public void pushDebugGroup(Supplier<String> label) {}
    @Override
    public void popDebugGroup() {}
    @Override
    public void enableScissor(int x, int y, int width, int height) {
        if (width <= 0 || height <= 0 || x < areaX || y < areaY || (long)x + width > (long)areaX + areaW ||
            (long)y + height > (long)areaY + areaH)
            throw new IllegalArgumentException("Invalid scissor");
        scissorX = x;
        scissorY = y;
        scissorW = width;
        scissorH = height;
    }
    @Override
    public void disableScissor() {
        scissorX = areaX;
        scissorY = areaY;
        scissorW = areaW;
        scissorH = areaH;
    }
    @Override
    public void setVertexBuffer(int slot, GpuBufferSlice buffer) {
        vertices[slot] = buffer;
        if (buffer == null)
            vertexMask &= ~(1 << slot);
        else
            vertexMask |= 1 << slot;
    }
    @Override
    public void setIndexBuffer(GpuBuffer buffer, IndexType type) {
        indexBuffer = buffer;
        indexType = type;
    }
    @Override
    public void writeTimestamp(GpuQueryPool pool, int index) {
        tape.writeTimestamp((RayniumQueryPool)pool, index);
    }

    CommandTape tape() {
        if (!isOpen())
            throw new IllegalStateException("Render pass is closed");
        return tape;
    }

    void terrain(CommandTape.TerrainView view, int stage, TextureViewAndSampler atlas, TextureViewAndSampler light,
                 GpuTextureView bounds, GpuTextureView transmittance0, GpuTextureView transmittance1) {
        tape().terrain(view, stage, scissorX, scissorY, scissorW, scissorH, atlas, light, bounds, transmittance0, transmittance1);
    }

    private static void binding(MemorySegment record, long at, long handle, int kind, int slot, int format, long offset, long size) {
        record.set(JAVA_LONG, at, handle)
        ;
        record.set(JAVA_LONG, at + 8, offset)
        ;
        record.set(JAVA_LONG, at + 16, size)
        ;
        record.set(JAVA_BYTE, at + 24, (byte)kind)
        ;
        record.set(JAVA_BYTE, at + 25, (byte)format)
        ;
        record.set(JAVA_SHORT, at + 26, (short)slot)
        ;
        record.set(JAVA_INT, at + 28, 0)
        ;
    }
    private void writeBuffer(MemorySegment record, long at, int kind, int slot, GpuBuffer buffer, long offset, long size, int format, int usage) {
        if (!(buffer instanceof NativeBuffer resource))
            throw new IllegalArgumentException("Invalid GPU buffer");
        tape.validateBuffer(resource, offset, size, usage);
        tape.retain(resource);
        binding(record, at, resource.handle(), kind, slot, format, offset, size);
        record.set(JAVA_LONG, at + 32, 0)
        ;
        record.set(JAVA_LONG, at + 40, 0)
        ;
    }
    private void writeSlice(MemorySegment record, long at, int kind, int slot, GpuBufferSlice slice, int format, int usage) {
        writeBuffer(record, at, kind, slot, slice.buffer(), slice.offset(), slice.length(), format, usage);
    }
    private void writeUniforms(MemorySegment record, long at, List<RayniumPrograms.Route> routes, int[] map) {
        for (int i = 0; i < routes.size(); ++i, at += 48) {
            var route = routes.get(i);
            var value = uniforms[map == null ? i : map[i]];
            if (value == null)
                throw new IllegalStateException("Unbound shader resource " + i);
            switch (route.type()) {
                case UNIFORM_BUFFER -> writeSlice(record, at, 3, route.binding(), (GpuBufferSlice)value, 0, GpuBuffer.USAGE_UNIFORM);
                case TEXEL_BUFFER -> writeSlice(record, at, 4, route.binding(), (GpuBufferSlice)value,
                                                NativeTexture.Format.from(route.format()).ordinal(), GpuBuffer.USAGE_UNIFORM_TEXEL_BUFFER);
                case COMBINED_IMAGE_SAMPLER -> {
                    var pair = (TextureViewAndSampler)value;
                    var view = pair.view();
                    var sampler = pair.sampler();
                    if (view.isClosed() || sampler.isClosed() || !(view.texture() instanceof NativeTexture image) ||
                        route.cube() != ((image.flags() & NativeTexture.CUBE) != 0))
                        throw new IllegalArgumentException("Invalid sampled texture");
                    tape.validateTexture(image);
                    tape.retain(image);
                    binding(record, at, image.handle(), 5, route.binding(), RayniumSampler.flags(sampler), 0, 0);
                    record.set(JAVA_INT, at + 32, view.baseMipLevel())
                    ;
                    record.set(JAVA_INT, at + 36, view.mipLevels())
                    ;
                    record.set(JAVA_FLOAT, at + 40, (float)sampler.getMaxLod().orElse(1000))
                    ;
                    record.set(JAVA_INT, at + 44, sampler.getMaxAnisotropy())
                    ;
                }
            }
        }
    }
    boolean instancedGeometry(String mode, long uploadOffset, int instanceCount, int instanceStride, int[] runMeshIds, int[] runFirstInstances, int[] runInstanceCounts, int runCount) {
        if (pipeline == null || !pipeline.supportsInstanced(mode))
            return false;
        if (instanceCount <= 0 || runCount <= 0)
            return true;
        if (runCount > runMeshIds.length || runCount > runFirstInstances.length || runCount > runInstanceCounts.length)
            throw new IllegalArgumentException("Instanced run table is shorter than run count");
        int bindingCount = pipeline.instancedRoutes.size();
        var record = tape.beginInstancedGeometry(pipeline, uploadOffset, instanceCount, instanceStride, runCount,
                                                 scissorX, scissorY, scissorW, scissorH, bindingCount, push, pushLength);
        try {
            writeUniforms(record, 64, pipeline.instancedRoutes, pipeline.instancedUniformMap);
        } catch (RuntimeException | Error failure) {
            tape.discardLastRecord(record);
            throw failure;
        }
        long runAt = 64 + 48L * bindingCount;
        for (int i = 0; i < runCount; ++i) {
            record.set(JAVA_INT, runAt, runMeshIds[i])
            ;
            record.set(JAVA_INT, runAt + 4, runFirstInstances[i])
            ;
            record.set(JAVA_INT, runAt + 8, runInstanceCounts[i])
            ;
            record.set(JAVA_INT, runAt + 12, 0)
            ;
            runAt += 16;
        }
        return true;
    }

    private void drawCommand(int kind, int count, int instances, int first, int baseVertex, int firstInstance, GpuBufferSlice indirect) {
        if (pipeline == null || pipeline.isClosed())
            throw new IllegalStateException("Missing render pipeline");
        if (count < 0 || instances < 0 || first < 0 || firstInstance < 0)
            throw new IllegalArgumentException("Invalid draw arguments");
        if (count == 0 || instances == 0)
            return;
        if (indirect == null && pipeline.topology == PrimitiveTopology.POINTS) {
            if (instances != 1 || firstInstance != 0)
                throw new IllegalArgumentException("Instanced point expansion");
            firstInstance = Math.addExact(first, baseVertex);
            if (firstInstance < 0)
                throw new IllegalArgumentException("Negative point offset");
            instances = count;
            count = 6;
            first = baseVertex = kind = 0;
        } else if (indirect == null && pipeline.topology == PrimitiveTopology.TRIANGLE_FAN) {
            if (count < 3)
                return;
            baseVertex = Math.addExact(baseVertex, first);
            kind = 4;
            first = 0;
        }
        boolean indexed = kind == 1 || kind == 3;
        if (indexed && indexBuffer == null)
            throw new IllegalStateException("Index buffer required");
        int bindings = Integer.bitCount(vertexMask) + (indexed ? 1 : 0) + (indirect != null ? 1 : 0) + uniformCount;
        var record = tape.beginDraw(pipeline, kind, count, instances, first, firstInstance, baseVertex, scissorX, scissorY, scissorW, scissorH, bindings, push, pushLength);
        try {
            long at = 64;
            for (int mask = vertexMask; mask != 0; mask &= mask - 1) {
                int slot = Integer.numberOfTrailingZeros(mask);
                writeSlice(record, at, 0, slot, vertices[slot], 0, GpuBuffer.USAGE_VERTEX);
                at += 48;
            }
            if (indexed) {
                writeBuffer(record, at, 1, 0, indexBuffer, 0, indexBuffer.size(), indexType.bytes, GpuBuffer.USAGE_INDEX);
                at += 48;
            }
            if (indirect != null) {
                writeSlice(record, at, 2, 0, indirect, 0, GpuBuffer.USAGE_INDIRECT_PARAMETERS);
                at += 48;
            }
            writeUniforms(record, at, pipeline.routes, null);
        } catch (RuntimeException | Error failure) {
            tape.discardLastRecord(record);
            throw failure;
        }
    }
    @Override
    public void draw(int count, int instances, int first, int firstInstance) {
        drawCommand(0, count, instances, first, 0, firstInstance, null);
    }
    @Override
    public void drawIndexed(int count, int instances, int first, int base, int firstInstance) {
        drawCommand(1, count, instances, first, base, firstInstance, null);
    }
    @Override
    public void drawIndirect(GpuBufferSlice commands, int count) {
        drawCommand(2, count, 1, 0, 0, 0, commands);
    }
    @Override
    public void drawIndexedIndirect(GpuBufferSlice commands, int count) {
        drawCommand(3, count, 1, 0, 0, 0, commands);
    }
    @Override
    public void multiDraw(IntBuffer args, int instances, int firstInstance, int count) {
        Objects.checkFromIndexSize(0, Math.multiplyExact(2, count), args.remaining());
        for (int i = 0; i < count; ++i) {
            int p = args.position() + 2 * i;
            draw(args.get(p + 1), instances, args.get(p), firstInstance);
        }
    }
    @Override
    public void multiDrawIndexed(IntBuffer args, int instances, int firstInstance, int count) {
        Objects.checkFromIndexSize(0, Math.multiplyExact(3, count), args.remaining());
        for (int i = 0; i < count; ++i) {
            int p = args.position() + 3 * i;
            drawIndexed(args.get(p + 1), instances, args.get(p), args.get(p + 2), firstInstance);
        }
    }
    @Override
    public void multiDraw(IntBuffer first, IntBuffer counts, int count) {
        Objects.checkFromIndexSize(0, count, Math.min(first.remaining(), counts.remaining()));
        for (int i = 0; i < count; ++i)
            draw(counts.get(counts.position() + i), 1, first.get(first.position() + i), 0);
    }
    @Override
    public void multiDrawIndexed(PointerBuffer offsets, IntBuffer counts, IntBuffer bases, int count) {
        Objects.checkFromIndexSize(0, count, Math.min(offsets.remaining(), Math.min(counts.remaining(), bases.remaining())));
        for (int i = 0; i < count; ++i)
            drawIndexed(counts.get(counts.position() + i), 1, Math.toIntExact(offsets.get(offsets.position() + i) / indexType.bytes), bases.get(bases.position() + i), 0);
    }
}
