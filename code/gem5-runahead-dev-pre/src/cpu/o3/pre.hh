#ifndef __CPU_O3_PRE_HH__
#define __CPU_O3_PRE_HH__

#include <algorithm>
#include <array>
#include <list>
#include <optional>
#include <unordered_map>
#include <vector>

#include "base/types.hh"
#include "config/the_isa.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/reg_class.hh"
#include "cpu/static_inst_fwd.hh"

namespace gem5
{

struct BaseO3CPUParams;

namespace o3
{

class CPU;

/** DVR 发现阶段使用的 32 项步幅检测表（RPT）。
 *
 * 每项记录 load PC、上次地址、带符号步幅和 2 位置信度。
 * 相同的非零步幅出现两次后，生成 DVR 候选。
 * 本类只负责检测；CPU 在确认依赖链和循环边界后启动向量辅助线程。
 */
class DVRStrideDetector
{
  public:
    // The paper uses a 2-bit saturating confidence counter (0..3).  A
    // counter value of two is enough to admit a stable stride; three is the
    // saturated state, not an extra confidence width.
    static constexpr uint8_t CandidateConfidence = 2;
    static constexpr uint8_t MaxConfidence = 3;
    struct Candidate
    {
        Addr pc;
        Addr address;
        int64_t stride;
        // True when this PC was observed again while another Discovery
        // generation was still active.  The caller can then switch the
        // discovery target to this more-inner striding load.
        bool repeatedDuringDiscovery = false;
    };

  private:
    struct Entry
    {
        bool valid = false;
        Addr pc = 0;
        Addr lastAddress = 0;
        int64_t stride = 0;
        uint8_t confidence = 0;
        uint64_t age = 0;
        // A candidate is emitted once when confidence enters the admitted
        // range.  It is re-armed after confidence falls below the threshold
        // or after a Discovery generation ends.
        bool candidatePending = false;
    };

    // Discovery starts at dispatch, therefore these one-bit RPT updates are
    // speculative.  Only false-to-true transitions need recording.
    struct DiscoverySeenWrite
    {
        InstSeqNum sequence = 0;
        unsigned entry = 0;
    };

    std::vector<Entry> entries;
    // One bit per RPT entry, matching the paper's global innermost-detection
    // register.  It is separate from Entry so resetting Discovery state does
    // not alter stride training or confidence.
    std::vector<uint64_t> discoveryBits;
    std::vector<DiscoverySeenWrite> discoverySeenWrites;
    uint64_t timestamp = 0;

    bool discoverySeen(unsigned entry) const
    {
        return (discoveryBits[entry / 64] &
                (uint64_t(1) << (entry % 64))) != 0;
    }

    void setDiscoverySeen(unsigned entry, bool value)
    {
        const uint64_t bit = uint64_t(1) << (entry % 64);
        if (value)
            discoveryBits[entry / 64] |= bit;
        else
            discoveryBits[entry / 64] &= ~bit;
    }

  public:
    explicit DVRStrideDetector(unsigned num_entries);
    std::optional<Candidate> observe(Addr pc, Addr address);
    std::optional<Candidate> observeDispatch(Addr pc,
                                             bool discovery_active = false,
                                             Addr trigger_pc = 0,
                                             InstSeqNum sequence = 0);
    /** Initialize the one-bit-per-RPT-entry discovery register. */
    void beginDiscovery(Addr trigger_pc);
    /** Undo seen-bit updates from instructions removed by an O3 squash. */
    void squashDiscovery(InstSeqNum squash_sequence);
    /** Clear only per-Discovery state; preserve RPT training. */
    void endDiscovery();
    void reset();
};

/** 按提交顺序管理一次 DVR 发现区间的状态机。 */
class DVRDiscoveryController
{
  public:
    enum class Event
    {
        None,
        Started,
        Completed,
        TimedOut,
        Abandoned
    };

    struct Result
    {
        Event event = Event::None;
        Addr triggerPC = 0;
        int64_t stride = 0;
        unsigned instructions = 0;
    };

  private:
    enum class State
    {
        Idle,
        Armed,
        Discovering
    };

    State state = State::Idle;
    Addr triggerPC = 0;
    int64_t triggerStride = 0;
    InstSeqNum triggerSequence = 0;
    InstSeqNum stopSequence = 0;
    unsigned instructions = 0;
    const unsigned maxInstructions;

    void finish();

