#include "cpu/o3/dvr_predicate.hh"

#include <array>
#include <cassert>

using gem5::o3::DVRLanePredicateTracker;

int
main()
{
    DVRLanePredicateTracker tracker;
    // Learned discriminator bit 8 separates two genuine source-value paths.
    tracker.begin(128, 2, {0x100, 0x100, 0, 0},
                  {0x000, 0x100, 0, 0});
    for (unsigned lane = 0; lane < 128; ++lane) {
        // Deliberately use groups of three, not lane parity.
        const uint64_t value = ((lane / 3) & 1) ? 0x134 : 0x034;
        assert(tracker.observe(lane, value) ==
               DVRLanePredicateTracker::Observation::Selected);
    }
    assert(tracker.divergent());
    assert(tracker.selectedPaths() == 2);
    assert(tracker.pathMask(0)[0] != 0 && tracker.pathMask(0)[1] != 0);
    assert(tracker.pathMask(1)[0] != 0 && tracker.pathMask(1)[1] != 0);

    tracker.begin(2, 2, {0x3, 0x1, 0, 0}, {0x1, 0x1, 0, 0});
    assert(tracker.observe(0, 1) ==
           DVRLanePredicateTracker::Observation::Ambiguous);
    assert(tracker.observe(1, 2) ==
           DVRLanePredicateTracker::Observation::NoMatch);
    assert(tracker.observe(1, 1) ==
           DVRLanePredicateTracker::Observation::Duplicate);
    assert(tracker.unmatchedMask()[0] == 0x3);
    return 0;
}
