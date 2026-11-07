#!/usr/bin/env python3
"""Integration regression: invalid Vulkan half-image modules become valid Vulkan shaders.

Usage: python3 tests/test_spirv_images.py /path/to/spirv_rewrite_test
Requires spirv-as, spirv-val and spirv-dis from SPIRV-Tools.
"""
import pathlib
import struct
import subprocess
import sys
import tempfile


SHADER = """
OpCapability Shader
OpCapability Float16
OpCapability StorageImageExtendedFormats
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %source %target
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %source DescriptorSet 0
OpDecorate %source Binding 0
OpDecorate %target DescriptorSet 0
OpDecorate %target Binding 1
%void = OpTypeVoid
%function = OpTypeFunction %void
%half = OpTypeFloat 16
FLOAT_DECL
%half4 = OpTypeVector %half 4
%int = OpTypeInt 32 1
%int2 = OpTypeVector %int 2
%zero = OpConstant %int 0
%xy = OpConstantComposite %int2 %zero %zero
%halfimage = OpTypeImage %half 2D 0 0 0 2 Rgba16f
%halfptr = OpTypePointer UniformConstant %halfimage
%source = OpVariable %halfptr UniformConstant
TARGET_TYPES
%main = OpFunction %void None %function
%entry = OpLabel
%input = OpLoad %halfimage %source
%texel = OpImageRead %half4 %input %xy
%arithmetic = OpFAdd %half4 %texel %texel
%output = OpLoad TARGET_IMAGE %target
OpImageWrite %output %xy TARGET_TEXEL
OpReturn
OpFunctionEnd
"""


def run(*args, success=True):
    result = subprocess.run(args, text=True, capture_output=True)
    assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
    return result


def shader(extra_float=True, duplicate=False):
    text = SHADER.replace("FLOAT_DECL", "%float = OpTypeFloat 32" if extra_float else "")
    if duplicate:
        text = text.replace("TARGET_TYPES", """
%float4 = OpTypeVector %float 4
%floatimage = OpTypeImage %float 2D 0 0 0 2 Rgba16f
%floatptr = OpTypePointer UniformConstant %floatimage
%target = OpVariable %floatptr UniformConstant
""")
        # Half texels are intentionally converted by the adapter at this boundary.
        return text.replace("TARGET_IMAGE", "%floatimage").replace("TARGET_TEXEL", "%arithmetic")
    return text.replace("TARGET_TYPES", "%target = OpVariable %halfptr UniformConstant").replace("TARGET_IMAGE", "%halfimage").replace("TARGET_TEXEL", "%arithmetic")


