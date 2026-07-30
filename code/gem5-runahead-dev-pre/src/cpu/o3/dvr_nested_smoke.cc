#include "cpu/o3/dvr_nested.hh"

#include <cassert>

using gem5::o3::DVRNestedController;

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
    assert(controller.observeCommit().event ==
           DVRNestedController::Event::None);
    assert(controller.observeCommit().event ==
           DVRNestedController::Event::None);
    const auto timeout = controller.observeCommit();
    assert(timeout.event == DVRNestedController::Event::TimedOut);
    assert(timeout.id == child.id);
    assert(controller.depth() == 1);
    assert(controller.statistics().nestedTimeouts == 1);
    return 0;
}
