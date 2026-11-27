package dev.raynium.harness.benchmark;

import com.google.gson.GsonBuilder;
import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import com.mojang.jtracy.TracyClient;
import dev.raynium.harness.RayniumHarnessBridge;
import java.io.IOException;
import java.lang.management.ManagementFactory;
import java.lang.management.ThreadMXBean;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Supplier;
import jdk.jfr.FlightRecorder;
import jdk.jfr.RecordingState;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientTickEvents;
import net.minecraft.client.CameraType;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.screens.PauseScreen;
import net.minecraft.client.gui.screens.inventory.InventoryScreen;
import net.minecraft.client.player.ClientInput;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.level.GameType;
import net.minecraft.world.phys.Vec3;
import org.lwjgl.system.Checks;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

// See doc/guide/build-test.md for the opt-in benchmark protocol and capture boundaries.
public final class BenchmarkSession {
    private static final Logger LOGGER = LoggerFactory.getLogger("raynium.benchmark");
    private static final String CONFIG = System.getProperty("raynium.benchmark.config");
    private static final String[] WRITE_SOURCE_NAMES = {
        "HOST_SHADOW", "TRANSIENT", "MAPPABLE_RING", "STAGED_VERTEX"};
    private static BenchmarkSession active;
    private final boolean rayniumLoaded;
    private final BenchmarkRoute route;
    private final Path directory;
    private final double warmupSeconds;
    private final double startupTimeoutSeconds;
    private final long started = System.nanoTime();
    private final JsonObject result = new JsonObject();
    private CompletableFuture<Void> preparation;
    private String phase = "loading";
    private long phaseStarted;
    private long routeStarted;
    private long nativeFrames;
    private long routeFirstFrame;
    private long endpointFrame = -1;
    private int ticks;
    private final boolean profiler;
    private final boolean oit;
    private final String screen;
    private final GameType playerMode;
    private final JsonArray commands;
    private final boolean clearBenchmarkEntities;
    private final BenchmarkMeasurements measurements;
    private final ThreadMXBean threadBean = ManagementFactory.getThreadMXBean();
    private final com.sun.management.ThreadMXBean allocationBean = (com.sun.management.ThreadMXBean)threadBean;
    private final long[] nativeTimes = new long[19];
    private long frameStarted;
    private long frameCpuStarted;
    private long previousFrame;
    private long frameInterval;
    private long allocatedStart;
    private long gcCountStart;
    private long gcMillisStart;
    private final long[] mappedWriteBytes = new long[WRITE_SOURCE_NAMES.length];
    private boolean measuringFrame;
    private Object measuredRenderer;

    BenchmarkSession(Path config) throws IOException {
        this(config, false);
    }

    BenchmarkSession(Path config, boolean rayniumLoaded) throws IOException {
        this.rayniumLoaded = rayniumLoaded;
        directory = config.toAbsolutePath().getParent();
        JsonObject json = JsonParser.parseString(Files.readString(config)).getAsJsonObject();
        route = new BenchmarkRoute(json.getAsJsonArray("route"));
        profiler = json.has("profilerEnabled") && json.get("profilerEnabled").getAsBoolean();
        oit = json.has("oit") && json.get("oit").getAsBoolean();
        screen = json.has("screen") ? json.get("screen").getAsString() : "none";
        playerMode = switch (json.has("playerMode") ? json.get("playerMode").getAsString() : "spectator") {
            case "spectator" -> GameType.SPECTATOR;
            case "creative" -> GameType.CREATIVE;
            case "survival" -> GameType.SURVIVAL;
            default -> throw new IllegalArgumentException("Unsupported benchmark playerMode");
        };
        commands = json.has("commands") ? json.getAsJsonArray("commands") : new JsonArray();
        clearBenchmarkEntities = json.has("clearBenchmarkEntities") && json.get("clearBenchmarkEntities").getAsBoolean();
        measurements = new BenchmarkMeasurements((int)Math.min(2_000_000, Math.max(16384, Math.ceil(route.duration() * 2000))));
        warmupSeconds = json.get("warmupSeconds").getAsDouble();
        startupTimeoutSeconds = json.get("startupTimeoutSeconds").getAsDouble();
        if (!Double.isFinite(warmupSeconds) || warmupSeconds < 0 || !Double.isFinite(startupTimeoutSeconds) || startupTimeoutSeconds <= 0) {
            throw new IllegalArgumentException("Invalid benchmark timeouts");
        }
        result.addProperty("startedAt", Instant.now().toString());
        result.addProperty("pid", ProcessHandle.current().pid());
        result.addProperty("renderer", rayniumLoaded ? "raynium" : "vanilla");
        result.addProperty("profilerEnabled", profiler);
        Files.writeString(directory.resolve("client-start.json"), result.toString());
    }

