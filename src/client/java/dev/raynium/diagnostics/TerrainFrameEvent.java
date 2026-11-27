package dev.raynium.diagnostics;

import jdk.jfr.Category;
import jdk.jfr.Event;
import jdk.jfr.EventType;
import jdk.jfr.Label;
import jdk.jfr.Name;
import jdk.jfr.StackTrace;
import jdk.jfr.Timespan;

/** Java terrain preparation and owner-thread submission time for one batch sequence. */
@Name("dev.raynium.TerrainFrame")
@Label("Terrain frame")
@Category("Raynium")
@StackTrace(false)
public final class TerrainFrameEvent extends Event {
    private static final EventType TYPE = EventType.getEventType(TerrainFrameEvent.class);
    public long sequence;
    @Timespan(Timespan.NANOSECONDS)
    public long inputPreparation;
    @Timespan(Timespan.NANOSECONDS)
    public long preparation;
    @Timespan(Timespan.NANOSECONDS)
    public long ffm;
    public int visible;

    public static TerrainFrameEvent start() {
        if (!TYPE.isEnabled()) {
            return null;
        }
        var event = new TerrainFrameEvent();
        event.begin();
        return event;
    }
}
