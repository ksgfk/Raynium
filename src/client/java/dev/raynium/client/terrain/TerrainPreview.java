package dev.raynium.client.terrain;

import com.mojang.renderpearl.backend.vulkan.*;
import dev.raynium.bridge.*;
import dev.raynium.client.preview.VulkanPreviewImage;
import net.minecraft.client.Minecraft;
import org.slf4j.Logger;
import java.util.*;
import static org.lwjgl.vulkan.VK12.*;

/** Render-thread input exchange. The native queue never CPU-waits for this frame's unsent MC work. */
public final class TerrainPreview implements AutoCloseable {
    private final NativeRenderer renderer;
    private final VulkanDevice device;
    private final VulkanCommandEncoder encoder;
    private final Logger logger;
    private final boolean direct;
    private NativeSharedTextures textures;
    private boolean splitMips;
    private int indexCapacity,anisotropy;
    private final List<VulkanPreviewImage> images=new ArrayList<>();
    private final Map<Long,MeshPayload> uploaded=new HashMap<>();
    private VulkanGpuTexture atlas;
    private long sequence=1,epoch,sourceEpoch;
    private final Set<Long> pendingRemovals=new HashSet<>();
    private record Position(int x,int y,int z) {}
    private Map<Position,TerrainCapture.Draw> previous=new HashMap<>();
    private final boolean profile=Boolean.getBoolean("raynium.terrain.profile");
    private final double[] prepareTimes=new double[120], drawTimes=new double[120];
    private int sampleCount;
    private double prepareMs;
    private long uploadBytes,textureBytes;
    private boolean pending;
    private int changedTextures;