    public static void register(boolean rayniumLoaded) {
        String config = CONFIG;
        if (config == null) {
            return;
        }
        try {
            active = new BenchmarkSession(Path.of(config), rayniumLoaded);
        } catch (IOException | RuntimeException failure) {
            throw new IllegalStateException("Cannot load benchmark configuration", failure);
        }
        ClientTickEvents.END_CLIENT_TICK.register(client -> active.tick(client));
        if (rayniumLoaded) {
            RayniumHarnessBridge.installBenchmarkTelemetry(new RayniumHarnessBridge.BenchmarkCallbacks() {
                @Override
                public boolean isMeasuring() {
                    return BenchmarkSession.isMeasuring();
                }

                @Override
                public void onMappedWrites(int writeSourceOrdinal, long bytes) {
                    mappedWrites(writeSourceOrdinal, bytes);
                }

                @Override
                public void onPresentedFrames(long count) {
                    nativeFrames(count);
                }

                @Override
                public long syncWaitStart() {
                    return BenchmarkSession.syncWaitStart();
                }

                @Override
                public void syncWaitEnd(long startedNs) {
                    BenchmarkSession.syncWaitEnd(startedNs);
                }

                @Override
                public void onBatchSubmitted(Object renderer, long sequence, long javaPrepareNs, long nativeCallNs) {
                    nativeBatch(renderer, sequence, javaPrepareNs, nativeCallNs);
                }
            });
        }
    }

    public static boolean isActive() {
        return active != null && !active.phase.equals("finished");
    }

    public static boolean isMeasuring() {
        return active != null && active.phase.equals("measuring");
    }

    public static long syncWaitStart() {
        return CONFIG != null && active != null && active.measuringFrame ? System.nanoTime() : 0;
    }

    public static void syncWaitEnd(long start) {
        active.nativeTimes[18] += System.nanoTime() - start;
    }

    public static void mappedWrites(int writeSourceOrdinal, long bytes) {
        if (active != null && active.measuringFrame && writeSourceOrdinal >= 0 && writeSourceOrdinal < active.mappedWriteBytes.length) {
            active.mappedWriteBytes[writeSourceOrdinal] += bytes;
        }
    }

    public static void nativeFrame() {
        if (active != null) {
            active.nativeFrames++;
        }
    }

    public static boolean profilerEnabled() {
        return isActive() && active.profiler;
    }

    public static void nativeFrames(long count) {
        if (isActive() && Minecraft.getInstance().level != null) {
            active.nativeFrames += count;
        }
    }

    public static void nativeBatch(Object renderer, long sequence, long javaPrepare, long nativeCall) {
        if (active == null || !active.measuringFrame || sequence <= 0) {
            return;
        }
        active.measuredRenderer = renderer;
        active.nativeTimes[0] = Math.max(0, active.nativeTimes[0]) + javaPrepare;
        active.nativeTimes[1] = Math.max(0, active.nativeTimes[1]) + nativeCall;
        active.measurements.expect(sequence);
    }

    public static void referenceGpuSample(long nanos) {
        if (active != null && active.phase.equals("measuring")) {
            active.measurements.gpu(nanos);
        }
    }

    public static void afterFrame() {
        if (active == null || !active.measuringFrame || !active.phase.equals("measuring")) {
            return;
        }
        long end = System.nanoTime();
        active.measurements.frame(end - active.routeStarted, active.frameInterval, end - active.frameStarted,
                                  active.threadBean.getCurrentThreadCpuTime() - active.frameCpuStarted, active.nativeTimes);
        active.measuringFrame = false;
        if (active.measuredRenderer != null) {
            RayniumHarnessBridge.drainTelemetry(active.measuredRenderer, active.measurements::receive);
        }
    }

    public static void beforeFrame(Minecraft client) {
        if (active != null) {
            long now = System.nanoTime();
            active.measuringFrame = active.phase.equals("measuring");
            active.frameInterval = active.previousFrame == 0 ? -1 : now - active.previousFrame;
            active.previousFrame = now;
            if (active.measuringFrame) {
                active.frameStarted = now;
                active.frameCpuStarted = active.threadBean.getCurrentThreadCpuTime();
                Arrays.fill(active.nativeTimes, -1);
                if (active.rayniumLoaded) {
                    active.nativeTimes[18] = 0;
                }
            }
        }
        if (active == null || client.player == null) {
            return;
        }
        if (!active.phase.equals("warmup") && !active.phase.equals("measuring")) {
            return;
        }
        double elapsed = active.phase.equals("measuring") ? secondsSince(active.routeStarted) : 0;
        var point = active.route.sample(elapsed);
        client.player.setDeltaMovement(Vec3.ZERO);
        client.player.absSnapTo(point.x(), point.y(), point.z(), point.yaw(), point.pitch());
        if (elapsed >= active.route.duration() && active.endpointFrame < 0) {
            active.endpointFrame = active.nativeFrames;
        }
    }

