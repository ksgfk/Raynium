package dev.raynium.bridge;

import jdk.jfr.*;

/** Frame-associated Java preparation and FFM envelope; the latter includes native Render. */
@Name("dev.raynium.TerrainFrame")
@Label("Terrain frame")
@Category("Raynium")
@StackTrace(false)
public final class TerrainFrameEvent extends Event {
    private static final EventType TYPE=EventType.getEventType(TerrainFrameEvent.class);
    public long sequence;
    @Timespan(Timespan.NANOSECONDS) public long inputPreparation;
    @Timespan(Timespan.NANOSECONDS) public long preparation;
    @Timespan(Timespan.NANOSECONDS) public long ffm;
    public int visible;
    public static TerrainFrameEvent start() {
        if(!TYPE.isEnabled()) return null;
        var event=new TerrainFrameEvent(); event.begin(); return event;
    }
}
