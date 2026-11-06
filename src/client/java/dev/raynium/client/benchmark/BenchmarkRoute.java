package dev.raynium.client.benchmark;

import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import java.util.List;
import java.util.ArrayList;

public final class BenchmarkRoute {
    public record Point(double seconds, double x, double y, double z, float yaw, float pitch) {}
    private final List<Point> points;

    public BenchmarkRoute(JsonArray source) {
        var parsed = new ArrayList<Point>();
        for (var value : source) {
            JsonObject p = value.getAsJsonObject();
            var point = new Point(number(p, "seconds"), number(p, "x"), number(p, "y"), number(p, "z"),
                    (float) number(p, "yaw"), (float) number(p, "pitch"));
            if (Math.abs(point.x()) > 29_999_984 || Math.abs(point.z()) > 29_999_984
                    || Math.abs(point.y()) > 20_000_000 || Math.abs(point.pitch()) > 90
                    || !Float.isFinite(point.yaw()) || Math.abs(point.yaw()) > 360)
                throw new IllegalArgumentException("Route coordinate or angle out of range");
            if (parsed.isEmpty() ? point.seconds() != 0 : point.seconds() <= parsed.getLast().seconds())
                throw new IllegalArgumentException("Route must start at 0 with strictly increasing seconds");
            parsed.add(point);
        }
        if (parsed.size() < 2) throw new IllegalArgumentException("Route needs at least two points");
        points = List.copyOf(parsed);
    }

    private static double number(JsonObject p, String key) {
        var value = p.getAsJsonPrimitive(key);
        if (value == null || !value.isNumber() || !Double.isFinite(value.getAsDouble()))
            throw new IllegalArgumentException("Route requires a finite number: " + key);
        return value.getAsDouble();
    }

    public double duration() { return points.getLast().seconds(); }
    public Point first() { return points.getFirst(); }

    public Point sample(double seconds) {
        if (seconds <= 0) return first();
        if (seconds >= duration()) return points.getLast();
        int low = 1, high = points.size() - 1;
        while (low < high) {
            int middle = (low + high) >>> 1;
            if (points.get(middle).seconds() < seconds) low = middle + 1;
            else high = middle;
        }
        Point a = points.get(low - 1), b = points.get(low);
        double t = (seconds - a.seconds()) / (b.seconds() - a.seconds());
        double turn = ((b.yaw() - a.yaw()) % 360 + 540) % 360 - 180;
        return new Point(seconds, lerp(a.x(), b.x(), t), lerp(a.y(), b.y(), t), lerp(a.z(), b.z(), t),
                (float) (a.yaw() + turn * t), (float) lerp(a.pitch(), b.pitch(), t));
    }

    private static double lerp(double a, double b, double t) { return a + (b - a) * t; }
}
