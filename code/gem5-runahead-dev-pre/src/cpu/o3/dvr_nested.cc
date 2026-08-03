#include "cpu/o3/dvr_nested.hh"

#include <algorithm>
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

DVRNestedDiscoveryMode::DVRNestedDiscoveryMode(
    unsigned lane_threshold, unsigned max_instructions)
    : laneThreshold(lane_threshold), maxInstructions(max_instructions)
{
    assert(laneThreshold > 0);
    assert(maxInstructions > 0);
}

DVRNestedDiscoveryMode::Result
DVRNestedDiscoveryMode::snapshot(Event event) const
{
    return {event, currentState, innerLoadPC, increment, innerLanes,
            outerLoadPC, outerAddress, outerStride, committedInstructions,
            control};
}

DVRNestedDiscoveryMode::Result
DVRNestedDiscoveryMode::start(
    Addr inner_load_pc, int64_t loop_increment, unsigned inner_lanes)
{
    if (active() || inner_load_pc == 0 || inner_lanes == 0 ||
        inner_lanes >= laneThreshold) {
        return snapshot(Event::None);
    }

    reset();
    currentState = State::SeekingOuter;
    innerLoadPC = inner_load_pc;
    increment = loop_increment;
    innerLanes = inner_lanes;
    control = {};
    control.inductionIncrement = loop_increment;
    ++counters.attempts;
    return snapshot(Event::Started);
}

void
DVRNestedDiscoveryMode::captureLoopRegisters(
    int8_t induction_register, int8_t bound_register,
    RegVal induction_value, RegVal bound_value,
    uint64_t remaining_iterations)
{
    if (!active())
        return;
    control.inductionRegister = induction_register;
    control.boundRegister = bound_register;
    control.inductionValue = induction_value;
    control.boundValue = bound_value;
    control.remainingIterations = remaining_iterations;
    control.inductionIncrement = increment;
    if (induction_register >= 0)
        ++counters.ilrCaptures;
    if (bound_register >= 0)
        ++counters.lcrCaptures;
}

bool
DVRNestedDiscoveryMode::observeInnerBranch(
    Addr branch_pc, Addr branch_target, Addr fallthrough_pc, bool taken)
{
    if (!active() || branch_pc <= innerLoadPC ||
        branch_target > innerLoadPC || branch_target >= branch_pc)
        return false;
    if (control.valid)
        return false;

    control.innerBranchPC = branch_pc;
    control.innerBranchTarget = branch_target;
    control.innerFallthrough = fallthrough_pc;
    // NDM executes the loop body once more, then redirects the helper to the
    // outer-loop search path.  The recorded direction is retained for
    // auditing; the inverted branch is independent of the main-thread
    // predictor outcome.
    control.branchTaken = taken;
    control.branchInverted = true;
    control.reconvergencePC = fallthrough_pc;
    const unsigned active_lanes = std::min(innerLanes, 128U);
    for (unsigned lane = 0; lane < active_lanes; ++lane) {
        const unsigned word = lane / 64;
        const uint64_t bit = uint64_t(1) << (lane % 64);
        // The original taken path is parked on the reconvergence stack.  The
        // NDM shadow executes the inverted, fall-through path immediately.
        control.takenMask[word] |= bit;
        control.fallthroughMask[word] |= bit;
    }
    control.valid = true;
    ++counters.irCaptures;
    ++counters.branchInversions;
    return true;
}

bool
DVRNestedDiscoveryMode::recordOuterInvocation(
    Addr inner_start, unsigned lanes, Addr inner_trigger_pc,
    Addr inner_flr_pc, int64_t inner_stride)
{
    if ((currentState != State::OuterFound &&
         currentState != State::Vectorizing) ||
        pendingOuterConsumed >= pendingOuterCount || inner_start == 0 || lanes == 0 ||
        inner_trigger_pc == 0 || inner_flr_pc == 0 || !control.valid)
        return false;
    ++counters.outerInvocations;
    if (invocationCount < MaxOuterInvocations) {
        auto &invocation = invocations[invocationCount];
        invocation.outerBase = pendingOuterBases[pendingOuterConsumed++];
        invocation.innerStart = inner_start;
        invocation.innerLanes = std::min(lanes, 128U);
        invocation.innerTriggerPC = inner_trigger_pc;
        invocation.innerFLRPC = inner_flr_pc;
        invocation.innerStride = inner_stride;
        invocation.predicate = control.fallthroughMask;
        invocation.reconvergencePC = control.reconvergencePC;
        ++invocationCount;
    }
    // NDM needs at least two distinct outer invocations before flattening;
    // the CPU-side invocation batch remains bounded by the same eight-entry
    // hardware structure.
    if (invocationCount >= 2)
        currentState = State::Vectorizing;
    return true;
}

