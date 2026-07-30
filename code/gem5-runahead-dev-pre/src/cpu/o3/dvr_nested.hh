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
 * 第一阶段只负责低 lane 数触发、IR/ILR 状态、已提交 outer stride 候选和
 * timeout/fallback。它不生成 helper，也不假装已经完成 branch inversion 或
 * inner-lane flatten。
 */
class DVRNestedDiscoveryMode
{
  public:
    enum class State { Idle, SeekingOuter, OuterFound };
    enum class Event { None, Started, OuterAccepted, TimedOut, Fallback };

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
    };

    struct Statistics
    {
        uint64_t attempts = 0;
        uint64_t outerFound = 0;
        uint64_t fallbacks = 0;
        uint64_t timeouts = 0;
    };

  private:
    State currentState = State::Idle;
    unsigned laneThreshold;
    unsigned maxInstructions;
    unsigned committedInstructions = 0;
    Addr innerLoadPC = 0;
    int64_t increment = 0;
    unsigned innerLanes = 0;
    Addr outerLoadPC = 0;
    Addr outerAddress = 0;
    int64_t outerStride = 0;
    Statistics counters = {};

    Result snapshot(Event event) const;

  public:
    DVRNestedDiscoveryMode(unsigned lane_threshold,
                           unsigned max_instructions);

    /** Start NDM only for a trusted, non-empty lane count below threshold. */
    Result start(Addr inner_load_pc, int64_t loop_increment,
                 unsigned inner_lanes);

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
    const Statistics &statistics() const { return counters; }
    void reset();
    void clear();
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_DVR_NESTED_HH__
