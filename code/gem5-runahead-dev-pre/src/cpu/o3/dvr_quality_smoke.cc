#include "dvr_quality.hh"

#include <cassert>
#include <cmath>
#include <iostream>

using gem5::o3::DVRQualityTracker;

int main()
{
    using Origin = DVRQualityTracker::FillOrigin;
    DVRQualityTracker q(2, 2);
    // Physical block addresses need block-size normalization before indexing.
    DVRQualityTracker physical(2, 1, 64);
    physical.demandLookup(0, false, 0);
    physical.demandLookup(64, false, 1);
    physical.demandLookup(0, true, 2);
    assert(physical.counters().shadowDemandMisses == 2);

    q.generated();
    q.issued(1, 4, 64, 10);
    q.completed(1, 20);
    q.cacheFill(4, Origin::DVR, 20);
    q.demandLookup(4, true, 35); // timely and useful

    // A demand-only resident can be a useful prefetch hit without covering a
    // counterfactual miss; accuracy and coverage must not share a numerator.
    q.demandLookup(8, false, 36);
    q.cacheFill(8, Origin::Demand, 37);
    q.generated();
    q.issued(4, 8, 64, 38);
    q.completed(4, 39);
    q.cacheFill(8, Origin::DVR, 39);
    q.demandLookup(8, true, 40);

    q.generated();
    q.issued(2, 5, 64, 40);
    q.demandLookup(5, false, 45); // late; also a shadow miss
    q.completed(2, 50);
    q.cacheFill(5, Origin::DVR, 50);

    // Establish a demand-only resident, then have a DVR fill evict it.
    q.demandLookup(2, false, 60);
    q.cacheFill(2, Origin::Demand, 70);
    q.generated();
    q.issued(3, 6, 64, 75);
    q.completed(3, 80);
    q.cacheFill(6, Origin::DVR, 80, 2, Origin::Demand);
    q.demandLookup(2, false, 90); // shadow hit => attributable pollution

    const auto &c = q.counters();
    assert(c.generated == 4 && c.issued == 4 && c.completed == 4);
    assert(c.issuedBytes == 256 && c.completedBytes == 256);
    assert(c.fills == 4);
    assert(c.usefulTimely == 2 && c.usefulLate == 1);
    assert(c.coveredMisses == 1);
    assert(c.pollutionEvictions == 1 && c.pollutionMisses == 1);
    assert(c.shadowDemandMisses == 4);
    assert(std::abs(q.issuedAccuracy() - 0.5) < 1e-12);
    assert(std::abs(q.fillAccuracy() - 0.5) < 1e-12);
    assert(std::abs(q.coverage() - 0.25) < 1e-12);
    assert(std::abs(q.timeliness() - 2.0 / 3.0) < 1e-12);
    assert(q.averageLeadTime() == 8.0);
    std::cout << "DVR_QUALITY_SMOKE_PASSED\n";
}
