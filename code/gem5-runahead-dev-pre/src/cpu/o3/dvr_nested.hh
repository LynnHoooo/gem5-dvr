#ifndef __CPU_O3_DVR_NESTED_HH__
#define __CPU_O3_DVR_NESTED_HH__

#include <array>
#include <cstdint>
#include <optional>

#include "base/types.hh"
#include "cpu/inst_seq.hh"

namespace gem5
{

namespace o3
{

/** 有界的嵌套 DVR 发现控制器。
 *
 * 本类只管理控制状态。Started 事件表示 CPU 可以记录子 trigger 到 FLR
 * 的指令片段，但本类不会发起内存访问。各帧按 LIFO 顺序完成，
 * 因此子发现不会晚于父发现结束。
 */
class DVRNestedController
{
  public:
    static constexpr unsigned MaxDepth = 2;
    using DiscoveryId = uint64_t;

    enum class Event
    {
        None,
        Started,
        Completed,
        TimedOut,
        RejectedDepth,
        RejectedParent,
        RejectedOrder
    };

    struct Result
    {
        Event event = Event::None;
        DiscoveryId id = 0;
        DiscoveryId parentId = 0;
        unsigned depth = 0;
        Addr triggerPC = 0;
        Addr finalLoadPC = 0;
        unsigned committedInstructions = 0;
    };

    struct Statistics
    {
        uint64_t rootStarts = 0;
        uint64_t nestedStarts = 0;
        uint64_t rootCompletions = 0;
        uint64_t nestedCompletions = 0;
        uint64_t rootTimeouts = 0;
        uint64_t nestedTimeouts = 0;
        uint64_t depthRejects = 0;
        uint64_t parentRejects = 0;
        uint64_t orderRejects = 0;
    };

  private:
    struct Frame
    {
        DiscoveryId id = 0;
        DiscoveryId parentId = 0;
        Addr triggerPC = 0;
        InstSeqNum triggerSequence = 0;
        unsigned committedInstructions = 0;
    };

    std::array<Frame, MaxDepth> frames = {};
    unsigned activeDepth = 0;
    unsigned maxInstructions;
    DiscoveryId nextId = 1;
    Statistics counters = {};

    Result reject(Event event);
    Result timeoutTop();

  public:
    explicit DVRNestedController(unsigned max_instructions);

    /** 启动外层发现；栈非空时拒绝。 */
    Result startRoot(Addr trigger_pc, InstSeqNum sequence);

    /**
     * 在当前栈顶帧下启动子发现。
     * parent_id 必须指向该帧；MaxDepth 将嵌套限制为一层子发现。
     */
    Result startNested(DiscoveryId parent_id, Addr trigger_pc,
                       InstSeqNum sequence);

    /**
     * 为所有活动发现记录一条已提交指令。
     * 超时后弹出栈顶；后续提交仍需继续检查父发现是否超时。
     */
    Result observeCommit(Addr pc, InstSeqNum sequence);

    /** 完成当前栈顶发现并返回结束事件。 */
    Result complete(DiscoveryId id, Addr final_load_pc);

    bool active() const { return activeDepth != 0; }
    unsigned depth() const { return activeDepth; }
    std::optional<DiscoveryId> currentId() const;
    std::optional<DiscoveryId> rootId() const;
    const Statistics &statistics() const { return counters; }

    /** 清除活动帧，但保留累计统计。 */
    void reset();
    /** 同时清除活动帧和累计统计。 */
    void clear();
};

/**
 * 论文 Nested Discovery Mode 的控制状态。
 *
 * NDM uses the committed stream as a non-architectural control-flow shadow:
 * it inverts the first inner backedge, scans the fall-through path for an
 * outer stride, and records independently bounded inner invocations.
 */
class DVRNestedDiscoveryMode
{
  public:
    enum class State { Idle, SeekingOuter, OuterFound, Vectorizing };
    enum class Event
    {
        None,
        Started,
        OuterAccepted,
        VectorizeReady,
        TimedOut,
        Fallback
    };

    /** The NDM control registers described by the paper's IR/ILR/LCR. */
    struct ControlState
    {
        Addr innerBranchPC = 0;       // IR: branch instruction
        Addr innerBranchTarget = 0;   // IR: taken/backward target
        Addr innerFallthrough = 0;    // IR: exit path
        Addr reconvergencePC = 0;     // LCR: first post-inner PC
        int8_t inductionRegister = -1; // ILR
        int8_t boundRegister = -1;     // LCR
        RegVal inductionValue = 0;
        RegVal boundValue = 0;
        int64_t inductionIncrement = 0;
        uint64_t remainingIterations = 0;
        bool branchTaken = false;
        bool branchInverted = false;
        bool valid = false;
        std::array<uint64_t, 2> takenMask = {};
        std::array<uint64_t, 2> fallthroughMask = {};
    };

    struct OuterInvocation
    {
        Addr outerBase = 0;
        Addr innerStart = 0;
        unsigned innerLanes = 0;
        Addr innerTriggerPC = 0;
        Addr innerFLRPC = 0;
        int64_t innerStride = 0;
        std::array<uint64_t, 2> predicate = {};
        Addr reconvergencePC = 0;
    };

