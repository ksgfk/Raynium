package dev.raynium.client.terrain;

import com.mojang.renderpearl.backend.vulkan.*;
import dev.raynium.bridge.*;
import dev.raynium.client.preview.VulkanPreviewImage;
import net.minecraft.client.Minecraft;
import org.lwjgl.system.MemoryStack;
import org.lwjgl.vulkan.VkPhysicalDeviceFeatures;
import org.slf4j.Logger;
import java.util.*;
import static org.lwjgl.vulkan.VK12.*;

/** Render-thread input exchange. The native queue never CPU-waits for this frame's unsent MC work. */
public final class TerrainPreview implements AutoCloseable {
    private final NativeRuntime runtime;
    private final VulkanDevice device;
    private final VulkanCommandEncoder encoder;
    private final Logger logger;
    private final boolean direct;
    private NativeTerrain terrain;
    private final List<VulkanPreviewImage> images=new ArrayList<>();
    private final Set<Long> uploaded=new HashSet<>();
    private VulkanGpuTexture atlas;
    private long sequence=1,epoch;
    private final Set<Long> pendingRemovals=new HashSet<>();
    private record Position(int x,int y,int z) {}
    private Map<Position,NativeTerrain.Draw> previous=new HashMap<>();
    private final boolean profile=Boolean.getBoolean("raynium.terrain.profile");
    private final double[] prepareTimes=new double[120], drawTimes=new double[120];
    private int sampleCount;
    private double prepareMs;
    private long uploadBytes,textureBytes;
    private boolean pending,atlasChanged;

