package dev.raynium.client.terrain;

import dev.raynium.bridge.*;
import dev.raynium.client.takeover.TakeoverHandles;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.texture.TextureAtlas;

public final class TakeoverTerrain {
    private final TerrainResidency residency=new TerrainResidency();
    private long sourceEpoch,epoch=1;
    private boolean pending;
    private NativeRenderer rendererOwner;
    public long epoch() { return epoch; }
    public long beginFrame(NativeRenderer renderer) {
        pending=false;
        if(sourceEpoch!=TerrainCapture.epoch()) {
            sourceEpoch=TerrainCapture.epoch(); epoch=renderer.nextEpoch(); residency.reset(epoch);
        }
        return epoch;
    }
    public void prepare(NativeRenderer renderer,Minecraft client,NativeRenderer.Frame frame) {
        if(rendererOwner!=renderer) {
            if(rendererOwner!=null) {
                pending=false; residency.reset(0); sourceEpoch=0;
                if(client.level!=null) client.levelRenderer.invalidateCompiledGeometry(client.level,client.options,client.gameRenderer.mainCamera(),client.getBlockColors());
                else TerrainCapture.invalidate();
            }
            rendererOwner=renderer;
        }
        var snapshot=TerrainCapture.snapshot();
        if(snapshot==null) return;
        if(sourceEpoch!=snapshot.epoch) {
            sourceEpoch=snapshot.epoch; epoch=renderer.nextEpoch(); residency.reset(epoch);
        }
        TerrainCapture.drainRemoved(residency.pendingRemovals);
        var atlas=(TakeoverHandles.Texture)client.getTextureManager().getTexture(TextureAtlas.LOCATION_BLOCKS).getTexture();
        var light=(TakeoverHandles.Texture)client.gameRenderer.levelLightmap().texture();
        frame.epoch=epoch;
        frame.flags=NativeRenderer.FLIP_Y|(snapshot.rgss?NativeRenderer.RGSS:0)|(snapshot.improvedTransparency?NativeRenderer.IMPROVED_TRANSPARENCY:0)|
            ((dev.raynium.client.benchmark.BenchmarkSession.isActive() || Boolean.getBoolean("raynium.terrain.profile"))?NativeRenderer.PROFILE:0);
        frame.projection=snapshot.projection; frame.view=snapshot.view; frame.cameraBlock=snapshot.block; frame.cameraFraction=snapshot.fraction;
        frame.fogColor=snapshot.fog; frame.fogDistances=snapshot.distances; frame.clearColor=snapshot.fog;
        residency.prepare(snapshot,renderer.frameBuffer(),atlas.nativeTexture().id(),light.nativeTexture().id());
        pending=true;
    }
    public void commit() { if(pending) { residency.commit(); pending=false; } }
}
