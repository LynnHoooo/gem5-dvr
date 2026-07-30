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
    Result observeCommit();

    /** 完成当前栈顶发现并返回结束事件。 */
    Result complete(DiscoveryId id, Addr final_load_pc);

    bool active() const { return activeDepth != 0; }
    unsigned depth() const { return activeDepth; }
    std::optional<DiscoveryId> currentId() const;
    const Statistics &statistics() const { return counters; }

    /** 清除活动帧，但保留累计统计。 */
    void reset();
    /** 同时清除活动帧和累计统计。 */
    void clear();
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_DVR_NESTED_HH__
