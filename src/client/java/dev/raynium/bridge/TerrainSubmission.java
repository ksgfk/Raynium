package dev.raynium.bridge;

/** Renderer creation policy; auto requires native multi-record indirect and nonzero firstInstance. */
public enum TerrainSubmission {
    AUTO(0,"auto"), DIRECT(1,"direct"), INDIRECT(2,"indirect");
    private final int flags;
    private final String property;
    TerrainSubmission(int flags,String property) { this.flags=flags; this.property=property; }
    int flags() { return flags; }
    public String propertyValue() { return property; }
    public static TerrainSubmission fromProperty(String value) {
        return switch(value) {
            case "auto" -> AUTO;
            case "direct" -> DIRECT;
            case "indirect" -> INDIRECT;
            default -> throw new IllegalArgumentException("Invalid raynium.terrain.submission='"+value+"'; expected auto, direct or indirect");
        };
    }
}
