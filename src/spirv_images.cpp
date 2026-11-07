#include "spirv_images.h"

#include <spirv-tools/libspirv.h>
#include <spirv/unified1/spirv.hpp11>

#include <algorithm>
#include <cstring>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace ffxvk {
namespace {

using Op = spv::Op;
struct Instruction {
    std::vector<uint32_t> words;
    std::vector<uint16_t> references;
    uint32_t type = 0;
    uint32_t result = 0;
    Op op() const { return static_cast<Op>(words[0] & 0xffff); }
};

spv_result_t instruction(void* data, const spv_parsed_instruction_t* parsed) {
    Instruction item{{parsed->words, parsed->words + parsed->num_words}, {}, parsed->type_id, parsed->result_id};
    for (uint16_t i = 0; i < parsed->num_operands; ++i) {
        const auto& operand = parsed->operands[i];
        // The parser distinguishes IDs from literals, including mixed image operands and
        // switch cases. Replacing words by numeric value would corrupt unrelated literals.
        if (operand.type == SPV_OPERAND_TYPE_ID || operand.type == SPV_OPERAND_TYPE_TYPE_ID ||
            operand.type == SPV_OPERAND_TYPE_SCOPE_ID || operand.type == SPV_OPERAND_TYPE_MEMORY_SEMANTICS_ID)
            item.references.push_back(operand.offset);
    }
    static_cast<std::vector<Instruction>*>(data)->push_back(std::move(item));
    return SPV_SUCCESS;
}

void emit(std::vector<uint32_t>& output, Op op, std::initializer_list<uint32_t> operands) {
    output.push_back((uint32_t(operands.size() + 1) << 16) | uint32_t(op));
    output.insert(output.end(), operands);
}

bool mergeable(Op op) {
    // These non-aggregate types must have unique declarations. Aggregate types can have
    // meaningful separate identities (and member decorations), so leave those distinct.
    return op == Op::OpTypeImage || op == Op::OpTypeSampledImage || op == Op::OpTypePointer ||
           op == Op::OpTypeFunction;
}

bool readImage(Op op) {
    return uint32_t(op) >= uint32_t(Op::OpImageSampleImplicitLod) &&
           uint32_t(op) <= uint32_t(Op::OpImageRead);
}

ImageRewrite invalid(std::string message) {
    return {ImageRewriteStatus::Invalid, {}, std::move(message)};
}

} // namespace

