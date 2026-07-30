#include "cpu/o3/probe/dvr_quality_probe.hh"

#include "base/logging.hh"
#include "sim/core.hh"

namespace gem5
{

namespace o3
{

DVRQualityProbe::DVRQualityProbe(const DVRQualityProbeParams &params)
    : ProbeListenerObject(params),
      tracker(params.sets, params.assoc, params.line_bytes),
      dvrQualityStats(this, tracker)
{
    fatal_if(!params.sets || !params.assoc || !params.line_bytes,
             "%s: shadow-cache geometry must be non-zero", name());
}

void
DVRQualityProbe::regProbeListeners()
{
    typedef ProbeListenerArg<DVRQualityProbe, DVRCacheQualityEvent>
        QualityListener;
    listeners.push_back(new QualityListener(this, "DVR Quality",
                &DVRQualityProbe::handleEvent));
}

void
DVRQualityProbe::handleEvent(const DVRCacheQualityEvent &event)
{
    const auto now = static_cast<DVRQualityTracker::Time>(curTick());

    switch (event.kind) {
      case DVRCacheQualityEvent::Kind::DemandLookup:
        tracker.demandLookup(event.line, event.hit, now);
        break;

      case DVRCacheQualityEvent::Kind::Fill:
        if (event.victims.empty()) {
            tracker.cacheFill(event.line, toFillOrigin(event.origin), now);
        } else {
            // The tracker attributes pollution to a single displaced line.
            // Extra victims of the same fill are still real removals, so
            // report them explicitly instead of dropping their provenance.
            const auto &primary = event.victims.front();
            tracker.cacheFill(event.line, toFillOrigin(event.origin), now,
                              primary.line, toFillOrigin(primary.origin));
            for (std::size_t i = 1; i < event.victims.size(); ++i) {
                tracker.cacheRemove(event.victims[i].line,
                                    toFillOrigin(event.victims[i].origin));
            }
        }
        break;

      case DVRCacheQualityEvent::Kind::Remove:
        tracker.cacheRemove(event.line, toFillOrigin(event.origin));
        break;
    }
}

DVRQualityProbe::DVRQualityProbeStats::DVRQualityProbeStats(
        statistics::Group *parent, const DVRQualityTracker &_tracker)
    : statistics::Group(parent),
      tracker(_tracker),
      ADD_STAT(demandAccesses, statistics::units::Count::get(),
               "Demand tag lookups observed in the measured L1D"),
      ADD_STAT(actualDemandMisses, statistics::units::Count::get(),
               "Demand lookups that actually missed"),
      ADD_STAT(shadowDemandMisses, statistics::units::Count::get(),
               "Demand misses in the counterfactual demand-only shadow cache"),
      ADD_STAT(fills, statistics::units::Count::get(),
               "Lines installed by DVR helper requests"),
      ADD_STAT(redundantFills, statistics::units::Count::get(),
               "DVR fills for a line already installed by DVR"),
      ADD_STAT(usefulTimely, statistics::units::Count::get(),
               "Demand hits on a not-yet-used DVR line"),
      ADD_STAT(usefulLate, statistics::units::Count::get(),
               "Demand misses on a line with a DVR request still in flight"),
      ADD_STAT(coveredMisses, statistics::units::Count::get(),
               "Timely DVR hits that the shadow cache would have missed"),
      ADD_STAT(unusedEvictions, statistics::units::Count::get(),
               "DVR lines evicted or invalidated before any demand use"),
      ADD_STAT(pollutionEvictions, statistics::units::Count::get(),
               "Demand-resident lines displaced by a DVR fill"),
      ADD_STAT(pollutionMisses, statistics::units::Count::get(),
               "Demand misses attributable to a DVR-displaced line"),
      ADD_STAT(leadTime, statistics::units::Tick::get(),
               "Summed fill-to-use distance over timely DVR hits"),

      ADD_STAT(fillAccuracy, statistics::units::Ratio::get(),
               "Timely DVR hits per DVR fill"),
      ADD_STAT(coverage, statistics::units::Ratio::get(),
               "Shadow demand misses eliminated by DVR"),
      ADD_STAT(averageLeadTime, statistics::units::Rate<
                   statistics::units::Tick, statistics::units::Count>::get(),
               "Mean ticks between a DVR fill and its first demand use"),
      ADD_STAT(timeliness, statistics::units::Ratio::get(),
               "Timely share of useful DVR lines; requires the CPU-side "
               "issue stream for usefulLate to be non-zero")
{
    fillAccuracy = usefulTimely / fills;
    coverage = coveredMisses / shadowDemandMisses;
    averageLeadTime = leadTime / usefulTimely;
    timeliness = usefulTimely / (usefulTimely + usefulLate);

    fillAccuracy.precision(6);
    coverage.precision(6);
    averageLeadTime.precision(2);
    timeliness.precision(6);
}

void
DVRQualityProbe::DVRQualityProbeStats::preDumpStats()
{
    statistics::Group::preDumpStats();

    // The tracker owns the accounting; these stats are a view of it, so
    // assign rather than accumulate.
    const auto &c = tracker.counters();
    demandAccesses = c.demandAccesses;
    actualDemandMisses = c.actualDemandMisses;
    shadowDemandMisses = c.shadowDemandMisses;
    fills = c.fills;
    redundantFills = c.redundantFills;
    usefulTimely = c.usefulTimely;
    usefulLate = c.usefulLate;
    coveredMisses = c.coveredMisses;
    unusedEvictions = c.unusedEvictions;
    pollutionEvictions = c.pollutionEvictions;
    pollutionMisses = c.pollutionMisses;
    leadTime = c.leadTime;
}

} // namespace o3
} // namespace gem5
