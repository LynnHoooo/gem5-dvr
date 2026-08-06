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

std::optional<DVRStrideDetector::Candidate>
DVRStrideDetector::observeDispatch(Addr pc, bool discovery_active,
                                   Addr trigger_pc, InstSeqNum sequence)
{
    for (unsigned index = 0; index < entries.size(); ++index) {
        auto &entry = entries[index];
        if (entry.valid && entry.pc == pc && entry.stride != 0 &&
            entry.confidence >= 2) {
            const bool repeated = discovery_active && entry.discoverySeen;
            if (discovery_active && !entry.discoverySeen) {
                discoverySeenWrites.push_back({sequence, index});
                entry.discoverySeen = true;
            }
            return Candidate{pc, static_cast<Addr>(
                static_cast<int64_t>(entry.lastAddress) + entry.stride),
                entry.stride, repeated && pc != trigger_pc};
        }
    }
    return std::nullopt;
}

void
DVRStrideDetector::beginDiscovery(Addr trigger_pc)
{
    // This models the paper's one-bit register: every RPT entry starts clear
    // for a new Discovery generation, then the trigger entry is marked as
    // soon as Discovery opens.  A second observation of another striding PC
    // therefore identifies a more-inner candidate.
    for (auto &entry : entries)
        entry.discoverySeen = false;
    discoverySeenWrites.clear();
    for (auto &entry : entries) {
        if (entry.valid && entry.pc == trigger_pc) {
            entry.discoverySeen = true;
            break;
        }
    }
}

void
DVRStrideDetector::squashDiscovery(InstSeqNum squash_sequence)
{
    // Dispatch order follows sequence number. Reverse undo retains an
    // earlier observation of the same RPT entry on the surviving path.
    while (!discoverySeenWrites.empty() &&
           discoverySeenWrites.back().sequence >= squash_sequence) {
        entries[discoverySeenWrites.back().entry].discoverySeen = false;
        discoverySeenWrites.pop_back();
    }
}

void
DVRStrideDetector::endDiscovery()
{
    for (auto &entry : entries)
        entry.discoverySeen = false;
    discoverySeenWrites.clear();
}

void
DVRStrideDetector::reset()
{
    // 清空所有训练状态。
    for (auto &entry : entries)
        entry = Entry{};
    discoverySeenWrites.clear();
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
    // Discovery starts speculatively at dispatch; commit observes its end.
    if (state != State::Idle)
        return;

    state = State::Discovering;
    triggerPC = candidate.pc;
    triggerStride = candidate.stride;
    triggerSequence = sequence;
    stopSequence = 0;
    instructions = 0;
}

void
DVRDiscoveryController::restart(
    const DVRStrideDetector::Candidate &candidate, InstSeqNum sequence)
{
    finish();
    arm(candidate, sequence);
}

