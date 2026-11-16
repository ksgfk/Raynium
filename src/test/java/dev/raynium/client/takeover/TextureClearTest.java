package dev.raynium.client.takeover;

import dev.raynium.bridge.NativeTexture;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import org.joml.Vector4f;
import org.junit.jupiter.api.Test;
import static org.junit.jupiter.api.Assertions.*;

class TextureClearTest {
    @Test void normalizedAndFloatFormatsPreserveTheirDifferentChannelLayouts() {
        var color=new Vector4f(.5f,2,-1,1);
        assertArrayEquals(new byte[]{(byte)128,(byte)128,(byte)128},TakeoverHandles.clearPixels(NativeTexture.Format.R8,3,1,color));
        assertArrayEquals(new byte[]{(byte)128,(byte)255,0,(byte)255,(byte)128,(byte)255,0,(byte)255},
            TakeoverHandles.clearPixels(NativeTexture.Format.RGBA8,2,1,color));
        for(var format:new NativeTexture.Format[]{NativeTexture.Format.RGBA16F,NativeTexture.Format.RGBA32F}) {
            var bytes=ByteBuffer.wrap(TakeoverHandles.clearPixels(format,3,2,color)).order(ByteOrder.nativeOrder());
            for(int pixel=0;pixel<6;pixel++) for(float expected:new float[]{.5f,2,-1,1})
                assertEquals(expected,format==NativeTexture.Format.RGBA16F?Float.float16ToFloat(bytes.getShort()):bytes.getFloat());
            assertFalse(bytes.hasRemaining());
        }
        assertThrows(IllegalArgumentException.class,()->TakeoverHandles.clearPixels(NativeTexture.Format.DEPTH32F,1,1,color));
    }
}
