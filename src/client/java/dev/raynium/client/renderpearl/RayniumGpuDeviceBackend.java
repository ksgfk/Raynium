package dev.raynium.client.renderpearl;

import com.mojang.renderpearl.api.GpuFormat;
import com.mojang.renderpearl.api.buffers.GpuBuffer;
import com.mojang.renderpearl.api.commands.GpuQueryPool;
import com.mojang.renderpearl.api.device.DeviceFeatures;
import com.mojang.renderpearl.api.device.DeviceInfo;
import com.mojang.renderpearl.api.device.DeviceLimits;
import com.mojang.renderpearl.api.device.DeviceType;
import com.mojang.renderpearl.api.device.HintsAndWorkarounds;
import com.mojang.renderpearl.api.textures.AddressMode;
import com.mojang.renderpearl.api.textures.FilterMode;
import com.mojang.renderpearl.api.textures.GpuSampler;
import com.mojang.renderpearl.api.textures.GpuTexture;
import com.mojang.renderpearl.api.textures.GpuTextureView;
import com.mojang.renderpearl.backend.api.BackendRenderPipeline;
import com.mojang.renderpearl.backend.api.GpuDeviceBackend;
import dev.raynium.bridge.NativeRenderer;
import dev.raynium.bridge.NativeRuntime;
import dev.raynium.bridge.NativeTexture;
import java.nio.ByteBuffer;
import java.util.List;
import java.util.OptionalDouble;
import java.util.Set;
import java.util.function.BooleanSupplier;
import java.util.function.Supplier;

final class RayniumGpuDeviceBackend implements GpuDeviceBackend {
    final NativeRuntime runtime;
    final NativeRenderer renderer;
    final RayniumCommandEncoderBackend encoder;
    final DeviceInfo info;
    final int maxPushBytes;
    private final boolean debug;
    RayniumGpuDeviceBackend(NativeRuntime runtime, NativeRenderer renderer, boolean debug) {
        this.runtime = runtime;
        this.renderer = renderer;
        this.debug = debug;
        var data = runtime.deviceInfo();
        var caps = data.transfer();
        maxPushBytes = caps.maxPushConstantBytes();
        int indirect = (int)Math.min(Integer.MAX_VALUE, caps.maxIndirectDrawCount());
        boolean canIndirect = (data.flags() & 2) != 0;
        var limits = new DeviceLimits(Math.max(1, (int)data.maxAnisotropy()), Math.toIntExact(data.uniformOffsetAlignment()), data.maxTextureSize(),
                                      caps.maxBufferSize(), Integer.MAX_VALUE, data.maxColorAttachments(), indirect);
        var features = new DeviceFeatures(data.wireframe(), false, true, true, canIndirect && indirect > 1, canIndirect, (data.flags() & 4) != 0, (data.flags() & 16) != 0);
        var type = switch (data.deviceType()) { case 1->DeviceType.INTEGRATED; case 2->DeviceType.DISCRETE; case 3->DeviceType.CPU; default->DeviceType.OTHER; };
        info = new DeviceInfo(data.name(), data.vendor(), data.driver(), true, "Raynium", (float)data.timestamps().tickPeriodNs(), limits, features,
                              Set.of(), new HintsAndWorkarounds(false, false, false, false), type);
        encoder = new RayniumCommandEncoderBackend(renderer, caps.hostCoherentAtomSize());
    }
    @Override
    public RayniumGpuSurfaceBackend createSurface(long window, BooleanSupplier iconified) {
        return new RayniumGpuSurfaceBackend(renderer, window, iconified);
    }
    @Override
    public RayniumCommandEncoderBackend createCommandEncoder() {
        return encoder;
    }
    @Override
    public GpuTexture createTexture(String label, int usage, GpuFormat format, int w, int h, int layers, int mips) {
        if (w <= 0 || h <= 0 || w > info.limits().maxTextureSize() || h > info.limits().maxTextureSize() || mips <= 0 ||
            mips > 32 - Integer.numberOfLeadingZeros(Math.max(w, h)) || layers <= 0)
            throw new IllegalArgumentException("Invalid texture dimensions");
        boolean cube = (usage & GpuTexture.USAGE_CUBEMAP_COMPATIBLE) != 0;
        if (cube && (layers != 6 || w != h) || !cube && layers != 1)
            throw new UnsupportedOperationException("Unsupported RenderPearl texture dimension");
        return renderer.createTexture(label, usage, format, w, h, layers, mips);
    }
    @Override
    public GpuTextureView createTextureView(GpuTexture texture, int base, int mips) {
        ((NativeTexture)texture).checkRecordingOwner(renderer);
        return new RayniumTextureView(texture, base, mips);
    }
    @Override
    public GpuSampler createSampler(AddressMode u, AddressMode v, FilterMode min, FilterMode mag, int anisotropy, OptionalDouble maxLod) {
        if (anisotropy < 1 || anisotropy > info.limits().maxAnisotropy())
            throw new IllegalArgumentException("Invalid anisotropy");
        return new RayniumSampler(u, v, min, mag, anisotropy, maxLod);
    }
    @Override
    public GpuBuffer createBuffer(Supplier<String> label, int usage, long size) {
        return renderer.createBuffer(usage, size, null, encoder::writeStaging);
    }
    @Override
    public GpuBuffer createBuffer(Supplier<String> label, int usage, ByteBuffer bytes) {
        return renderer.createBuffer(usage, bytes.remaining(), bytes, encoder::writeStaging);
    }
    @Override
    public GpuQueryPool createTimestampQueryPool(int size) {
        return new RayniumQueryPool(renderer, size);
    }
    @Override
    public long getTimestampCalibrationOffset() {
        try (var queries = new RayniumQueryPool(renderer, 1); var tape = new CommandTape(renderer); var done = tape.createFence()) {
            tape.writeTimestamp(queries, 0);
            long before = System.nanoTime();
            tape.submitSequence(false);
            done.awaitCompletion(-1);
            long after = System.nanoTime();
            return before + (after - before) / 2 - (long)(queries.getValue(0).orElseThrow() * info.timestampPeriod());
        }
    }
    @Override
    public DeviceInfo getDeviceInfo() {
        return info;
    }
    @Override
    public List<String> getLastDebugMessages() {
        return List.of();
    }
    @Override
    public boolean isDebuggingEnabled() {
        return debug;
    }
    @Override
    public BackendRenderPipeline.Pending compilePipeline(BackendRenderPipeline.CreateInfo unused) {
        throw new UnsupportedOperationException("RenderPearl pipelines are compiled through GpuDevice.compilePipeline");
    }
    @Override
    public void close() {
        try (runtime; renderer) {
            encoder.close();
        }
    }
}
