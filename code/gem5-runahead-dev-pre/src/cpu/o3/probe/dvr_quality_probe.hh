/**
 * @file Binds a DVRQualityTracker to one selected L1 data cache.
 *
 * The cache exports exact DemandLookup/Fill/Remove facts on its "DVR Quality"
 * probe.  This listener is the last hop described in README_DVR_REPRO.md
 * section 9.3.6: it forwards those facts to the tracker and publishes the
 * resulting metrics as ordinary gem5 stats.
 */
#ifndef __CPU_O3_PROBE_DVR_QUALITY_PROBE_HH__
#define __CPU_O3_PROBE_DVR_QUALITY_PROBE_HH__

#include "base/statistics.hh"
#include "cpu/o3/dvr_quality.hh"
#include "mem/cache/dvr_quality_event.hh"
#include "params/DVRQualityProbe.hh"
#include "sim/probe/probe.hh"

namespace gem5
{

namespace o3
{

class DVRQualityProbe : public ProbeListenerObject
{
  public:
    DVRQualityProbe(const DVRQualityProbeParams &params);

    void regProbeListeners() override;

    std::string
    name() const override
    {
        return ProbeListenerObject::name() + ".dvrQuality";
    }

  private:
    /** Single entry point for the cache's exact event stream. */
    void handleEvent(const DVRCacheQualityEvent &event);

    /**
     * Non-DVR provenance collapses to Demand.  The tracker only needs to know
     * whether a line came from a DVR helper; an ordinary hardware prefetch is
     * not DVR traffic and must not be credited to DVR accuracy.
     */
    static DVRQualityTracker::FillOrigin
    toFillOrigin(DVRCacheQualityEvent::Origin origin)
    {
        return origin == DVRCacheQualityEvent::Origin::DVR ?
            DVRQualityTracker::FillOrigin::DVR :
            DVRQualityTracker::FillOrigin::Demand;
    }

    DVRQualityTracker tracker;
    DVRQualityTracker::RequestId nextRequestId = 1;

    struct DVRQualityProbeStats : public statistics::Group
    {
        DVRQualityProbeStats(statistics::Group *parent,
                             const DVRQualityTracker &tracker);

        /** Refresh from the tracker; stats are derived, not incremented. */
        void preDumpStats() override;

        const DVRQualityTracker &tracker;

        statistics::Scalar demandAccesses;
        statistics::Scalar issued;
        statistics::Scalar completed;
        statistics::Scalar actualDemandMisses;
        statistics::Scalar shadowDemandMisses;
        statistics::Scalar fills;
        statistics::Scalar redundantFills;
        statistics::Scalar usefulTimely;
        statistics::Scalar usefulLate;
        statistics::Scalar coveredMisses;
        statistics::Scalar unusedEvictions;
        statistics::Scalar pollutionEvictions;
        statistics::Scalar pollutionMisses;
        statistics::Scalar leadTime;

        statistics::Formula fillAccuracy;
        statistics::Formula coverage;
        statistics::Formula averageLeadTime;

        /**
         * usefulLate can only be observed from the CPU-side issue stream,
         * which this cache-only listener does not see.  Timeliness is
         * therefore reported as a raw pair rather than as a ratio that would
         * read as a constant 1.0.  See README_DVR_REPRO.md section 9.3.6.
         */
        statistics::Formula timeliness;
    } dvrQualityStats;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_PROBE_DVR_QUALITY_PROBE_HH__