    struct Result
    {
        Event event = Event::None;
        State state = State::Idle;
        Addr innerLoadPC = 0;
        int64_t increment = 0;
        unsigned innerLanes = 0;
        Addr outerLoadPC = 0;
        Addr outerAddress = 0;
        int64_t outerStride = 0;
        unsigned committedInstructions = 0;
        ControlState control = {};
    };

    struct Statistics
    {
        uint64_t attempts = 0;
        uint64_t outerFound = 0;
        uint64_t fallbacks = 0;
        uint64_t timeouts = 0;
        uint64_t branchInversions = 0;
        uint64_t irCaptures = 0;
        uint64_t ilrCaptures = 0;
        uint64_t lcrCaptures = 0;
        uint64_t outerInvocations = 0;
    };

  private:
    // The paper gives NDM a short, local search window: if no outer stride
    // appears within 200 NDM instructions, it falls back to the inner-loop
    // plan.  This is not the lifetime of the outer invocation plan after an
    // outer stride has been found.
    static constexpr unsigned PaperOuterSearchInstructions = 200;
    State currentState = State::Idle;
    unsigned laneThreshold;
    unsigned outerSearchMaxInstructions;
    unsigned outerCollectionMaxInstructions;
    unsigned committedInstructions = 0;
    Addr innerLoadPC = 0;
    int64_t increment = 0;
    unsigned innerLanes = 0;
    Addr outerLoadPC = 0;
    Addr outerAddress = 0;
    int64_t outerStride = 0;
    // The paper forms the outer vector from up to sixteen invocations.  The
    // inner loop contributes up to eight scalar lanes per outer lane, for a
    // maximum of 16 * 8 = 128 scalar-equivalent lanes.
    static constexpr unsigned MaxOuterInvocations = 16;
    std::array<OuterInvocation, MaxOuterInvocations> invocations = {};
    unsigned invocationCount = 0;
    struct OuterProbe
    {
        Addr pc = 0;
        Addr address = 0;
        int64_t stride = 0;
        unsigned samples = 0;
        std::array<Addr, 3> recentAddresses = {};
    };
    std::array<OuterProbe, 8> outerProbes = {};
    std::array<Addr, MaxOuterInvocations> pendingOuterBases = {};
    unsigned pendingOuterCount = 0;
    unsigned pendingOuterConsumed = 0;
    ControlState control = {};
    Statistics counters = {};

    Result snapshot(Event event) const;

  public:
    DVRNestedDiscoveryMode(unsigned lane_threshold,
                           unsigned max_instructions);

    /** Start NDM only for a trusted, non-empty lane count below threshold. */
    Result start(Addr inner_load_pc, int64_t loop_increment,
                 unsigned inner_lanes);

    /** Paper policy: NDM is useful only when the inner loop has < 64 lanes. */
    bool eligible(unsigned inner_lanes) const
    {
        return inner_lanes != 0 && inner_lanes < laneThreshold;
    }

    /** Capture IR/ILR/LCR after the inner loop bound is inferred. */
    void captureLoopRegisters(int8_t induction_register,
                              int8_t bound_register,
                              RegVal induction_value, RegVal bound_value,
                              uint64_t remaining_iterations);

    /** Invert the observed inner backward branch and save its exit PC. */
    bool observeInnerBranch(Addr branch_pc, Addr branch_target,
                            Addr fallthrough_pc, bool taken);

    /** Observe real committed addresses; three samples confirm the stride. */
    Result observeOuterLoad(Addr pc, Addr address);

    /** Record one independently bounded outer invocation. */
    bool recordOuterInvocation(Addr inner_start, unsigned lanes,
                               Addr inner_trigger_pc, Addr inner_flr_pc,
                               int64_t inner_stride);

    /** True after enough independent outer invocations are available. */
    bool readyToVectorize() const
    {
        return currentState == State::Vectorizing;
    }
    unsigned outerInvocationCount() const { return invocationCount; }
    const std::array<OuterInvocation, MaxOuterInvocations> &outerInvocations()
        const
    {
        return invocations;
    }

    /** Number of invocations represented by the learned outer plan. */
    unsigned plannedInvocationCount() const
    {
        return invocationCount > pendingOuterCount ?
            invocationCount : pendingOuterCount;
    }

    /** Outer bases captured while confirming the NDM stride. */
    const std::array<Addr, MaxOuterInvocations> &plannedOuterBases() const
    {
        return pendingOuterBases;
    }

    /** Consume the current NDM plan after the nested vector is launched. */
    void finishVectorization();

    /** Age an active NDM generation by one committed instruction. */
    Result observeCommit();

    /** Accept a distinct, committed outer striding-load candidate. */
    Result acceptOuter(Addr outer_load_pc, Addr outer_address,
                       int64_t outer_stride);

    /** Finish this control-only generation via ordinary inner DVR fallback. */
    Result fallback();

    bool active() const { return currentState != State::Idle; }
    State state() const { return currentState; }
    unsigned threshold() const { return laneThreshold; }
    unsigned innerLaneCount() const { return innerLanes; }
    int64_t innerIncrement() const { return increment; }
    Addr outerBaseAddress() const { return outerAddress; }
    Addr outerLoadPCValue() const { return outerLoadPC; }
    int64_t outerStrideValue() const { return outerStride; }
    const Statistics &statistics() const { return counters; }
    const ControlState &controlState() const { return control; }
    void reset();
    void clear();
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_DVR_NESTED_HH__
