package dev.raynium.client.takeover;
public final class UnsupportedCapture {
    private static boolean warned;
    private UnsupportedCapture() {}
    public static void warn() {
        if(!warned) { warned=true; org.slf4j.LoggerFactory.getLogger("raynium.takeover").info("Raynium screenshots and panorama capture are unavailable"); }
    }
}
