package dev.raynium.client.takeover.scene;

import net.minecraft.client.renderer.state.GameRenderState;
import org.joml.Matrix4fc;

public interface SceneEnvironmentSource { void raynium$capture(GameRenderState state,Matrix4fc projection,Matrix4fc view,boolean oit); }
