package dev.raynium.client.benchmark;

import com.google.gson.GsonBuilder;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import dev.raynium.client.terrain.TerrainCapture;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Instant;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientTickEvents;
import net.minecraft.client.CameraType;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.ClientInput;
import net.minecraft.world.level.GameType;
import net.minecraft.world.phys.Vec3;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

// See doc/guide/build-test.md for the opt-in benchmark protocol and capture boundaries.
public final class BenchmarkSession {
    private static final Logger LOGGER = LoggerFactory.getLogger("raynium.benchmark");
    private static BenchmarkSession active;
    private final BenchmarkRoute route;
    private final Path directory;
    private final double warmupSeconds, startupTimeoutSeconds;
    private final long started = System.nanoTime();
    private final JsonObject result = new JsonObject();
    private CompletableFuture<Void> preparation;
    private String phase = "loading";
    private long phaseStarted, routeStarted, nativeFrames, routeFirstFrame, endpointFrame = -1;
    private int ticks;

    private BenchmarkSession(Path config) throws IOException {
        directory = config.toAbsolutePath().getParent();
        JsonObject json = JsonParser.parseString(Files.readString(config)).getAsJsonObject();
        route = new BenchmarkRoute(json.getAsJsonArray("route"));
        warmupSeconds = json.get("warmupSeconds").getAsDouble();
        startupTimeoutSeconds = json.get("startupTimeoutSeconds").getAsDouble();
        if (!Double.isFinite(warmupSeconds) || warmupSeconds < 0
                || !Double.isFinite(startupTimeoutSeconds) || startupTimeoutSeconds <= 0)
            throw new IllegalArgumentException("Invalid benchmark timeouts");
        result.addProperty("startedAt", Instant.now().toString());
        result.addProperty("pid", ProcessHandle.current().pid());
    }

    public static void register() {
        String config = System.getProperty("raynium.benchmark.config");
        if (config == null) return;
        try {
            active = new BenchmarkSession(Path.of(config));
        } catch (IOException | RuntimeException failure) {
            throw new IllegalStateException("Cannot load benchmark configuration", failure);
        }
        ClientTickEvents.END_CLIENT_TICK.register(client -> active.tick(client));
    }

    public static void nativeFrame() {
        if (active != null) active.nativeFrames++;
    }

    public static void beforeFrame(Minecraft client) {
        if (active == null || client.player == null) return;
        if (!active.phase.equals("warmup") && !active.phase.equals("measuring")) return;
        double elapsed = active.phase.equals("measuring") ? secondsSince(active.routeStarted) : 0;
        var point = active.route.sample(elapsed);
        client.player.setDeltaMovement(Vec3.ZERO);
        client.player.absSnapTo(point.x(), point.y(), point.z(), point.yaw(), point.pitch());
        if (elapsed >= active.route.duration() && active.endpointFrame < 0)
            active.endpointFrame = active.nativeFrames;
    }

    private void tick(Minecraft client) {
        if (phase.equals("finished")) return;
        try {
            if (++ticks % 20 == 0 && Files.exists(directory.resolve("stop.request"))) {
                finish(client, false, "Stop requested by runner");
                return;
            }
            if (phase.equals("loading")) {
                if (secondsSince(started) > startupTimeoutSeconds)
                    throw new IllegalStateException("Timed out waiting for world, spectator, native frames and Tracy connection");
                if (client.player == null || client.level == null || !client.player.connection.hasClientLoaded()) return;
                var server = client.getSingleplayerServer();
                if (server == null) throw new IllegalStateException("Benchmark requires an integrated singleplayer server");
                if (preparation == null) {
                    var id = client.player.getUUID();
                    var point = route.first();
                    client.player.input = new ClientInput();
                    client.options.setCameraType(CameraType.FIRST_PERSON);
                    preparation = server.submit(() -> {
                        var player = server.getPlayerList().getPlayer(id);
                        if (player == null) throw new IllegalStateException("Benchmark player missing on server");
                        player.setGameMode(GameType.SPECTATOR);
                        if (!player.teleportTo(player.level(), point.x(), point.y(), point.z(), Set.of(), point.yaw(), point.pitch(), true))
                            throw new IllegalStateException("Cannot teleport to benchmark start");
                    });
                    return;
                }
                if (!preparation.isDone()) return;
                preparation.join();
                var p = route.first();
                if (!client.player.isSpectator() || client.player.distanceToSqr(p.x(), p.y(), p.z()) > 4
                        || client.gui.screen() != null || client.gui.overlay() != null
                        || nativeFrames == 0 || !Files.exists(directory.resolve("capture.ready"))) return;
                result.addProperty("dimension", client.level.dimension().toString());
                result.addProperty("windowWidth", client.getWindow().getWidth());
                result.addProperty("windowHeight", client.getWindow().getHeight());
                phase = "warmup";
                phaseStarted = System.nanoTime();
                LOGGER.info("Benchmark warmup started: {} seconds", warmupSeconds);
            } else {
                if (client.player == null || client.level == null || !client.player.isSpectator()
                        || client.gui.screen() != null || client.gui.overlay() != null || client.isPaused()
                        || client.getWindow().isIconified() || !TerrainCapture.active())
                    throw new IllegalStateException("World, spectator, window or native preview became unavailable");
                if (phase.equals("warmup") && secondsSince(phaseStarted) >= warmupSeconds) {
                    phase = "measuring";
                    routeStarted = System.nanoTime();
                    routeFirstFrame = nativeFrames;
                    result.addProperty("routeStartedAt", Instant.now().toString());
                    result.addProperty("routeFirstNativeFrame", routeFirstFrame);
                    LOGGER.info("Benchmark route started: {} seconds; native frame {}", route.duration(), routeFirstFrame);
                } else if (phase.equals("measuring") && endpointFrame >= 0 && nativeFrames > endpointFrame) {
                    finish(client, true, "Route completed");
                }
            }
        } catch (RuntimeException failure) {
            LOGGER.error("Benchmark failed", failure);
            finish(client, false, failure.toString());
        }
    }

    private void finish(Minecraft client, boolean success, String message) {
        result.addProperty("status", success ? "completed" : "failed");
        result.addProperty("message", message);
        result.addProperty("phase", phase);
        result.addProperty("finishedAt", Instant.now().toString());
        result.addProperty("nativeFrames", nativeFrames);
        result.addProperty("routeNativeFrames", routeStarted == 0 ? 0 : nativeFrames - routeFirstFrame);
        result.addProperty("routeElapsedSeconds", routeStarted == 0 ? 0 : secondsSince(routeStarted));
        phase = "finished";
        try {
            Files.writeString(directory.resolve("client-result.json"), new GsonBuilder().setPrettyPrinting().create().toJson(result));
        } catch (IOException failure) {
            LOGGER.error("Cannot save benchmark result", failure);
        }
        LOGGER.info("Benchmark {}: {}; requesting normal game shutdown", success ? "completed" : "failed", message);
        client.stop();
    }

    private static double secondsSince(long start) { return (System.nanoTime() - start) / 1_000_000_000.0; }
}