    public TerrainPreview(NativeRuntime runtime,VulkanDevice device,VulkanCommandEncoder encoder,Logger logger) {
        this.runtime=runtime; this.device=device; this.encoder=encoder; this.logger=logger;
        try(var stack=MemoryStack.stackPush()) {
            var features=VkPhysicalDeviceFeatures.calloc(stack); vkGetPhysicalDeviceFeatures(device.vkDevice().getPhysicalDevice(),features);
            direct=Boolean.getBoolean("raynium.terrain.directDraw") || !features.multiDrawIndirect() || !features.drawIndirectFirstInstance();
        }
    }
    private void create(VulkanGpuTexture texture,boolean split) {
        terrain=runtime.createTerrain(texture.getWidth(0),texture.getHeight(0),texture.getMipLevels(),split);
        try {
            for(int i=0;i<terrain.images().size();i++) images.add(VulkanPreviewImage.create(device.vkDevice(),
                device.graphicsQueue().queueFamilyIndex(),terrain.images().get(i),i==0 && !split?texture.getMipLevels():1,true));
        } catch(RuntimeException failure) {
            for(var image:images) image.close(); images.clear();
            terrain.close(); terrain=null; throw failure;
        }
        atlas=texture;
        logger.info("Raynium SOLID terrain inputs ready: {}x{} mips={} transfer={} draws={}",texture.getWidth(0),texture.getHeight(0),
            texture.getMipLevels(),split?"split-mip fallback":"direct shared mip texture",direct?"direct":"CPU indirect");
    }
    public void prepare(Minecraft client) {
        long start=System.nanoTime();
        if(pending) throw new IllegalStateException("Previous terrain input has not been consumed");
        var atlasObject=TerrainTextures.atlas();
        if(atlasObject==null || !(atlasObject.getTexture() instanceof VulkanGpuTexture texture)) return;
        boolean fresh=atlas!=texture;
        if(fresh) {
            close();
            String mode=System.getProperty("raynium.terrain.textureTransfer","auto");
            TextureTransfer.create(mode,()->{ create(texture,false); return terrain; },()->{ create(texture,true); return terrain; },
                failure->logger.warn("Direct mip sharing unavailable; trying split-mip fallback: {}",failure.getMessage()));
        }
        for(long id:TerrainCapture.drainRemoved()) pendingRemovals.add(id);
        var regions=TerrainTextures.drain(fresh); atlasChanged=!regions.isEmpty();
        boolean lightChanged=TerrainTextures.drainLight(fresh);
        if(!(client.gameRenderer.levelLightmap().texture() instanceof VulkanGpuTexture lightmap))
            throw new IllegalStateException("Lightmap is not a Vulkan texture");
        encoder.waitSemaphore(images.getFirst().readySemaphore(),sequence,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        var command=encoder.allocateAndBeginTransientCommandBuffer();
        for(var image:images) image.beginWrite(command);
        for(int mip=0;mip<texture.getMipLevels();mip++) {
            var image=images.get(terrain.splitMips()?mip:0);
            for(var r:regions) {
                int x=r.x()>>mip, y=r.y()>>mip;
                int endX=Math.min(texture.getWidth(mip),(r.x()+r.width()+(1<<mip)-1)>>mip);
                int endY=Math.min(texture.getHeight(mip),(r.y()+r.height()+(1<<mip)-1)>>mip);
                if(endX>x && endY>y) {
                    image.copyFrom(command,texture.vkImage(),mip,terrain.splitMips()?0:mip,x,y,endX-x,endY-y);
                    textureBytes+=(long)(endX-x)*(endY-y)*4;
                }
            }
        }
        if(lightChanged) images.getLast().copyFrom(command,lightmap.vkImage(),0,0,0,0,16,16);
        for(var image:images) image.endWrite(command);
        VulkanPreviewImage.check(vkEndCommandBuffer(command),"end terrain input copy"); encoder.execute(command);
        encoder.signalSemaphore(images.getFirst().consumedSemaphore(),sequence,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        pending=true; prepareMs=(System.nanoTime()-start)/1e6;
    }
    public boolean draw(NativePreview output,long outputSequence) {
        long start=System.nanoTime();
        var snapshot=TerrainCapture.snapshot();
        if(!pending || snapshot==null) return false;
        if(epoch!=snapshot.epoch()) { uploaded.clear(); previous.clear(); pendingRemovals.clear(); epoch=snapshot.epoch(); }
        var updates=new ArrayList<NativeTerrain.Mesh>(); var draws=new ArrayList<NativeTerrain.Draw>();
        var next=new HashMap<Position,NativeTerrain.Draw>(); var usedIds=new HashSet<Long>();
        long budget=8L*1024*1024,used=0;
        // MC's visible list is already ordered from the camera's section traversal.
        for(var visible:snapshot.visible()) {
            var payload=visible.payload();
            boolean ready=uploaded.contains(payload.id);
            if(!ready && (used==0 || used+payload.vertices.length<=budget)) {
                updates.add(new NativeTerrain.Mesh(payload.id,payload.vertices,payload.groupVertices)); used+=payload.vertices.length; ready=true;
            }
            var draw=visible.draw(); var key=new Position(draw.x(),draw.y(),draw.z());
            if(!ready && previous.containsKey(key) && uploaded.contains(previous.get(key).id())) {
                // Preserve the last ready version while its replacement is upload-budget limited.
                draw=new NativeTerrain.Draw(previous.get(key).id(),draw.x(),draw.y(),draw.z(),draw.visibility(),127);
            }
            draws.add(draw); next.put(key,draw); usedIds.add(draw.id());
        }
        long[] removed=pendingRemovals.stream().filter(id->!usedIds.contains(id)).mapToLong(Long::longValue).toArray();
        terrain.draw(output,outputSequence,new NativeTerrain.Frame(snapshot.epoch(),sequence,direct,atlasChanged,snapshot.rgss(),snapshot.anisotropy(),updates,draws,removed,
            snapshot.projection(),snapshot.view(),snapshot.block(),snapshot.fraction(),snapshot.fog(),snapshot.distances()));
        for(var update:updates) uploaded.add(update.id());
        for(long id:removed) { pendingRemovals.remove(id); uploaded.remove(id); }
        previous=next; sequence++; pending=false;
        if(profile) {
            uploadBytes+=used; prepareTimes[sampleCount]=prepareMs; drawTimes[sampleCount++]=(System.nanoTime()-start)/1e6;
            if(sampleCount==120) {
                Arrays.sort(prepareTimes); Arrays.sort(drawTimes);
                logger.info("Terrain CPU samples=120 input_ms_p50={} p95={} p99={} frame_ms_p50={} p95={} p99={} geometry_upload_bytes={} texture_copy_bytes={} visible={} resident={}",
                    prepareTimes[60],prepareTimes[114],prepareTimes[118],drawTimes[60],drawTimes[114],drawTimes[118],uploadBytes,textureBytes,draws.size(),uploaded.size());
                sampleCount=0; uploadBytes=textureBytes=0;
            }
        }
        return true;
    }
    public boolean pending() { return pending; }
    @Override public void close() {
        if(terrain==null) return;
        // Flush any queued input release before destroying imports, including a failed draw.
        try(var fence=encoder.createFence()) {
            encoder.submit();
            if(!fence.awaitCompletion(5_000_000_000L)) throw new IllegalStateException("MC terrain drain timed out; resources remain live");
        }
        images.getFirst().awaitReady(sequence);
        for(var image:images) image.close(); images.clear();
        terrain.close(); terrain=null; atlas=null; sequence=1; pending=false; uploaded.clear(); previous.clear(); pendingRemovals.clear(); epoch=0;
    }
}