  public:
    explicit DVRDiscoveryController(unsigned max_instructions);
    void arm(const DVRStrideDetector::Candidate &candidate,
             InstSeqNum sequence);
    /** Restart Discovery on a more-inner striding load. */
    void restart(const DVRStrideDetector::Candidate &candidate,
                 InstSeqNum sequence);
    bool observeDispatch(Addr pc, InstSeqNum sequence);
    Result observeCommit(Addr pc, InstSeqNum sequence);
    bool rollback(InstSeqNum squash_sequence);
    bool isDiscovering() const { return state == State::Discovering; }
    // Discovery's observation window closes when the trigger is dispatched
    // for the next iteration.  Completion is still committed in order, but
    // younger speculative loads must not participate in innermost detection.
    bool stopPending() const
    {
        return state == State::Discovering && stopSequence != 0;
    }
    // Commit-side Discovery body membership is sequence based.  A repeated
    // trigger may already be dispatched while older body instructions are
    // still waiting to commit; those older instructions remain part of the
    // generation, while the terminating trigger itself does not.
    bool inDiscoveryBody(InstSeqNum sequence) const
    {
        return state == State::Discovering &&
            sequence > triggerSequence &&
            (stopSequence == 0 || sequence < stopSequence);
    }
    InstSeqNum triggerSeq() const { return triggerSequence; }
    Addr currentTriggerPC() const { return triggerPC; }
    int64_t currentStride() const { return triggerStride; }
    void reset();
};

/** 跟踪 RISC-V 32 个架构整数寄存器的污点。 */
class DVRVectorTaintTracker
{
  private:
    static constexpr unsigned NumTrackedRegs = 32;
    uint32_t taint = 0;
    Addr finalLoadPC = 0;
    struct SpeculativeState
    {
        InstSeqNum sequence = 0;
        uint32_t taint = 0;
        Addr finalLoadPC = 0;
    };
    std::vector<SpeculativeState> speculativeHistory;

    bool tracked(const RegId &reg) const;
    bool isTainted(const RegId &reg) const;
    void setTainted(const RegId &reg, bool value);

  public:
    struct Observation
    {
        bool taintedInstruction = false;
        bool dependentLoad = false;
    };

    void begin(const DynInstPtr &initiating_load);
    Observation observe(const DynInstPtr &inst);
    /** Dispatch-side update with enough state to undo an O3 squash. */
    Observation observeSpeculative(const DynInstPtr &inst,
                                   InstSeqNum sequence);
    void squash(InstSeqNum squash_sequence);
    Observation classify(const DynInstPtr &inst) const;
    void reset();
    Addr flr() const { return finalLoadPC; }
    void setFLR(Addr pc) { finalLoadPC = pc; }
    uint32_t bits() const { return taint; }
};

/** 发现阶段记录的 trigger 到 FLR 的紧凑指令模板。 */
class DVRInstructionRecorder
{
  public:
    // The paper's eight decoded-uop front-end buffer is a refill window.  It
    // is not the capacity of the captured trigger-to-FLR metadata.  Keep the
    // complete template separate so a long discovery path can be replayed by
    // refilling the front-end while VIR state remains live.
    static constexpr unsigned FrontEndBufferUops = 8;
    static constexpr unsigned MaxUops = 256;

    struct ResourceCounts
    {
        unsigned alu = 0;
        unsigned shift = 0;
        unsigned multiply = 0;
        unsigned lsu = 0;
    };

    struct Uop
    {
        /**
         * 可直接重放的语义，不调用架构 ExecContext 的 StaticInst::execute。
         * 不支持的指令必须明确标记，不能静默近似。
         */
        enum class Semantic : uint8_t
        {
            Unsupported,
            Move,
            Add,
            Sub,
            And,
            Or,
            Xor,
            ShiftLeft,
            ShiftRightLogical,
            ShiftRightArithmetic,
            Multiply,
            MultiplyWord,
            Remainder,
            RemainderUnsigned,
            RemainderWord,
            RemainderUnsignedWord,
            AddWord,
            SubWord,
            AddImmediate,
            AddWordImmediate,
            ShiftLeftImmediate,
            ShiftLeftWordImmediate,
            AndImmediate,
            OrImmediate,
            XorImmediate,
            ShiftRightLogicalImmediate,
            ShiftRightArithmeticImmediate,
            ShiftRightLogicalWordImmediate,
            ShiftRightArithmeticWordImmediate,
            LoadAddress,
            LoadByteSigned,
            LoadByteUnsigned,
            LoadHalfSigned,
            LoadHalfUnsigned,
            LoadWordSigned,
            LoadWordUnsigned,
            LoadDouble,
            BranchEqual,
            BranchNotEqual,
            BranchSignedLess,
            BranchSignedGreaterEqual,
            BranchUnsignedLess,
            BranchUnsignedGreaterEqual
        };

