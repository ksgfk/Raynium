package dev.raynium.client.world;

import static java.lang.foreign.ValueLayout.JAVA_FLOAT;
import static java.lang.foreign.ValueLayout.JAVA_INT;

import com.mojang.renderpearl.api.commands.RenderPass;
import dev.raynium.client.renderpearl.CommandTape;
import dev.raynium.client.renderpearl.RayniumGpuDevice;
import java.lang.foreign.MemorySegment;
import java.util.List;
import net.minecraft.client.renderer.feature.ShadowFeatureRenderer;
import net.minecraft.client.renderer.oit.OitStage;
import net.minecraft.client.renderer.rendertype.PreparedRenderType;
import net.minecraft.util.ARGB;
import org.joml.Matrix4fc;
import org.jspecify.annotations.Nullable;

public final class ShadowFastTrack {
    public static final int INSTANCE_BYTES = 56;
    private static final int[] QUAD = {2}, FIRST_INSTANCE = {0};
    private final PreparedRenderType renderType;
    private final List<ShadowFeatureRenderer.Submit> submits;
    private final int[] count = new int[1];
    private CommandTape tape;
    private long offset;

    public ShadowFastTrack(PreparedRenderType renderType, List<ShadowFeatureRenderer.Submit> submits, CommandTape tape) {
        this.renderType = renderType;
        this.submits = submits;
        for (int i = 0; i < submits.size(); ++i)
            count[0] = Math.addExact(count[0], submits.get(i).pieces().size());
        upload(tape);
    }

    private void upload(CommandTape target) {
        tape = target;
        if (count[0] == 0)
            return;
        long bytes = (long)count[0] * INSTANCE_BYTES;
        offset = target.reserveInstanceUpload(bytes, 8);
        var dst = target.instanceUploadSlice(offset, bytes);
        long at = 0;
        for (int i = 0; i < submits.size(); ++i) {
            var submit = submits.get(i);
            var pieces = submit.pieces();
            for (int j = 0; j < pieces.size(); ++j) {
                var piece = pieces.get(j);
                var box = piece.shapeBelow().bounds();
                pack(dst, at, submit.pose(), submit.radius(), piece.relativeX() + (float)box.minX,
                     piece.relativeX() + (float)box.maxX, piece.relativeY() + (float)box.minY,
                     piece.relativeZ() + (float)box.minZ, piece.relativeZ() + (float)box.maxZ, ARGB.white(piece.alpha()));
                at += INSTANCE_BYTES;
            }
        }
    }

    static void pack(MemorySegment dst, long at, Matrix4fc m, float radius, float x0, float x1, float y,
                     float z0, float z1, int color) {
        dst.set(JAVA_FLOAT, at, org.joml.Math.fma(m.m00(), x0, org.joml.Math.fma(m.m10(), y, org.joml.Math.fma(m.m20(), z0, m.m30()))));
        dst.set(JAVA_FLOAT, at + 4, org.joml.Math.fma(m.m01(), x0, org.joml.Math.fma(m.m11(), y, org.joml.Math.fma(m.m21(), z0, m.m31()))));
        dst.set(JAVA_FLOAT, at + 8, org.joml.Math.fma(m.m02(), x0, org.joml.Math.fma(m.m12(), y, org.joml.Math.fma(m.m22(), z0, m.m32()))));
        dst.set(JAVA_FLOAT, at + 12, -x0 / 2f / radius + 0.5f);
        float dx = x1 - x0, dz = z1 - z0;
        dst.set(JAVA_FLOAT, at + 16, dx * m.m00());
        dst.set(JAVA_FLOAT, at + 20, dx * m.m01());
        dst.set(JAVA_FLOAT, at + 24, dx * m.m02());
        dst.set(JAVA_FLOAT, at + 28, -x1 / 2f / radius + 0.5f);
        dst.set(JAVA_FLOAT, at + 32, dz * m.m20());
        dst.set(JAVA_FLOAT, at + 36, dz * m.m21());
        dst.set(JAVA_FLOAT, at + 40, dz * m.m22());
        dst.set(JAVA_FLOAT, at + 44, -z0 / 2f / radius + 0.5f);
        dst.set(JAVA_FLOAT, at + 48, -z1 / 2f / radius + 0.5f);
        dst.set(JAVA_INT, at + 52, color);
    }

    void execute(@Nullable OitStage stage, RenderPass pass) {
        if (count[0] == 0)
            return;
        var current = RayniumGpuDevice.activeTape(pass);
        if (current != tape)
            upload(current);
        ModelFastTrack.bind(renderType, stage, pass);
        if (!RayniumGpuDevice.recordInstancedGeometry(pass, "shadow", offset, count[0], INSTANCE_BYTES, QUAD, FIRST_INSTANCE, count, 1))
            throw new IllegalStateException("Missing prepared entity shadow pipeline");
    }
}
