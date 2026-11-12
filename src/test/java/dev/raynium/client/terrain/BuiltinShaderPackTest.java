package dev.raynium.client.terrain;

import dev.raynium.bridge.NativeShaderPack;
import java.io.ByteArrayInputStream;
import java.io.UncheckedIOException;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;
import org.junit.jupiter.api.Test;
import static org.junit.jupiter.api.Assertions.*;

class BuiltinShaderPackTest {
    @Test void selectsOwnModAndClosesInputAndSourceStream() {
        var closed=new AtomicBoolean(); var inputClosed=new AtomicBoolean();
        var user=new BuiltinShaderPack.Source("user-pack",()-> { throw new AssertionError("User pack must not be read"); });
        var other=new BuiltinShaderPack.Source("other",()-> { throw new AssertionError("Other mod must not be read"); });
        var own=new BuiltinShaderPack.Source("raynium",()->new ByteArrayInputStream(new byte[]{1}) {
            @Override public void close() { inputClosed.set(true); }
        });
        assertNotNull(BuiltinShaderPack.load(List.of(user,other,own).stream().onClose(()->closed.set(true))));
        assertTrue(closed.get()); assertTrue(inputClosed.get());
    }
    @Test void missingBuiltInCannotUseUserOverride() {
        var closed=new AtomicBoolean();
        var user=new BuiltinShaderPack.Source("user-pack",()-> { throw new AssertionError("User pack must not be read"); });
        var own=new BuiltinShaderPack.Source("raynium",null);
        assertThrows(IllegalStateException.class,()->BuiltinShaderPack.load(List.of(user,own).stream().onClose(()->closed.set(true))));
        assertTrue(closed.get());
    }
    @Test void invalidStreamClosesAndKeepsDiagnostic() {
        var closed=new AtomicBoolean(); var inputClosed=new AtomicBoolean();
        var own=new BuiltinShaderPack.Source("raynium",()->new ByteArrayInputStream(new byte[0]) {
            @Override public void close() { inputClosed.set(true); }
        });
        assertThrows(UncheckedIOException.class,()->BuiltinShaderPack.load(List.of(own).stream().onClose(()->closed.set(true))));
        assertTrue(closed.get()); assertTrue(inputClosed.get());
        assertThrows(IllegalArgumentException.class,()->NativeShaderPack.fromBytes(new byte[0]));
    }
}