        Addr pc = 0;
        // Decoded instruction object retained by the helper program.  It is
        // metadata only: the helper does not enqueue a DynInst into O3 ROB/IQ.
        StaticInstPtr staticInst;
        Addr branchTargetPC = 0;
        Addr fallthroughPC = 0;
        Addr reconvergencePC = 0;
        uint32_t intSources = 0;
        uint32_t intDestinations = 0;
        uint32_t encoding = 0;
        int64_t immediate = 0;
        int8_t source0 = -1;
        int8_t source1 = -1;
        int8_t destination = -1;
        Semantic semantic = Semantic::Unsupported;
        // Width of the architectural load represented by this uop.  Address
        // generation still uses a full register value; the source response
        // applies the ISA-specific sign/zero extension using this metadata.
        uint8_t loadBytes = 0;
        bool encodingValid = false;
        bool load = false;
        bool control = false;
        bool conditional = false;
        bool branchTaken = false;
        // Captured at dispatch, before the tracker mutates destination
        // taint.  Only a tainted conditional can legitimately diverge across
        // helper lanes; untainted control is replayed on its observed path.
        bool tainted = false;
        bool alternatePath = false;
        // For a cached suffix, the PC at which the lane resumes after the
        // suffix's terminal uop.  This is distinct from the branch's
        // reconvergence key because the latter may be unresolved in a
        // single-path discovery.
        Addr alternateResumePC = 0;

        /**
         * 计算支持的标量地址生成操作。
         * LoadAddress 返回有效虚拟地址；内存访问和取值由辅助引擎负责。
         */
        bool evaluate(RegVal source0_value, RegVal source1_value,
                      RegVal &result) const;
        bool evaluateBranch(RegVal source0_value, RegVal source1_value,
                            bool &taken) const;
    };

  private:
    std::array<Uop, MaxUops> uops = {};
    unsigned count = 0;
    bool overflowed = false;

  public:
    void begin(const DynInstPtr &trigger);
    bool record(const DynInstPtr &inst, bool tainted = false);
    /** Build replay metadata directly from a helper-front-end decode. */
    static bool decodeStatic(const StaticInstPtr &inst, Addr pc, Uop &uop);
    /** Import a committed helper template for response-driven replay. */
    void import(const std::array<Uop, MaxUops> &source, unsigned size);
    /** Append a validated alternate suffix while retaining its reconvergence PC. */
    bool insertBeforePC(Addr reconvergence_pc, const std::vector<Uop> &path);
    /**
     * Apply the paper's FLR/LCR reconvergence policy.  Branches at or before
     * the FLR reconverge at the FLR.  A branch strictly between the FLR and
     * the loop back-edge (the LCR branch) must not reconverge at the already
     * consumed FLR; it reconverges at the loop boundary instead.  Branches
     * after the LCR retain their captured CFG metadata.
     */
    void setReconvergencePC(Addr flr_pc, Addr loop_branch_pc);

    /** True iff a conditional branch occurs strictly between FLR and LCR. */
    bool hasConditionalBetween(Addr flr_pc, Addr loop_branch_pc) const;
    void reset();
    /**
     * Limit a copied template to the trigger-to-FLR prefix used by the
     * ordinary initial VIR audit.  Post-FLR control flow is kept in the
     * persistent continuation template, but is not part of this bounded
     * prefix unless the paper's divergent-path rule explicitly requires it.
     */
    void truncate(unsigned size)
    {
        count = std::min(count, size);
    }
    unsigned size() const { return count; }
    bool overflow() const { return overflowed; }
    ResourceCounts resourceCounts() const;
    const Uop &operator[](unsigned index) const { return uops[index]; }
};

/**
 * RISC-V 向量重命名表：32 个架构整数寄存器，
 * 每个 16-lane 分块一项映射，最多支持 128 lanes。
 * 标量映射为 -1，带污点的目标寄存器分配向量物理寄存器。
 */
class DVRVectorRenameTable
{
  public:
    static constexpr unsigned NumArchitecturalRegs = 32;
    static constexpr unsigned NumChunks = 8;
    static constexpr unsigned NumPhysicalRegs = 128;
    using Mapping = std::array<std::array<int16_t, NumChunks>,
                               NumArchitecturalRegs>;

  private:
    Mapping mapping = {};
    unsigned nextPhysical = 0;