bool
DVRDiscoveryController::observeDispatch(Addr pc, InstSeqNum sequence)
{
    if (state != State::Discovering || sequence <= triggerSequence ||
        stopSequence != 0)
        return false;
    if (pc == triggerPC) {
        stopSequence = sequence;
        return true;
    }
    return false;
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

    if (stopSequence != 0 && sequence == stopSequence) {
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

bool
DVRDiscoveryController::rollback(InstSeqNum squash_sequence)
{
    if (state == State::Idle || triggerSequence < squash_sequence)
        return false;
    finish();
    return true;
}

void
DVRDiscoveryController::finish()
{
    state = State::Idle;
    triggerPC = 0;
    triggerStride = 0;
    triggerSequence = 0;
    stopSequence = 0;
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

DVRVectorTaintTracker::Observation
DVRVectorTaintTracker::observeSpeculative(const DynInstPtr &inst,
                                           InstSeqNum sequence)
{
    speculativeHistory.push_back({sequence, taint, finalLoadPC});
    return observe(inst);
}

void
DVRVectorTaintTracker::squash(InstSeqNum squash_sequence)
{
    // Dispatch is in sequence order.  Restoring the state immediately before
    // the first squashed instruction removes all wrong-path taint and FLR.
    while (!speculativeHistory.empty() &&
           speculativeHistory.back().sequence >= squash_sequence) {
        taint = speculativeHistory.back().taint;
        finalLoadPC = speculativeHistory.back().finalLoadPC;
        speculativeHistory.pop_back();
    }
}

DVRVectorTaintTracker::Observation
DVRVectorTaintTracker::classify(const DynInstPtr &inst) const
{
    bool source_tainted = false;
    for (int idx = 0; idx < inst->numSrcRegs(); ++idx) {
        if (isTainted(inst->srcRegIdx(idx))) {
            source_tainted = true;
            break;
        }
    }
    return {source_tainted, inst->isLoad() && source_tainted};
}

void
DVRVectorTaintTracker::reset()
{
    taint = 0;
    finalLoadPC = 0;
    speculativeHistory.clear();
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

        // C.LI rd, imm -- a zero-source immediate is represented as x0.
        if (quadrant == 1 && funct3 == 2 &&
            ((compressed >> 7) & 0x1f) != 0) {
            uop.semantic = Semantic::AddImmediate;
            uop.source0 = 0;
            uop.immediate = dvrSignExtend(
                (((compressed >> 12) & 0x1) << 5) |
                ((compressed >> 2) & 0x1f), 6);
            return;
        }

        // C.ADDI rd, imm -- the normal source/destination metadata is
        // retained, only the compressed immediate needs decoding.
        if (quadrant == 1 && funct3 == 0 &&
            ((compressed >> 7) & 0x1f) != 0) {
            uop.semantic = Semantic::AddImmediate;
            uop.immediate = dvrSignExtend(
                (((compressed >> 12) & 0x1) << 5) |
                ((compressed >> 2) & 0x1f), 6);
            return;
        }

        // C.ADDIW rd, imm -- RV64 sign-extended word add.
        if (quadrant == 1 && funct3 == 1 &&
            ((compressed >> 7) & 0x1f) != 0) {
            uop.semantic = Semantic::AddWordImmediate;
            uop.immediate = dvrSignExtend(
                (((compressed >> 12) & 0x1) << 5) |
                ((compressed >> 2) & 0x1f), 6);
            return;
        }

        // C.LUI rd, imm -- model the upper immediate as x0 + imm.
        if (quadrant == 1 && funct3 == 3 &&
            ((compressed >> 7) & 0x1f) != 0 &&
            ((compressed >> 7) & 0x1f) != 2) {
            uop.semantic = Semantic::AddImmediate;
            uop.source0 = 0;
            uop.immediate = dvrSignExtend(
                (((compressed >> 12) & 0x1) << 5) |
                ((compressed >> 2) & 0x1f), 6) << 12;
            return;
        }

        // C.LD rd', uimm(rs1') -- RV64C quadrant 0, funct3 011.
        if (quadrant == 0 && funct3 == 3) {
            uop.semantic = Semantic::LoadDouble;
            uop.loadBytes = 8;
            uop.immediate =
                ((compressed >> 10) & 0x7) << 3 |
                ((compressed >> 5) & 0x3) << 6;
            return;
        }

        // C.LW rd', uimm(rs1') -- RV64C quadrant 0, funct3 010.
        // The helper only needs the effective address; the loaded word is
        // supplied by the source-response path just like other loads.
        if (quadrant == 0 && funct3 == 2) {
            uop.semantic = Semantic::LoadWordSigned;
            uop.loadBytes = 4;
            uop.immediate =
                ((compressed >> 10) & 0x7) << 3 |
                ((compressed >> 6) & 0x1) << 2;
            return;
        }

        // RV64C quadrant 1, funct3 100: compressed shifts, ANDI, and
        // register logical operations.  These are common in compiler
        // generated address/control paths and must not become a semantic
        // fallback merely because they use a 16-bit encoding.
        if (quadrant == 1 && funct3 == 4) {
            const bool wide_register_op = ((compressed >> 12) & 0x1) != 0;
            const unsigned op_select = (compressed >> 10) & 0x3;
            if (op_select == 0) {
                uop.semantic = Semantic::ShiftRightLogicalImmediate;
                uop.immediate = ((compressed >> 12) & 0x1) << 5 |
                    ((compressed >> 2) & 0x1f);
                return;
            }
            if (op_select == 1) {
                uop.semantic = Semantic::ShiftRightArithmeticImmediate;
                uop.immediate = ((compressed >> 12) & 0x1) << 5 |
                    ((compressed >> 2) & 0x1f);
                return;
            }
            if (op_select == 2) {
                uop.semantic = Semantic::AndImmediate;
                uop.immediate = dvrSignExtend(
                    (((compressed >> 12) & 0x1) << 5) |
                    ((compressed >> 2) & 0x1f), 6);
                return;
            }
            if (!wide_register_op && op_select == 3) {
                switch ((compressed >> 5) & 0x3) {
                  case 0: uop.semantic = Semantic::Sub; return;
                  case 1: uop.semantic = Semantic::Xor; return;
                  case 2: uop.semantic = Semantic::Or; return;
                  case 3: uop.semantic = Semantic::And; return;
                }
            }
            if (wide_register_op && op_select == 3) {
                switch ((compressed >> 5) & 0x3) {
                  case 0: uop.semantic = Semantic::SubWord; return;
                  case 1: uop.semantic = Semantic::AddWord; return;
                }
            }
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

        // C.BEQZ/C.BNEZ rs1', offset -- quadrant 1.  The second
        // comparison operand is architectural x0 and is represented by the
        // absent source1 register, which the VIR evaluator treats as zero.
        if (quadrant == 1 && (funct3 == 6 || funct3 == 7)) {
            uop.semantic = funct3 == 6 ? Semantic::BranchEqual :
                                         Semantic::BranchNotEqual;
            return;
        }
        if (quadrant == 1 && funct3 == 5) {
            // C.J is a direct transfer.  C.JR/C.JALR remain excluded
            // because their target is data-dependent.
            uop.semantic = Semantic::AddImmediate;
            uop.source0 = 0;
            uop.immediate = 0;
            return;
        }
        return;
    }

    const uint32_t opcode = uop.encoding & 0x7f;
    const uint32_t funct3 = (uop.encoding >> 12) & 0x7;
    const uint32_t funct7 = (uop.encoding >> 25) & 0x7f;

    if (opcode == 0x37) { // LUI
        uop.semantic = Semantic::AddImmediate;
        uop.source0 = 0;
        uop.immediate = dvrSignExtend(uop.encoding & 0xfffff000, 32);
        return;
    }
    if (opcode == 0x17) { // AUIPC
        uop.semantic = Semantic::AddImmediate;
        uop.source0 = 0;
        uop.immediate = static_cast<int64_t>(uop.pc) +
            dvrSignExtend(uop.encoding & 0xfffff000, 32);
        return;
    }
    if (opcode == 0x6f) { // JAL; redirect is handled by lane execution.
        uop.semantic = Semantic::AddImmediate;
        uop.source0 = 0;
        uop.immediate = static_cast<int64_t>(uop.pc + 4);
        return;
    }

    if (opcode == 0x33) {
        if (funct3 == 0 && funct7 == 0) uop.semantic = Semantic::Add;
        else if (funct3 == 0 && funct7 == 0x20) uop.semantic = Semantic::Sub;
        else if (funct3 == 7 && funct7 == 0) uop.semantic = Semantic::And;
        else if (funct3 == 6 && funct7 == 0) uop.semantic = Semantic::Or;
        else if (funct3 == 4 && funct7 == 0) uop.semantic = Semantic::Xor;
        else if (funct3 == 1 && funct7 == 0) uop.semantic = Semantic::ShiftLeft;
        else if (funct3 == 5 && funct7 == 0) uop.semantic = Semantic::ShiftRightLogical;
        else if (funct3 == 5 && funct7 == 0x20) uop.semantic = Semantic::ShiftRightArithmetic;
        else if (funct3 == 0 && funct7 == 1) uop.semantic = Semantic::Multiply;
        return;
    }

    if (opcode == 0x3b) {
        if (funct3 == 0 && funct7 == 0)
            uop.semantic = Semantic::AddWord;
        else if (funct3 == 0 && funct7 == 0x20)
            uop.semantic = Semantic::SubWord;
        else if (funct3 == 0 && funct7 == 1)
            uop.semantic = Semantic::MultiplyWord;
        return;
    }

    if (opcode == 0x13) {
        if (funct3 == 0) {
            uop.semantic = Semantic::AddImmediate;
            uop.immediate = dvrSignExtend(uop.encoding >> 20, 12);
        } else if (funct3 == 1 && (uop.encoding >> 26) == 0) {
            uop.semantic = Semantic::ShiftLeftImmediate;
            uop.immediate = (uop.encoding >> 20) & 0x3f;
        } else if (funct3 == 7) {
            uop.semantic = Semantic::AndImmediate;
            uop.immediate = dvrSignExtend(uop.encoding >> 20, 12);
        } else if (funct3 == 6) {
            uop.semantic = Semantic::OrImmediate;
            uop.immediate = dvrSignExtend(uop.encoding >> 20, 12);
        } else if (funct3 == 4) {
            uop.semantic = Semantic::XorImmediate;
            uop.immediate = dvrSignExtend(uop.encoding >> 20, 12);
        } else if (funct3 == 5 && ((uop.encoding >> 26) == 0)) {
            uop.semantic = Semantic::ShiftRightLogicalImmediate;
            uop.immediate = (uop.encoding >> 20) & 0x3f;
        } else if (funct3 == 5 && ((uop.encoding >> 26) == 0x10)) {
            uop.semantic = Semantic::ShiftRightArithmeticImmediate;
            uop.immediate = (uop.encoding >> 20) & 0x3f;
        }
        return;
    }

    if (opcode == 0x1b) {
        if (funct3 == 0) {
            uop.semantic = Semantic::AddWordImmediate;
            uop.immediate = dvrSignExtend(uop.encoding >> 20, 12);
        } else if (funct3 == 1 && (uop.encoding >> 25) == 0) {
            uop.semantic = Semantic::ShiftLeftWordImmediate;
            uop.immediate = (uop.encoding >> 20) & 0x1f;
        } else if (funct3 == 5 && (uop.encoding >> 25) == 0) {
            uop.semantic = Semantic::ShiftRightLogicalWordImmediate;
            uop.immediate = (uop.encoding >> 20) & 0x1f;
        } else if (funct3 == 5 && (uop.encoding >> 25) == 0x20) {
            uop.semantic = Semantic::ShiftRightArithmeticWordImmediate;
            uop.immediate = (uop.encoding >> 20) & 0x1f;
        }
        return;
    }

    if (opcode == 0x03) {
        switch (funct3) {
          case 0:
            uop.semantic = Semantic::LoadByteSigned;
            uop.loadBytes = 1;
            break;
          case 1:
            uop.semantic = Semantic::LoadHalfSigned;
            uop.loadBytes = 2;
            break;
          case 2:
            uop.semantic = Semantic::LoadWordSigned;
            uop.loadBytes = 4;
            break;
          case 3:
            uop.semantic = Semantic::LoadDouble;
            uop.loadBytes = 8;
            break;
          case 4:
            uop.semantic = Semantic::LoadByteUnsigned;
            uop.loadBytes = 1;
            break;
          case 5:
            uop.semantic = Semantic::LoadHalfUnsigned;
            uop.loadBytes = 2;
            break;
          default: uop.semantic = Semantic::Unsupported; return;
        }
        uop.immediate = dvrSignExtend(uop.encoding >> 20, 12);
        return;
    }

    // Preserve the actual RV64 branch comparison for value-driven VIR
    // continuation; branchTaken only describes the path observed during
    // discovery and must not be used as the predicate itself.
    if (opcode == 0x63) {
        switch (funct3) {
          case 0:
            uop.semantic = Semantic::BranchEqual;
            break;
          case 1:
            uop.semantic = Semantic::BranchNotEqual;
            break;
          case 4:
            uop.semantic = Semantic::BranchSignedLess;
            break;
          case 5:
            uop.semantic = Semantic::BranchSignedGreaterEqual;
            break;
          case 6:
            uop.semantic = Semantic::BranchUnsignedLess;
            break;
          case 7:
            uop.semantic = Semantic::BranchUnsignedGreaterEqual;
            break;
          default:
            break;
        }
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
      case Semantic::And:
        result = source0_value & source1_value;
        return true;
      case Semantic::Or:
        result = source0_value | source1_value;
        return true;
      case Semantic::Xor:
        result = source0_value ^ source1_value;
        return true;
      case Semantic::ShiftLeft:
        result = source0_value << (source1_value & 0x3f);
        return true;
      case Semantic::ShiftRightLogical:
        result = source0_value >> (source1_value & 0x3f);
        return true;
      case Semantic::ShiftRightArithmetic:
        result = static_cast<RegVal>(static_cast<int64_t>(source0_value) >>
            (source1_value & 0x3f));
        return true;
      case Semantic::Multiply:
        result = source0_value * source1_value;
        return true;
      case Semantic::MultiplyWord:
        result = static_cast<RegVal>(static_cast<int64_t>(static_cast<int32_t>(
            static_cast<uint32_t>(source0_value) *
            static_cast<uint32_t>(source1_value))));
        return true;
      case Semantic::AddWord:
        result = static_cast<RegVal>(static_cast<int64_t>(static_cast<int32_t>(
            static_cast<uint32_t>(source0_value) +
            static_cast<uint32_t>(source1_value))));
        return true;
      case Semantic::SubWord:
        result = static_cast<RegVal>(static_cast<int64_t>(static_cast<int32_t>(
            static_cast<uint32_t>(source0_value) -
            static_cast<uint32_t>(source1_value))));
        return true;
      case Semantic::AddImmediate:
      case Semantic::LoadAddress:
      case Semantic::LoadByteSigned:
      case Semantic::LoadByteUnsigned:
      case Semantic::LoadHalfSigned:
      case Semantic::LoadHalfUnsigned:
      case Semantic::LoadWordSigned:
      case Semantic::LoadWordUnsigned:
      case Semantic::LoadDouble:
        result = source0_value + static_cast<RegVal>(immediate);
        return true;
      case Semantic::AddWordImmediate:
        result = static_cast<RegVal>(static_cast<int64_t>(static_cast<int32_t>(
            static_cast<uint32_t>(source0_value) +
            static_cast<uint32_t>(immediate))));
        return true;
      case Semantic::ShiftLeftImmediate:
        result = source0_value << (static_cast<unsigned>(immediate) & 0x3f);
        return true;
      case Semantic::ShiftLeftWordImmediate:
        result = static_cast<RegVal>(static_cast<int64_t>(static_cast<int32_t>(
            static_cast<uint32_t>(source0_value) <<
            (static_cast<unsigned>(immediate) & 0x1f))));
        return true;
      case Semantic::AndImmediate:
        result = source0_value & static_cast<RegVal>(immediate);
        return true;
      case Semantic::OrImmediate:
        result = source0_value | static_cast<RegVal>(immediate);
        return true;
      case Semantic::XorImmediate:
        result = source0_value ^ static_cast<RegVal>(immediate);
        return true;
      case Semantic::ShiftRightLogicalImmediate:
        result = source0_value >> (static_cast<unsigned>(immediate) & 0x3f);
        return true;
      case Semantic::ShiftRightArithmeticImmediate:
        result = static_cast<RegVal>(static_cast<int64_t>(source0_value) >>
            (static_cast<unsigned>(immediate) & 0x3f));
        return true;
      case Semantic::ShiftRightLogicalWordImmediate:
        result = static_cast<RegVal>(static_cast<int64_t>(static_cast<int32_t>(
            static_cast<uint32_t>(source0_value) >>
            (static_cast<unsigned>(immediate) & 0x1f))));
        return true;
      case Semantic::ShiftRightArithmeticWordImmediate:
        result = static_cast<RegVal>(static_cast<int64_t>(static_cast<int32_t>(
            static_cast<int32_t>(source0_value) >>
            (static_cast<unsigned>(immediate) & 0x1f))));
        return true;
      case Semantic::BranchEqual:
      case Semantic::BranchNotEqual:
      case Semantic::BranchSignedLess:
      case Semantic::BranchSignedGreaterEqual:
      case Semantic::BranchUnsignedLess:
      case Semantic::BranchUnsignedGreaterEqual:
      case Semantic::Unsupported:
        return false;
    }
    return false;
}

bool
DVRInstructionRecorder::Uop::evaluateBranch(
    RegVal source0_value, RegVal source1_value, bool &taken) const
{
    switch (semantic) {
      case Semantic::BranchEqual:
        taken = source0_value == source1_value;
        return true;
      case Semantic::BranchNotEqual:
        taken = source0_value != source1_value;
        return true;
      case Semantic::BranchSignedLess:
        taken = static_cast<int64_t>(source0_value) <
            static_cast<int64_t>(source1_value);
        return true;
      case Semantic::BranchSignedGreaterEqual:
        taken = static_cast<int64_t>(source0_value) >=
            static_cast<int64_t>(source1_value);
        return true;
      case Semantic::BranchUnsignedLess:
        taken = source0_value < source1_value;
        return true;
      case Semantic::BranchUnsignedGreaterEqual:
        taken = source0_value >= source1_value;
        return true;
      default:
        return false;
    }
}

void
DVRInstructionRecorder::begin(const DynInstPtr &trigger)
{
    reset();
    record(trigger);
}

bool
DVRInstructionRecorder::record(const DynInstPtr &inst, bool tainted)
{
    // The first instruction observed after a conditional branch is the
    // committed reconvergence boundary for this captured path.  Preserve it
    // in the branch uop instead of making VIR infer a boundary from the next
    // array slot at execution time.
    if (count != 0 && uops[count - 1].conditional &&
        uops[count - 1].reconvergencePC == 0)
        uops[count - 1].reconvergencePC = inst->pcState().instAddr();

    // 记录有限数量的 uop；超出容量后禁止使用该模板。
    if (count >= MaxUops) {
        overflowed = true;
        return false;
    }

    Uop &uop = uops[count++];
    uop.pc = inst->pcState().instAddr();
    uop.staticInst = inst->staticInst;
    if (inst->isDirectCtrl())
        uop.branchTargetPC = inst->branchTarget()->instAddr();
    uop.intSources = dvrIntRegisterMask(inst, true);
    uop.intDestinations = dvrIntRegisterMask(inst, false);
    uop.source0 = dvrFirstIntRegister(inst, true, 0);
    uop.source1 = dvrFirstIntRegister(inst, true, 1);
    uop.destination = dvrFirstIntRegister(inst, false, 0);
    uop.load = inst->isLoad();
    uop.control = inst->isControl();
    uop.conditional = inst->isCondCtrl();
    uop.branchTaken = inst->pcState().branching();
    uop.tainted = tainted;
    dvrDecodeRiscvSemantic(uop, inst);
    // RISC-V compressed control-flow instructions advance by two bytes.
    // Using a fixed four-byte fall-through silently sends C.BEQZ/C.BNEZ to
    // the wrong PC and makes a valid alternate path look external.
    uop.fallthroughPC = uop.pc +
        (uop.encodingValid && (uop.encoding & 0x3) != 0x3 ? 2 : 4);
    return true;
}

void
DVRInstructionRecorder::import(const std::array<Uop, MaxUops> &source,
                                unsigned size)
{
    reset();
    count = std::min(size, MaxUops);
    for (unsigned index = 0; index < count; ++index)
        uops[index] = source[index];
}

bool
DVRInstructionRecorder::insertBeforePC(
    Addr reconvergence_pc, const std::vector<Uop> &path)
{
    if (reconvergence_pc == 0 || path.empty() || count >= MaxUops ||
        path.size() > MaxUops - count)
        return false;

    bool has_reconvergence = false;
    for (unsigned index = 0; index < count; ++index) {
        if (uops[index].pc == reconvergence_pc) {
            has_reconvergence = true;
            break;
        }
    }
    if (!has_reconvergence)
        return false;
    // PC, not physical array position, selects a helper successor.  Keeping
    // the discovered path contiguous means its final sequential uop still
    // reaches reconvergence; an alternate suffix appended here is entered
    // only when a decoded branch actually selects its target PC.
    for (unsigned index = 0; index < path.size(); ++index)
        uops[count + index] = path[index];
    count += path.size();
    return true;
}

void
DVRInstructionRecorder::setReconvergencePC(Addr pc)
{
    if (pc == 0)
        return;
    for (unsigned index = 0; index < count; ++index) {
        if (uops[index].conditional)
            uops[index].reconvergencePC = pc;
    }
}

void
DVRInstructionRecorder::reset()
{
    uops = {};
    count = 0;
    overflowed = false;
}

DVRInstructionRecorder::ResourceCounts
DVRInstructionRecorder::resourceCounts() const
{
    ResourceCounts resources;
    for (unsigned index = 0; index < count; ++index) {
        const auto &uop = uops[index];
        if (uop.load || uop.semantic == Uop::Semantic::LoadAddress) {
            ++resources.lsu;
            continue;
        }
        if (uop.semantic == Uop::Semantic::Multiply ||
            uop.semantic == Uop::Semantic::MultiplyWord) {
            ++resources.multiply;
            continue;
        }
        switch (uop.semantic) {
          case Uop::Semantic::ShiftLeft:
          case Uop::Semantic::ShiftRightLogical:
          case Uop::Semantic::ShiftRightArithmetic:
          case Uop::Semantic::ShiftLeftImmediate:
          case Uop::Semantic::ShiftLeftWordImmediate:
          case Uop::Semantic::ShiftRightLogicalImmediate:
          case Uop::Semantic::ShiftRightArithmeticImmediate:
          case Uop::Semantic::ShiftRightLogicalWordImmediate:
          case Uop::Semantic::ShiftRightArithmeticWordImmediate:
            ++resources.shift;
            break;
          default:
            ++resources.alu;
            break;
        }
    }
    return resources;
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
DVRVectorInstructionRegister::executeLanePC(
    const DVRInstructionRecorder &program, unsigned lanes,
    unsigned max_helper_uops,
    const std::array<RegVal, 32> &initial_regs, unsigned start_index,
    int source_destination, RegVal source_value)
{
    reset();
    Result result;
    lanes = std::min(lanes, 128U);
    if (program.size() == 0 || start_index >= program.size() ||
        lanes == 0 || max_helper_uops == 0)
        return result;

    std::array<bool, 128> lane_active = {};
    std::array<unsigned, 128> lane_window_start = {};
    std::array<unsigned, 128> lane_window_end = {};
    const Addr entry = program[start_index].pc;
    for (unsigned lane = 0; lane < lanes; ++lane) {
        lane_active[lane] = true;
        lanePC[lane] = entry;
        lane_window_start[lane] = start_index;
        lane_window_end[lane] = std::min(
            start_index + DVRInstructionRecorder::FrontEndBufferUops,
            program.size());
        activeMask[lane / 64] |= uint64_t(1) << (lane % 64);
        for (unsigned reg = 0;
             reg < DVRVectorRenameTable::NumArchitecturalRegs; ++reg)
            vectorRegs[reg][lane] = initial_regs[reg];
        if (source_destination >= 0 &&
            source_destination < DVRVectorRenameTable::NumArchitecturalRegs)
            vectorRegs[source_destination][lane] = source_value;
    }
    result.activeLanes = lanes;

    const unsigned chunks = (lanes + 15) / 16;
    for (unsigned step = 0; step < max_helper_uops; ++step) {
        // The stack is a SIMT scheduler, not just a counter.  A deferred
        // path is held out of issue until the selected path reaches the
        // common DVR termination point.  At that point restore its mask and
        // continue at the saved target PC, as in the paper's Figure 6.
        bool resumed_path = false;
        if (stackDepth != 0) {
            const auto &top = stack[stackDepth - 1];
            bool at_reconvergence = false;
            bool selected_path_alive = false;
            for (unsigned lane = 0; lane < lanes; ++lane) {
                if (!lane_active[lane] || laneBlocked[lane])
                    continue;
                selected_path_alive = true;
                at_reconvergence |= lanePC[lane] == top.pc;
            }
            if (at_reconvergence || !selected_path_alive) {
                const auto deferred = top.deferredMask;
                const Addr deferred_pc = top.deferredPC;
                bool deferred_alternate = false;
                for (unsigned candidate = 0; candidate < program.size();
                     ++candidate) {
                    if (program[candidate].pc == deferred_pc) {
                        deferred_alternate = program[candidate].alternatePath;
                        break;
                    }
                }
                for (unsigned lane = 0; lane < lanes; ++lane) {
                    if (deferred[lane / 64] &
                        (uint64_t(1) << (lane % 64))) {
                        laneBlocked[lane] = false;
                        lanePC[lane] = deferred_pc;
                    }
                }
                --stackDepth;
                if (at_reconvergence)
                    ++result.reconvergences;
                if (at_reconvergence &&
                    (deferred_alternate || top.alternatePath))
                    ++result.alternatePathReconvergences;
                resumed_path = true;
            }
        }
        int op_index = -1;
        Addr group_pc = 0;
        if (resumed_path && stackDepth < ReconvergenceEntries) {
            // Prefer the path just restored so it reaches the common PC
            // before the already-finished path executes past it.
            for (unsigned lane = 0; lane < lanes; ++lane) {
                if (lane_active[lane] && !laneBlocked[lane] &&
                    lanePC[lane] != 0) {
                    group_pc = lanePC[lane];
                    break;
                }
            }
        }
        for (unsigned lane = 0; lane < lanes; ++lane) {
            if (!lane_active[lane] || laneBlocked[lane])
                continue;
            if (resumed_path && group_pc != 0 && lanePC[lane] != group_pc)
                continue;
            for (unsigned candidate = lane_window_start[lane];
                 candidate < lane_window_end[lane]; ++candidate) {
                if (program[candidate].pc == lanePC[lane]) {
                    op_index = candidate;
                    group_pc = lanePC[lane];
                    break;
                }
            }
            // Refill only this lane's front-end window.  VIR state (registers,
            // lane PC, and reconvergence metadata) is deliberately retained.
            if (op_index < 0) {
                for (unsigned candidate = 0; candidate < program.size();
                     ++candidate) {
                    if (program[candidate].pc != lanePC[lane])
                        continue;
                    lane_window_start[lane] = candidate;
                    lane_window_end[lane] = std::min(
                        candidate + DVRInstructionRecorder::FrontEndBufferUops,
                        program.size());
                    op_index = candidate;
                    group_pc = lanePC[lane];
                    break;
                }
            }
            if (op_index >= 0)
                break;
            if (lanePC[lane] != 0) {
                result.unsupportedControlFlow = true;
                ++result.externalPathLanes;
            }
            lane_active[lane] = false;
            activeMask[lane / 64] &= ~(uint64_t(1) << (lane % 64));
        }
        if (op_index < 0)
            break;

        const auto &op = program[op_index];
        if (op.alternatePath)
            ++result.alternatePathUops;

        std::array<bool, 128> executing = {};
        std::array<bool, 128> taken_lanes = {};
        bool has_taken = false;
        bool has_fallthrough = false;
        bool first_path_taken = false;
        bool first_path_set = false;
        for (unsigned lane = 0; lane < lanes; ++lane) {
            if (!lane_active[lane] || lanePC[lane] != group_pc)
                continue;
            executing[lane] = true;
            const RegVal lhs = op.source0 >= 0 ?
                vectorRegs[op.source0][lane] : 0;
            const RegVal rhs = op.source1 >= 0 ?
                vectorRegs[op.source1][lane] : 0;
            if (op.conditional) {
                // The initial vector pass is a control-flow audit and runs
                // before asynchronous source values exist.  Preserve its
                // conservative discovered-path behavior.  A response-driven
                // continuation, in contrast, has a real source value and
                // must evaluate the decoded architectural comparison.
                bool lane_taken = op.branchTaken;
                if (op.tainted && start_index != 0 &&
                    !op.evaluateBranch(lhs, rhs, lane_taken)) {
                    result.unsupportedControlFlow = true;
                    ++result.externalPathLanes;
                }
                const Addr next = lane_taken ? op.branchTargetPC :
                    (op.fallthroughPC != 0 ? op.fallthroughPC :
                     (op_index + 1 < program.size() ?
                      program[op_index + 1].pc : 0));
                lanePC[lane] = next;
                if (next == 0)
                    ++result.earlyExitLanes;
                taken_lanes[lane] = lane_taken;
                if (!first_path_set) {
                    first_path_taken = lane_taken;
                    first_path_set = true;
                }
                has_taken |= lane_taken;
                has_fallthrough |= !lane_taken;
                continue;
            }

            // A direct unconditional jump is control metadata, not an ALU
            // operation.  Follow its resolved target so an alternate path
            // can reach the shared reconvergence PC without being classified
            // as an unsupported semantic.
            if (op.control && !op.conditional && op.branchTargetPC != 0) {
                lanePC[lane] = op.branchTargetPC;
                if (lanePC[lane] == 0)
                    ++result.earlyExitLanes;
                else if (lanePC[lane] == program[0].pc)
                    ++result.normalTerminatedLanes;
                continue;
            }

            if (op.semantic == DVRInstructionRecorder::Uop::Semantic::Unsupported &&
                !op.control && op.intDestinations == 0) {
                lanePC[lane] = op_index + 1 < program.size() ?
                    program[op_index + 1].pc : 0;
                if (lanePC[lane] == 0)
                    ++result.normalTerminatedLanes;
                continue;
            }

            RegVal value = 0;
            if (!op.evaluate(lhs, rhs, value)) {
                result.unsupportedControlFlow = true;
                ++result.unsupportedSemanticLanes;
                if (result.unsupportedSemanticPC == 0) {
                    result.unsupportedSemanticPC = op.pc;
                    result.unsupportedSemanticEncoding = op.encoding;
                    result.unsupportedSemantic =
                        static_cast<uint8_t>(op.semantic);
                }
                lane_active[lane] = false;
                activeMask[lane / 64] &= ~(uint64_t(1) << (lane % 64));
                continue;
            }
            if (op.destination >= 0)
                vectorRegs[op.destination][lane] = value;
            lanePC[lane] = op_index + 1 < program.size() ?
                program[op_index + 1].pc : 0;
            if (lanePC[lane] == 0)
                ++result.normalTerminatedLanes;
        }

        if (has_taken && has_fallthrough) {
            ++result.divergentBranches;
            if (stackDepth >= ReconvergenceEntries) {
                result.stackOverflow = true;
                return result;
            }
            const Addr reconvergence = op.reconvergencePC != 0 ?
                op.reconvergencePC : op.fallthroughPC;
            std::array<uint64_t, 2> deferred = {};
            Addr deferred_pc = 0;
            bool alternate_path = false;
            for (unsigned lane = 0; lane < lanes; ++lane) {
                if (!executing[lane])
                    continue;
                const bool defer = taken_lanes[lane] != first_path_taken;
                if (defer) {
                    deferred[lane / 64] |= uint64_t(1) << (lane % 64);
                    deferred_pc = lanePC[lane];
                    laneBlocked[lane] = true;
                }
                for (unsigned candidate = 0; candidate < program.size();
                     ++candidate) {
                    if (program[candidate].pc == lanePC[lane] &&
                        program[candidate].alternatePath) {
                        alternate_path = true;
                        break;
                    }
                }
            }
            stack[stackDepth].deferredMask = deferred;
            stack[stackDepth].deferredPC = deferred_pc;
            stack[stackDepth].pc = reconvergence;
            stack[stackDepth].alternatePath = alternate_path;
            ++stackDepth;
        }

        for (unsigned chunk = 0; chunk < chunks; ++chunk) {
            const unsigned first = chunk * 16;
            const unsigned last = std::min(first + 16, lanes);
            unsigned active_in_chunk = 0;
            for (unsigned lane = first; lane < last; ++lane)
                active_in_chunk += executing[lane] && lane_active[lane];
            if (active_in_chunk == 0)
                continue;
            issuedChunks |= uint16_t(1) << chunk;
            executedChunks |= uint16_t(1) << chunk;
            ++result.chunkIssues;
            ++result.chunkExecutions;
        }
        ++result.helperUops;

        bool any_active = false;
        for (unsigned lane = 0; lane < lanes; ++lane)
            any_active |= lane_active[lane] && lanePC[lane] != 0;
        // A selected path can terminate before the reconvergence point.  The
        // next iteration will pop the deferred path even though no selected
        // lane remains issueable.
        if (!any_active)
            if (stackDepth == 0)
                break;
    }

    if (result.helperUops >= max_helper_uops) {
        bool any_active = false;
        for (unsigned lane = 0; lane < lanes; ++lane)
            any_active |= lane_active[lane] && lanePC[lane] != 0;
        result.timedOut = any_active;
    }
    return result;
}

DVRVectorInstructionRegister::Result
DVRVectorInstructionRegister::execute(
    const DVRInstructionRecorder &program, unsigned lanes,
    unsigned max_helper_uops,
    const std::array<RegVal, 32> &initial_regs)
{
    return executeLanePC(program, lanes, max_helper_uops, initial_regs);

    // Legacy mask-only implementation retained below as a reference while
    // the lane-PC executor is validated against existing VIR counters.
    // 建立 active mask 和私有向量寄存器文件，再按 uop/lane 实际执行。
    reset();
    Result result;
    lanes = std::min(lanes, 128U);
    if (program.size() == 0 || lanes == 0 || max_helper_uops == 0)
        return result;
    for (unsigned lane = 0; lane < lanes; ++lane) {
        activeMask[lane / 64] |= uint64_t(1) << (lane % 64);
        lanePC[lane] = program[0].pc;
        // lane ordinal is the only value known before asynchronous source
        // loads return; it provides a deterministic seed for address-only
        // instructions.  Real load values are consumed by the helper request
        // path and never become architectural state.
        for (unsigned reg = 0; reg < DVRVectorRenameTable::NumArchitecturalRegs;
             ++reg)
            vectorRegs[reg][lane] = lane;
    }

    const unsigned chunks = (lanes + 15) / 16;
    result.activeLanes = lanes;
    for (unsigned uop = 0; uop < program.size(); ++uop) {
        const auto &op = program[uop];

        if (stackDepth != 0 && stack[stackDepth - 1].pc == op.pc) {
            const auto deferred = stack[stackDepth - 1].deferredMask;
            activeMask[0] |= deferred[0];
            activeMask[1] |= deferred[1];
            --stackDepth;
            ++result.reconvergences;
        }

        // Execute a conditional uop as a SIMT control-flow operation.  Each
        // lane now records its selected target/fall-through PC; the deferred
        // mask remains available at the recorded reconvergence boundary.
        if (op.conditional) {
            std::array<uint64_t, 2> taken = {};
            std::array<uint64_t, 2> deferred = {};
            for (unsigned lane = 0; lane < lanes; ++lane) {
                const uint64_t bit = uint64_t(1) << (lane % 64);
                const auto word = lane / 64;
                if (!(activeMask[word] & bit) || lanePC[lane] != op.pc)
                    continue;
                const RegVal predicate = op.source0 >= 0 ?
                    vectorRegs[op.source0][lane] : 0;
                const bool value_taken = predicate != 0;
                const bool lane_taken = value_taken == op.branchTaken;
                lanePC[lane] = lane_taken ? op.branchTargetPC :
                    (op.reconvergencePC != 0 ? op.reconvergencePC :
                     op.fallthroughPC);
                (lane_taken ? taken : deferred)[word] |= bit;
            }

            const bool has_taken = taken[0] || taken[1];
            const bool has_deferred = deferred[0] || deferred[1];
            if (has_taken && has_deferred) {
                if (stackDepth >= ReconvergenceEntries) {
                    result.stackOverflow = true;
                    return result;
                }
                stack[stackDepth].deferredMask = deferred;
                stack[stackDepth].pc = op.reconvergencePC != 0 ?
                    op.reconvergencePC :
                    (uop + 1 < program.size() ? program[uop + 1].pc :
                                                 op.fallthroughPC);
                ++stackDepth;
                ++result.divergentBranches;
                activeMask = taken;
            } else if (has_deferred) {
                activeMask = deferred;
            } else {
                activeMask = taken;
            }
        }

        for (unsigned chunk = 0; chunk < chunks; ++chunk) {
            unsigned active_in_chunk = 0;
            const unsigned first = chunk * 16;
            const unsigned last = std::min(first + 16, lanes);
            for (unsigned lane = first; lane < last; ++lane) {
                if (!(activeMask[lane / 64] & (uint64_t(1) << (lane % 64))) ||
                    lanePC[lane] != op.pc)
                    continue;
                RegVal lhs = 0;
                RegVal rhs = 0;
                if (op.source0 >= 0)
                    lhs = vectorRegs[op.source0][lane];
                if (op.source1 >= 0)
                    rhs = vectorRegs[op.source1][lane];
                RegVal value = 0;
                if (!op.evaluate(lhs, rhs, value))
                    continue;
                if (op.destination >= 0)
                    vectorRegs[op.destination][lane] = value;
                lanePC[lane] = uop + 1 < program.size() ?
                    program[uop + 1].pc : 0;
                ++active_in_chunk;
            }
            if (active_in_chunk == 0)
                continue;
            issuedChunks |= uint16_t(1) << chunk;
            executedChunks |= uint16_t(1) << chunk;
            ++result.chunkIssues;
            ++result.chunkExecutions;
            // The helper budget is expressed in issued vector micro-ops.
            // Charging once per scalar lane made every useful 128-lane
            // program exceed a 200-uop budget before its first replay.
            ++result.helperUops;
            if (result.helperUops >= max_helper_uops) {
                result.timedOut = uop + 1 < program.size() ||
                    chunk + 1 < chunks;
                return result;
            }
        }

    }
    return result;
}

DVRVectorInstructionRegister::Result
DVRVectorInstructionRegister::executeFromSource(
    const std::array<DVRInstructionRecorder::Uop,
                     DVRInstructionRecorder::MaxUops> &source,
    unsigned size, int source_destination, RegVal source_value,
    unsigned max_helper_uops,
    const std::array<RegVal, 32> &initial_regs)
{
    DVRInstructionRecorder program;
    program.import(source, size);
    // Entry zero is the source load whose value arrived asynchronously.
    // Start at the following captured uop so it cannot overwrite the value
    // with an address calculation a second time.
    return executeLanePC(program, 1, max_helper_uops, initial_regs, 1,
                         source_destination, source_value);
}

void
DVRVectorInstructionRegister::initializeSourceContinuation(
    const std::array<DVRInstructionRecorder::Uop,
                     DVRInstructionRecorder::MaxUops> &source,
    unsigned size, unsigned lanes,
    const std::array<RegVal, 32> &initial_regs,
    unsigned scalar_count, bool continue_past_flr)
{
    reset();
    continuationLanes = std::min(lanes, 128U);
    continuationStopIndex = scalar_count > 1 ? scalar_count - 1 : 0;
    continuationContinuePastFLR = continue_past_flr;
    if (size <= 1 || continuationLanes == 0)
        return;

    continuationInitialized = true;
    const Addr entry = source[1].pc;
    for (unsigned lane = 0; lane < continuationLanes; ++lane) {
        laneActive[lane] = true;
        laneReady[lane] = false;
        laneBlocked[lane] = false;
        lanePC[lane] = entry;
        laneWindowStart[lane] = 1;
        laneWindowEnd[lane] = std::min(
            1 + DVRInstructionRecorder::FrontEndBufferUops, size);
        activeMask[lane / 64] |= uint64_t(1) << (lane % 64);
        for (unsigned reg = 0;
             reg < DVRVectorRenameTable::NumArchitecturalRegs; ++reg)
            vectorRegs[reg][lane] = initial_regs[reg];
    }
}

DVRVectorInstructionRegister::Result
DVRVectorInstructionRegister::resumeSourceLane(
    const std::array<DVRInstructionRecorder::Uop,
                     DVRInstructionRecorder::MaxUops> &source,
    unsigned size, unsigned lane, int source_destination,
    RegVal source_value, unsigned max_helper_uops)
{
    Result result;
    if (!continuationInitialized || size <= 1 ||
        lane >= continuationLanes || !laneActive[lane] ||
        max_helper_uops == 0)
        return result;

    result.activeLanes = 1;
    if (source_destination >= 0 &&
        source_destination < DVRVectorRenameTable::NumArchitecturalRegs)
        vectorRegs[source_destination][lane] = source_value;

    for (unsigned step = 0; step < max_helper_uops; ++step) {
        int op_index = -1;
        for (unsigned candidate = laneWindowStart[lane];
             candidate < laneWindowEnd[lane]; ++candidate) {
            if (source[candidate].pc == lanePC[lane]) {
                op_index = candidate;
                break;
            }
        }
        if (op_index < 0) {
            for (unsigned candidate = 1; candidate < size; ++candidate) {
                if (source[candidate].pc != lanePC[lane])
                    continue;
                laneWindowStart[lane] = candidate;
                laneWindowEnd[lane] = std::min(
                    candidate + DVRInstructionRecorder::FrontEndBufferUops,
                    size);
                op_index = candidate;
                break;
            }
        }

        if (op_index < 0) {
            if (lanePC[lane] == source[0].pc) {
                ++result.normalTerminatedLanes;
                laneActive[lane] = false;
                activeMask[lane / 64] &=
                    ~(uint64_t(1) << (lane % 64));
                break;
            }
            if (lanePC[lane] != 0) {
                result.unsupportedControlFlow = true;
                ++result.externalPathLanes;
            }
            laneActive[lane] = false;
            activeMask[lane / 64] &= ~(uint64_t(1) << (lane % 64));
            break;
        }

        const auto &op = source[op_index];
        if (lanePendingReconvergence[lane][op_index]) {
            ++result.reconvergences;
            ++result.alternatePathReconvergences;
            lanePendingReconvergence[lane][op_index] = false;
            if (laneStackDepth[lane] != 0)
                --laneStackDepth[lane];
        }

        const RegVal lhs = op.source0 >= 0 ?
            vectorRegs[op.source0][lane] : 0;
        const RegVal rhs = op.source1 >= 0 ?
            vectorRegs[op.source1][lane] : 0;
        if (op.conditional) {
            bool taken = op.branchTaken;
            if (op.tainted && !op.evaluateBranch(lhs, rhs, taken)) {
                result.unsupportedControlFlow = true;
                ++result.externalPathLanes;
                taken = (lhs != 0) == op.branchTaken;
            }
            const Addr next = taken ? op.branchTargetPC :
                (op.fallthroughPC != 0 ? op.fallthroughPC :
                 (op_index + 1 < size ? source[op_index + 1].pc : 0));
            const Addr captured_target = op.branchTaken ?
                op.branchTargetPC : op.fallthroughPC;
            if (next != captured_target && op.reconvergencePC != 0) {
                for (unsigned candidate = 1; candidate < size; ++candidate) {
                    if (source[candidate].pc == op.reconvergencePC) {
                        lanePendingReconvergence[lane][candidate] = true;
                        break;
                    }
                }
            }
            lanePC[lane] = next;
            ++result.helperUops;
            ++result.chunkIssues;
            ++result.chunkExecutions;
            if (next == 0) {
                ++result.earlyExitLanes;
                laneActive[lane] = false;
                activeMask[lane / 64] &=
                    ~(uint64_t(1) << (lane % 64));
                break;
            }
            if (next == source[0].pc) {
                ++result.normalTerminatedLanes;
                laneActive[lane] = false;
                activeMask[lane / 64] &=
                    ~(uint64_t(1) << (lane % 64));
                break;
            }
            continue;
        }

        if (op.control && !op.conditional && op.branchTargetPC != 0) {
            const Addr captured_target = op.branchTaken ?
                op.branchTargetPC : op.fallthroughPC;
            if (op.branchTargetPC != captured_target &&
                op.reconvergencePC != 0) {
                for (unsigned candidate = 1; candidate < size; ++candidate) {
                    if (source[candidate].pc == op.reconvergencePC) {
                        lanePendingReconvergence[lane][candidate] = true;
                        break;
                    }
                }
            }
            lanePC[lane] = op.branchTargetPC;
            ++result.helperUops;
            ++result.chunkIssues;
            ++result.chunkExecutions;
            if (lanePC[lane] == 0) {
                ++result.earlyExitLanes;
                laneActive[lane] = false;
                activeMask[lane / 64] &=
                    ~(uint64_t(1) << (lane % 64));
                break;
            }
            if (lanePC[lane] == source[0].pc) {
                ++result.normalTerminatedLanes;
                laneActive[lane] = false;
                activeMask[lane / 64] &=
                    ~(uint64_t(1) << (lane % 64));
                break;
            }
            continue;
        }

        if (op.semantic == DVRInstructionRecorder::Uop::Semantic::Unsupported &&
            !op.control && op.intDestinations == 0) {
            lanePC[lane] = op_index + 1 < size ? source[op_index + 1].pc : 0;
            ++result.helperUops;
            ++result.chunkIssues;
            ++result.chunkExecutions;
            if (lanePC[lane] == 0) {
                ++result.normalTerminatedLanes;
                laneActive[lane] = false;
                activeMask[lane / 64] &=
                    ~(uint64_t(1) << (lane % 64));
                break;
            }
            continue;
        }

        RegVal value = 0;
        if (!op.evaluate(lhs, rhs, value)) {
            result.unsupportedControlFlow = true;
            ++result.unsupportedSemanticLanes;
            if (result.unsupportedSemanticPC == 0) {
                result.unsupportedSemanticPC = op.pc;
                result.unsupportedSemanticEncoding = op.encoding;
                result.unsupportedSemantic =
                    static_cast<uint8_t>(op.semantic);
            }
            laneActive[lane] = false;
            activeMask[lane / 64] &= ~(uint64_t(1) << (lane % 64));
            break;
        }
        if (op.destination >= 0)
            vectorRegs[op.destination][lane] = value;
        lanePC[lane] = op_index + 1 < size ? source[op_index + 1].pc : 0;
        ++result.helperUops;
        ++result.chunkIssues;
        ++result.chunkExecutions;
        if (lanePC[lane] == 0) {
            ++result.normalTerminatedLanes;
            laneActive[lane] = false;
            activeMask[lane / 64] &= ~(uint64_t(1) << (lane % 64));
            break;
        }
        if (!continuationContinuePastFLR &&
            op_index == continuationStopIndex) {
            ++result.normalTerminatedLanes;
            lanePC[lane] = 0;
            laneActive[lane] = false;
            activeMask[lane / 64] &=
                ~(uint64_t(1) << (lane % 64));
            break;
        }
    }

    if (result.helperUops >= max_helper_uops && laneActive[lane])
        result.timedOut = true;
    return result;
}

DVRVectorInstructionRegister::Result
DVRVectorInstructionRegister::resumeSourceLanes(
    const std::array<DVRInstructionRecorder::Uop,
                     DVRInstructionRecorder::MaxUops> &source,
    unsigned size, unsigned lane, int source_destination,
    RegVal source_value, unsigned max_helper_uops)
{
    Result result;
    if (!continuationInitialized || size <= 1 ||
        lane >= continuationLanes || !laneActive[lane] ||
        max_helper_uops == 0)
        return result;

    if (source_destination >= 0 &&
        source_destination < DVRVectorRenameTable::NumArchitecturalRegs)
        vectorRegs[source_destination][lane] = source_value;
    laneReady[lane] = true;

    // A memory response only makes a lane eligible to issue.  Do not execute
    // the first response immediately: the next response can arrive before
    // the next helper issue opportunity and join the same-PC group.  This is
    // the response-side analogue of the paper's in-order VIR issue register.
    unsigned ready_lanes = 0;
    for (unsigned candidate_lane = 0;
         candidate_lane < continuationLanes; ++candidate_lane) {
        if (laneActive[candidate_lane] && laneReady[candidate_lane])
            ++ready_lanes;
    }
    if (ready_lanes < 2)
        return result;

    for (unsigned step = 0; step < max_helper_uops; ++step) {
        bool resumed_path = false;
        if (stackDepth != 0) {
            const auto &top = stack[stackDepth - 1];
            bool at_reconvergence = false;
            bool selected_path_alive = false;
            for (unsigned candidate_lane = 0;
                 candidate_lane < continuationLanes; ++candidate_lane) {
                if (!laneActive[candidate_lane] ||
                    !laneReady[candidate_lane] || laneBlocked[candidate_lane])
                    continue;
                selected_path_alive = true;
                at_reconvergence |= lanePC[candidate_lane] == top.pc;
            }
            if (at_reconvergence || !selected_path_alive) {
                const auto deferred = top.deferredMask;
                const Addr deferred_pc = top.deferredPC;
                bool deferred_alternate = false;
                for (unsigned candidate = 0; candidate < size; ++candidate) {
                    if (source[candidate].pc == deferred_pc) {
                        deferred_alternate = source[candidate].alternatePath;
                        break;
                    }
                }
                for (unsigned candidate_lane = 0;
                     candidate_lane < continuationLanes; ++candidate_lane) {
                    if (deferred[candidate_lane / 64] &
                        (uint64_t(1) << (candidate_lane % 64))) {
                        laneBlocked[candidate_lane] = false;
                        lanePC[candidate_lane] = deferred_pc;
                    }
                }
                --stackDepth;
                if (at_reconvergence)
                    ++result.reconvergences;
                if (at_reconvergence &&
                    (deferred_alternate || top.alternatePath))
                    ++result.alternatePathReconvergences;
                resumed_path = true;
            }
        }
        Addr group_pc = 0;
        int op_index = -1;
        if (resumed_path) {
            for (unsigned candidate_lane = 0;
                 candidate_lane < continuationLanes; ++candidate_lane) {
                if (laneActive[candidate_lane] &&
                    laneReady[candidate_lane] &&
                    !laneBlocked[candidate_lane]) {
                    group_pc = lanePC[candidate_lane];
                    break;
                }
            }
        }
        for (unsigned candidate_lane = 0;
             candidate_lane < continuationLanes; ++candidate_lane) {
            if (!laneActive[candidate_lane] || !laneReady[candidate_lane] ||
                laneBlocked[candidate_lane])
                continue;
            if (resumed_path && group_pc != 0 &&
                lanePC[candidate_lane] != group_pc)
                continue;
            group_pc = lanePC[candidate_lane];
            for (unsigned candidate = laneWindowStart[candidate_lane];
                 candidate < laneWindowEnd[candidate_lane]; ++candidate) {
                if (source[candidate].pc == group_pc) {
                    op_index = candidate;
                    break;
                }
            }
            if (op_index < 0) {
                for (unsigned candidate = 1; candidate < size; ++candidate) {
                    if (source[candidate].pc != group_pc)
                        continue;
                    laneWindowStart[candidate_lane] = candidate;
                    laneWindowEnd[candidate_lane] = std::min(
                        candidate + DVRInstructionRecorder::FrontEndBufferUops,
                        size);
                    op_index = candidate;
                    break;
                }
            }
            break;
        }
        if (op_index < 0) {
            for (unsigned candidate_lane = 0;
                 candidate_lane < continuationLanes; ++candidate_lane) {
                if (!laneActive[candidate_lane] ||
                    !laneReady[candidate_lane] || laneBlocked[candidate_lane])
                    continue;
                laneActive[candidate_lane] = false;
                laneReady[candidate_lane] = false;
                activeMask[candidate_lane / 64] &=
                    ~(uint64_t(1) << (candidate_lane % 64));
                if (lanePC[candidate_lane] == source[0].pc) {
                    ++result.normalTerminatedLanes;
                } else if (lanePC[candidate_lane] != 0) {
                    result.unsupportedControlFlow = true;
                    ++result.externalPathLanes;
                }
            }
            break;
        }

        const auto &op = source[op_index];
        if (op.alternatePath)
            ++result.alternatePathUops;
        for (unsigned candidate_lane = 0;
             candidate_lane < continuationLanes; ++candidate_lane) {
            if (laneActive[candidate_lane] && laneReady[candidate_lane] &&
                lanePC[candidate_lane] == group_pc &&
                lanePendingReconvergence[candidate_lane][op_index]) {
                ++result.alternatePathReconvergences;
                lanePendingReconvergence[candidate_lane][op_index] = false;
            }
        }
        ++result.pcGroups;
        bool any_active = false;
        unsigned group_lanes = 0;
        std::array<bool, 128> taken_lanes = {};
        bool has_taken = false;
        bool has_fallthrough = false;
        bool first_path_taken = false;
        bool first_path_set = false;
        for (unsigned candidate_lane = 0;
             candidate_lane < continuationLanes; ++candidate_lane) {
            if (!laneActive[candidate_lane] || !laneReady[candidate_lane] ||
                lanePC[candidate_lane] != group_pc)
                continue;
            any_active = true;
            ++group_lanes;
            const RegVal lhs = op.source0 >= 0 ?
                vectorRegs[op.source0][candidate_lane] : 0;
            const RegVal rhs = op.source1 >= 0 ?
                vectorRegs[op.source1][candidate_lane] : 0;
            if (op.conditional) {
                bool taken = op.branchTaken;
                if (op.tainted && !op.evaluateBranch(lhs, rhs, taken)) {
                    result.unsupportedControlFlow = true;
                    ++result.externalPathLanes;
                    taken = (lhs != 0) == op.branchTaken;
                }
                const Addr next = taken ? op.branchTargetPC :
                    (op.fallthroughPC != 0 ? op.fallthroughPC :
                     (op_index + 1 < size ? source[op_index + 1].pc : 0));
                lanePC[candidate_lane] = next;
                taken_lanes[candidate_lane] = taken;
                if (!first_path_set) {
                    first_path_taken = taken;
                    first_path_set = true;
                }
                has_taken |= taken;
                has_fallthrough |= !taken;
                if (next == 0) {
                    ++result.earlyExitLanes;
                    laneActive[candidate_lane] = false;
                    laneReady[candidate_lane] = false;
                    activeMask[candidate_lane / 64] &=
                        ~(uint64_t(1) << (candidate_lane % 64));
                } else if (next == source[0].pc) {
                    ++result.normalTerminatedLanes;
                    laneActive[candidate_lane] = false;
                    laneReady[candidate_lane] = false;
                    activeMask[candidate_lane / 64] &=
                        ~(uint64_t(1) << (candidate_lane % 64));
                }
                continue;
            }

            if (op.control && !op.conditional && op.branchTargetPC != 0) {
                lanePC[candidate_lane] = op.branchTargetPC;
                ++result.helperUops;
                ++result.chunkIssues;
                ++result.chunkExecutions;
                if (lanePC[candidate_lane] == 0) {
                    ++result.earlyExitLanes;
                    laneActive[candidate_lane] = false;
                    laneReady[candidate_lane] = false;
                    activeMask[candidate_lane / 64] &=
                        ~(uint64_t(1) << (candidate_lane % 64));
                } else if (lanePC[candidate_lane] == source[0].pc) {
                    ++result.normalTerminatedLanes;
                    laneActive[candidate_lane] = false;
                    laneReady[candidate_lane] = false;
                    activeMask[candidate_lane / 64] &=
                        ~(uint64_t(1) << (candidate_lane % 64));
                }
                continue;
            }

            if (op.semantic == DVRInstructionRecorder::Uop::Semantic::Unsupported &&
                !op.control && op.intDestinations == 0) {
                lanePC[candidate_lane] = op_index + 1 < size ?
                    source[op_index + 1].pc : 0;
                ++result.helperUops;
                ++result.chunkIssues;
                ++result.chunkExecutions;
                if (lanePC[candidate_lane] == 0) {
                    ++result.normalTerminatedLanes;
                    laneActive[candidate_lane] = false;
                    laneReady[candidate_lane] = false;
                    activeMask[candidate_lane / 64] &=
                        ~(uint64_t(1) << (candidate_lane % 64));
                }
                continue;
            }

            RegVal value = 0;
            if (!op.evaluate(lhs, rhs, value)) {
                result.unsupportedControlFlow = true;
                ++result.unsupportedSemanticLanes;
                if (result.unsupportedSemanticPC == 0) {
                    result.unsupportedSemanticPC = op.pc;
                    result.unsupportedSemanticEncoding = op.encoding;
                    result.unsupportedSemantic =
                        static_cast<uint8_t>(op.semantic);
                }
                laneActive[candidate_lane] = false;
                laneReady[candidate_lane] = false;
                activeMask[candidate_lane / 64] &=
                    ~(uint64_t(1) << (candidate_lane % 64));
                continue;
            }
            if (op.destination >= 0)
                vectorRegs[op.destination][candidate_lane] = value;
            lanePC[candidate_lane] = op_index + 1 < size ?
                source[op_index + 1].pc : 0;
            if (lanePC[candidate_lane] == 0) {
                ++result.normalTerminatedLanes;
                laneActive[candidate_lane] = false;
                laneReady[candidate_lane] = false;
                activeMask[candidate_lane / 64] &=
                    ~(uint64_t(1) << (candidate_lane % 64));
            } else if (!continuationContinuePastFLR &&
                       op_index == continuationStopIndex) {
                ++result.normalTerminatedLanes;
                lanePC[candidate_lane] = 0;
                laneActive[candidate_lane] = false;
                laneReady[candidate_lane] = false;
                activeMask[candidate_lane / 64] &=
                    ~(uint64_t(1) << (candidate_lane % 64));
            }
        }

        if (!any_active)
            break;
        result.activeLanes += group_lanes;
        result.maxPCGroupLanes = std::max(result.maxPCGroupLanes,
                                          group_lanes);
        if (has_taken && has_fallthrough) {
            ++result.divergentBranches;
            if (stackDepth >= ReconvergenceEntries) {
                result.stackOverflow = true;
                return result;
            }
            const Addr reconvergence = op.reconvergencePC != 0 ?
                op.reconvergencePC : op.fallthroughPC;
            std::array<uint64_t, 2> deferred = {};
            Addr deferred_pc = 0;
            bool alternate_path = false;
            for (unsigned candidate_lane = 0;
                 candidate_lane < continuationLanes; ++candidate_lane) {
                if (!any_active || !laneActive[candidate_lane] ||
                    !laneReady[candidate_lane] ||
                    lanePC[candidate_lane] == 0)
                    continue;
                if (taken_lanes[candidate_lane] != first_path_taken) {
                    deferred[candidate_lane / 64] |=
                        uint64_t(1) << (candidate_lane % 64);
                    deferred_pc = lanePC[candidate_lane];
                    laneBlocked[candidate_lane] = true;
                }
                for (unsigned candidate = 0; candidate < size; ++candidate) {
                    if (source[candidate].pc == lanePC[candidate_lane] &&
                        source[candidate].alternatePath) {
                        alternate_path = true;
                        break;
                    }
                }
            }
            stack[stackDepth].deferredMask = deferred;
            stack[stackDepth].deferredPC = deferred_pc;
            stack[stackDepth].pc = reconvergence;
            stack[stackDepth].alternatePath = alternate_path;
            ++stackDepth;
        }
        ++result.helperUops;
        ++result.chunkIssues;
        ++result.chunkExecutions;
    }

    if (result.helperUops >= max_helper_uops) {
        for (unsigned candidate_lane = 0;
             candidate_lane < continuationLanes; ++candidate_lane) {
            if (laneActive[candidate_lane] && laneReady[candidate_lane] &&
                !laneBlocked[candidate_lane]) {
                result.timedOut = true;
                break;
            }
        }
    }
    return result;
}

RegVal
DVRVectorInstructionRegister::laneValue(unsigned reg, unsigned lane) const
{
    assert(reg < DVRVectorRenameTable::NumArchitecturalRegs);
    assert(lane < 128);
    return vectorRegs[reg][lane];
}

void
DVRVectorInstructionRegister::reset()
{
    activeMask = {};
    stack = {};
    vectorRegs = {};
    lanePC = {};
    laneActive = {};
    laneReady = {};
    laneBlocked = {};
    lanePendingReconvergence = {};
    laneWindowStart = {};
    laneWindowEnd = {};
    laneStackDepth = {};
    laneStack = {};
    continuationLanes = 0;
    continuationStopIndex = 0;
    continuationContinuePastFLR = false;
    continuationInitialized = false;
    stackDepth = 0;
    issuedChunks = 0;
    executedChunks = 0;
}

/* old implementation removed: VIR now executes supported uops per lane. */
/*
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
*/

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
    lastComparePC = 0;
    loopBranchPC = 0;
    loopTargetPC = 0;
    boundSource0 = -1;
    boundSource1 = -1;
    compareDestination = -1;
    branchValue0 = 0;
    branchValue1 = 0;
    branchValuesValid = false;
    comparison = Comparison::Unknown;
    seenBranch = false;
}

DVRLoopBoundDetector::Observation
DVRLoopBoundDetector::observe(const DynInstPtr &inst,
                              const RegisterSnapshot *regs)
{
    uint64_t encoded = 0;
    const size_t encoded_size = inst->staticInst->asBytes(
        &encoded, sizeof(encoded));
    const uint64_t opcode = encoded & 0x7f;
    const uint64_t funct3 = (encoded >> 12) & 0x7;

    // LCR: retain the most recent register-register SLT/SLTU after FLR.
    // RISC-V materializes a compare result in an integer destination; a
    // following BEQ/BNE must consume that destination to close the LCR/SBB
    // pair described by the paper.
    if (!seenBranch && finalLoadPC != 0 && encoded_size >= 4 &&
        opcode == 0x33 && (funct3 == 2 || funct3 == 3)) {
        int sources[2] = {-1, -1};
        unsigned found = 0;
        for (int idx = 0; idx < inst->numSrcRegs() && found < 2; ++idx) {
            const RegId &src = inst->srcRegIdx(idx);
            if (src.classValue() == IntRegClass)
                sources[found++] = src.index();
        }
        int destination = -1;
        for (int idx = 0; idx < inst->numDestRegs(); ++idx) {
            const RegId &dest = inst->destRegIdx(idx);
            if (dest.classValue() == IntRegClass) {
                destination = dest.index();
                break;
            }
        }
        if (found == 2 && destination >= 0) {
            lastComparePC = inst->pcState().instAddr();
            boundSource0 = sources[0];
            boundSource1 = sources[1];
            compareDestination = destination;
            comparison = funct3 == 2 ? Comparison::SignedLess :
                                       Comparison::UnsignedLess;
            branchValuesValid = false;
        }
        return {};
    }

    if (!inst->isCondCtrl() || !inst->isDirectCtrl())
        return {};
    const Addr pc = inst->pcState().instAddr();
    const auto target_state = inst->branchTarget();
    if (!target_state)
        return {};
    const Addr target = target_state->instAddr();
    if (target > pc)
        return {};

    const bool encloses_chain = finalLoadPC != 0 && target <= triggerPC &&
                                pc >= finalLoadPC;
    int integer_sources[2] = {-1, -1};
    unsigned found = 0;
    for (int idx = 0; idx < inst->numSrcRegs() && found < 2; ++idx) {
        const RegId &src = inst->srcRegIdx(idx);
        if (src.classValue() == IntRegClass)
            integer_sources[found++] = src.index();
    }

    bool consumes_compare = false;
    if (encoded_size >= 4 && opcode == 0x63 &&
        (funct3 == 0 || funct3 == 1) && compareDestination >= 0) {
        if (found == 2 &&
            ((integer_sources[0] == compareDestination &&
              integer_sources[1] == 0) ||
             (integer_sources[1] == compareDestination &&
              integer_sources[0] == 0))) {
            consumes_compare = true;
        }
    }

    // RISC-V commonly fuses the compare and branch.  Model BLT/BGE/BLTU/BGEU
    // as a virtual LCR whose result is consumed by that same branch, rather
    // than forcing normal compiler output down the 128-lane fallback path.
    bool virtual_compare = false;
    if (!seenBranch && encoded_size >= 4 && opcode == 0x63 && found == 2 &&
        funct3 >= 4 && funct3 <= 7) {
        switch (funct3) {
          case 4:
            comparison = Comparison::SignedLess;
            break;
          case 5:
            comparison = Comparison::SignedGreaterEqual;
            break;
          case 6:
            comparison = Comparison::UnsignedLess;
            break;
          case 7:
            comparison = Comparison::UnsignedGreaterEqual;
            break;
        }
        lastComparePC = pc;
        boundSource0 = integer_sources[0];
        boundSource1 = integer_sources[1];
        compareDestination = -1;
        virtual_compare = true;
    }

    if (!seenBranch && encloses_chain &&
        (consumes_compare || virtual_compare)) {
        // BNE cmp, x0 takes the loop back-edge when compare is true; BEQ
        // takes it when compare is false, so invert the LCR predicate.
        if (funct3 == 0) {
            if (comparison == Comparison::SignedLess)
                comparison = Comparison::SignedGreaterEqual;
            else if (comparison == Comparison::UnsignedLess)
                comparison = Comparison::UnsignedGreaterEqual;
        }
        loopBranchPC = pc;
        loopTargetPC = target;
        if (regs) {
            branchValue0 = (*regs)[boundSource0];
            branchValue1 = (*regs)[boundSource1];
            branchValuesValid = true;
        }
        seenBranch = true;
    }

    return {true, seenBranch && encloses_chain &&
                  (consumes_compare || virtual_compare)};
}

DVRLoopBoundDetector::Inference
DVRLoopBoundDetector::infer(const RegisterSnapshot &start,
                            const RegisterSnapshot &finish,
                            unsigned max_lanes) const
{
    // 一个寄存器应保持不变作为 bound，另一个应按固定步幅变化。
    Inference inference;
    // Paper fallback: if the loop-control relation cannot be proven, still
    // launch a bounded 128-element speculative runahead generation.
    inference.fallback = true;
    inference.lanes = std::min<unsigned>(128, max_lanes);
    if (!seenBranch || boundSource0 < 0 || boundSource1 < 0 ||
        boundSource0 >= MaxArchitecturalIntRegs ||
        boundSource1 >= MaxArchitecturalIntRegs)
        return inference;

    const RegVal start0 = start[boundSource0];
    const RegVal start1 = start[boundSource1];
    const RegVal finish0 = finish[boundSource0];
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
    } else if (branchValuesValid && start0 == branchValue0 &&
               start1 != branchValue1) {
        bound = branchValue0;
        current = branchValue1;
        increment = static_cast<int64_t>(branchValue1 - start1);
    } else if (branchValuesValid && start1 == branchValue1 &&
               start0 != branchValue0) {
        bound = branchValue1;
        current = branchValue0;
        increment = static_cast<int64_t>(branchValue0 - start0);
    } else {
        return inference;
    }

    uint64_t distance = 0;
    uint64_t step = 0;
    const int64_t signed_bound = static_cast<int64_t>(bound);
    const int64_t signed_current = static_cast<int64_t>(current);
    switch (comparison) {
      case Comparison::SignedLess:
        if (increment <= 0 || signed_current >= signed_bound)
            return inference;
        distance = static_cast<uint64_t>(signed_bound - signed_current);
        step = increment;
        break;
      case Comparison::UnsignedLess:
        if (increment <= 0 || current >= bound)
            return inference;
        distance = bound - current;
        step = increment;
        break;
      case Comparison::SignedGreaterEqual:
        if (increment >= 0 || signed_current < signed_bound)
            return inference;
        distance = static_cast<uint64_t>(signed_current - signed_bound) + 1;
        step = uint64_t(-(increment + 1)) + 1;
        break;
      case Comparison::UnsignedGreaterEqual:
        if (increment >= 0 || current < bound)
            return inference;
        distance = current - bound + 1;
        step = uint64_t(-(increment + 1)) + 1;
        break;
      default:
        return inference;
    }

    inference.matched = true;
    inference.fallback = false;
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
    lastComparePC = 0;
    loopBranchPC = 0;
    loopTargetPC = 0;
    boundSource0 = -1;
    boundSource1 = -1;
    compareDestination = -1;
    branchValue0 = 0;
    branchValue1 = 0;
    branchValuesValid = false;
    comparison = Comparison::Unknown;
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
