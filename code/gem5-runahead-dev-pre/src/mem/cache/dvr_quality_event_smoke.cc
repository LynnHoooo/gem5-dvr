#include "mem/cache/dvr_quality_event.hh"

#include <cassert>
#include <iostream>

int
main()
{
    using Event = gem5::DVRCacheQualityEvent;
    const Event lookup{
        Event::Kind::DemandLookup, 0x1000, 7, true,
        Event::Origin::Demand, false, {}};
    assert(lookup.hit && lookup.victims.empty());

    const Event fill{
        Event::Kind::Fill, 0x2000, 7, false, Event::Origin::DVR, false,
        {{0x3000, Event::Origin::Demand, false},
         {0x4000, Event::Origin::OtherPrefetch, false}}};
    assert(fill.origin == Event::Origin::DVR);
    assert(fill.victims.size() == 2);
    assert(fill.victims.front().origin == Event::Origin::Demand);
    assert(fill.victims.back().origin == Event::Origin::OtherPrefetch);
    std::cout << "DVR_CACHE_QUALITY_EVENT_SMOKE_PASSED\n";
}
