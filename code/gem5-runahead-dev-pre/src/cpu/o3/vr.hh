/*
 * Copyright (c) 2026 Matrixera. All rights reserved.
 *
 * Vector Runahead (VR) reproduction -- ISCA 2020, Naithani et al.
 *
 * This is an ISA-adapted RISC-V/gem5 prototype of Vector Runahead built on
 * top of the Precise Runahead Execution (PRE) substrate already present in
 * this tree (src/cpu/o3/pre.{cc,hh}).  It reuses the DVR building blocks
 * (DVRInstructionRecorder for chain capture, DVRVectorRenameTable for the
 * VRAT) read-only and adds the VR-specific structures below.
 *
 * The fork has no real RVV vector instructions (DummyVecRegContainer), so
 * vectorization is modeled microarchitecturally: a "vector gather" is a
 * group of per-lane addresses issued as real prefetches through the L1D
 * data port, mirroring how the DVR helper issues its timing-path
 * prefetches.
 */

#ifndef __CPU_O3_VR_HH__
#define __CPU_O3_VR_HH__

#include <array>
#include <deque>
#include <optional>
#include <vector>

#include "base/types.hh"
#include "config/the_isa.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/pre.hh"
#include "cpu/reg_class.hh"
#include "mem/packet.hh"

namespace gem5
{

namespace o3
{

class CPU;

/**
 * VR stride detector: a reference prediction table indexed by load PC.
 *
 * Each entry keeps the last accessed address, the last observed stride, a
 * 2-bit saturating confidence counter and the terminator -- the PC of the
 * last dependent load in the chain rooted at this striding load.  Vector
 * Runahead enters vectorization only once confidence reaches 3 (the paper
 * Section III-B), unlike the DVR candidate threshold of 2.
 */
class VRStrideDetector
{
  public:
    struct Candidate
    {
        Addr pc;
        Addr address;
        int64_t stride;
    };

  private:
    struct Entry
    {
        bool valid = false;
        Addr pc = 0;
        Addr lastAddress = 0;
        int64_t stride = 0;
        uint8_t confidence = 0;
        Addr terminator = 0;
        uint64_t age = 0;
    };

    std::vector<Entry> entries;
    uint64_t timestamp = 0;

  public:
    explicit VRStrideDetector(unsigned num_entries);

    /** Observe a load execution; report a vector-runahead candidate when
     *  the entry reaches confidence 3 with a non-zero stride. */
    std::optional<Candidate> observe(Addr pc, Addr address);

    /** Learn the chain terminator for a striding load PC (filled in during
     *  a vector-runahead round, paper Section III-B field 4). */
    void setTerminator(Addr pc, Addr terminator);

    /** Terminator previously learned for a striding load PC (0 if none). */
    Addr terminator(Addr pc) const;

    void reset();
};

/**
 * VR taint vector: two bits per architectural integer register --
 * vectorize and invalid -- propagated along register dependences
 * (paper Section III-D).  Instructions with no tag are loop-invariant for
 * the current round; invalid tags discard the instruction; vectorize tags
 * vectorize it.
 */
class VRVectorTaintTracker
{
  private:
    static constexpr unsigned NumTrackedRegs = 32;
    uint32_t vectorize = 0;
    uint32_t invalid = 0;

    bool tracked(const RegId &reg) const;

  public:
    struct Observation
    {
        bool vectorized = false;
        bool invalid = false;
        bool dependentLoad = false;
    };

    /** Start a round: only the triggering striding load's destination is
     *  marked vectorized. */
    void begin(const DynInstPtr &trigger);

    /** Propagate taint across one instruction's register operands. */
    Observation observe(const DynInstPtr &inst);

    void reset();
};

/** State of a single vector-runahead round. */
struct VRRound
{
    bool active = false;
    Addr triggerPC = 0;
    Addr triggerAddress = 0;
    InstSeqNum triggerSequence = 0;
    ThreadID tid = 0;
    int64_t stride = 0;
    unsigned lanes = 0;
    unsigned instructions = 0;
    unsigned unrollsIssued = 0;
    unsigned pipelineGroups = 0;
    unsigned outstanding = 0;
    uint64_t activeLaneMask = 0;
    bool draining = false;
    Addr terminator = 0;
    // Architectural RVV state captured at round entry.
    unsigned vl = 0;
    unsigned vstart = 0;
    unsigned sew = 64;
    int lmul = 0;
    uint64_t v0Mask = ~uint64_t(0);
};

/** One memory request in the VR prefetch queue. */
struct VRPrefetchEntry
{
    Addr address;
    Addr pc;
    ThreadID tid;
    bool source;     // striding (level-0) load; ReadReq so data feeds the chain
    unsigned level;  // 0 = striding gather, 1+ = dependent gather level
    unsigned lane;   // lane index within the gather
    unsigned group = 0; // software-pipelined unroll group
    unsigned nextUop = 1; // next recorded uop to evaluate on a response
    int8_t valueReg = -1; // register receiving the previous load value
    uint8_t loadBytes = 8;
    bool loadSigned = true;
};

/** Sender state attached to each VR prefetch packet. */
struct VRPrefetchSenderState : public Packet::SenderState
{
    VRPrefetchSenderState(bool is_source, unsigned chain_level,
                          unsigned lane_index, ThreadID thread_id,
                          unsigned next_uop = 1, int8_t value_reg = -1,
                          unsigned group_id = 0, uint8_t load_bytes = 8,
                          bool load_signed = true)
        : source(is_source), level(chain_level), lane(lane_index),
          tid(thread_id), nextUop(next_uop), valueReg(value_reg),
          group(group_id), loadBytes(load_bytes), loadSigned(load_signed)
    {}

    bool source;
    unsigned level;
    unsigned lane;
    ThreadID tid;
    unsigned nextUop;
    int8_t valueReg;
    unsigned group;
    uint8_t loadBytes;
    bool loadSigned;
};

/** Explicit per-lane vector register file used by the VR helper engine. */
class VRVectorRegisterFile
{
  public:
    static constexpr unsigned MaxGroups = 8;
    static constexpr unsigned MaxLanes = 64;
    static constexpr unsigned NumRegs = 32;
    using Register = std::array<std::array<RegVal, MaxLanes>, NumRegs>;

  private:
    std::array<Register, MaxGroups> values = {};
    std::array<uint64_t, MaxGroups> validMask = {};

  public:
    void reset();
    void seed(unsigned group,
              const DVRLoopBoundDetector::RegisterSnapshot &regs,
              unsigned lanes);
    RegVal read(unsigned group, unsigned reg, unsigned lane) const;
    void write(unsigned group, unsigned reg, unsigned lane, RegVal value);
    void invalidate(unsigned group, unsigned lane);
    uint64_t mask(unsigned group) const;
};

} // namespace o3
} // namespace gem5

#endif //__CPU_O3_VR_HH__
