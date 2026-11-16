package dev.raynium.client.takeover.scene;

import dev.raynium.bridge.NativeSceneFrame;
import java.lang.foreign.MemorySegment;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import net.minecraft.client.renderer.state.GameRenderState;

/** Built-in post effects retain vanilla request order and the GUI blur boundary. */
public final class ScenePost {
    private static final ByteBuffer GLOBALS=ByteBuffer.allocate(256).order(ByteOrder.nativeOrder());
    private static boolean checkedOptionalHook;
    private ScenePost() {}
    private static int globals(NativeSceneFrame frame,GameRenderState state) {
        for(int i=0;i<256;i+=8) GLOBALS.putLong(i,0);
        SceneUniforms.globals(GLOBALS,state);
        return frame.copy(MemorySegment.ofBuffer(GLOBALS));
    }
    public static void world(NativeSceneFrame frame,GameRenderState state) {
        if(state.requestedPostEffects.isEmpty()) return;
        int constants=-1;
        for(var effect:state.requestedPostEffects) {
            if(!effect.getNamespace().equals("minecraft")) throw new IllegalArgumentException("Unsupported custom post effect: "+effect);
            if(effect.getPath().equals("end_of_frame")) {
                if(!checkedOptionalHook) {
                    if(net.minecraft.client.Minecraft.getInstance().getResourceManager().getResource(
                        net.minecraft.resources.Identifier.withDefaultNamespace("post_effect/end_of_frame.json")).isPresent())
                        throw new IllegalArgumentException("Resource-pack end_of_frame post effects are unsupported");
                    checkedOptionalHook=true;
                }
                continue;
            }
            int chain=switch(effect.getPath()) {
                case "blur" -> 1;
                case "creeper" -> 2;
                case "invert" -> 4;
                case "spider" -> 5;
                default -> throw new IllegalArgumentException("Unmapped vanilla post effect: "+effect);
            };
            if(constants<0) constants=globals(frame,state);
            frame.effect(chain,0,constants);
        }
    }
    public static void reload() { checkedOptionalHook=false; }
    public static void gui(NativeSceneFrame frame,GameRenderState state,int split) {
        frame.guiBlurSplit(split);
        frame.effect(1,1,globals(frame,state));
    }
}
