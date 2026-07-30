#include "cpu/o3/dvr_predicate.hh"

#include <algorithm>
#include <cassert>

namespace gem5
{
namespace o3
{

void
DVRLanePredicateTracker::begin(
    unsigned requested_lanes, unsigned requested_paths,
    const std::array<uint64_t, MaxPaths> &path_masks,
    const std::array<uint64_t, MaxPaths> &path_patterns)
{
    reset();
    lanes = std::min(requested_lanes, MaxLanes);
    paths = std::min(requested_paths, MaxPaths);
    masks = path_masks;
    patterns = path_patterns;
    for (unsigned path = 0; path < paths; ++path)
        patterns[path] &= masks[path];
}

DVRLanePredicateTracker::Observation
DVRLanePredicateTracker::observe(unsigned lane, uint64_t value)
{
    if (lane >= lanes)
        return Observation::InvalidLane;
    const uint64_t bit = uint64_t(1) << (lane % 64);
    if (observed[lane / 64] & bit)
        return Observation::Duplicate;
    observed[lane / 64] |= bit;

    unsigned match = MaxPaths;
    unsigned matches = 0;
    for (unsigned path = 0; path < paths; ++path) {
        // With multiple paths, an empty discriminator cannot identify one.
        if (paths > 1 && masks[path] == 0)
            continue;
        if ((value & masks[path]) == patterns[path]) {
            match = path;
            ++matches;
        }
    }

    if (matches != 1) {
        unmatched[lane / 64] |= bit;
        return matches == 0 ? Observation::NoMatch : Observation::Ambiguous;
    }
    selected[match][lane / 64] |= bit;
    return Observation::Selected;
}

void
DVRLanePredicateTracker::reset()
{
    lanes = 0;
    paths = 0;
    masks = {};
    patterns = {};
    selected = {};
    unmatched = {};
    observed = {};
}

const DVRLanePredicateTracker::LaneMask &
DVRLanePredicateTracker::pathMask(unsigned path) const
{
    assert(path < paths);
    return selected[path];
}

unsigned
DVRLanePredicateTracker::selectedPaths() const
{
    unsigned count = 0;
    for (unsigned path = 0; path < paths; ++path) {
        if (selected[path][0] || selected[path][1])
            ++count;
    }
    return count;
}

} // namespace o3
} // namespace gem5
