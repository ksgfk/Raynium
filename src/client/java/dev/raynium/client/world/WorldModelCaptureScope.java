package dev.raynium.client.world;

/** Scopes rigid model fast-tracking strictly to LevelRenderer world feature preparation. */
public final class WorldModelCaptureScope {
    private static int depth;

    private WorldModelCaptureScope() {}

    public static void enter() { ++depth; }
    public static void exit() { if (depth > 0) --depth; }
    public static boolean isActive() { return depth > 0; }
}
