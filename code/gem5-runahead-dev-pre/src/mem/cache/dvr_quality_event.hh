#ifndef __MEM_CACHE_DVR_QUALITY_EVENT_HH__
#define __MEM_CACHE_DVR_QUALITY_EVENT_HH__

#include <cstdint>
#include <vector>

#include "base/types.hh"

namespace gem5
{

/**
 * Exact cache-side facts exported for DVR quality instrumentation.
 *
 * This deliberately contains no O3 CPU type, so any ProbeListenerArgBase can
 * bind to a selected cache's "DVR Quality" probe without creating a cache ->
 * CPU dependency.
 */
struct DVRCacheQualityEvent
{
    enum class Kind { DemandLookup, Fill, Remove };
    enum class Origin { Demand, DVR, OtherPrefetch };

    struct Victim
    {
        Addr line;
        Origin origin;
        bool secure;
    };

    Kind kind;
    Addr line;
    uint16_t requestorId;
    bool hit;
    Origin origin;
    bool secure;
    std::vector<Victim> victims;
};

} // namespace gem5

#endif // __MEM_CACHE_DVR_QUALITY_EVENT_HH__