DVRNestedDiscoveryMode::Result
DVRNestedDiscoveryMode::observeOuterLoad(Addr pc, Addr address)
{
    if (!control.valid || pc == 0 || pc == innerLoadPC || address == 0) {
        return snapshot(Event::None);
    }
    if (currentState == State::OuterFound) {
        if (pc == outerLoadPC &&
            static_cast<int64_t>(address) -
                static_cast<int64_t>(outerAddress) == outerStride) {
            outerAddress = address;
            if (pendingOuterCount < pendingOuterBases.size())
                pendingOuterBases[pendingOuterCount++] = address;
        }
        return snapshot(Event::None);
    }
    if (currentState != State::SeekingOuter)
        return snapshot(Event::None);

    OuterProbe *probe = nullptr;
    for (auto &candidate : outerProbes) {
        if (candidate.pc == pc) {
            probe = &candidate;
            break;
        }
        if (candidate.pc == 0 && probe == nullptr)
            probe = &candidate;
    }
    if (probe == nullptr)
        return snapshot(Event::None);
    if (probe->pc == 0) {
        probe->pc = pc;
        probe->address = address;
        probe->samples = 1;
        probe->recentAddresses[0] = address;
        return snapshot(Event::None);
    }

    const int64_t delta = static_cast<int64_t>(address) -
        static_cast<int64_t>(probe->address);
    probe->address = address;
    probe->recentAddresses[2] = probe->recentAddresses[1];
    probe->recentAddresses[1] = probe->recentAddresses[0];
    probe->recentAddresses[0] = address;
    if (delta == 0) {
        probe->samples = 1;
        probe->stride = 0;
        return snapshot(Event::None);
    }
    if (probe->samples == 1) {
        probe->stride = delta;
        probe->samples = 2;
        return snapshot(Event::None);
    }
    if (delta != probe->stride) {
        probe->stride = delta;
        probe->samples = 2;
        return snapshot(Event::None);
    }

    ++probe->samples;
    const Result result = acceptOuter(pc, address, probe->stride);
    // Preserve the exact three committed addresses that established the
    // stride.  Each independently bounded child consumes one FIFO entry.
    for (int index = 2; index >= 0; --index) {
        if (probe->recentAddresses[index] != 0 &&
            pendingOuterCount < pendingOuterBases.size()) {
            pendingOuterBases[pendingOuterCount++] =
                probe->recentAddresses[index];
        }
    }
    return result;
}

void
DVRNestedDiscoveryMode::finishVectorization()
{
    if (currentState == State::Vectorizing)
        reset();
}

DVRNestedDiscoveryMode::Result
DVRNestedDiscoveryMode::observeCommit()
{
    if (!active())
        return {};

    ++committedInstructions;
    const uint64_t budget = currentState == State::SeekingOuter ?
        maxInstructions :
        static_cast<uint64_t>(maxInstructions) * MaxOuterInvocations;
    if (committedInstructions < budget)
        return snapshot(Event::None);

    const Result result = snapshot(Event::TimedOut);
    ++counters.timeouts;
    ++counters.fallbacks;
    reset();
    return result;
}

DVRNestedDiscoveryMode::Result
DVRNestedDiscoveryMode::acceptOuter(
    Addr outer_load_pc, Addr outer_address, int64_t outer_stride)
{
    if (currentState != State::SeekingOuter || !control.valid ||
        outer_load_pc == 0 || outer_load_pc == innerLoadPC ||
        outer_stride == 0) {
        return snapshot(Event::None);
    }

    currentState = State::OuterFound;
    outerLoadPC = outer_load_pc;
    outerAddress = outer_address;
    outerStride = outer_stride;
    // Searching and invocation collection are distinct NDM phases.  Give the
    // latter its own bounded window; otherwise a stride confirmed near the
    // search deadline can never collect even one independently closed child.
    committedInstructions = 0;
    ++counters.outerFound;
    return snapshot(Event::OuterAccepted);
}

DVRNestedDiscoveryMode::Result
DVRNestedDiscoveryMode::fallback()
{
    if (!active())
        return {};

    const Result result = snapshot(Event::Fallback);
    ++counters.fallbacks;
    reset();
    return result;
}

void
DVRNestedDiscoveryMode::reset()
{
    currentState = State::Idle;
    committedInstructions = 0;
    innerLoadPC = 0;
    increment = 0;
    innerLanes = 0;
    outerLoadPC = 0;
    outerAddress = 0;
    outerStride = 0;
    invocations = {};
    invocationCount = 0;
    outerProbes = {};
    pendingOuterBases = {};
    pendingOuterCount = 0;
    pendingOuterConsumed = 0;
    control = {};
}

void
DVRNestedDiscoveryMode::clear()
{
    reset();
    counters = {};
}

} // namespace o3
} // namespace gem5