    private void tick(Minecraft client) {
        if (phase.equals("finished")) {
            return;
        }
        try {
            if (++ticks % 20 == 0 && Files.exists(directory.resolve("stop.request"))) {
                finish(client, false, "Stop requested by runner");
                return;
            }
            if (phase.equals("loading")) {
                if (secondsSince(started) > startupTimeoutSeconds) {
                    throw new IllegalStateException("Timed out waiting for world, spectator, native frames and Tracy connection");
                }
                if (client.player == null || client.level == null || !client.player.connection.hasClientLoaded()) {
                    return;
                }
                var server = client.getSingleplayerServer();
                if (server == null) {
                    throw new IllegalStateException("Benchmark requires an integrated singleplayer server");
                }
                if (preparation == null) {
                    var id = client.player.getUUID();
                    client.options.pauseOnLostFocus = false;
                    client.options.improvedTransparency().set(oit);
                    var point = route.first();
                    client.player.input = new ClientInput();
                    client.options.setCameraType(CameraType.FIRST_PERSON);
                    preparation = server.submit(() -> {
                        var player = server.getPlayerList().getPlayer(id);
                        if (player == null) {
                            throw new IllegalStateException("Benchmark player missing on server");
                        }
                        player.setGameMode(playerMode);
                        if (clearBenchmarkEntities) {
                            var previous = new ArrayList<Entity>();
                            for (var entity : player.level().getAllEntities()) {
                                if (entity.entityTags().contains("raynium_bench")) {
                                    previous.add(entity);
                                }
                            }
                            previous.forEach(Entity::discard);
                            result.addProperty("removedBenchmarkEntities", previous.size());
                        }
                        var source = server.createCommandSourceStack().withEntity(player).withLevel(player.level()).withPosition(player.position()).withSuppressedOutput();
                        for (var command : commands) {
                            var failed = new AtomicBoolean();
                            server.getCommands().performPrefixedCommand(source.withCallback((ok, value) -> {
                                if (!ok) {
                                    failed.set(true);
                                }
                            }),
                                                                        command.getAsString());
                            if (failed.get()) {
                                throw new IllegalArgumentException("Benchmark command failed: " + command.getAsString());
                            }
                        }
                        if (clearBenchmarkEntities) {
                            int tagged = 0;
                            for (var entity : player.level().getAllEntities()) {
                                if (entity.entityTags().contains("raynium_bench")) {
                                    tagged++;
                                }
                            }
                            result.addProperty("preparedBenchmarkEntities", tagged);
                        }
                        if (!player.teleportTo(player.level(), point.x(), point.y(), point.z(), Set.of(), point.yaw(), point.pitch(), true)) {
                            throw new IllegalStateException("Cannot teleport to benchmark start");
                        }
                    });
                    return;
                }
                if (!preparation.isDone()) {
                    return;
                }
                preparation.join();
                var p = route.first();
                if (client.gameMode.getPlayerMode() != playerMode || client.player.distanceToSqr(p.x(), p.y(), p.z()) > 4 || client.gui.screen() != null || client.gui.overlay() != null || (rayniumLoaded && !RayniumHarnessBridge.isTerrainReady()) || nativeFrames == 0 || (profiler && !Files.exists(directory.resolve("capture.ready")))) {
                    return;
                }
                result.addProperty("dimension", client.level.dimension().toString());
                result.addProperty("windowWidth", client.getWindow().getWidth());
                result.addProperty("windowHeight", client.getWindow().getHeight());
                boolean javaTracy = TracyClient.isAvailable();
                boolean jfr = FlightRecorder.isInitialized() && FlightRecorder.getFlightRecorder()
                                                                    .getRecordings()
                                                                    .stream()
                                                                    .anyMatch(r -> r.getState() == RecordingState.RUNNING);
                boolean lwjglChecks = Checks.CHECKS;
                boolean assertions = BenchmarkSession.class.desiredAssertionStatus();
                result.addProperty("javaTracyLoaded", javaTracy);
                result.addProperty("jfrRecording", jfr);
                result.addProperty("lwjglChecks", lwjglChecks);
                result.addProperty("javaAssertions", assertions);
                switch (screen) {
                    case "none" -> {
                    }
                    case "inventory" -> client.gui.setScreen(new InventoryScreen(client.player));
                    case "pause" -> client.gui.setScreen(new PauseScreen(true));
                    default -> throw new IllegalArgumentException("Unknown benchmark screen: " + screen);
                }
                phase = "warmup";
                phaseStarted = System.nanoTime();
                LOGGER.info("Benchmark warmup started: {} seconds", warmupSeconds);
            } else {
                if (client.player == null || client.level == null || client.gameMode.getPlayerMode() != playerMode || (screen.equals("none") && client.gui.screen() != null) || client.gui.overlay() != null || (screen.equals("none") && client.isPaused()) || client.getWindow().isIconified() || (rayniumLoaded && !RayniumHarnessBridge.isTerrainReady())) {
                    throw new IllegalStateException("World, spectator, window or native renderer became unavailable");
                }
                if (phase.equals("warmup") && secondsSince(phaseStarted) >= warmupSeconds) {
                    phase = "measuring";
                    routeStarted = System.nanoTime();
                    routeFirstFrame = nativeFrames;
                    allocatedStart = allocationBean.getTotalThreadAllocatedBytes();
                    gcCountStart = gcCount();
                    gcMillisStart = gcMillis();
                    result.addProperty("routeStartedAt", Instant.now().toString());
                    result.addProperty("routeFirstNativeFrame", routeFirstFrame);
                    LOGGER.info("Benchmark route started: {} seconds; native frame {}", route.duration(), routeFirstFrame);
                } else if (phase.equals("measuring") && endpointFrame >= 0 && nativeFrames > endpointFrame) {
                    finish(client, true, "Route completed");
                }
            }
        } catch (Throwable failure) {
            if (phase.equals("finished")) {
                throw failure;
            }
            LOGGER.error("Benchmark failed", failure);
            finish(client, false, failure.toString());
            if (failure instanceof Error error) {
                throw error;
            }
        }
    }

