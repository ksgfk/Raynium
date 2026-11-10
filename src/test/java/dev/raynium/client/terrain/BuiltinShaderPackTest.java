package dev.raynium.client.terrain;

import dev.raynium.bridge.NativeShaderPack;
import java.io.ByteArrayInputStream;
import java.io.UncheckedIOException;
import java.lang.reflect.Proxy;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;
import net.fabricmc.fabric.api.resource.v1.pack.ModPackResources;
import net.fabricmc.loader.api.metadata.ModMetadata;
import net.minecraft.server.packs.PackResources;
import net.minecraft.server.packs.resources.IoSupplier;
import net.minecraft.server.packs.resources.ResourceManager;
import org.junit.jupiter.api.Test;
import static org.junit.jupiter.api.Assertions.*;

class BuiltinShaderPackTest {
    private static <T> T proxy(Class<T> type, java.lang.reflect.InvocationHandler handler) {
        return type.cast(Proxy.newProxyInstance(type.getClassLoader(), new Class<?>[]{type}, handler));
    }
    private static ModPackResources mod(String id, IoSupplier<java.io.InputStream> supplier) {
        var metadata=proxy(ModMetadata.class,(p,m,a)-> {
            if(m.getName().equals("getId")) return id;
            throw new AssertionError(m.getName());
        });
        return proxy(ModPackResources.class,(p,m,a)->switch(m.getName()) {
            case "getFabricModMetadata" -> metadata;
            case "getResource" -> { assertEquals(BuiltinShaderPack.DEFAULT,a[1]); yield supplier; }
            default -> throw new AssertionError(m.getName());
        });
    }
    private static ResourceManager manager(List<PackResources> packs, AtomicBoolean closed) {
        return proxy(ResourceManager.class,(p,m,a)-> {
            if(m.getName().equals("listPacks")) return packs.stream().onClose(()->closed.set(true));
            throw new AssertionError("Must select built-in source, not priority lookup: "+m.getName());
        });
    }
    @Test void selectsOwnModAndClosesInputAndPackStream() {
        var closed=new AtomicBoolean(); var inputClosed=new AtomicBoolean();
        var user=proxy(PackResources.class,(p,m,a)-> { throw new AssertionError("User pack must not be read"); });
        var other=mod("other",()-> { throw new AssertionError("Other mod must not be read"); });
        var own=mod("raynium",()->new ByteArrayInputStream(new byte[]{1}) {
            @Override public void close() { inputClosed.set(true); }
        });
        assertNotNull(BuiltinShaderPack.load(manager(List.of(user,other,own),closed)));
        assertTrue(closed.get()); assertTrue(inputClosed.get());
    }
    @Test void missingBuiltInCannotUseUserOverride() {
        var closed=new AtomicBoolean();
        assertThrows(IllegalStateException.class,()->BuiltinShaderPack.load(manager(List.of(mod("raynium",null)),closed)));
        assertTrue(closed.get());
    }
    @Test void invalidStreamClosesAndKeepsDiagnostic() {
        var closed=new AtomicBoolean(); var inputClosed=new AtomicBoolean();
        var own=mod("raynium",()->new ByteArrayInputStream(new byte[0]) {
            @Override public void close() { inputClosed.set(true); }
        });
        assertThrows(UncheckedIOException.class,()->BuiltinShaderPack.load(manager(List.of(own),closed)));
        assertTrue(closed.get()); assertTrue(inputClosed.get());
        assertThrows(IllegalArgumentException.class,()->NativeShaderPack.fromBytes(new byte[0]));
    }
}
