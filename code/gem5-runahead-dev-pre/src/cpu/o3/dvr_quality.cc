#include "cpu/o3/dvr_quality.hh"

#include <cassert>
#include <stdexcept>

namespace gem5::o3
{

DVRQualityTracker::ShadowCache::ShadowCache(std::size_t set_count,
                                             std::size_t way_count,
                                             std::size_t line_bytes)
  : sets(set_count), ways(way_count), lineBytes(line_bytes)
{
    if (!set_count || !way_count || !line_bytes)
        throw std::invalid_argument("DVR shadow cache geometry must be nonzero");
}

bool
DVRQualityTracker::ShadowCache::contains(Line line) const
{
    const auto &set = sets[(line / lineBytes) % sets.size()];
    for (Line candidate : set)
        if (candidate == line)
            return true;
    return false;
}

bool
DVRQualityTracker::ShadowCache::access(Line line)
{
    auto &set = sets[(line / lineBytes) % sets.size()];
    for (auto it = set.begin(); it != set.end(); ++it) {
        if (*it != line)
            continue;
        set.splice(set.begin(), set, it);
        return true;
    }
    set.push_front(line);
    if (set.size() > ways)
        set.pop_back();
    return false;
}

void
DVRQualityTracker::ShadowCache::erase(Line line)
{
    auto &set = sets[(line / lineBytes) % sets.size()];
    set.remove(line);
}

DVRQualityTracker::DVRQualityTracker(std::size_t set_count,
                                     std::size_t way_count,
                                     std::size_t line_bytes)
  : demandShadow(set_count, way_count, line_bytes)
{}

void
DVRQualityTracker::issued(RequestId id, Line line, uint64_t bytes, Time now)
{
    if (!outstanding.emplace(id, Request{line, bytes, now}).second)
        throw std::logic_error("duplicate DVR quality request id");
    outstandingByLine.emplace(line, id);
    ++counts.issued;
    counts.issuedBytes += bytes;
}

void
DVRQualityTracker::completed(RequestId id, Time now)
{
    auto it = outstanding.find(id);
    if (it == outstanding.end())
        throw std::logic_error("unknown DVR quality completion id");
    const Request request = it->second;
    auto range = outstandingByLine.equal_range(request.line);
    for (auto pos = range.first; pos != range.second; ++pos) {
        if (pos->second == id) {
            outstandingByLine.erase(pos);
            break;
        }
    }
    outstanding.erase(it);
    ++counts.completed;
    counts.completedBytes += request.bytes;
    (void)now;
}

void
DVRQualityTracker::dropped(RequestId id)
{
    auto it = outstanding.find(id);
    if (it == outstanding.end())
        return;
    auto range = outstandingByLine.equal_range(it->second.line);
    for (auto pos = range.first; pos != range.second; ++pos) {
        if (pos->second == id) {
            outstandingByLine.erase(pos);
            break;
        }
    }
    outstanding.erase(it);
}

void
DVRQualityTracker::demandLookup(Line line, bool actual_hit, Time now)
{
    ++counts.demandAccesses;
    if (!actual_hit)
        ++counts.actualDemandMisses;
    const bool shadow_hit = demandShadow.access(line);
    if (!shadow_hit)
        ++counts.shadowDemandMisses;

    auto useful = prefetched.find(line);
    if (actual_hit && useful != prefetched.end() && !useful->second.used) {
        useful->second.used = true;
        ++counts.usefulTimely;
        if (!shadow_hit)
            ++counts.coveredMisses;
        counts.leadTime += now - useful->second.filledAt;
    } else if (!actual_hit && outstandingByLine.find(line) !=
               outstandingByLine.end() && lateReported.insert(line).second) {
        ++counts.usefulLate;
    }

    if (!actual_hit && shadow_hit && pollutionVictims.erase(line))
        ++counts.pollutionMisses;
    // Any demand reference ends the attribution window for this victim.
    pollutionVictims.erase(line);
}

void
DVRQualityTracker::cacheFill(Line line, FillOrigin origin, Time now,
                             std::optional<Line> victim,
                             std::optional<FillOrigin> victim_origin)
{
    if (victim.has_value() != victim_origin.has_value())
        throw std::invalid_argument("DVR fill victim metadata is incomplete");
    if (victim) {
        auto old_prefetch = prefetched.find(*victim);
        if (old_prefetch != prefetched.end()) {
            if (!old_prefetch->second.used)
                ++counts.unusedEvictions;
            prefetched.erase(old_prefetch);
        }
        pollutionVictims.erase(*victim);
        if (origin == FillOrigin::DVR &&
            *victim_origin == FillOrigin::Demand &&
            demandShadow.contains(*victim)) {
            pollutionVictims.insert(*victim);
            ++counts.pollutionEvictions;
        }
    }
    if (origin == FillOrigin::DVR) {
        ++counts.fills;
        const bool already_used = lateReported.erase(line) != 0;
        if (!prefetched.emplace(line, PrefetchedLine{now, already_used}).second)
            ++counts.redundantFills;
    } else {
        prefetched.erase(line);
        pollutionVictims.erase(line);
    }
}

void
DVRQualityTracker::cacheRemove(Line line, FillOrigin origin)
{
    if (origin == FillOrigin::DVR) {
        auto it = prefetched.find(line);
        if (it != prefetched.end() && !it->second.used)
            ++counts.unusedEvictions;
        prefetched.erase(line);
    }
    pollutionVictims.erase(line);
    lateReported.erase(line);
}

static double ratio(uint64_t n, uint64_t d)
{ return d ? static_cast<double>(n) / d : 0.0; }

double DVRQualityTracker::issuedAccuracy() const
{ return ratio(counts.usefulTimely, counts.issued); }
double DVRQualityTracker::fillAccuracy() const
{ return ratio(counts.usefulTimely, counts.fills); }
double DVRQualityTracker::coverage() const
{ return ratio(counts.coveredMisses, counts.shadowDemandMisses); }
double DVRQualityTracker::timeliness() const
{ return ratio(counts.usefulTimely, counts.usefulTimely + counts.usefulLate); }
double DVRQualityTracker::averageLeadTime() const
{ return ratio(counts.leadTime, counts.usefulTimely); }

} // namespace gem5::o3
