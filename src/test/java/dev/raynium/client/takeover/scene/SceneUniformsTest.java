package dev.raynium.client.takeover.scene;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import org.joml.Matrix4f;
import org.junit.jupiter.api.Test;
import static org.junit.jupiter.api.Assertions.*;

class SceneUniformsTest {
    @Test void writesColumnMajorMatricesIntoHeapStorageAtNonzeroOffsets() {
        var bytes=ByteBuffer.allocate(96).order(ByteOrder.nativeOrder());
        var matrix=new Matrix4f().translate(2,3,4).rotateXYZ(.2f,.4f,.7f).scale(2,3,4);
        SceneUniforms.matrix(matrix,bytes,16);
        var expected=matrix.get(new float[16]);
        for(int i=0;i<16;i++) assertEquals(expected[i],bytes.getFloat(16+i*4));
        assertEquals(0,bytes.getLong(0)); assertEquals(0,bytes.getLong(80)); assertEquals(0,bytes.position());
    }
}
