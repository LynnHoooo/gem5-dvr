#include "cpu/o3/dvr_nested.hh"

#include <cassert>

using gem5::o3::DVRNestedController;
using gem5::o3::DVRNestedDiscoveryMode;

int
main()
{
    DVRNestedController controller(3);
    auto root = controller.startRoot(0x100, 1);
    assert(root.event == DVRNestedController::Event::Started);
    auto child = controller.startNested(root.id, 0x200, 2);
    assert(child.event == DVRNestedController::Event::Started);
    assert(controller.depth() == 2);
    assert(controller.startNested(child.id, 0x300, 3).event ==
           DVRNestedController::Event::RejectedDepth);
    assert(controller.complete(root.id, 0x400).event ==
           DVRNestedController::Event::RejectedOrder);
    assert(controller.complete(child.id, 0x500).event ==
           DVRNestedController::Event::Completed);
    assert(controller.complete(root.id, 0x600).event ==
           DVRNestedController::Event::Completed);

    controller.clear();
    root = controller.startRoot(0x100, 1);
    child = controller.startNested(root.id, 0x200, 2);
    assert(controller.observeCommit(0x104, 3).event ==
           DVRNestedController::Event::None);
    assert(controller.observeCommit(0x104, 4).event ==
           DVRNestedController::Event::None);
    const auto timeout = controller.observeCommit(0x104, 5);
    assert(timeout.event == DVRNestedController::Event::TimedOut);
    assert(timeout.id == child.id);
    assert(controller.depth() == 1);
    assert(controller.statistics().nestedTimeouts == 1);

    DVRNestedDiscoveryMode ndm(64, 3);
    assert(ndm.start(0x200, 1, 64).event ==
           DVRNestedDiscoveryMode::Event::None);
    const auto started = ndm.start(0x200, 2, 63);
    assert(started.event == DVRNestedDiscoveryMode::Event::Started);
    assert(started.innerLoadPC == 0x200);
    assert(started.increment == 2);
    assert(started.innerLanes == 63);
    assert(ndm.start(0x220, 1, 8).event ==
           DVRNestedDiscoveryMode::Event::None);
    assert(ndm.acceptOuter(0x200, 0x1000, 8).event ==
           DVRNestedDiscoveryMode::Event::None);
    const auto outer = ndm.acceptOuter(0x100, 0x2000, 8);
    assert(outer.event == DVRNestedDiscoveryMode::Event::OuterAccepted);
    assert(outer.outerLoadPC == 0x100);
    assert(outer.outerAddress == 0x2000);
    assert(outer.outerStride == 8);
    assert(ndm.fallback().event == DVRNestedDiscoveryMode::Event::Fallback);
    assert(!ndm.active());
    assert(ndm.statistics().attempts == 1);
    assert(ndm.statistics().outerFound == 1);
    assert(ndm.statistics().fallbacks == 1);

    ndm.clear();
    assert(ndm.start(0x300, -1, 4).event ==
           DVRNestedDiscoveryMode::Event::Started);
    assert(ndm.observeCommit().event == DVRNestedDiscoveryMode::Event::None);
    assert(ndm.observeCommit().event == DVRNestedDiscoveryMode::Event::None);
    const auto ndm_timeout = ndm.observeCommit();
    assert(ndm_timeout.event == DVRNestedDiscoveryMode::Event::TimedOut);
    assert(ndm_timeout.committedInstructions == 3);
    assert(!ndm.active());
    assert(ndm.statistics().attempts == 1);
    assert(ndm.statistics().timeouts == 1);
    assert(ndm.statistics().fallbacks == 1);
    return 0;
}
