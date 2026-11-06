package dev.raynium.client.benchmark;

import com.google.gson.JsonParser;
import org.junit.jupiter.api.Test;
import static org.junit.jupiter.api.Assertions.*;

class BenchmarkRouteTest {
    private static BenchmarkRoute route(String json) {
        return new BenchmarkRoute(JsonParser.parseString(json).getAsJsonArray());
    }

    @Test void interpolatesPositionAndShortestYawAcrossWrap() {
        var route = route("""
                [{"seconds":0,"x":0,"y":80,"z":10,"yaw":170,"pitch":0},
                 {"seconds":10,"x":100,"y":100,"z":30,"yaw":-170,"pitch":20},
                 {"seconds":20,"x":100,"y":100,"z":30,"yaw":-170,"pitch":20}]
                """);
        var middle = route.sample(5);
        assertEquals(50, middle.x());
        assertEquals(90, middle.y());
        assertEquals(20, middle.z());
        assertEquals(180, middle.yaw());
        assertEquals(10, middle.pitch());
        assertEquals(0, route.sample(-1).seconds());
        assertEquals(20, route.sample(100).seconds());
        assertEquals(100, route.sample(15).x());
        assertEquals(20, route.duration());
    }

    @Test void rejectsAmbiguousTimingAndInvalidAngles() {
        String valid = """
                [{"seconds":0,"x":0,"y":80,"z":0,"yaw":0,"pitch":0},
                 {"seconds":10,"x":1,"y":80,"z":0,"yaw":0,"pitch":20}]
                """;
        assertThrows(IllegalArgumentException.class, () -> route("[]"));
        assertThrows(IllegalArgumentException.class, () -> route(valid.replace("\"seconds\":10", "\"seconds\":0")));
        assertThrows(IllegalArgumentException.class, () -> route(valid.replace("\"seconds\":0", "\"seconds\":2")));
        assertThrows(IllegalArgumentException.class, () -> route(valid.replace("\"pitch\":20", "\"pitch\":91")));
        assertThrows(IllegalArgumentException.class, () -> route(valid.replace("\"x\":1", "\"x\":1e999")));
    }
}