def main():
    executable = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="ffx-spirv-") as folder:
        root = pathlib.Path(folder)
        duplicates = shader(duplicate=True).replace("%floatptr", "%100").replace("%zero =", "%literal = OpConstant %int 100\n%zero =")
        for name, assembly in [("read-write", shader()), ("new-float", shader(False)), ("duplicate-types", duplicates)]:
            source = root / f"{name}.spvasm"
            source.write_text(assembly)
            original, output, again = [root / f"{name}.{suffix}.spv" for suffix in ["original", "rewritten", "again"]]
            run("spirv-as", "--preserve-numeric-ids", "--target-env", "spv1.5", str(source), "-o", str(original))
            run("spirv-val", "--target-env", "vulkan1.2", str(original), success=False)
            run(str(executable), str(original), str(output))
            run("spirv-val", "--target-env", "vulkan1.2", str(output))
            disassembly = run("spirv-dis", str(output)).stdout
            assert "OpFAdd %v4half" in disassembly, disassembly
            assert disassembly.count("OpFConvert") == 2, disassembly
            if name == "duplicate-types":
                assert "OpConstant %int 100" in disassembly, "a literal matching an eliminated type ID must survive"
            run(str(executable), str(output), str(again))
            assert again.read_bytes() == output.read_bytes(), "second rewrite must be a byte-for-byte no-op"
            print(f"PASS {name}: Vulkan validation and preserved half arithmetic")

        sampled = shader().replace("%halfimage =", """
%float2 = OpTypeVector %float 2
%fzero = OpConstant %float 0
%uv = OpConstantComposite %float2 %fzero %fzero
%sampledimage = OpTypeImage %half 2D 0 0 0 1 Unknown
%combined = OpTypeSampledImage %sampledimage
%combinedptr = OpTypePointer UniformConstant %combined
%texture = OpVariable %combinedptr UniformConstant
%halfimage =""").replace("%source %target", "%source %target %texture").replace("OpDecorate %source DescriptorSet 0", "OpDecorate %texture DescriptorSet 0\nOpDecorate %texture Binding 2\nOpDecorate %source DescriptorSet 0").replace("%texel = OpImageRead %half4 %input %xy", "%sample = OpLoad %combined %texture\n%texel = OpImageSampleExplicitLod %half4 %sample %uv Lod %fzero")
        source.write_text(sampled)
        run("spirv-as", "--target-env", "spv1.5", str(source), "-o", str(original))
        run("spirv-val", "--target-env", "vulkan1.2", str(original), success=False)
        run(str(executable), str(original), str(output))
        run("spirv-val", "--target-env", "vulkan1.2", str(output))
        print("PASS sampled half image and storage write validate")

        valid = shader().replace("OpTypeImage %half", "OpTypeImage %float").replace("OpImageRead %half4", "OpImageRead %float4").replace("%half4 =", "%float4 = OpTypeVector %float 4\n%half4 =").replace("OpFAdd %half4", "OpFAdd %float4")
        source.write_text(valid)
        run("spirv-as", "--target-env", "spv1.5", str(source), "-o", str(original))
        run("spirv-val", "--target-env", "vulkan1.2", str(original))
        run(str(executable), str(original), str(output))
        assert original.read_bytes() == output.read_bytes()
        print("PASS valid image shader is unchanged")

        # The driver postpass binds this recurrent-state resource to an RGBA8 UNORM
        # view despite declaring Rgba32f, and writes three-component color to RGBA
        # views. SPIR-V syntax validation alone misses the latter dispatch-time VUID.
        short = shader(duplicate=True).replace("%half4 =", "%half3 = OpTypeVector %half 3\n%half4 =")
        short = short.replace("%arithmetic = OpFAdd %half4 %texel %texel", "%rgb = OpVectorShuffle %half3 %texel %texel 0 1 2\n%arithmetic = OpFAdd %half3 %rgb %rgb")
        short = short.replace("%floatimage = OpTypeImage %float 2D 0 0 0 2 Rgba16f", "%floatimage = OpTypeImage %float 2D 0 0 0 2 Rgba32f")
        short = short.replace("OpDecorate %source DescriptorSet 0", 'OpName %target "rw_recurrent_0"\nOpDecorate %source DescriptorSet 0')
        source.write_text(short)
        run("spirv-as", "--target-env", "spv1.5", str(source), "-o", str(original))
        run(str(executable), str(original), str(output), "--driver")
        run("spirv-val", "--target-env", "vulkan1.2", str(output))
        disassembly = run("spirv-dis", str(output)).stdout
        assert "OpTypeImage %float 2D 0 0 0 2 Unknown" in disassembly
        assert "OpCapability StorageImageReadWithoutFormat" in disassembly
        assert "OpCapability StorageImageWriteWithoutFormat" in disassembly
        assert "OpCompositeConstruct %v4float" in disassembly and "%float_1" in disassembly
        assert "OpFAdd %v3half" in disassembly
        run(str(executable), str(output), str(again), "--driver")
        assert again.read_bytes() == output.read_bytes()
        print("PASS driver recurrent format and alpha padding; second rewrite is unchanged")

        short32 = short.replace("%half = OpTypeFloat 16", "").replace("OpTypeVector %half", "OpTypeVector %float").replace("OpTypeImage %half", "OpTypeImage %float")
        short32 = short32.replace("%float4 = OpTypeVector %float 4", "")
        # Keep a proper RGB type for the source and its arithmetic while the write
        # workaround creates a new RGBA type in an otherwise float32-only module.
        source.write_text(short32)
        run("spirv-as", "--target-env", "spv1.5", str(source), "-o", str(original))
        run(str(executable), str(original), str(output))
        assert original.read_bytes() == output.read_bytes(), "driver workarounds must be opt-in"
        run(str(executable), str(original), str(output), "--driver")
        run("spirv-val", "--target-env", "vulkan1.2", str(output))
        print("PASS driver fixes apply without half images and are disabled by default")

        words = list(struct.unpack(f"{len(output.read_bytes()) // 4}I", output.read_bytes()))
        for name, malformed in [("truncated-header", words[:4]), ("zero-length-instruction", words[:5] + [0]), ("truncated-instruction", words[:5] + [0xffff0001]), ("bad-bound", [*words[:3], 1, *words[4:]])]:
            original.write_bytes(struct.pack(f"{len(malformed)}I", *malformed))
            run(str(executable), str(original), str(output), success=False)
            print(f"PASS rejects {name}")


if __name__ == "__main__":
    main()
