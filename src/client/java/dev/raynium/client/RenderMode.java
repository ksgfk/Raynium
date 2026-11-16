package dev.raynium.client;

/** Selected before mixin application; the reference renderer never loads the native payload. */
public final class RenderMode {
    public static final boolean REFERENCE=Boolean.getBoolean("raynium.reference");
    private RenderMode() {}
}
