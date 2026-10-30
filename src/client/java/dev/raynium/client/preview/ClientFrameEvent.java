package dev.raynium.client.preview;

import jdk.jfr.Category;
import jdk.jfr.Event;
import jdk.jfr.EventType;
import jdk.jfr.Label;
import jdk.jfr.Name;
import jdk.jfr.StackTrace;

@Name("dev.raynium.ClientFrame")
@Label("Minecraft client frame")
@Category("Raynium")
@StackTrace(false)
public final class ClientFrameEvent extends Event {
    private static final EventType TYPE = EventType.getEventType(ClientFrameEvent.class);

    @Label("World loaded")
    public boolean worldLoaded;

    public static ClientFrameEvent start(boolean worldLoaded) {
        if (!TYPE.isEnabled()) return null;
        var event = new ClientFrameEvent();
        event.worldLoaded = worldLoaded;
        event.begin();
        return event;
    }
}