    private void finish(Minecraft client, boolean success, String message) {
        finish(success, message, !rayniumLoaded,
               () -> measuredRenderer != null ? measuredRenderer : RayniumHarnessBridge.activeNativeRenderer(), client::stop);
    }

    void finish(boolean success, String message, boolean reference,
                Supplier<Object> activeRenderer, Runnable stop) {
        Throwable failure = null;
        try {
            Object renderer = reference ? null : activeRenderer.get();
            if (renderer != null) {
                RayniumHarnessBridge.flushAndDrain(renderer, measurements::receive);
            }
        } catch (Throwable next) {
            failure = next;
        } finally {
            if (failure != null) {
                LOGGER.error("Benchmark telemetry collection failed", failure);
            }
            try {
                writeResult(success && failure == null, failure == null ? message : message + "; " + failure);
            } finally {
                stop.run();
            }
        }
        if (failure instanceof Error error) {
            throw error;
        }
    }

    private void writeResult(boolean success, String message) {
        result.addProperty("status", success ? "completed" : "failed");
        result.addProperty("message", message);
        result.addProperty("phase", phase);
        result.addProperty("finishedAt", Instant.now().toString());
        var writes = new JsonObject();
        for (int i = 0; i < WRITE_SOURCE_NAMES.length; i++) {
            writes.addProperty(WRITE_SOURCE_NAMES[i], mappedWriteBytes[i]);
        }
        result.add("mappedWriteBytes", writes);
        result.addProperty("nativeFrames", nativeFrames);
        result.addProperty("routeNativeFrames", routeStarted == 0 ? 0 : nativeFrames - routeFirstFrame);
        result.addProperty("routeElapsedSeconds", routeStarted == 0 ? 0 : secondsSince(routeStarted));
        result.addProperty("allocatedBytes", allocatedStart == 0 ? 0 : allocationBean.getTotalThreadAllocatedBytes() - allocatedStart);
        result.addProperty("gcCollections", gcCount() - gcCountStart);
        result.addProperty("gcMillis", gcMillis() - gcMillisStart);
        phase = "finished";
        try {
            result.add("measurements", measurements.finish(directory));
            Files.writeString(directory.resolve("client-result.json"), new GsonBuilder().setPrettyPrinting().create().toJson(result));
        } catch (IOException failure) {
            LOGGER.error("Cannot save benchmark result", failure);
        }
        LOGGER.info("Benchmark {}: {}; requesting normal game shutdown", success ? "completed" : "failed", message);
    }

    private static long gcCount() {
        return ManagementFactory.getGarbageCollectorMXBeans().stream().mapToLong(bean -> Math.max(0, bean.getCollectionCount())).sum();
    }

    private static long gcMillis() {
        return ManagementFactory.getGarbageCollectorMXBeans().stream().mapToLong(bean -> Math.max(0, bean.getCollectionTime())).sum();
    }

    private static double secondsSince(long start) {
        return (System.nanoTime() - start) / 1_000_000_000.0;
    }
}