    public TerrainPreview(NativeRenderer renderer,VulkanDevice device,VulkanCommandEncoder encoder,Logger logger) {
        this.renderer=renderer; this.device=device; this.encoder=encoder; this.logger=logger;
        direct=Boolean.getBoolean("raynium.terrain.directDraw");
    }
    private void create(VulkanGpuTexture texture,boolean split) {
        textures=renderer.createSharedTextures(List.of(
            new NativeSharedTextures.Texture(texture.getWidth(0),texture.getHeight(0),texture.getMipLevels(),split),
            new NativeSharedTextures.Texture(16,16,1,false)));
        splitMips=split;
        try {
            for(var exported:textures.exports()) images.add(VulkanPreviewImage.create(device.vkDevice(),
                device.graphicsQueue().queueFamilyIndex(),exported.image(),exported.mipCount(),true));
        } catch(RuntimeException failure) {
            for(var image:images) image.close(); images.clear();
            textures.close(); textures=null; throw failure;
        }
        atlas=texture;
        logger.info("Raynium SOLID terrain inputs ready: {}x{} mips={} transfer={} draws={}",texture.getWidth(0),texture.getHeight(0),
            texture.getMipLevels(),split?"split-mip fallback":"direct shared mip texture",direct?"direct":"native capability selection");
    }
    public void prepare(Minecraft client) {
        long start=System.nanoTime();
        if(pending) throw new IllegalStateException("Previous terrain input has not been consumed");
        var atlasObject=TerrainTextures.atlas();
        if(atlasObject==null || !(atlasObject.getTexture() instanceof VulkanGpuTexture texture)) return;
        boolean fresh=atlas!=texture;
        if(fresh) {
            closeInputs();
            String mode=System.getProperty("raynium.terrain.textureTransfer","auto");
            TextureTransfer.create(mode,()->{ create(texture,false); return textures; },()->{ create(texture,true); return textures; },
                failure->logger.warn("Direct mip sharing unavailable; trying split-mip fallback: {}",failure.getMessage()));
        }
        for(long id:TerrainCapture.drainRemoved()) pendingRemovals.add(id);
        var regions=TerrainTextures.drain(fresh); changedTextures=regions.isEmpty()?0:1;
        boolean lightChanged=TerrainTextures.drainLight(fresh);
        if(lightChanged) changedTextures|=2;
        if(!(client.gameRenderer.levelLightmap().texture() instanceof VulkanGpuTexture lightmap))
            throw new IllegalStateException("Lightmap is not a Vulkan texture");
        encoder.waitSemaphore(images.getFirst().readySemaphore(),sequence,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        var command=encoder.allocateAndBeginTransientCommandBuffer();
        for(var image:images) image.beginWrite(command);
        for(int mip=0;mip<texture.getMipLevels();mip++) {
            var image=images.get(splitMips?mip:0);
            for(var r:regions) {
                int x=r.x()>>mip, y=r.y()>>mip;
                int endX=Math.min(texture.getWidth(mip),(r.x()+r.width()+(1<<mip)-1)>>mip);
                int endY=Math.min(texture.getHeight(mip),(r.y()+r.height()+(1<<mip)-1)>>mip);
                if(endX>x && endY>y) {
                    image.copyFrom(command,texture.vkImage(),mip,splitMips?0:mip,x,y,endX-x,endY-y);
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
    public boolean draw(NativeRenderTarget output,long outputSequence) {
        long start=System.nanoTime();
        var snapshot=TerrainCapture.snapshot();
        if(!pending || snapshot==null) return false;
        if(sourceEpoch!=snapshot.epoch()) {
            uploaded.clear(); previous.clear(); pendingRemovals.clear(); sourceEpoch=snapshot.epoch();
            epoch=renderer.nextEpoch(); indexCapacity=anisotropy=0;
        }
        var frame=new NativeRenderer.Frame(); frame.epoch=epoch; frame.outputSequence=outputSequence;
        frame.inputSequence=sequence; frame.changedTextures=changedTextures;
        frame.flags=(direct?NativeRenderer.DIRECT:0)|(snapshot.rgss()?NativeRenderer.RGSS:0)|(profile?NativeRenderer.PROFILE:0);
        frame.projection=snapshot.projection(); frame.view=snapshot.view(); frame.cameraBlock=snapshot.block();
        frame.cameraFraction=snapshot.fraction(); frame.fogColor=snapshot.fog(); frame.fogDistances=snapshot.distances();
        var updates=new ArrayList<NativeRenderer.Mesh>(); var primitives=new ArrayList<NativeRenderer.Primitive>();
        var visibleItems=new ArrayList<NativeRenderer.Visible>(); var instances=new ArrayList<NativeRenderer.Instance>();
        var staged=new HashMap<Long,MeshPayload>();
        var next=new HashMap<Position,TerrainCapture.Draw>(); var usedIds=new HashSet<Long>();
        long budget=8L*1024*1024,used=0; int needed=indexCapacity,order=0;
        for(var visible:snapshot.visible()) {
            var payload=visible.payload();
            boolean ready=uploaded.containsKey(payload.id) || staged.containsKey(payload.id);
            if(!ready && payload.vertices.length>0 && (used==0 || used+payload.vertices.length<=budget)) {
                updates.add(TerrainScene.mesh(payload)); primitives.add(new NativeRenderer.Primitive(payload.id,payload.id,1));
                staged.put(payload.id,payload); used+=payload.vertices.length; ready=true;
                needed=Math.max(needed,payload.vertices.length/28);
            }
            var draw=visible.draw(); var key=new Position(draw.x(),draw.y(),draw.z());
            if(!ready) {
                var old=previous.get(key);
                if(old==null || !uploaded.containsKey(old.id())) continue;
                draw=new TerrainCapture.Draw(old.id(),draw.x(),draw.y(),draw.z(),draw.visibility(),127);
                payload=uploaded.get(old.id());
            }
            int instance=instances.size(); instances.add(new NativeRenderer.Instance(draw.x(),draw.y(),draw.z(),draw.visibility()));
            int submesh=0;
            for(int group=0;group<7;group++) if(payload.groupVertices[group]>0) {
                if((draw.groupMask()&(1<<group))!=0) visibleItems.add(new NativeRenderer.Visible(draw.id(),submesh,order/64,instance,1));
                submesh++;
            }
            order++; next.put(key,draw); usedIds.add(draw.id());
        }
        long[] removed=pendingRemovals.stream().filter(id->!usedIds.contains(id)).mapToLong(Long::longValue).toArray();
        int capacity=indexCapacity;
        if(needed>indexCapacity) {
            capacity=Math.max(4,Integer.highestOneBit(needed-1)<<1);
            frame.indices=List.of(new NativeRenderer.Index(1,TerrainScene.quadIndices(capacity)));
        }
        if(anisotropy!=snapshot.anisotropy()) frame.materials=List.of(new NativeRenderer.Material(1,NativeRenderer.TEXTURED,0,1,snapshot.anisotropy()));
        frame.meshes=updates; frame.primitives=primitives; frame.visible=visibleItems; frame.instances=instances;
        frame.removedMeshes=removed; frame.removedPrimitives=removed;
        renderer.render(output,frame);
        indexCapacity=capacity; anisotropy=snapshot.anisotropy(); uploaded.putAll(staged);
        for(long id:removed) { pendingRemovals.remove(id); uploaded.remove(id); }
        previous=next; sequence++; pending=false;
        if(profile) {
            uploadBytes+=used; prepareTimes[sampleCount]=prepareMs; drawTimes[sampleCount++]=(System.nanoTime()-start)/1e6;
            if(sampleCount==120) {
                Arrays.sort(prepareTimes); Arrays.sort(drawTimes);
                logger.info("Terrain CPU samples=120 input_ms_p50={} p95={} p99={} frame_ms_p50={} p95={} p99={} geometry_upload_bytes={} texture_copy_bytes={} visible={} resident={}",
                    prepareTimes[60],prepareTimes[114],prepareTimes[118],drawTimes[60],drawTimes[114],drawTimes[118],uploadBytes,textureBytes,instances.size(),uploaded.size());
                sampleCount=0; uploadBytes=textureBytes=0;
            }
        }
        return true;
    }
    public boolean pending() { return pending; }
    private void closeInputs() {
        if(textures==null) return;
        if(!images.isEmpty()) {
            // Flush queued input releases before destroying imports, including a failed draw.
            try(var fence=encoder.createFence()) {
                encoder.submit();
                if(!fence.awaitCompletion(5_000_000_000L)) throw new IllegalStateException("MC terrain drain timed out; resources remain live");
            }
            images.getFirst().awaitReady(sequence);
            for(var image:images) image.close(); images.clear();
        }
        textures.close(); textures=null; atlas=null; sequence=1; pending=false;
    }
    @Override public void close() {
        closeInputs(); uploaded.clear(); previous.clear(); pendingRemovals.clear(); sourceEpoch=0;
    }
}
