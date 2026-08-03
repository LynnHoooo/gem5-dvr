#include "cpu/o3/dvr_nested.hh"

#include <cassert>

using gem5::o3::DVRNestedDiscoveryMode;

int
main()
{
    DVRNestedDiscoveryMode ndm(64, 8);
    auto started = ndm.start(0x100, 8, 16);
    assert(started.event == DVRNestedDiscoveryMode::Event::Started);

    ndm.captureLoopRegisters(5, 6, 12, 64, 4);
    assert(ndm.controlState().inductionRegister == 5);
    assert(ndm.controlState().boundRegister == 6);
    assert(ndm.controlState().remainingIterations == 4);

    assert(ndm.observeInnerBranch(0x140, 0x100, 0x144, true));
    assert(ndm.controlState().valid);
    assert(ndm.controlState().branchInverted);
    assert(ndm.controlState().innerBranchTarget == 0x100);
    assert(ndm.controlState().innerFallthrough == 0x144);

    auto outer = ndm.acceptOuter(0x200, 0x8000, 64);
    assert(outer.event == DVRNestedDiscoveryMode::Event::OuterAccepted);
    assert(ndm.recordOuterInvocation(0x8000, 16));
    assert(!ndm.readyToVectorize());
    assert(ndm.recordOuterInvocation(0x8040, 12));
    assert(ndm.readyToVectorize());
    assert(ndm.outerInvocationCount() == 2);
    ndm.finishVectorization();
    assert(!ndm.active());

    DVRNestedDiscoveryMode timeout(64, 2);
    assert(timeout.start(0x100, 8, 8).event ==
           DVRNestedDiscoveryMode::Event::Started);
    assert(timeout.observeCommit().event == DVRNestedDiscoveryMode::Event::None);
    assert(timeout.observeCommit().event ==
           DVRNestedDiscoveryMode::Event::TimedOut);
    assert(!timeout.active());
    return 0;
}
