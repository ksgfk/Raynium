package dev.raynium.client.terrain;
import java.util.function.Consumer;
import java.util.function.Supplier;

/** A failed attempt must release its partial imports before returning control here. */
public final class TextureTransfer {
    public static <T> T create(String mode,Supplier<T> direct,Supplier<T> split,Consumer<RuntimeException> report) {
        return switch(mode) {
            case "direct" -> direct.get();
            case "split" -> split.get();
            case "auto" -> {
                try { yield direct.get(); }
                catch(RuntimeException failure) {
                    report.accept(failure);
                    try { yield split.get(); }
                    catch(RuntimeException fallback) { fallback.addSuppressed(failure); throw fallback; }
                }
            }
            default -> throw new IllegalArgumentException("textureTransfer must be auto, direct or split");
        };
    }
}
