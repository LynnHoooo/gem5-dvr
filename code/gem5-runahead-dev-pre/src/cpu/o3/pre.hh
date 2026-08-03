#ifndef __CPU_O3_PRE_HH__
#define __CPU_O3_PRE_HH__

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
    struct Candidate
    {
        Addr pc;
        Addr address;
        int64_t stride;
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
    };

    std::vector<Entry> entries;
    uint64_t timestamp = 0;

  public:
    explicit DVRStrideDetector(unsigned num_entries);
    std::optional<Candidate> observe(Addr pc, Addr address);
    std::optional<Candidate> observeDispatch(Addr pc) const;
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
    bool observeDispatch(Addr pc, InstSeqNum sequence);
    Result observeCommit(Addr pc, InstSeqNum sequence);
    bool rollback(InstSeqNum squash_sequence);
    bool isDiscovering() const { return state == State::Discovering; }
    InstSeqNum triggerSeq() const { return triggerSequence; }
    void reset();
};

/** 跟踪 RISC-V 32 个架构整数寄存器的污点。 */
class DVRVectorTaintTracker
{
  private:
    static constexpr unsigned NumTrackedRegs = 32;
    uint32_t taint = 0;
    Addr finalLoadPC = 0;

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
    Observation classify(const DynInstPtr &inst) const;
    void reset();
    Addr flr() const { return finalLoadPC; }
    uint32_t bits() const { return taint; }
};

/** 发现阶段记录的 trigger 到 FLR 的紧凑指令模板。 */
class DVRInstructionRecorder
{
  public:
    static constexpr unsigned MaxUops = 8;

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
            Add,
            Sub,
            And,
            Or,
            Xor,
            ShiftLeft,
            ShiftRightLogical,
            ShiftRightArithmetic,
            Multiply,
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
            LoadHalfSigned,
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
        bool encodingValid = false;
        bool load = false;
        bool control = false;
        bool conditional = false;
        bool branchTaken = false;

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
    bool record(const DynInstPtr &inst);
    /** Import a committed helper template for response-driven replay. */
    void import(const std::array<Uop, MaxUops> &source, unsigned size);
    void reset();
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
        Addr pc = 0;
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
    std::array<std::array<bool, DVRInstructionRecorder::MaxUops>, 128>
        lanePendingReconvergence = {};
    std::array<uint8_t, 128> laneStackDepth = {};
    std::array<std::array<ReconvergenceEntry, ReconvergenceEntries>, 128>
        laneStack = {};
    unsigned continuationLanes = 0;
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
        unsigned pcGroups = 0;
        unsigned maxPCGroupLanes = 0;
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
        const std::array<RegVal, 32> &initial_regs);
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
    Addr loopBranchPC = 0;
    Addr loopTargetPC = 0;
    int boundSource0 = -1;
    int boundSource1 = -1;
    Comparison comparison = Comparison::Unknown;
    bool seenBranch = false;

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
        uint64_t bound = 0;
        int64_t increment = 0;
        uint64_t remaining = 0;
        unsigned lanes = 0;
    };

    void begin(Addr trigger_pc);
    void updateFinalLoad(Addr final_load_pc);
    Observation observe(const DynInstPtr &inst);
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