ImageRewrite rewriteHalfImageTypes(std::span<const uint32_t> code, bool driverCompatibility) {
    if (code.size() < 5 || code[0] != spv::MagicNumber || !code[3] || code[4])
        return invalid("invalid SPIR-V header");
    std::vector<Instruction> instructions;
    spv_context context = spvContextCreate(SPV_ENV_UNIVERSAL_1_6);
    spv_diagnostic diagnostic = nullptr;
    const auto parsed = spvBinaryParse(context, &instructions, code.data(), code.size(), nullptr, instruction, &diagnostic);
    std::string problem = diagnostic && diagnostic->error ? diagnostic->error : "invalid SPIR-V instruction stream";
    spvDiagnosticDestroy(diagnostic);
    spvContextDestroy(context);
    if (parsed != SPV_SUCCESS) return invalid(problem);

    std::unordered_set<uint32_t> halves;
    std::unordered_set<uint32_t> defined;
    std::unordered_map<uint32_t, uint32_t> valueTypes;
    std::unordered_map<uint32_t, const Instruction*> types;
    std::unordered_set<uint32_t> capabilities;
    uint32_t full = 0;
    bool hasHalfImages = false;
    for (const auto& i : instructions) {
        if (i.result && (i.result >= code[3] || !defined.insert(i.result).second))
            return invalid("SPIR-V result ID is duplicated or exceeds the header bound");
        for (auto offset : i.references)
            if (!i.words[offset] || i.words[offset] >= code[3]) return invalid("SPIR-V ID exceeds the header bound");
        if (i.type) valueTypes[i.result] = i.type;
        if (i.result && !i.type) types[i.result] = &i;
        if (i.op() == Op::OpCapability) capabilities.insert(i.words[1]);
        if (i.op() == Op::OpTypeFloat && i.words.size() == 3) {
            if (i.words[2] == 16) halves.insert(i.result);
            if (i.words[2] == 32) full = i.result;
        }
    }
    for (const auto& i : instructions)
        if (i.op() == Op::OpTypeImage && halves.contains(i.words[2])) hasHalfImages = true;

    std::unordered_set<uint32_t> formatless;
    if (driverCompatibility) {
        // The driver's postpass declares this UNORM recurrent-state image as
        // Rgba32f. Let the bound view supply conversion instead. Keep this workaround
        // scoped to the observed resource; never strip formats from ordinary shaders.
        for (const auto& i : instructions) {
            if (i.op() != Op::OpName || std::strcmp(reinterpret_cast<const char*>(i.words.data() + 2), "rw_recurrent_0")) continue;
            if (!valueTypes.contains(i.words[1]) || !types.contains(valueTypes.at(i.words[1]))) continue;
            const auto& pointer = *types.at(valueTypes.at(i.words[1]));
            if (pointer.op() != Op::OpTypePointer || !types.contains(pointer.words[3])) continue;
            const auto& image = *types.at(pointer.words[3]);
            if (image.op() == Op::OpTypeImage && image.words[7] == 2 && image.words[8] == uint32_t(spv::ImageFormat::Rgba32f))
                formatless.insert(image.result);
        }
    }
    std::unordered_set<uint32_t> paddedWrites;
    if (driverCompatibility) {
        for (const auto& i : instructions) {
            if (i.op() != Op::OpImageWrite || !valueTypes.contains(i.words[3]) || !valueTypes.contains(i.words[1])) continue;
            const uint32_t texelType = valueTypes.at(i.words[3]);
            const uint32_t imageType = valueTypes.at(i.words[1]);
            if (!types.contains(texelType) || !types.contains(imageType)) continue;
            const auto& texel = *types.at(texelType);
            const auto& image = *types.at(imageType);
            // Four components also cover formatless RGBA views. Additional components
            // are ignored by narrower views, preserving the original first channels.
            if (texel.op() == Op::OpTypeVector && texel.words[3] == 3 && image.op() == Op::OpTypeImage &&
                image.words[7] == 2 && types.contains(texel.words[2]) && types.at(texel.words[2])->op() == Op::OpTypeFloat &&
                (types.at(texel.words[2])->words[2] == 16 || types.at(texel.words[2])->words[2] == 32))
                paddedWrites.insert(i.words[3]);
        }
    }
    if (!hasHalfImages && formatless.empty() && paddedWrites.empty()) return {};

    // Keep generated IDs below SPIR-V's maximum bound and guard hostile input overflow.
    uint32_t next = code[3];
    constexpr uint32_t maxBound = 0x3fffff;
    if (next >= maxBound || instructions.size() > (maxBound - next - 1) / 5)
        return invalid("SPIR-V ID space cannot accommodate image conversions");
    const bool newFloat = !full;
    if (newFloat) full = next++;
    std::unordered_map<uint32_t, uint32_t> promoted;
    for (uint32_t half : halves) promoted[half] = full;
    std::map<uint32_t, uint32_t> fullVectors;
    for (const auto& i : instructions)
        if (i.op() == Op::OpTypeVector && i.words[2] == full) fullVectors[i.words[3]] = i.result;
    std::vector<uint32_t> newVectors;
    for (const auto& i : instructions) {
        if (i.op() != Op::OpTypeVector || !halves.contains(i.words[2])) continue;
        auto [it, inserted] = fullVectors.try_emplace(i.words[3], next);
        if (inserted) {
            ++next;
            emit(newVectors, Op::OpTypeVector, {it->second, full, i.words[3]});
        }
        promoted[i.result] = it->second;
    }
    uint32_t paddingType = 0, alpha = 0;
    if (!paddedWrites.empty()) {
        auto [it, inserted] = fullVectors.try_emplace(4, next);
        paddingType = it->second;
        if (inserted) {
            ++next;
            emit(newVectors, Op::OpTypeVector, {paddingType, full, 4});
        }
        for (const auto& i : instructions)
            if (i.op() == Op::OpConstant && i.type == full && i.words.size() == 4 && i.words[3] == 0x3f800000) alpha = i.result;
        if (!alpha) {
            alpha = next++;
            emit(newVectors, Op::OpConstant, {full, alpha, 0x3f800000});
        }
    }

    // A sparse image result is a struct containing both residency and texel data. It needs
    // reconstruction rather than a scalar/vector FConvert; refuse instead of emitting an
    // invalid or subtly altered shader. The FSR upscaler's ordinary image IO is supported.
    std::unordered_set<uint32_t> sparseHalfTypes;
    for (const auto& i : instructions) {
        if (i.op() == Op::OpTypeStruct)
            for (size_t n = 2; n < i.words.size(); ++n)
                if (promoted.contains(i.words[n])) sparseHalfTypes.insert(i.result);
    }
    for (const auto& i : instructions) {
        const uint32_t op = uint32_t(i.op());
        if (((op >= 305 && op <= 315) || i.op() == Op::OpImageSparseRead) && sparseHalfTypes.contains(i.type))
            return invalid("sparse half-image IO is not supported");
        if (i.op() == Op::OpImageTexelPointer) {
            for (const auto& type : instructions)
                if (type.result == i.type && type.op() == Op::OpTypePointer && promoted.contains(type.words[3]))
                    return invalid("half-image texel pointers are not supported");
        }
    }
    for (auto& i : instructions)
        if (i.op() == Op::OpTypeImage && halves.contains(i.words[2])) i.words[2] = full;
    for (auto& i : instructions)
        if (formatless.contains(i.result)) i.words[8] = uint32_t(spv::ImageFormat::Unknown);

    // Image promotion can collapse formerly distinct image, sampled-image and pointer
    // declarations. Canonicalize them and every actual ID reference to their old types.
    std::unordered_map<uint32_t, uint32_t> aliases;
    const auto resolve = [&](uint32_t id) {
        while (aliases.contains(id)) id = aliases.at(id);
        return id;
    };
    bool changed;
    do {
        changed = false;
        std::map<std::vector<uint32_t>, uint32_t> canonical;
        for (auto& i : instructions) {
            for (auto offset : i.references) i.words[offset] = resolve(i.words[offset]);
            if (!mergeable(i.op()) || aliases.contains(i.result)) continue;
            std::vector<uint32_t> key{i.words[0]};
            key.insert(key.end(), i.words.begin() + 2, i.words.end());
            auto [it, unique] = canonical.try_emplace(std::move(key), i.result);
            if (!unique) { aliases[i.result] = it->second; changed = true; }
        }
    } while (changed);

    ImageRewrite output{ImageRewriteStatus::Rewritten, {code.begin(), code.begin() + 5}, {}};
    if (!formatless.empty()) {
        for (const auto capability : {spv::Capability::StorageImageReadWithoutFormat, spv::Capability::StorageImageWriteWithoutFormat})
            if (!capabilities.contains(uint32_t(capability))) emit(output.words, Op::OpCapability, {uint32_t(capability)});
    }
    bool floatAdded = !newFloat, vectorsAdded = newVectors.empty();
    for (auto& i : instructions) {
        for (auto offset : i.references) i.words[offset] = resolve(i.words[offset]);
        if (aliases.contains(i.result)) continue;
        if (i.op() == Op::OpTypeImage && !floatAdded) {
            emit(output.words, Op::OpTypeFloat, {full, 32});
            floatAdded = true;
        }
        if (i.op() == Op::OpFunction && !vectorsAdded) {
            output.words.insert(output.words.end(), newVectors.begin(), newVectors.end());
            vectorsAdded = true;
        }
        if (readImage(i.op()) && promoted.contains(i.type)) {
            const uint32_t temporary = next++;
            i.words[1] = promoted.at(i.type);
            i.words[2] = temporary;
            output.words.insert(output.words.end(), i.words.begin(), i.words.end());
            emit(output.words, Op::OpFConvert, {i.type, i.result, temporary});
        } else if (i.op() == Op::OpImageWrite) {
            const uint32_t originalTexel = i.words[3];
            if (valueTypes.contains(originalTexel) && promoted.contains(valueTypes.at(originalTexel))) {
                const uint32_t temporary = next++;
                emit(output.words, Op::OpFConvert, {promoted.at(valueTypes.at(originalTexel)), temporary, originalTexel});
                i.words[3] = temporary;
            }
            if (paddedWrites.contains(originalTexel)) {
                const uint32_t temporary = next++;
                emit(output.words, Op::OpCompositeConstruct, {paddingType, temporary, i.words[3], alpha});
                i.words[3] = temporary;
            }
            output.words.insert(output.words.end(), i.words.begin(), i.words.end());
        } else {
            output.words.insert(output.words.end(), i.words.begin(), i.words.end());
        }
    }
    if (!vectorsAdded) output.words.insert(output.words.end(), newVectors.begin(), newVectors.end());
    output.words[3] = next;
    return output;
}

} // namespace ffxvk
