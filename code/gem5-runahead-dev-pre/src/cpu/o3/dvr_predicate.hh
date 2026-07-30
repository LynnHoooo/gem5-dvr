#ifndef __CPU_O3_DVR_PREDICATE_HH__
#define __CPU_O3_DVR_PREDICATE_HH__

#include <array>
#include <cstdint>

namespace gem5
{
namespace o3
{

/**
 * Collect the real values returned by DVR source loads into per-path lane
 * masks.  This class deliberately knows nothing about lane numbers beyond
 * their bit positions: a path is selected only when a learned value
 * predicate matches the value returned for that lane.
 */
class DVRLanePredicateTracker
{
  public:
    static constexpr unsigned MaxLanes = 128;
    static constexpr unsigned MaxPaths = 4;
    using LaneMask = std::array<uint64_t, 2>;

    enum class Observation
    {
        Selected,
        NoMatch,
        Ambiguous,
        Duplicate,
        InvalidLane
    };

    void begin(unsigned lanes, unsigned paths,
               const std::array<uint64_t, MaxPaths> &masks,
               const std::array<uint64_t, MaxPaths> &patterns);
    Observation observe(unsigned lane, uint64_t value);
    void reset();

    unsigned laneCount() const { return lanes; }
    unsigned pathCount() const { return paths; }
    const LaneMask &pathMask(unsigned path) const;
    const LaneMask &unmatchedMask() const { return unmatched; }
    unsigned selectedPaths() const;
    bool divergent() const { return selectedPaths() > 1; }

  private:
    unsigned lanes = 0;
    unsigned paths = 0;
    std::array<uint64_t, MaxPaths> masks = {};
    std::array<uint64_t, MaxPaths> patterns = {};
    std::array<LaneMask, MaxPaths> selected = {};
    LaneMask unmatched = {};
    LaneMask observed = {};
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_DVR_PREDICATE_HH__
