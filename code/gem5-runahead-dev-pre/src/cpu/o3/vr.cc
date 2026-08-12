#include "cpu/o3/vr.hh"

#include "cpu/o3/dyn_inst.hh"

namespace gem5
{

namespace o3
{

VRStrideDetector::VRStrideDetector(unsigned num_entries)
    : entries(num_entries)
{
    assert(num_entries > 0);
}

std::optional<VRStrideDetector::Candidate>
VRStrideDetector::observe(Addr pc, Addr address)
{
    ++timestamp;
    Entry *entry = nullptr;

    for (auto &candidate : entries) {
        if (candidate.valid && candidate.pc == pc) {
            entry = &candidate;
            break;
        }
    }

    if (!entry) {
        entry = &entries.front();
        for (auto &candidate : entries) {
            if (!candidate.valid) {
                entry = &candidate;
                break;
            }
            if (candidate.age < entry->age)
                entry = &candidate;
        }
        *entry = Entry{};
        entry->valid = true;
        entry->pc = pc;
        entry->lastAddress = address;
        entry->age = timestamp;
        return std::nullopt;
    }

    const int64_t observed_stride = static_cast<int64_t>(address) -
                                    static_cast<int64_t>(entry->lastAddress);
    if (observed_stride != 0 && observed_stride == entry->stride) {
        if (entry->confidence < 3)
            ++entry->confidence;
    } else {
        if (entry->confidence > 0)
            --entry->confidence;
        if (entry->confidence == 0)
            entry->stride = observed_stride;
    }

    entry->lastAddress = address;
    entry->age = timestamp;

    // Vector Runahead enters vectorization at confidence == 3.
    if (entry->stride != 0 && entry->confidence >= 3)
        return Candidate{pc, address, entry->stride};
    return std::nullopt;
}

void
VRStrideDetector::setTerminator(Addr pc, Addr terminator)
{
    for (auto &candidate : entries) {
        if (candidate.valid && candidate.pc == pc) {
            candidate.terminator = terminator;
            return;
        }
    }
}

Addr
VRStrideDetector::terminator(Addr pc) const
{
    for (const auto &candidate : entries) {
        if (candidate.valid && candidate.pc == pc)
            return candidate.terminator;
    }
    return 0;
}

void
VRStrideDetector::reset()
{
    for (auto &entry : entries)
        entry = Entry{};
    timestamp = 0;
}

bool
VRVectorTaintTracker::tracked(const RegId &reg) const
{
    return reg.classValue() == IntRegClass && reg.index() < NumTrackedRegs;
}

void
VRVectorTaintTracker::begin(const DynInstPtr &trigger)
{
    reset();
    for (int idx = 0; idx < trigger->numDestRegs(); ++idx)
        if (tracked(trigger->destRegIdx(idx)))
            vectorize |= uint32_t(1) << trigger->destRegIdx(idx).index();
}

VRVectorTaintTracker::Observation
VRVectorTaintTracker::observe(const DynInstPtr &inst)
{
    bool source_vectorized = false;
    bool source_invalid = false;
    for (int idx = 0; idx < inst->numSrcRegs(); ++idx) {
        const RegId &reg = inst->srcRegIdx(idx);
        if (!tracked(reg))
            continue;
        if (vectorize & (uint32_t(1) << reg.index()))
            source_vectorized = true;
        if (invalid & (uint32_t(1) << reg.index()))
            source_invalid = true;
    }

    const bool dependent_load = inst->isLoad() && source_vectorized;

    for (int idx = 0; idx < inst->numDestRegs(); ++idx) {
        const RegId &reg = inst->destRegIdx(idx);
        if (!tracked(reg))
            continue;
        const uint32_t bit = uint32_t(1) << reg.index();
        // Any vectorized source taints the destination as vectorized; any
        // invalid source taints the destination as invalid.
        if (source_vectorized)
            vectorize |= bit;
        else
            vectorize &= ~bit;
        if (source_invalid)
            invalid |= bit;
        else
            invalid &= ~bit;
    }

    return {source_vectorized, source_invalid, dependent_load};
}

void
VRVectorTaintTracker::reset()
{
    vectorize = 0;
    invalid = 0;
}

} // namespace o3
} // namespace gem5
