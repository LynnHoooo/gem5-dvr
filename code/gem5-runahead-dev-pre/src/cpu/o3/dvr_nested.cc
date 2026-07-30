#include "cpu/o3/dvr_nested.hh"

#include <cassert>

namespace gem5
{

namespace o3
{

DVRNestedController::DVRNestedController(unsigned max_instructions)
    : maxInstructions(max_instructions)
{
    assert(maxInstructions > 0);
}

DVRNestedController::Result
DVRNestedController::reject(Event event)
{
    if (event == Event::RejectedDepth)
        ++counters.depthRejects;
    else if (event == Event::RejectedParent)
        ++counters.parentRejects;
    else if (event == Event::RejectedOrder)
        ++counters.orderRejects;
    return {event, 0, 0, activeDepth, 0, 0, 0};
}

DVRNestedController::Result
DVRNestedController::startRoot(Addr trigger_pc, InstSeqNum sequence)
{
    if (active())
        return reject(Event::RejectedParent);

    Frame &frame = frames[0];
    frame = {nextId++, 0, trigger_pc, sequence, 0};
    activeDepth = 1;
    ++counters.rootStarts;
    return {Event::Started, frame.id, 0, 1, trigger_pc, 0, 0};
}

DVRNestedController::Result
DVRNestedController::startNested(
    DiscoveryId parent_id, Addr trigger_pc, InstSeqNum sequence)
{
    if (!active() || frames[activeDepth - 1].id != parent_id)
        return reject(Event::RejectedParent);
    if (activeDepth == MaxDepth)
        return reject(Event::RejectedDepth);

    Frame &frame = frames[activeDepth];
    frame = {nextId++, parent_id, trigger_pc, sequence, 0};
    ++activeDepth;
    ++counters.nestedStarts;
    return {
        Event::Started, frame.id, parent_id, activeDepth, trigger_pc, 0, 0};
}

DVRNestedController::Result
DVRNestedController::timeoutTop()
{
    const Frame frame = frames[activeDepth - 1];
    frames[activeDepth - 1] = {};
    if (activeDepth == 1)
        ++counters.rootTimeouts;
    else
        ++counters.nestedTimeouts;
    --activeDepth;
    return {
        Event::TimedOut, frame.id, frame.parentId, activeDepth + 1,
        frame.triggerPC, 0, frame.committedInstructions};
}

DVRNestedController::Result
DVRNestedController::observeCommit(Addr pc, InstSeqNum sequence)
{
    if (!active())
        return {};

    const Frame &top = frames[activeDepth - 1];
    if (activeDepth > 1 && sequence > top.triggerSequence &&
        pc == top.triggerPC) {
        return complete(top.id, pc);
    }

    for (unsigned i = 0; i < activeDepth; ++i)
        ++frames[i].committedInstructions;

    if (frames[activeDepth - 1].committedInstructions >= maxInstructions)
        return timeoutTop();
    return {};
}

DVRNestedController::Result
DVRNestedController::complete(DiscoveryId id, Addr final_load_pc)
{
    if (!active() || frames[activeDepth - 1].id != id)
        return reject(Event::RejectedOrder);

    const Frame frame = frames[activeDepth - 1];
    frames[activeDepth - 1] = {};
    if (activeDepth == 1)
        ++counters.rootCompletions;
    else
        ++counters.nestedCompletions;
    --activeDepth;
    return {
        Event::Completed, frame.id, frame.parentId, activeDepth + 1,
        frame.triggerPC, final_load_pc, frame.committedInstructions};
}

std::optional<DVRNestedController::DiscoveryId>
DVRNestedController::currentId() const
{
    if (!active())
        return std::nullopt;
    return frames[activeDepth - 1].id;
}

std::optional<DVRNestedController::DiscoveryId>
DVRNestedController::rootId() const
{
    if (!active())
        return std::nullopt;
    return frames[0].id;
}

void
DVRNestedController::reset()
{
    frames = {};
    activeDepth = 0;
}

void
DVRNestedController::clear()
{
    reset();
    counters = {};
    nextId = 1;
}

} // namespace o3
} // namespace gem5
