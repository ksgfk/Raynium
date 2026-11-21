import sys
from pathlib import Path
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from convert_vanilla_shaders import (
    expand, raw_block, resource_declaration, link_varyings,
    instanced_entity_vertex_source, instanced_particle_vertex_source,
)


class VanillaShaderLayoutTest(unittest.TestCase):
    def test_unconsumed_vertex_output_cannot_shift_packed_fragment_signature(self):
        vertex = """struct SPIRV_Cross_Output {
    float unused : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float4 position : SV_Position;
};
    stage_output.unused = unused;
    stage_output.uv = uv;
    stage_output.position = position;
"""
        pixel = 'struct SPIRV_Cross_Input { float2 uv : TEXCOORD1; };'
        result = link_varyings(vertex, pixel)
        self.assertNotIn('unused', result)
        self.assertIn('float2 uv : TEXCOORD1;', result)
        self.assertIn('float4 position : SV_Position;', result)
        self.assertIn('stage_output.uv = uv;', result)
        self.assertNotIn('TEXCOORD', link_varyings(vertex, 'void main() {}'))

    def test_cloud_texels_preserve_the_byte_format_and_reject_unknown_buffers(self):
        source=resource_declaration('Buffer<int4> CloudFaces : register(t5, space1);','t',5,{'CloudFaces':'R8_SINT'})
        self.assertIn('vk::image_format("r8i")',source)
        self.assertIn('vk::binding(10, 1)',source)
        self.assertIn('#ifdef __spirv__',source)
        with self.assertRaises(ValueError):
            resource_declaration('Buffer<int4> Unknown : register(t0, space1);','t',0,{})
        self.assertIn('"rg32ui"', resource_declaration('Buffer<uint4> Faces : register(t0, space1);','t',0,{'Faces':'RG32_UINT'}))
        with self.assertRaises(ValueError):
            resource_declaration('Buffer<float4> Faces : register(t0, space1);','t',0,{'Faces':'RG32_UINT'})

    def test_std140_vec3_and_trailing_scalar_share_the_original_register(self):
        source, size = raw_block('Globals', '''
            int3 CameraBlockPos : packoffset(c0);
            float GlintAlpha : packoffset(c0.w);
            float3 CameraOffset : packoffset(c1);
            float GameTime : packoffset(c1.w);
        ''', 2)
        self.assertEqual(32, size)
        self.assertIn('asint(Raw_Globals[0].xyz)', source)
        self.assertIn('asfloat(Raw_Globals[0].w)', source)
        self.assertIn('asfloat(Raw_Globals[1].xyz)', source)
        self.assertIn('asfloat(Raw_Globals[1].w)', source)
        self.assertIn('binding(2, 0)', source)

    def test_matrix_preserves_all_four_rows_and_register_padding(self):
        source, size = raw_block('Projection', 'row_major float4x4 ProjMat : packoffset(c1);', 0)
        self.assertEqual(80, size)
        self.assertIn('float4x4('+', '.join(f'asfloat(Raw_Projection[{i}])' for i in range(1, 5))+')', source)
        with self.assertRaises(ValueError):
            raw_block('Invalid', 'float4 Value : packoffset(c0.y);', 0)
        with self.assertRaises(ValueError):
            raw_block('Invalid', 'float4 Values[2] : packoffset(c0);', 0)

    def test_include_resolution_rejects_cycles_and_unsupported_namespaces(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); (root/'include').mkdir()
            shader = root/'main.vsh'
            shader.write_text('#include <minecraft:a.glsl>\n')
            (root/'include/a.glsl').write_text('#include <minecraft:a.glsl>\n')
            with self.assertRaisesRegex(ValueError, 'Cyclic'):
                expand(shader, root)
            shader.write_text('#include <other:a.glsl>\n')
            with self.assertRaisesRegex(ValueError, 'Unsupported'):
                expand(shader, root)

    def test_instanced_entity_and_particle_transforms_preserve_shader_contracts(self):
        entity_vsh = (
            '#version 330\n'
            'layout(location = 0) in vec3 Position;\n'
            'layout(location = 1) in vec4 Color;\n'
            'layout(location = 2) in vec2 UV0;\n'
            'layout(location = 3) in ivec2 UV1;\n'
            'layout(location = 4) in ivec2 UV2;\n'
            'layout(location = 5) in vec3 Normal;\n'
            'void main() { gl_Position = vec4(Position + Normal, 1.0); }\n'
        )
        transformed_entity = instanced_entity_vertex_source(entity_vsh)
        self.assertIn('in vec3 LocalPos;', transformed_entity)
        self.assertIn('in vec4 InstNormalZ;', transformed_entity)
        self.assertIn('LocalNormal.x * InstNormalX.xyz', transformed_entity)

        particle_vsh = (
            '#version 330\n'
            'layout(location = 0) in vec3 Position;\n'
            'layout(location = 1) in vec2 UV0;\n'
            'layout(location = 2) in vec4 Color;\n'
            'layout(location = 3) in ivec2 UV2;\n'
            'void main() { gl_Position = vec4(Position, 1.0); }\n'
        )
        transformed_particle = instanced_particle_vertex_source(particle_vsh)
        self.assertIn('in vec2 CornerPos;', transformed_particle)
        self.assertIn('in vec4 InstRot;', transformed_particle)
        self.assertIn('InstColor.bgra', transformed_particle)
