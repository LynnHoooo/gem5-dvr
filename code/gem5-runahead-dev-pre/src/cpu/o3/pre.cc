#include "cpu/o3/pre.hh"

#include <algorithm>

#include "cpu/o3/dyn_inst.hh"

namespace gem5
{

namespace o3
{

DVRStrideDetector::DVRStrideDetector(unsigned num_entries)
    : entries(num_entries)
{
    assert(num_entries > 0);
}

std::optional<DVRStrideDetector::Candidate>
DVRStrideDetector::observe(Addr pc, Addr address)
{
    // 为每个 load PC 维护上一地址、步幅和置信度。
    ++timestamp;
    Entry *entry = nullptr;

    for (auto &candidate : entries) {
        if (candidate.valid && candidate.pc == pc) {
            entry = &candidate;
            break;
        }
    }

    if (!entry) {
        entry = &entries.front();
        // 新 PC 使用空表项；表满时淘汰最旧项。
        for (auto &candidate : entries) {
            if (!candidate.valid) {
                entry = &candidate;
                break;
            }
            if (candidate.age < entry->age)
                entry = &candidate;
        }
        *entry = Entry{};
        entry->valid = true;
        entry->pc = pc;
        entry->lastAddress = address;
        entry->age = timestamp;
        return std::nullopt;
    }

    const int64_t observed_stride = static_cast<int64_t>(address) -
                                    static_cast<int64_t>(entry->lastAddress);
    // 连续看到相同步幅时提高置信度，否则降低置信度并重新学习步幅。
    if (observed_stride != 0 && observed_stride == entry->stride) {
        if (entry->confidence < 3)
            ++entry->confidence;
    } else {
        if (entry->confidence > 0)
            --entry->confidence;
        if (entry->confidence == 0)
            entry->stride = observed_stride;
    }

    entry->lastAddress = address;
    entry->age = timestamp;

    if (entry->stride != 0 && entry->confidence >= 2)
        // 置信度达到阈值后，报告一个可用于 Discovery 的候选。
        return Candidate{pc, address, entry->stride};
    return std::nullopt;
}

void
DVRStrideDetector::reset()
{
    // 清空所有训练状态。
    for (auto &entry : entries)
        entry = Entry{};
    timestamp = 0;
}

DVRDiscoveryController::DVRDiscoveryController(unsigned max_instructions)
    : maxInstructions(max_instructions)
{
    assert(maxInstructions > 0);
}

void
DVRDiscoveryController::arm(
    const DVRStrideDetector::Candidate &candidate, InstSeqNum sequence)
{
    // 只允许同时存在一个待启动的 Discovery。
    if (state != State::Idle)
        return;

    state = State::Armed;
    triggerPC = candidate.pc;
    triggerStride = candidate.stride;
    triggerSequence = sequence;
    instructions = 0;
}

DVRDiscoveryController::Result
DVRDiscoveryController::observeCommit(Addr pc, InstSeqNum sequence)
{
    // 只有 trigger load 真正提交后，才进入 Discovery。
    if (state == State::Armed) {
        if (sequence == triggerSequence && pc == triggerPC) {
            state = State::Discovering;
            return {Event::Started, triggerPC, triggerStride, 0};
        }
        // 提交有序；序列号超过候选值，说明候选指令已在提交前被清除。
        if (sequence > triggerSequence) {
            const Result result{
                Event::Abandoned, triggerPC, triggerStride, 0};
            finish();
            return result;
        }
        return {};
    }

    if (state != State::Discovering || sequence <= triggerSequence)
        return {};

    if (pc == triggerPC) {
        // 再次提交相同 trigger PC，表示本次 Discovery 到达循环边界。
        const Result result{
            Event::Completed, triggerPC, triggerStride, instructions};
        finish();
        return result;
    }

    ++instructions;
    if (instructions >= maxInstructions) {
        // 防止错误路径或异常循环让 Discovery 无限持续。
        const Result result{
            Event::TimedOut, triggerPC, triggerStride, instructions};
        finish();
        return result;
    }

    return {};
}

void
DVRDiscoveryController::finish()
{
    state = State::Idle;
    triggerPC = 0;
    triggerStride = 0;
    triggerSequence = 0;
    instructions = 0;
}

void
DVRDiscoveryController::reset()
{
    finish();
}

bool
DVRVectorTaintTracker::tracked(const RegId &reg) const
{
    return reg.classValue() == IntRegClass &&
           reg.index() < NumTrackedRegs;
}

bool
DVRVectorTaintTracker::isTainted(const RegId &reg) const
{
    return tracked(reg) && (taint & (uint32_t(1) << reg.index()));
}

void
DVRVectorTaintTracker::setTainted(const RegId &reg, bool value)
{
    if (!tracked(reg))
        return;

    const uint32_t mask = uint32_t(1) << reg.index();
    if (value)
        taint |= mask;
    else
        taint &= ~mask;
}

void
DVRVectorTaintTracker::begin(const DynInstPtr &initiating_load)
{
    // Discovery 开始时，只把 trigger load 的目标寄存器标为污点。
    reset();
    for (int idx = 0; idx < initiating_load->numDestRegs(); ++idx)
        setTainted(initiating_load->destRegIdx(idx), true);
}

DVRVectorTaintTracker::Observation
DVRVectorTaintTracker::observe(const DynInstPtr &inst)
{
    // 任一整数源寄存器带污点，就认为当前指令属于依赖链。
    bool source_tainted = false;
    for (int idx = 0; idx < inst->numSrcRegs(); ++idx) {
        if (isTainted(inst->srcRegIdx(idx))) {
            source_tainted = true;
            break;
        }
    }

    const bool dependent_load = inst->isLoad() && source_tainted;
    if (dependent_load) {
        // 最后一个依赖 load 的 PC 作为 FLR。
        finalLoadPC = inst->pcState().instAddr();
    }
    for (int idx = 0; idx < inst->numDestRegs(); ++idx)
        setTainted(inst->destRegIdx(idx), source_tainted);

    return {source_tainted, dependent_load};
}

void
DVRVectorTaintTracker::reset()
{
    taint = 0;
    finalLoadPC = 0;
}

namespace
{

uint32_t
dvrIntRegisterMask(const DynInstPtr &inst, bool sources)
{
    // 将整数源或目标寄存器编码成位图。
    uint32_t mask = 0;
    const int count = sources ? inst->numSrcRegs() : inst->numDestRegs();
    for (int index = 0; index < count; ++index) {
        const RegId &reg = sources ? inst->srcRegIdx(index) :
                                     inst->destRegIdx(index);
        if (reg.classValue() == IntRegClass && reg.index() < 32)
            mask |= uint32_t(1) << reg.index();
    }
    return mask;
}

int8_t
dvrFirstIntRegister(const DynInstPtr &inst, bool sources, unsigned ordinal)
{
    // 按出现顺序取第 ordinal 个整数寄存器。
    const int count = sources ? inst->numSrcRegs() : inst->numDestRegs();
    unsigned found = 0;
    for (int index = 0; index < count; ++index) {
        const RegId &reg = sources ? inst->srcRegIdx(index) :
                                     inst->destRegIdx(index);
        if (reg.classValue() != IntRegClass || reg.index() >= 32)
            continue;
        if (found++ == ordinal)
            return reg.index();
    }
    return -1;
}

int64_t
dvrSignExtend(uint64_t value, unsigned width)
{
    // 将指定宽度的立即数符号扩展为 64 位整数。
    const uint64_t sign = uint64_t(1) << (width - 1);
    return static_cast<int64_t>((value ^ sign) - sign);
}

void
dvrDecodeRiscvSemantic(DVRInstructionRecorder::Uop &uop,
                       const DynInstPtr &inst)
{
    /* StaticInst::asBytes() 是与 ISA 无关的接口，用于获取指令编码。
     * RiscvStaticInst 写入 ExtMachInst；低 32 位是架构指令。
     */
    uint64_t encoded = 0;
    const size_t encoded_size =
        inst->staticInst->asBytes(&encoded, sizeof(encoded));
    if (encoded_size == 0 || encoded_size > sizeof(encoded))
        return;

    uop.encoding = static_cast<uint32_t>(encoded);
    uop.encodingValid = true;

    using Semantic = DVRInstructionRecorder::Uop::Semantic;

    /*
     * RV64C forms emitted by the dependent-load benchmark.  Register
     * identities still come from the decoded StaticInst above; only the
     * operation and immediate are recovered here.
     */
    if ((uop.encoding & 0x3) != 0x3) {
        const uint16_t compressed = uop.encoding & 0xffff;
        const uint32_t quadrant = compressed & 0x3;
        const uint32_t funct3 = (compressed >> 13) & 0x7;

        // C.LD rd', uimm(rs1') -- RV64C quadrant 0, funct3 011.
        if (quadrant == 0 && funct3 == 3) {
            uop.semantic = Semantic::LoadAddress;
            uop.immediate =
                ((compressed >> 10) & 0x7) << 3 |
                ((compressed >> 5) & 0x3) << 6;
            return;
        }

        // C.SLLI rd, shamt -- quadrant 2, funct3 000.
        if (quadrant == 2 && funct3 == 0 &&
            ((compressed >> 7) & 0x1f) != 0) {
            uop.semantic = Semantic::ShiftLeftImmediate;
            uop.immediate =
                ((compressed >> 12) & 0x1) << 5 |
                ((compressed >> 2) & 0x1f);
            return;
        }

        // C.BEQZ/C.BNEZ -- quadrant 1, compare rs1' against x0.
        if (quadrant == 1 && (funct3 == 6 || funct3 == 7)) {
            const uint32_t rs1p = 8 + ((compressed >> 7) & 0x7);
            const uint32_t imm = (((compressed >> 12) & 1) << 8) |
                (((compressed >> 10) & 0x3) << 6) |
                (((compressed >> 5) & 0x3) << 3) |
                (((compressed >> 3) & 0x3) << 1) |
                (((compressed >> 2) & 1) << 5);
            uop.source0 = rs1p;
            uop.source1 = 0;
            uop.intSources |= uint32_t(1) << 0;
            uop.semantic = funct3 == 6 ? Semantic::BranchEqual :
                           Semantic::BranchNotEqual;
            uop.immediate = dvrSignExtend(imm, 9);
            uop.conditional = true;
            uop.control = true;
            uop.target = uop.pc + uop.immediate;
            return;
        }

        // C.J -- quadrant 1, unconditional PC-relative jump.
        if (quadrant == 1 && funct3 == 5) {
            const uint32_t imm = (((compressed >> 12) & 1) << 11) |
                (((compressed >> 11) & 1) << 4) |
                (((compressed >> 9) & 0x3) << 8) |
                (((compressed >> 8) & 1) << 10) |
                (((compressed >> 7) & 1) << 6) |
                (((compressed >> 6) & 1) << 7) |
                (((compressed >> 3) & 0x7) << 1) |
                (((compressed >> 2) & 1) << 5);
            uop.semantic = Semantic::JumpAndLink;
            uop.immediate = dvrSignExtend(imm, 12);
            uop.control = true;
            uop.target = uop.pc + uop.immediate;
            return;
        }

        /*
         * C.ADD rd, rs2 -- quadrant 2, funct3 100, bit12=1 and both
         * registers non-zero.  C.JALR/C.EBREAK share the major encoding.
         */
        if (quadrant == 2 && funct3 == 4 &&
            ((compressed >> 12) & 1) != 0 &&
            ((compressed >> 7) & 0x1f) != 0 &&
            ((compressed >> 2) & 0x1f) != 0) {
            uop.semantic = Semantic::Add;
            return;
        }
        // C.JR/C.JALR -- quadrant 2, funct3 100, rs1 != 0, rs2 == 0.
        if (quadrant == 2 && funct3 == 4 &&
            ((compressed >> 12) & 1) <= 1 &&
            ((compressed >> 7) & 0x1f) != 0 &&
            ((compressed >> 2) & 0x1f) == 0) {
            uop.source0 = (compressed >> 7) & 0x1f;
            uop.semantic = ((compressed >> 12) & 1) ?
                Semantic::JumpAndLinkRegister : Semantic::JumpAndLinkRegister;
            uop.control = true;
            return;
        }
        return;
    }

    const uint32_t opcode = uop.encoding & 0x7f;
    const uint32_t funct3 = (uop.encoding >> 12) & 0x7;
    const uint32_t funct7 = (uop.encoding >> 25) & 0x7f;

    if (opcode == 0x33) {
        if (funct3 == 0 && funct7 == 0)
            uop.semantic = Semantic::Add;
        else if (funct3 == 0 && funct7 == 0x20)
            uop.semantic = Semantic::Sub;
        else if (funct3 == 0 && funct7 == 1)
            uop.semantic = Semantic::Mul;
        else if (funct3 == 6 && funct7 == 0)
            uop.semantic = Semantic::Or;
        else if (funct3 == 4 && funct7 == 0)
            uop.semantic = Semantic::Xor;
        else if (funct3 == 7 && funct7 == 0)
            uop.semantic = Semantic::And;
        else if (funct3 == 2 && funct7 == 0)
            uop.semantic = Semantic::Slt;
        else if (funct3 == 3 && funct7 == 0)
            uop.semantic = Semantic::Sltu;
        else if (funct3 == 1 && funct7 == 0)
            uop.semantic = Semantic::Sll;
        else if (funct3 == 5 && funct7 == 0)
            uop.semantic = Semantic::Srl;
        else if (funct3 == 5 && funct7 == 0x20)
            uop.semantic = Semantic::Sra;
        return;
    }

    // RV64 W-form integer operations (results are sign-extended from 32 bits).
    if (opcode == 0x3b) {
        if (funct3 == 0 && funct7 == 0)
            uop.semantic = Semantic::AddWord;
        else if (funct3 == 0 && funct7 == 0x20)
            uop.semantic = Semantic::SubWord;
        else if (funct3 == 1 && funct7 == 0)
            uop.semantic = Semantic::ShiftLeftWord;
        else if (funct3 == 5 && funct7 == 0)
            uop.semantic = Semantic::ShiftRightWord;
        else if (funct3 == 5 && funct7 == 0x20)
            uop.semantic = Semantic::ShiftRightArithmeticWord;
        return;
    }

    if (opcode == 0x13) {
        if (funct3 == 0) {
            uop.semantic = Semantic::AddImmediate;
            uop.immediate = dvrSignExtend(uop.encoding >> 20, 12);
        } else if (funct3 == 1 && (uop.encoding >> 26) == 0) {
            uop.semantic = Semantic::ShiftLeftImmediate;
            uop.immediate = (uop.encoding >> 20) & 0x3f;
        } else if (funct3 == 5) {
            uop.semantic = Semantic::ShiftRightImmediate;
            uop.immediate = (uop.encoding >> 20) & 0x3f;
            if ((uop.encoding >> 30) & 1)
                uop.semantic = Semantic::ShiftRightArithmeticImmediate;
        } else if (funct3 == 2) {
            uop.semantic = Semantic::SltImmediate;
            uop.immediate = dvrSignExtend(uop.encoding >> 20, 12);
        } else if (funct3 == 3) {
            uop.semantic = Semantic::SltuImmediate;
            uop.immediate = dvrSignExtend(uop.encoding >> 20, 12);
        } else if (funct3 == 6) {
            uop.semantic = Semantic::OrImmediate;
            uop.immediate = dvrSignExtend(uop.encoding >> 20, 12);
        } else if (funct3 == 4) {
            uop.semantic = Semantic::XorImmediate;
            uop.immediate = dvrSignExtend(uop.encoding >> 20, 12);
        } else if (funct3 == 7) {
            uop.semantic = Semantic::AndImmediate;
            uop.immediate = dvrSignExtend(uop.encoding >> 20, 12);
        }
        return;
    }

    if (opcode == 0x1b) {
        const int64_t imm = dvrSignExtend(uop.encoding >> 20, 12);
        if (funct3 == 0) {
            uop.semantic = Semantic::AddWordImmediate;
            uop.immediate = imm;
        } else if (funct3 == 1) {
            uop.semantic = Semantic::ShiftLeftWordImmediate;
            uop.immediate = (uop.encoding >> 20) & 0x1f;
        } else if (funct3 == 5) {
            uop.immediate = (uop.encoding >> 20) & 0x1f;
            uop.semantic = ((uop.encoding >> 30) & 1) ?
                Semantic::ShiftRightArithmeticWordImmediate :
                Semantic::ShiftRightWordImmediate;
        }
        return;
    }

    if (opcode == 0x37) {
        uop.semantic = Semantic::Lui;
        uop.immediate = static_cast<int64_t>(uop.encoding & 0xfffff000);
        return;
    }
    if (opcode == 0x17) {
        uop.semantic = Semantic::Auipc;
        uop.immediate = static_cast<int64_t>(uop.encoding & 0xfffff000);
        return;
    }

    if (opcode == 0x03) {
        uop.semantic = Semantic::LoadAddress;
        uop.immediate = dvrSignExtend(uop.encoding >> 20, 12);
        switch (funct3) {
          case 0: uop.loadBytes = 1; uop.loadSigned = true; break; // LB
          case 1: uop.loadBytes = 2; uop.loadSigned = true; break; // LH
          case 2: uop.loadBytes = 4; uop.loadSigned = true; break; // LW
          case 3: uop.loadBytes = 8; uop.loadSigned = true; break; // LD
          case 4: uop.loadBytes = 1; uop.loadSigned = false; break; // LBU
          case 5: uop.loadBytes = 2; uop.loadSigned = false; break; // LHU
          case 6: uop.loadBytes = 4; uop.loadSigned = false; break; // LWU
          default: uop.semantic = Semantic::Unsupported; break;
        }
        return;
    }

    if (opcode == 0x63) {
        const int64_t branch_imm =
            dvrSignExtend(((uop.encoding >> 31) << 12) |
                          (((uop.encoding >> 7) & 1) << 11) |
                          (((uop.encoding >> 25) & 0x3f) << 5) |
                          (((uop.encoding >> 8) & 0xf) << 1), 13);
        uop.immediate = branch_imm;
        uop.conditional = true;
        switch (funct3) {
          case 0: uop.semantic = Semantic::BranchEqual; break;
          case 1: uop.semantic = Semantic::BranchNotEqual; break;
          case 4: uop.semantic = Semantic::BranchLess; break;
          case 5: uop.semantic = Semantic::BranchGreaterEqual; break;
          case 6: uop.semantic = Semantic::BranchLessUnsigned; break;
          case 7: uop.semantic = Semantic::BranchGreaterEqualUnsigned; break;
          default: break;
        }
        return;
    }

    if (opcode == 0x6f) { // JAL: J-immediate, target = pc + immediate.
        const int64_t jump_imm = dvrSignExtend(
            (((uop.encoding >> 31) & 1) << 20) |
            (((uop.encoding >> 12) & 0xff) << 12) |
            (((uop.encoding >> 20) & 1) << 11) |
            (((uop.encoding >> 21) & 0x3ff) << 1), 21);
        uop.semantic = Semantic::JumpAndLink;
        uop.immediate = jump_imm;
        uop.control = true;
        uop.target = uop.pc + jump_imm;
        return;
    }

    if (opcode == 0x67 && funct3 == 0) { // JALR: target = rs1 + imm.
        uop.semantic = Semantic::JumpAndLinkRegister;
        uop.immediate = dvrSignExtend(uop.encoding >> 20, 12);
        uop.control = true;
        return;
    }
}

} // anonymous namespace

bool
DVRInstructionRecorder::Uop::evaluate(
    RegVal source0_value, RegVal source1_value, RegVal &result) const
{
    // 只计算已解码的简单标量操作；不支持的语义返回 false。
    switch (semantic) {
      case Semantic::Add:
        result = source0_value + source1_value;
        return true;
      case Semantic::Sub:
        result = source0_value - source1_value;
        return true;
      case Semantic::Mul:
        result = source0_value * source1_value;
        return true;
      case Semantic::Or:
        result = source0_value | source1_value;
        return true;
      case Semantic::Xor:
        result = source0_value ^ source1_value;
        return true;
      case Semantic::And:
        result = source0_value & source1_value;
        return true;
      case Semantic::Slt:
        result = static_cast<int64_t>(source0_value) <
                 static_cast<int64_t>(source1_value);
        return true;
      case Semantic::Sltu:
        result = source0_value < source1_value;
        return true;
      case Semantic::Sll:
        result = source0_value << (source1_value & 0x3f);
        return true;
      case Semantic::Srl:
        result = source0_value >> (source1_value & 0x3f);
        return true;
      case Semantic::Sra:
        result = static_cast<RegVal>(static_cast<int64_t>(source0_value) >>
                                     (source1_value & 0x3f));
        return true;
      case Semantic::AddImmediate:
      case Semantic::LoadAddress:
        result = source0_value + static_cast<RegVal>(immediate);
        return true;
      case Semantic::SubImmediate:
        result = source0_value - static_cast<RegVal>(immediate);
        return true;
      case Semantic::OrImmediate:
        result = source0_value | static_cast<RegVal>(immediate);
        return true;
      case Semantic::XorImmediate:
        result = source0_value ^ static_cast<RegVal>(immediate);
        return true;
      case Semantic::SltImmediate:
        result = static_cast<int64_t>(source0_value) < immediate;
        return true;
      case Semantic::SltuImmediate:
        result = source0_value < static_cast<RegVal>(immediate);
        return true;
      case Semantic::Lui:
        result = static_cast<RegVal>(immediate);
        return true;
      case Semantic::Auipc:
        result = static_cast<RegVal>(pc) + static_cast<RegVal>(immediate);
        return true;
      case Semantic::ShiftLeftImmediate:
        result = source0_value << (static_cast<unsigned>(immediate) & 0x3f);
        return true;
      case Semantic::ShiftRightImmediate:
        result = source0_value >> (static_cast<unsigned>(immediate) & 0x3f);
        return true;
      case Semantic::ShiftRightArithmeticImmediate:
        result = static_cast<RegVal>(static_cast<int64_t>(source0_value) >>
                                     (static_cast<unsigned>(immediate) & 0x3f));
        return true;
      case Semantic::AndImmediate:
        result = source0_value & static_cast<RegVal>(immediate);
        return true;
      case Semantic::AddWord:
        result = static_cast<RegVal>(static_cast<int64_t>(
            static_cast<int32_t>(source0_value + source1_value)));
        return true;
      case Semantic::SubWord:
        result = static_cast<RegVal>(static_cast<int64_t>(
            static_cast<int32_t>(source0_value - source1_value)));
        return true;
      case Semantic::ShiftLeftWord:
        result = static_cast<RegVal>(static_cast<int64_t>(static_cast<int32_t>(
            static_cast<uint32_t>(source0_value) << (source1_value & 0x1f))));
        return true;
      case Semantic::ShiftRightWord:
        result = static_cast<RegVal>(static_cast<int64_t>(static_cast<int32_t>(
            static_cast<uint32_t>(source0_value) >> (source1_value & 0x1f))));
        return true;
      case Semantic::ShiftRightArithmeticWord:
        result = static_cast<RegVal>(static_cast<int64_t>(static_cast<int32_t>(
            static_cast<int32_t>(source0_value) >> (source1_value & 0x1f))));
        return true;
      case Semantic::AddWordImmediate:
        result = static_cast<RegVal>(static_cast<int64_t>(static_cast<int32_t>(
            static_cast<uint32_t>(source0_value) + static_cast<int32_t>(immediate))));
        return true;
      case Semantic::ShiftLeftWordImmediate:
        result = static_cast<RegVal>(static_cast<int64_t>(static_cast<int32_t>(
            static_cast<uint32_t>(source0_value) << (immediate & 0x1f))));
        return true;
      case Semantic::ShiftRightWordImmediate:
        result = static_cast<RegVal>(static_cast<int64_t>(static_cast<int32_t>(
            static_cast<uint32_t>(source0_value) >> (immediate & 0x1f))));
        return true;
      case Semantic::ShiftRightArithmeticWordImmediate:
        result = static_cast<RegVal>(static_cast<int64_t>(static_cast<int32_t>(
            static_cast<int32_t>(source0_value) >> (immediate & 0x1f))));
        return true;
      case Semantic::BranchEqual:
      case Semantic::BranchNotEqual:
      case Semantic::BranchLess:
      case Semantic::BranchGreaterEqual:
      case Semantic::BranchLessUnsigned:
      case Semantic::BranchGreaterEqualUnsigned:
        return false;
      case Semantic::JumpAndLink:
      case Semantic::JumpAndLinkRegister:
        return false;
      case Semantic::Unsupported:
        return false;
    }
    return false;
}

bool
DVRInstructionRecorder::Uop::predicate(RegVal source0_value,
                                       RegVal source1_value) const
{
    switch (semantic) {
      case Semantic::BranchEqual: return source0_value == source1_value;
      case Semantic::BranchNotEqual: return source0_value != source1_value;
      case Semantic::BranchLess:
        return static_cast<int64_t>(source0_value) <
               static_cast<int64_t>(source1_value);
      case Semantic::BranchGreaterEqual:
        return static_cast<int64_t>(source0_value) >=
               static_cast<int64_t>(source1_value);
      case Semantic::BranchLessUnsigned: return source0_value < source1_value;
      case Semantic::BranchGreaterEqualUnsigned:
        return source0_value >= source1_value;
      default: return source0_value != 0;
    }
}

void
DVRInstructionRecorder::begin(const DynInstPtr &trigger)
{
    reset();
    record(trigger);
}

bool
DVRInstructionRecorder::record(const DynInstPtr &inst)
{
    // 记录有限数量的 uop；超出容量后禁止使用该模板。
    if (count >= MaxUops) {
        overflowed = true;
        return false;
    }

    Uop &uop = uops[count++];
    uop.pc = inst->pcState().instAddr();
    if (inst->isDirectCtrl()) {
        auto branch_target = inst->branchTarget();
        uop.target = branch_target ? branch_target->instAddr() : 0;
    }
    uop.intSources = dvrIntRegisterMask(inst, true);
    uop.intDestinations = dvrIntRegisterMask(inst, false);
    uop.source0 = dvrFirstIntRegister(inst, true, 0);
    uop.source1 = dvrFirstIntRegister(inst, true, 1);
    uop.destination = dvrFirstIntRegister(inst, false, 0);
    uop.load = inst->isLoad();
    uop.sideEffect = inst->isStore() || inst->isAtomic() ||
                     inst->isSerializeBefore() || inst->isSerializeAfter() ||
                     inst->isSyscall() || inst->isStoreConditional();
    uop.control = inst->isControl();
    uop.conditional = inst->isCondCtrl();
    uop.branchTaken = inst->pcState().branching();
    dvrDecodeRiscvSemantic(uop, inst);
    return true;
}

void
DVRInstructionRecorder::reset()
{
    uops = {};
    count = 0;
    overflowed = false;
}

DVRVectorRenameTable::DVRVectorRenameTable()
{
    // 初始化所有架构寄存器的向量映射。
    reset();
}

void
DVRVectorRenameTable::reset()
{
    for (auto &reg : mapping)
        reg.fill(-1);
    nextPhysical = 0;
    mappedUops = 0;
}

unsigned
DVRVectorRenameTable::build(const DVRInstructionRecorder &program,
                            unsigned lanes)
{
    // 为每个带目标寄存器的 uop 和每个 lane 分块分配向量物理寄存器。
    reset();
    const unsigned chunks = std::min(NumChunks, (lanes + 15) / 16);
    unsigned allocations = 0;
    for (unsigned uop = 0; uop < program.size(); ++uop) {
        const uint32_t destinations = program[uop].intDestinations;
        for (unsigned reg = 0; reg < NumArchitecturalRegs; ++reg) {
            if (!(destinations & (uint32_t(1) << reg)))
                continue;
            for (unsigned chunk = 0; chunk < chunks; ++chunk) {
                mapping[reg][chunk] = nextPhysical++ % NumPhysicalRegs;
                ++allocations;
            }
        }
    }
    mappedUops = program.size();
    return allocations;
}

unsigned
DVRVectorRenameTable::extend(const DVRInstructionRecorder &program,
                              unsigned lanes)
{
    lanes = std::min(lanes, 128U);
    const unsigned chunks = std::min(NumChunks, (lanes + 15) / 16);
    unsigned allocations = 0;
    for (unsigned uop = mappedUops; uop < program.size(); ++uop) {
        const uint32_t destinations = program[uop].intDestinations;
        for (unsigned reg = 0; reg < NumArchitecturalRegs; ++reg) {
            if (!(destinations & (uint32_t(1) << reg)))
                continue;
            for (unsigned chunk = 0; chunk < chunks; ++chunk) {
                mapping[reg][chunk] = nextPhysical++ % NumPhysicalRegs;
                ++allocations;
            }
        }
    }
    mappedUops = program.size();
    return allocations;
}

int16_t
DVRVectorRenameTable::lookup(unsigned architectural, unsigned chunk) const
{
    assert(architectural < NumArchitecturalRegs);
    assert(chunk < NumChunks);
    return mapping[architectural][chunk];
}

DVRVectorInstructionRegister::Result
DVRVectorInstructionRegister::execute(
    const DVRInstructionRecorder &program, unsigned lanes,
    unsigned max_helper_uops)
{
    // 先建立 active mask，再按 uop 和 16-lane 分块统计执行。
    reset();
    Result result;
    lanes = std::min(lanes, 128U);
    for (unsigned lane = 0; lane < lanes; ++lane)
        activeMask[lane / 64] |= uint64_t(1) << (lane % 64);

    const unsigned chunks = (lanes + 15) / 16;
    result.activeLanes = lanes;
    for (unsigned uop = 0; uop < program.size(); ++uop) {
        /*
         * A conditional uop cannot be partitioned here: source-load values
         * arrive asynchronously after VIR construction.  The old prototype
         * fabricated taken/fall-through masks from lane parity, which made
         * reconvergence counters look active without executing a predicate.
         * DVRLanePredicateTracker now forms masks solely from returned lane
         * values and learned discriminating masks at response time.
         */

        for (unsigned chunk = 0; chunk < chunks; ++chunk) {
            if (result.helperUops >= max_helper_uops) {
                result.timedOut = true;
                return result;
            }
            issuedChunks |= uint16_t(1) << chunk;
            executedChunks |= uint16_t(1) << chunk;
            ++result.chunkIssues;
            ++result.chunkExecutions;
            ++result.helperUops;
        }

    }
    return result;
}

void
DVRVectorInstructionRegister::reset()
{
    activeMask = {};
    stack = {};
    stackDepth = 0;
    issuedChunks = 0;
    executedChunks = 0;
}

void
DVRLoopBoundDetector::begin(Addr trigger_pc)
{
    // 开始新的 Discovery 时清空循环边界状态。
    reset();
    triggerPC = trigger_pc;
}

void
DVRLoopBoundDetector::updateFinalLoad(Addr final_load_pc)
{
    // FLR 改变后重新寻找包围完整依赖链的回边。
    finalLoadPC = final_load_pc;
    loopBranchPC = 0;
    loopTargetPC = 0;
    boundSource0 = -1;
    boundSource1 = -1;
    seenBranch = false;
}

DVRLoopBoundDetector::Observation
DVRLoopBoundDetector::observe(const DynInstPtr &inst)
{
    // 只关注直接的条件回跳分支。
    if (!inst->isCondCtrl() || !inst->isDirectCtrl())
        return {};

    const Addr pc = inst->pcState().instAddr();
    const Addr target = inst->branchTarget()->instAddr();
    if (target > pc)
        return {};

    const bool encloses_chain = finalLoadPC != 0 && target <= triggerPC &&
                                pc >= finalLoadPC;
    // 回跳目标不晚于 trigger，且分支 PC 位于 FLR 之后，才包围完整链。
    if (!seenBranch && encloses_chain) {
        loopBranchPC = pc;
        loopTargetPC = target;
        for (int idx = 0; idx < inst->numSrcRegs(); ++idx) {
            const RegId &src = inst->srcRegIdx(idx);
            if (src.classValue() != IntRegClass)
                continue;
            if (boundSource0 < 0)
                boundSource0 = src.index();
            else if (boundSource1 < 0) {
                boundSource1 = src.index();
                break;
            }
        }
        seenBranch = true;
    }

    return {true, seenBranch && encloses_chain};
}

DVRLoopBoundDetector::Inference
DVRLoopBoundDetector::infer(const RegisterSnapshot &start,
                            const RegisterSnapshot &finish,
                            unsigned max_lanes) const
{
    // 一个寄存器应保持不变作为 bound，另一个应按固定步幅变化。
    Inference inference;
    inference.lanes = max_lanes;
    if (!seenBranch || boundSource0 < 0 || boundSource1 < 0 ||
        boundSource0 >= MaxArchitecturalIntRegs ||
        boundSource1 >= MaxArchitecturalIntRegs)
        return inference;

    const RegVal start0 = start[boundSource0];
    const RegVal finish0 = finish[boundSource0];
    const RegVal start1 = start[boundSource1];
    const RegVal finish1 = finish[boundSource1];

    RegVal bound;
    RegVal current;
    int64_t increment;
    if (start0 == finish0 && start1 != finish1) {
        bound = finish0;
        current = finish1;
        increment = static_cast<int64_t>(finish1 - start1);
    } else if (start1 == finish1 && start0 != finish0) {
        bound = finish1;
        current = finish0;
        increment = static_cast<int64_t>(finish0 - start0);
    } else {
        return inference;
    }

    uint64_t distance = 0;
    uint64_t step = 0;
    if (increment > 0 && current < bound) {
        distance = bound - current;
        step = increment;
    } else if (increment < 0 && current > bound) {
        distance = current - bound;
        step = uint64_t(-(increment + 1)) + 1;
    } else {
        return inference;
    }

    inference.matched = true;
    inference.bound = bound;
    inference.increment = increment;
    // 向上取整得到剩余迭代次数，再限制到最大 lane 数。
    inference.remaining = (distance + step - 1) / step;
    inference.lanes = std::min<uint64_t>(inference.remaining, max_lanes);
    return inference;
}

void
DVRLoopBoundDetector::reset()
{
    triggerPC = 0;
    finalLoadPC = 0;
    loopBranchPC = 0;
    loopTargetPC = 0;
    boundSource0 = -1;
    boundSource1 = -1;
    seenBranch = false;
}

SST::SST(CPU *_cpu, const BaseO3CPUParams &params)
    : cpu(_cpu),
      numEntries(params.numSSTEntries)
{
    assert(numEntries > 0);

    // L always has exactly numEntries elements.
    for (unsigned i = 0; i < numEntries; i++) {
        L.push_back(0);
    }
}

void
SST::addInst(Addr addr)
{
    AddrMapIter i = M.find(addr);

    // If x already exists, move it to the MRU position.
    if (i != M.end()) {
        L.splice(L.begin(), L, i->second);
    }

    // If x doesn't exists, insert it at the MRU position.
    else {
        // If SST is full, evict the LRU element first.
        if (M.size() == numEntries) {
            M.erase(L.back());
        }
        L.back() = addr;
        L.splice(L.begin(), L, --L.end());
        M[addr] = L.begin();
    }
}

void
SST::addInst(const DynInstPtr &inst)
{
    addInst(inst->pcState().instAddr());
}

bool
SST::hasInst(const DynInstPtr &inst)
{
    Addr addr = inst->pcState().instAddr();
    AddrMapIter i = M.find(addr);

    // If x exists, move it to the MRU position.
    if (i != M.end()) {
        L.splice(L.begin(), L, i->second);
        return true;
    }

    return false;
}

MispTable::MispTable()
{
    for (int i = 0; i < table.size(); i++) {
        for (int j = 0; j < table[i].size(); j++) {
            table[i][j].lru = j;
        }
    }
}

void
MispTable::add(const DynInstPtr &inst)
{
    Addr pc = inst->pcState().instAddr();
    bool misp = inst->mispredicted();
    int rowIdx = (pc >> 1) & (table.size() - 1);
    auto &row = table[rowIdx];

    for (int i = 0; i < row.size(); i++) {
        // hit
        if (row[i].pc == pc) {
            row[i].ref++;
            if (misp)
                row[i].misp++;
            if (row[i].ref == MAX_REF) {
                row[i].ref >>= 1;
                row[i].misp >>= 1;
            }
            int lru = row[i].lru;
            for (int j = 0; j < row.size(); j++) {
                if (j != i && row[j].lru < lru)
                    row[j].lru++;
            }
            row[i].lru = 0;
            return;
        }
    }

    // not hit
    for (int i = 0; i < row.size(); i++) {
        if (row[i].lru == row.size() - 1) {
            row[i].pc = pc;
            row[i].lru = 0;
            row[i].ref = 1;
            row[i].misp = misp;
            for (int j = 0; j < row.size(); j++) {
                if (j != i)
                    row[j].lru++;
            }
        }
    }
}

bool
MispTable::high(const DynInstPtr &inst)
{
    Addr pc = inst->pcState().instAddr();
    int rowIdx = (pc >> 1) & (table.size() - 1);
    auto &row = table[rowIdx];
    for (int i = 0; i < row.size(); i++) {
        if (row[i].pc == pc) {
            return HIGH(row[i].ref, row[i].misp);
        }
    }
    return false;
}

} // namespace o3
} // namespace gem5
