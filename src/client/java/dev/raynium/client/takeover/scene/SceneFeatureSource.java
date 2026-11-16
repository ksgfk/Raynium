package dev.raynium.client.takeover.scene;

import net.minecraft.client.renderer.SubmitNodeStorage;
import org.joml.Matrix4fc;

public interface SceneFeatureSource {
    default void raynium$capture(SubmitNodeStorage storage,Matrix4fc projection,Matrix4fc view,boolean oit) {
        raynium$capture(storage,projection,view,oit,-1,false);
    }
    void raynium$capture(SubmitNodeStorage storage,Matrix4fc projection,Matrix4fc view,boolean oit,int phase,boolean itemsLighting);
    void raynium$captureOffscreen(SubmitNodeStorage storage,Matrix4fc projection,Matrix4fc view,int width,int height);
}
