package dev.raynium.client.renderpearl;

import com.mojang.renderpearl.api.GpuFormat;
import com.mojang.renderpearl.api.buffers.*;
import com.mojang.renderpearl.api.commands.*;
import com.mojang.renderpearl.api.device.*;
import com.mojang.renderpearl.api.pipeline.*;
import com.mojang.renderpearl.api.textures.*;
import com.mojang.renderpearl.frontend.FrontendCommandEncoder;
import dev.raynium.bridge.*;
import java.nio.ByteBuffer;
import java.util.*;
import java.util.concurrent.*;
import java.util.function.*;
import org.slf4j.LoggerFactory;

public final class RayniumGpuDevice implements GpuDevice,AutoCloseable {
    private final RayniumGpuDeviceBackend backend;
    private final RayniumPrograms programs;
    private final FrontendCommandEncoder encoder;
    private final dev.raynium.client.terrain.RetainedTerrain terrain=new dev.raynium.client.terrain.RetainedTerrain();
    private final IdentityHashMap<RenderPipeline,RayniumPipeline> compiledPipelines=new IdentityHashMap<>();
    public CommandTape currentBatch() { return backend.encoder.tape(); }
    public NativeRenderer nativeRenderer() { return backend.renderer; }
    public boolean supportsInstanced(RenderPipeline pipeline,String mode) {
        var compiled=compiledPipelines.get(pipeline);
        return compiled!=null && compiled.supportsInstanced(mode);
    }
    public static CommandTape activeTape(RenderPass pass) {
        if(!(pass instanceof com.mojang.renderpearl.frontend.FrontendRenderPass frontend) ||
            !(frontend.backend instanceof RayniumRenderPassBackend.Lease lease)) throw new IllegalArgumentException("Invalid Raynium render pass");
        return lease.recorder().tape();
    }
    public static boolean recordInstancedGeometry(RenderPass pass,String mode,long uploadOffset,int instanceCount,int instanceStride,
            int[] runMeshIds,int[] runFirstInstances,int[] runInstanceCounts,int runCount) {
        if(!(pass instanceof com.mojang.renderpearl.frontend.FrontendRenderPass frontend) ||
            !(frontend.backend instanceof RayniumRenderPassBackend.Lease lease)) return false;
        return lease.recorder().instancedGeometry(mode,uploadOffset,instanceCount,instanceStride,runMeshIds,runFirstInstances,runInstanceCounts,runCount);
    }
    public CommandTape.TerrainView captureTerrain(net.minecraft.client.renderer.LevelRenderer level) {
        return terrain.capture(level,backend.renderer,currentBatch());
    }
    public static void recordTerrain(RenderPass pass,CommandTape.TerrainView view,int stage,
            com.mojang.renderpearl.util.TextureViewAndSampler atlas,com.mojang.renderpearl.util.TextureViewAndSampler light,GpuTextureView... inputs) {
        if(!(pass instanceof com.mojang.renderpearl.frontend.FrontendRenderPass frontend) ||
            !(frontend.backend instanceof RayniumRenderPassBackend.Lease lease)) throw new IllegalArgumentException("Invalid terrain render pass");
        lease.recorder().terrain(view,stage,atlas,light,inputs);
    }
    public RayniumGpuDevice(NativeRuntime runtime,NativeRenderer renderer,boolean debug) {
        this(runtime,renderer,debug,java.nio.file.Path.of("."));
    }
    public RayniumGpuDevice(NativeRuntime runtime,NativeRenderer renderer,boolean debug,java.nio.file.Path gameDirectory) {
        programs=new RayniumPrograms(gameDirectory);
        backend=new RayniumGpuDeviceBackend(runtime,renderer,debug);
        dev.raynium.client.terrain.TerrainCapture.invalidate();
        dev.raynium.client.terrain.TerrainCapture.activate(true);
        encoder=new FrontendCommandEncoder(null,backend,backend.encoder) {
            @Override public RenderPass createRenderPass(RenderPassDescriptor descriptor) {
                CommandTape.validatePass(descriptor);
                return super.createRenderPass(descriptor);
            }
            @Override public void submit() {
                if(isInRenderPass()) throw new IllegalStateException("Close the render pass before submission");
                super.submit();
            }
        };
    }
    @Override public CommandEncoder createCommandEncoder() { return encoder; }
    @Override public GpuSurface createSurface(long window,BooleanSupplier iconified) { return backend.createSurface(window,iconified).frontend(); }
    @Override public GpuSampler createSampler(AddressMode u,AddressMode v,FilterMode min,FilterMode mag,int anisotropy,OptionalDouble maxLod) {
        return backend.createSampler(u,v,min,mag,anisotropy,maxLod);
    }
    @Override public GpuTexture createTexture(Supplier<String> label,int usage,GpuFormat format,int w,int h,int layers,int mips) {
        return createTexture(label==null?null:label.get(),usage,format,w,h,layers,mips);
    }
    @Override public GpuTexture createTexture(String label,int usage,GpuFormat format,int w,int h,int layers,int mips) {
        return backend.createTexture(label,usage,format,w,h,layers,mips);
    }
    @Override public GpuTextureView createTextureView(GpuTexture texture) { return createTextureView(texture,0,texture.getMipLevels()); }
    @Override public GpuTextureView createTextureView(GpuTexture texture,int base,int count) { return backend.createTextureView(texture,base,count); }
    public GpuBuffer createFenceRotatedBuffer(int usage,long size,dev.raynium.bridge.NativeBuffer.WriteSource source) { return backend.renderer.createFenceRotatedBuffer(usage,size,backend.encoder::tape,source); }
    @Override public GpuBuffer createBuffer(Supplier<String> label,int usage,long size) { return backend.createBuffer(label,usage,size); }
    @Override public GpuBuffer createBuffer(Supplier<String> label,int usage,ByteBuffer data) { return backend.createBuffer(label,usage,data); }
    @Override public List<String> getLastDebugMessages() { return backend.getLastDebugMessages(); }
    @Override public boolean isDebuggingEnabled() { return backend.isDebuggingEnabled(); }
    @Override public DeviceInfo getDeviceInfo() { return backend.info; }
    @Override public GpuQueryPool createTimestampQueryPool(int size) { return backend.createTimestampQueryPool(size); }
    @Override public CompletableFuture<CompiledRenderPipeline.Pending> compilePipeline(RenderPipeline pipeline,ShaderSource source,Executor executor) {
        if(pipeline.getPolygonMode()==PolygonMode.WIREFRAME && !backend.info.features().wireframeFillMode()) {
            LoggerFactory.getLogger("raynium").warn("Wireframe pipeline unavailable on this device: {}",pipeline.getLocation());
            return CompletableFuture.completedFuture(CompiledRenderPipeline.Pending.NULL);
        }
        return CompletableFuture.supplyAsync(()->{
            try {
                int pushBytes=pipeline.pushConstantSize();
                if(pushBytes<0 || pushBytes%4!=0 || pushBytes>backend.maxPushBytes)
                    throw new IllegalArgumentException("Unsupported push constant range "+pushBytes);
                var prepared=programs.prepare(pipeline,source);
                return ()->RayniumPipeline.register(backend.renderer,pipeline,prepared,
                    compiledPipelines::put,
                    (p,rp)->{ if(compiledPipelines.get(p)==rp) compiledPipelines.remove(p); });
            } catch(RuntimeException failure) {
                LoggerFactory.getLogger("raynium").error("Cannot prepare pipeline {}",pipeline.getLocation(),failure);
                return CompiledRenderPipeline.Pending.NULL;
            }
        },executor);
    }
    @Override public void close() {
        dev.raynium.client.terrain.TerrainCapture.activate(false);
        dev.raynium.client.world.RetainedModelManager.onDeviceClose(backend.renderer);
        compiledPipelines.clear();
        backend.close();
    }
}
