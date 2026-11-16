package dev.raynium.client.takeover;
public interface LevelMaintenance {
    void raynium$maintain();
    void raynium$capture(org.joml.Matrix4fc projection,boolean renderOutline);
    int raynium$transientBlocks();
}
