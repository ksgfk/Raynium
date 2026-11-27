package dev.raynium.client.renderpearl;

import com.mojang.renderpearl.api.GpuFormat;
import com.mojang.renderpearl.api.buffers.GpuBuffer;
import com.mojang.renderpearl.api.commands.CommandEncoder;
import com.mojang.renderpearl.api.commands.GpuQueryPool;
import com.mojang.renderpearl.api.commands.RenderPass;
import com.mojang.renderpearl.api.commands.RenderPassDescriptor;
import com.mojang.renderpearl.api.device.DeviceInfo;
import com.mojang.renderpearl.api.device.GpuDevice;
import com.mojang.renderpearl.api.device.GpuSurface;
import com.mojang.renderpearl.api.pipeline.CompiledRenderPipeline;
import com.mojang.renderpearl.api.pipeline.PolygonMode;
import com.mojang.renderpearl.api.pipeline.RenderPipeline;
import com.mojang.renderpearl.api.pipeline.ShaderSource;
import com.mojang.renderpearl.api.textures.AddressMode;
import com.mojang.renderpearl.api.textures.FilterMode;
import com.mojang.renderpearl.api.textures.GpuSampler;
import com.mojang.renderpearl.api.textures.GpuTexture;
import com.mojang.renderpearl.api.textures.GpuTextureView;
import com.mojang.renderpearl.frontend.FrontendCommandEncoder;
import com.mojang.renderpearl.frontend.FrontendRenderPass;
import com.mojang.renderpearl.util.TextureViewAndSampler;
import dev.raynium.bridge.NativeBuffer;
import dev.raynium.bridge.NativeEnvironment;
import dev.raynium.bridge.NativeRenderer;
import dev.raynium.bridge.NativeRuntime;
import dev.raynium.client.terrain.RetainedTerrain;
import dev.raynium.client.terrain.TerrainCapture;
import dev.raynium.client.world.RetainedModelManager;
import java.nio.ByteBuffer;
import java.nio.file.Path;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Optional;
import java.util.OptionalDouble;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Executor;
import java.util.function.BooleanSupplier;
import java.util.function.Supplier;
import net.minecraft.client.renderer.LevelRenderer;
import org.slf4j.LoggerFactory;
public final class RayniumGpuDevice implements GpuDevice, AutoCloseable {
    private final RayniumGpuDeviceBackend backend;
    private final RayniumPrograms programs;
    private final FrontendCommandEncoder encoder;
    private final RetainedTerrain terrain = new RetainedTerrain();
    // The bundled catalog has 192 registered required/optional pipelines before custom additions.
    private static final int EXPECTED_COMPILED_PIPELINES = 192;
    private final IdentityHashMap<RenderPipeline, RayniumPipeline> compiledPipelines = new IdentityHashMap<>(EXPECTED_COMPILED_PIPELINES);
    public CommandTape currentBatch() {
        return backend.encoder.tape();
    }
    public NativeRenderer nativeRenderer() {
        return backend.renderer;
    }
    public NativeEnvironment.Backend backend() {
        return backend.runtime.backend();
    }
    public boolean supportsInstanced(RenderPipeline pipeline, String mode) {
        var compiled = compiledPipelines.get(pipeline);
        return compiled != null && compiled.supportsInstanced(mode);
    }
    private static RayniumRenderPassBackend recorder(RenderPass pass) {
        if (!(pass instanceof FrontendRenderPass frontend))
            return null;
        if (frontend.isClosed)
            throw new IllegalStateException("Render pass is closed");
        return frontend.backend instanceof RayniumRenderPassBackend recorder ? recorder : null;
    }
    public static CommandTape activeTape(RenderPass pass) {
        var recorder = recorder(pass);
        if (recorder == null)
            throw new IllegalArgumentException("Invalid Raynium render pass");
        return recorder.tape();
    }
    public static boolean recordInstancedGeometry(RenderPass pass, String mode, long uploadOffset, int instanceCount, int instanceStride,
                                                  int[] runMeshIds, int[] runFirstInstances, int[] runInstanceCounts, int runCount) {
        var recorder = recorder(pass);
        if (recorder == null)
            return false;
        return recorder.instancedGeometry(mode, uploadOffset, instanceCount, instanceStride, runMeshIds, runFirstInstances, runInstanceCounts, runCount);
    }
    public CommandTape.TerrainView captureTerrain(LevelRenderer level) {
        return terrain.capture(level, backend.renderer, currentBatch());
    }
    public static void recordTerrain(RenderPass pass, CommandTape.TerrainView view, int stage,
                                     TextureViewAndSampler atlas, TextureViewAndSampler light, GpuTextureView... inputs) {
        if (inputs.length != (stage == 4 ? 1 : stage == 5 ? 3
                                                          : 0))
            throw new IllegalArgumentException("Invalid terrain stage inputs");
        recordTerrainStage(pass, view, stage, atlas, light, inputs.length > 0 ? inputs[0] : null,
                           inputs.length == 3 ? inputs[1] : null, inputs.length == 3 ? inputs[2] : null);
    }
    public static void recordTerrainStage(RenderPass pass, CommandTape.TerrainView view, int stage,
                                          TextureViewAndSampler atlas, TextureViewAndSampler light,
                                          GpuTextureView bounds, GpuTextureView transmittance0, GpuTextureView transmittance1) {
        var recorder = recorder(pass);
        if (recorder == null)
            throw new IllegalArgumentException("Invalid terrain render pass");
        recorder.terrain(view, stage, atlas, light, bounds, transmittance0, transmittance1);
    }
    public RayniumGpuDevice(NativeRuntime runtime, NativeRenderer renderer, boolean debug) {
        this(runtime, renderer, debug, Path.of("."), Optional.empty());
    }
    public RayniumGpuDevice(NativeRuntime runtime, NativeRenderer renderer, boolean debug, Path gameDirectory) {
        this(runtime, renderer, debug, gameDirectory, Optional.empty());
    }
    public RayniumGpuDevice(NativeRuntime runtime, NativeRenderer renderer, boolean debug, Path gameDirectory, Optional<Path> shaderToolsDir) {
        programs = new RayniumPrograms(gameDirectory, shaderToolsDir);
        backend = new RayniumGpuDeviceBackend(runtime, renderer, debug);
        TerrainCapture.invalidate();
        TerrainCapture.activate(true);
        encoder = new FrontendCommandEncoder(null, backend, backend.encoder) {
            @Override
            public RenderPass createRenderPass(RenderPassDescriptor descriptor) {
                CommandTape.validatePass(descriptor);
                return super.createRenderPass(descriptor);
            }
            @Override
            public void submit() {
                if (isInRenderPass())
                    throw new IllegalStateException("Close the render pass before submission");
                super.submit();
            }
        };
    }
    @Override
    public CommandEncoder createCommandEncoder() {
        return encoder;
    }
    @Override
    public GpuSurface createSurface(long window, BooleanSupplier iconified) {
        return backend.createSurface(window, iconified).frontend();
    }
    @Override
    public GpuSampler createSampler(AddressMode u, AddressMode v, FilterMode min, FilterMode mag, int anisotropy, OptionalDouble maxLod) {
        return backend.createSampler(u, v, min, mag, anisotropy, maxLod);
    }
    @Override
    public GpuTexture createTexture(Supplier<String> label, int usage, GpuFormat format, int w, int h, int layers, int mips) {
        return createTexture(label == null ? null : label.get(), usage, format, w, h, layers, mips);
    }
    @Override
    public GpuTexture createTexture(String label, int usage, GpuFormat format, int w, int h, int layers, int mips) {
        return backend.createTexture(label, usage, format, w, h, layers, mips);
    }
    @Override
    public GpuTextureView createTextureView(GpuTexture texture) {
        return createTextureView(texture, 0, texture.getMipLevels());
    }
    @Override
    public GpuTextureView createTextureView(GpuTexture texture, int base, int count) {
        return backend.createTextureView(texture, base, count);
    }
    public GpuBuffer createFenceRotatedBuffer(int usage, long size, NativeBuffer.WriteSource source) {
        return backend.renderer.createFenceRotatedBuffer(usage, size, backend.encoder::writeStaging, source);
    }
    @Override
    public GpuBuffer createBuffer(Supplier<String> label, int usage, long size) {
        return backend.createBuffer(label, usage, size);
    }
    @Override
    public GpuBuffer createBuffer(Supplier<String> label, int usage, ByteBuffer data) {
        return backend.createBuffer(label, usage, data);
    }
    @Override
    public List<String> getLastDebugMessages() {
        return backend.getLastDebugMessages();
    }
    @Override
    public boolean isDebuggingEnabled() {
        return backend.isDebuggingEnabled();
    }
    @Override
    public DeviceInfo getDeviceInfo() {
        return backend.info;
    }
    @Override
    public GpuQueryPool createTimestampQueryPool(int size) {
        return backend.createTimestampQueryPool(size);
    }
    @Override
    public CompletableFuture<CompiledRenderPipeline.Pending> compilePipeline(RenderPipeline pipeline, ShaderSource source, Executor executor) {
        if (pipeline.getPolygonMode() == PolygonMode.WIREFRAME && !backend.info.features().wireframeFillMode()) {
            LoggerFactory.getLogger("raynium").warn("Wireframe pipeline unavailable on this device: {}", pipeline.getLocation());
            return CompletableFuture.completedFuture(CompiledRenderPipeline.Pending.NULL);
        }
        return CompletableFuture.supplyAsync(() -> {
            try {
                int pushBytes = pipeline.pushConstantSize();
                if (pushBytes < 0 || pushBytes % 4 != 0 || pushBytes > backend.maxPushBytes)
                    throw new IllegalArgumentException("Unsupported push constant range " + pushBytes);
                var prepared = programs.prepare(pipeline, source);
                return () -> RayniumPipeline.register(backend.renderer, pipeline, prepared, compiledPipelines::put, (p, rp) -> { if(compiledPipelines.get(p)==rp) compiledPipelines.remove(p); });
            } catch (RuntimeException failure) {
                LoggerFactory.getLogger("raynium").error("Cannot prepare pipeline {}", pipeline.getLocation(), failure);
                return CompiledRenderPipeline.Pending.NULL;
            }
        }, executor);
    }
    public void retireTerrain() {
        TerrainCapture.invalidate();
        terrain.retire();
    }
    @Override
    public void close() {
        terrain.retire();
        TerrainCapture.activate(false);
        RetainedModelManager.onDeviceClose(backend.renderer);
        compiledPipelines.clear();
        backend.close();
    }
}
