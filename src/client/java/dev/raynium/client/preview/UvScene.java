package dev.raynium.client.preview;

import dev.raynium.bridge.NativeRenderer;
import dev.raynium.bridge.NativeRenderTarget;
import java.util.List;

/** A procedural triangle described through the same scene API as captured geometry. */
public final class UvScene {
    private final NativeRenderer renderer;
    private final long epoch;
    private boolean resident;
    public UvScene(NativeRenderer renderer) { this.renderer=renderer; epoch=renderer.nextEpoch(); }
    public void render(NativeRenderTarget target,long sequence) {
        var frame=new NativeRenderer.Frame(); frame.epoch=epoch; frame.outputSequence=sequence;
        if(!resident) {
            frame.meshes=List.of(new NativeRenderer.Mesh(1,new byte[0],0,3,0,NativeRenderer.PROCEDURAL,List.of(new NativeRenderer.Submesh(0,3,0))));
            frame.materials=List.of(new NativeRenderer.Material(1,NativeRenderer.UV,0,0,1));
            frame.primitives=List.of(new NativeRenderer.Primitive(1,1,1));
        }
        frame.instances=List.of(new NativeRenderer.Instance(0,0,0,1));
        frame.visible=List.of(new NativeRenderer.Visible(1,0,0,0,1));
        renderer.render(target,frame); resident=true;
    }
}