  public:
    DVRVectorRenameTable();
    unsigned build(const DVRInstructionRecorder &program, unsigned lanes);
    void reset();
    int16_t lookup(unsigned architectural, unsigned chunk) const;
};

/** 按 16-lane 分块记录已捕获程序的 VIR 发射状态。 */
class DVRVectorInstructionRegister
{
  private:
    static constexpr unsigned ReconvergenceEntries = 8;
    struct ReconvergenceEntry
    {
        std::array<uint64_t, 2> deferredMask = {};
        Addr deferredPC = 0;
        Addr pc = 0;
        bool alternatePath = false;
    };

    std::array<uint64_t, 2> activeMask = {};
    std::array<ReconvergenceEntry, ReconvergenceEntries> stack = {};
    // Vector register file used by the helper interpreter.  Values are
    // private to the DVR context and are never copied to architectural regs.
    std::array<std::array<RegVal, 128>,
               DVRVectorRenameTable::NumArchitecturalRegs> vectorRegs = {};
    std::array<Addr, 128> lanePC = {};
    std::array<bool, 128> laneActive = {};
    std::array<bool, 128> laneReady = {};
    // A deferred SIMT path remains live, but is not eligible for issue until
    // the currently selected path reaches the stack's reconvergence PC.
    std::array<bool, 128> laneBlocked = {};
    std::array<std::array<bool, DVRInstructionRecorder::MaxUops>, 128>
        lanePendingReconvergence = {};
    // Each lane has its own decoded-uop window.  A divergent target can move
    // one lane to a different window while the other lanes keep issuing.
    std::array<unsigned, 128> laneWindowStart = {};
    std::array<unsigned, 128> laneWindowEnd = {};
    std::array<uint8_t, 128> laneStackDepth = {};
    std::array<std::array<ReconvergenceEntry, ReconvergenceEntries>, 128>
        laneStack = {};
    unsigned continuationLanes = 0;
    unsigned continuationStopIndex = 0;
    bool continuationContinuePastFLR = false;
    bool continuationInitialized = false;
    unsigned stackDepth = 0;
    uint16_t issuedChunks = 0;
    uint16_t executedChunks = 0;


  public:
    struct Result
    {
        unsigned activeLanes = 0;
        unsigned chunkIssues = 0;
        unsigned chunkExecutions = 0;
        unsigned divergentBranches = 0;
        unsigned reconvergences = 0;
        unsigned helperUops = 0;
        unsigned normalTerminatedLanes = 0;
        unsigned earlyExitLanes = 0;
        unsigned externalPathLanes = 0;
        unsigned unsupportedSemanticLanes = 0;
        unsigned alternatePathUops = 0;
        unsigned alternatePathReconvergences = 0;
        unsigned pcGroups = 0;
        unsigned maxPCGroupLanes = 0;
        Addr unsupportedSemanticPC = 0;
        uint32_t unsupportedSemanticEncoding = 0;
        uint8_t unsupportedSemantic = 0;
        bool timedOut = false;
        bool stackOverflow = false;
        // A lane selected a nonzero PC outside the captured recorder.
        // This is unsupported control flow, not normal completion.
        bool unsupportedControlFlow = false;
    };

  private:
    Result executeLanePC(const DVRInstructionRecorder &program,
                         unsigned lanes, unsigned max_helper_uops,
                         const std::array<RegVal, 32> &initial_regs,
                         unsigned start_index = 0,
                         int source_destination = -1,
                         RegVal source_value = 0);

  public:

    Result execute(const DVRInstructionRecorder &program, unsigned lanes,
                   unsigned max_helper_uops,
                   const std::array<RegVal, 32> &initial_regs);
    /**
     * Resume one captured lane after its asynchronous source load returns.
     * The returned value replaces the trigger-load destination and execution
     * starts at the first uop after that load.
     */
    Result executeFromSource(const std::array<DVRInstructionRecorder::Uop,
                                               DVRInstructionRecorder::MaxUops>
                                 &source,
                             unsigned size, int source_destination,
                             RegVal source_value, unsigned max_helper_uops,
                             const std::array<RegVal, 32> &initial_regs);
    /** Initialize a persistent context for all source-response lanes. */
    void initializeSourceContinuation(
        const std::array<DVRInstructionRecorder::Uop,
                         DVRInstructionRecorder::MaxUops> &source,
        unsigned size, unsigned lanes,
        const std::array<RegVal, 32> &initial_regs,
        unsigned scalar_count = 0, bool continue_past_flr = false);
    /** Resume one lane in the persistent source-response context. */
    Result resumeSourceLane(
        const std::array<DVRInstructionRecorder::Uop,
                         DVRInstructionRecorder::MaxUops> &source,
        unsigned size, unsigned lane, int source_destination,
        RegVal source_value, unsigned max_helper_uops);
    /** Resume all source-ready lanes in current-PC groups. */
    Result resumeSourceLanes(
        const std::array<DVRInstructionRecorder::Uop,
                         DVRInstructionRecorder::MaxUops> &source,
        unsigned size, unsigned lane, int source_destination,
        RegVal source_value, unsigned max_helper_uops);
    void reset();
    const std::array<uint64_t, 2> &mask() const { return activeMask; }
    RegVal laneValue(unsigned reg, unsigned lane) const;
};

/** 适配 RISC-V 分支的 LCR/SBB 风格循环边界检测器。 */
class DVRLoopBoundDetector
{
  private:
    enum class Comparison : uint8_t
    {
        Unknown,
        Equal,
        NotEqual,
        SignedLess,
        SignedGreaterEqual,
        UnsignedLess,
        UnsignedGreaterEqual
    };
    Addr triggerPC = 0;
    Addr finalLoadPC = 0;
    Addr lastComparePC = 0;
    Addr loopBranchPC = 0;
    Addr loopTargetPC = 0;
    int boundSource0 = -1;
    int boundSource1 = -1;
    int compareDestination = -1;
    RegVal branchValue0 = 0;
    RegVal branchValue1 = 0;
    bool branchValuesValid = false;
    Comparison comparison = Comparison::Unknown;
    bool seenBranch = false;
    bool earlyExitSeen = false;

  public:
    static constexpr unsigned MaxArchitecturalIntRegs = 32;
    using RegisterSnapshot =
        std::array<RegVal, MaxArchitecturalIntRegs>;

    struct Observation
    {
        bool backwardBranch = false;
        bool boundFound = false;
    };

    struct Inference
    {
        bool matched = false;
        bool fallback = false;
        uint64_t bound = 0;
        int64_t increment = 0;
        uint64_t remaining = 0;
        unsigned lanes = 0;
    };

    void begin(Addr trigger_pc);
    void updateFinalLoad(Addr final_load_pc);
    Observation observe(const DynInstPtr &inst,
                        const RegisterSnapshot *regs = nullptr);
    Inference infer(const RegisterSnapshot &start,
                    const RegisterSnapshot &finish,
                    unsigned max_lanes) const;
    void reset();
    bool hasBound() const { return seenBranch; }
    Addr branchPC() const { return loopBranchPC; }
    Addr targetPC() const { return loopTargetPC; }
    int8_t boundSource0Reg() const { return boundSource0; }
    int8_t boundSource1Reg() const { return boundSource1; }
    uint8_t comparisonKind() const
    {
        return static_cast<uint8_t>(comparison);
    }
    int source0() const { return boundSource0; }
    int source1() const { return boundSource1; }
};

/**
 * Stalling slice table class.
 */
class SST
{
    typedef std::list<Addr> AddrList;
    typedef AddrList::iterator AddrListIter;

    typedef std::unordered_map<Addr, AddrListIter> AddrMap;
    typedef AddrMap::iterator AddrMapIter;

    /** A hash list composed of addrList and addrMap. 
     *  This is for storing instructions and maintaining LRU order.
     */
    AddrList L;
    AddrMap M;

    /** Pointer to the CPU. */
    CPU *cpu;

    /** Number of instructions that SST can store. */
    unsigned numEntries;

  public:
    SST(CPU *_cpu, const BaseO3CPUParams &params);

    /** Adds an instruction of the stalling slice to SST.
     *  Newly added instructions may replace obsolate ones.
     */
    void addInst(Addr addr);
    void addInst(const DynInstPtr &inst);

    /** Determines if SST has this instruction. */
    bool hasInst(const DynInstPtr &inst);
};

/**
 * Misprediction table class.
 */
class MispTable
{
    struct Cell {
        Addr pc;
        short lru; // head is 0
        short ref;
        short misp;
    };

    typedef std::array<Cell, 8> Row;

    std::array<Row, 8> table;

    static constexpr int MAX_REF = 256;

    bool HIGH(short ref, short misp) {
        return ref >= 64 && misp >= ref / 8;
    }

  public:
    MispTable();

    /** Adds a branch instruction into the table. */
    void add(const DynInstPtr &inst);

    /** Queries whether the branch has high misprediction rate. */
    bool high(const DynInstPtr &inst);
};

} // namespace o3
} // namespace gem5

#endif //__CPU_O3_PRE_HH__
