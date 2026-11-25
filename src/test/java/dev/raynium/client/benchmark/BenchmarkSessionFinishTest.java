package dev.raynium.client.benchmark;

import com.google.gson.JsonParser;
import dev.raynium.bridge.NativeEnvironment;
import dev.raynium.bridge.NativeLibraryLocator;
import dev.raynium.bridge.NativeShaderPack;
import dev.raynium.client.renderpearl.CommandTape;
import java.io.IOException;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Tag;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import org.slf4j.LoggerFactory;
import static org.junit.jupiter.api.Assertions.*;

class BenchmarkSessionFinishTest {
    @TempDir Path directory;

    private BenchmarkSession session() throws IOException {
        var config=directory.resolve("input.json");
        Files.writeString(config,"""
            {"warmupSeconds":0,"startupTimeoutSeconds":10,"route":[
              {"seconds":0,"x":0,"y":64,"z":0,"yaw":0,"pitch":0},
              {"seconds":1,"x":1,"y":64,"z":0,"yaw":0,"pitch":0}]}
            """);
        return new BenchmarkSession(config);
    }
    private void checkResult(String status, AtomicInteger stops) throws IOException {
        var result=JsonParser.parseString(Files.readString(directory.resolve("client-result.json"))).getAsJsonObject();
        assertEquals(status,result.get("status").getAsString());
        assertTrue(result.has("measurements"));
        assertTrue(Files.isRegularFile(directory.resolve("frames.csv")));
        assertTrue(Files.isRegularFile(directory.resolve("gpu.csv")));
        assertEquals(1,stops.get());
    }

    @Test void referenceFinishesWithoutLookingUpNativeDevice() throws IOException {
        var stops=new AtomicInteger();
        session().finish(true,"Route completed",true,()->{throw new AssertionError("reference device lookup");},stops::incrementAndGet);
        checkResult("completed",stops);
    }

    enum Failure { ALREADY_TERMINAL, COMPLETION_ERROR }
    @Tag("native") @ParameterizedTest @EnumSource
    void failedRendererStillWritesResultAndStops(Failure fault) throws IOException {
        var payload=new NativeLibraryLocator(directory,null).locate().orElseThrow();
        var session=session();
        try(var input=getClass().getResourceAsStream("/assets/raynium/shaderpacks/default.rnsp");
            var env=new NativeEnvironment(payload,NativeEnvironment.Backend.VULKAN,LoggerFactory.getLogger(getClass()));
            var runtime=env.createRuntime(); var renderer=runtime.createRenderer(NativeShaderPack.read(input));
            var arena=Arena.ofConfined(); var tape=new CommandTape(renderer)) {
            var stops=new AtomicInteger();
            if(fault==Failure.ALREADY_TERMINAL) {
                var bad=arena.allocate(8,8);
                bad.set(ValueLayout.JAVA_INT,0,-1); bad.set(ValueLayout.JAVA_INT,4,8);
                assertThrows(IllegalStateException.class,()->renderer.submitFrame(bad,MemorySegment.NULL,null));
                session.finish(true,"Route completed",false,()->renderer,stops::incrementAndGet);
            } else {
                var original=new AssertionError("completion failure");
                tape.onCompleted(()->{throw original;});
                tape.submit();
                assertSame(original,assertThrows(AssertionError.class,
                    ()->session.finish(true,"Route completed",false,()->renderer,stops::incrementAndGet)));
            }
            checkResult("failed",stops);
        }
    }
}
