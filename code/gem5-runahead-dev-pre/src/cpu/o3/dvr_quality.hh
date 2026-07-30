#ifndef __CPU_O3_DVR_QUALITY_HH__
#define __CPU_O3_DVR_QUALITY_HH__

#include <cstddef>
#include <cstdint>
#include <list>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gem5::o3
{

/**
 * Event-driven DVR prefetch quality accounting.
 *
 * This class deliberately does not infer cache events from request latency.
 * The caller must report the cache lookup result, fill, and eviction observed
 * by the measured cache.  Coverage uses an internal, demand-only LRU shadow
 * cache with the same geometry.  Consequently its definitions are auditable:
 * no "possibly useful" proxy is promoted to an accuracy/coverage result.
 */
class DVRQualityTracker
{
  public:
    using Line = uint64_t;
    using RequestId = uint64_t;
    using Time = uint64_t;

    enum class FillOrigin { Demand, DVR };

    struct Counters
    {
        uint64_t generated = 0;
        uint64_t issued = 0;
        uint64_t issuedBytes = 0;
        uint64_t completed = 0;
        uint64_t completedBytes = 0;
        uint64_t fills = 0;
        uint64_t redundantFills = 0;
        uint64_t usefulTimely = 0;
        uint64_t usefulLate = 0;
        uint64_t coveredMisses = 0;
        uint64_t unusedEvictions = 0;
        uint64_t demandAccesses = 0;
        uint64_t demandAddressesObserved = 0;
        uint64_t actualDemandMisses = 0;
        uint64_t shadowDemandMisses = 0;
        uint64_t pollutionEvictions = 0;
        uint64_t pollutionMisses = 0;
        uint64_t leadTime = 0;
    };

  private:
    struct Request { Line line; uint64_t bytes; Time issuedAt; };
    struct PrefetchedLine { Time filledAt; bool used = false; };

    class ShadowCache
    {
      private:
        std::vector<std::list<Line>> sets;
        std::size_t ways;
        std::size_t lineBytes;
      public:
        ShadowCache(std::size_t set_count, std::size_t way_count,
                    std::size_t line_bytes);
        bool access(Line line);
        bool contains(Line line) const;
        void erase(Line line);
    };

    Counters counts = {};
    ShadowCache demandShadow;
    std::unordered_map<RequestId, Request> outstanding;
    std::unordered_multimap<Line, RequestId> outstandingByLine;
    std::unordered_map<Line, PrefetchedLine> prefetched;
    std::unordered_set<Line> lateReported;
    // Demand-origin lines directly displaced by DVR fills.  A later actual
    // miss is pollution only while the demand-only shadow still contains it.
    std::unordered_set<Line> pollutionVictims;

  public:
    DVRQualityTracker(std::size_t set_count, std::size_t way_count,
                      std::size_t line_bytes = 1);

    void generated() { ++counts.generated; }
    void issued(RequestId id, Line line, uint64_t bytes, Time now);
    void completed(RequestId id, Time now);
    void dropped(RequestId id);

    /** Address-only observation; does not claim a cache hit or miss. */
    void demandAddressObserved() { ++counts.demandAddressesObserved; }

    /** Report an actual tag lookup before any demand fill is installed. */
    void demandLookup(Line line, bool actual_hit, Time now);

    /**
     * Report installation in the measured cache.  evicted_line and
     * evicted_origin must describe the actual victim, if any.
     */
    void cacheFill(Line line, FillOrigin origin, Time now,
                   std::optional<Line> evicted_line = std::nullopt,
                   std::optional<FillOrigin> evicted_origin = std::nullopt);

    /** Report non-fill removal (invalidate/coherence/replacement). */
    void cacheRemove(Line line, FillOrigin origin);

    const Counters &counters() const { return counts; }
    double issuedAccuracy() const;
    double fillAccuracy() const;
    double coverage() const;
    double timeliness() const;
    double averageLeadTime() const;
};

} // namespace gem5::o3

#endif
