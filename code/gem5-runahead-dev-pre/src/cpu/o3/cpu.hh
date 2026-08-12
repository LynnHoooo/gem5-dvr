/*
 * Copyright (c) 2011-2013, 2016-2020 ARM Limited
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2004-2005 The Regents of The University of Michigan
 * Copyright (c) 2011 Regents of the University of California
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __CPU_O3_CPU_HH__
#define __CPU_O3_CPU_HH__

#include <iostream>
#include <list>
#include <memory>
#include <queue>
#include <deque>
#include <set>
#include <utility>
#include <vector>

#include "arch/generic/pcstate.hh"
#include "base/statistics.hh"
#include "config/the_isa.hh"
#include "cpu/o3/comm.hh"
#include "cpu/o3/commit.hh"
#include "cpu/o3/decode.hh"
#include "cpu/o3/dvr_nested.hh"
#include "cpu/o3/dvr_predicate.hh"
#include "cpu/o3/dvr_quality.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/fetch.hh"
#include "cpu/o3/free_list.hh"
#include "cpu/o3/iew.hh"
#include "cpu/o3/limits.hh"
#include "cpu/o3/pre.hh"
#include "cpu/o3/vr.hh"
#include "cpu/o3/rename.hh"
#include "cpu/o3/rob.hh"
#include "cpu/o3/scoreboard.hh"
#include "cpu/o3/thread_state.hh"
#include "cpu/activity.hh"
#include "cpu/base.hh"
#include "cpu/simple_thread.hh"
#include "cpu/timebuf.hh"
#include "params/BaseO3CPU.hh"
#include "sim/process.hh"

namespace gem5
{

/**
 * My journal class.
 */
class MJ
{
  public:
    static bool enable;
    static Tick lastTick;

    MJ(const char *stage, const char *event);

    template <typename T>
    MJ &operator<<(const T &v)
    {
        if (enable) {
            std::cout << v;
        }
        return *this;
    }

    ~MJ() {
        if (enable) {
            std::cout << std::endl;
        }
    }
};

template <class>
class Checker;
class ThreadContext;

class Checkpoint;
class Process;

namespace o3
{

class ThreadContext;

/**
 * O3CPU class, has each of the stages (fetch through commit)
 * within it, as well as all of the time buffers between stages.  The
 * tick() function for the CPU is defined here.
 */
class CPU : public BaseCPU
{
  public:
    typedef std::list<DynInstPtr>::iterator ListIt;

    friend class ThreadContext;

  public:
    enum Status
    {
        Running,
        Idle,
        Halted,
        Blocked,
        SwitchedOut
    };

    BaseMMU *mmu;
    using LSQRequest = LSQ::LSQRequest;

    /** Overall CPU status. */
    Status _status;

  private:

    /** The tick event used for scheduling CPU ticks. */
    EventFunctionWrapper tickEvent;

    /** The exit event used for terminating all ready-to-exit threads */
    EventFunctionWrapper threadExitEvent;

    /** Schedule tick event, regardless of its current state. */
    void
    scheduleTickEvent(Cycles delay)
    {
        if (tickEvent.squashed())
            reschedule(tickEvent, clockEdge(delay));
        else if (!tickEvent.scheduled())
            schedule(tickEvent, clockEdge(delay));
    }

    /** Unschedule tick event, regardless of its current state. */
    void
    unscheduleTickEvent()
    {
        if (tickEvent.scheduled())
            tickEvent.squash();
    }

    /**
     * Check if the pipeline has drained and signal drain done.
     *
     * This method checks if a drain has been requested and if the CPU
     * has drained successfully (i.e., there are no instructions in
     * the pipeline). If the CPU has drained, it deschedules the tick
     * event and signals the drain manager.
     *
     * @return False if a drain hasn't been requested or the CPU
     * hasn't drained, true otherwise.
     */
    bool tryDrain();

    /**
     * Perform sanity checks after a drain.
     *
     * This method is called from drain() when it has determined that
     * the CPU is fully drained when gem5 is compiled with the NDEBUG
     * macro undefined. The intention of this method is to do more
     * extensive tests than the isDrained() method to weed out any
     * draining bugs.
     */
    void drainSanityCheck() const;

    /** Check if a system is in a drained state. */
    bool isCpuDrained() const;

  public:
    /** Constructs a CPU with the given parameters. */
    CPU(const BaseO3CPUParams &params);

    ProbePointArg<PacketPtr> *ppInstAccessComplete;
    ProbePointArg<std::pair<DynInstPtr, PacketPtr> > *ppDataAccessComplete;

    /** Register probe points. */
    void regProbePoints() override;

    void
    demapPage(Addr vaddr, uint64_t asn)
    {
        mmu->demapPage(vaddr, asn);
    }

    /** Ticks CPU, calling tick() on each stage, and checking the overall
     *  activity to see if the CPU should deschedule itself.
     */
    void tick();

    /** Initialize the CPU */
    void init() override;

    void startup() override;

    /** Returns the Number of Active Threads in the CPU */
    int
    numActiveThreads()
    {
        return activeThreads.size();
    }

    /** Add Thread to Active Threads List */
    void activateThread(ThreadID tid);

    /** Remove Thread from Active Threads List */
    void deactivateThread(ThreadID tid);

    /** Setup CPU to insert a thread's context */
    void insertThread(ThreadID tid);

    /** Remove all of a thread's context from CPU */
    void removeThread(ThreadID tid);

    /** Count the Total Instructions Committed in the CPU. */
    Counter totalInsts() const override;

    /** Count the Total Ops (including micro ops) committed in the CPU. */
    Counter totalOps() const override;

    /** Add Thread to Active Threads List. */
    void activateContext(ThreadID tid) override;

    /** Remove Thread from Active Threads List */
    void suspendContext(ThreadID tid) override;

    /** Remove Thread from Active Threads List &&
     *  Remove Thread Context from CPU.
     */
    void haltContext(ThreadID tid) override;

    /** Update The Order In Which We Process Threads. */
    void updateThreadPriority();

    /** Is the CPU draining? */
    bool isDraining() const { return drainState() == DrainState::Draining; }

    void serializeThread(CheckpointOut &cp, ThreadID tid) const override;
    void unserializeThread(CheckpointIn &cp, ThreadID tid) override;

    /** Insert tid to the list of threads trying to exit */
    void addThreadToExitingList(ThreadID tid);

    /** Is the thread trying to exit? */
    bool isThreadExiting(ThreadID tid) const;

    /**
     *  If a thread is trying to exit and its corresponding trap event
     *  has been completed, schedule an event to terminate the thread.
     */
    void scheduleThreadExitEvent(ThreadID tid);

    /** Terminate all threads that are ready to exit */
    void exitThreads();

  public:
    /** Starts draining the CPU's pipeline of all instructions in
     * order to stop all memory accesses. */
    DrainState drain() override;

    /** Resumes execution after a drain. */
    void drainResume() override;

    /**
     * Commit has reached a safe point to drain a thread.
     *
     * Commit calls this method to inform the pipeline that it has
     * reached a point where it is not executed microcode and is about
     * to squash uncommitted instructions to fully drain the pipeline.
     */
    void commitDrained(ThreadID tid);

    /** Switches out this CPU. */
    void switchOut() override;

    /** Takes over from another CPU. */
    void takeOverFrom(BaseCPU *oldCPU) override;

    void verifyMemoryMode() const override;

    /** Get the current instruction sequence number, and increment it. */
    InstSeqNum getAndIncrementInstSeq() { return globalSeqNum++; }

    /** Traps to handle given fault. */
    void trap(const Fault &fault, ThreadID tid, const StaticInstPtr &inst);

    /** Returns the Fault for any valid interrupt. */
    Fault getInterrupts();

    /** Processes any an interrupt fault. */
    void processInterrupts(const Fault &interrupt);

    /** Halts the CPU. */
    void halt() { panic("Halt not implemented!\n"); }

    /** Register accessors.  Index refers to the physical register index. */

    /** Reads a miscellaneous register. */
    RegVal readMiscRegNoEffect(int misc_reg, ThreadID tid) const;

    /** Reads a misc. register, including any side effects the read
     * might have as defined by the architecture.
     */
    RegVal readMiscReg(int misc_reg, ThreadID tid);

    /** Sets a miscellaneous register. */
    void setMiscRegNoEffect(int misc_reg, RegVal val, ThreadID tid);

    /** Sets a misc. register, including any side effects the write
     * might have as defined by the architecture.
     */
    void setMiscReg(int misc_reg, RegVal val, ThreadID tid);

    RegVal getReg(PhysRegIdPtr phys_reg);
    void getReg(PhysRegIdPtr phys_reg, void *val);
    void *getWritableReg(PhysRegIdPtr phys_reg);

    void setReg(PhysRegIdPtr phys_reg, RegVal val);
    void setReg(PhysRegIdPtr phys_reg, const void *val);

    /** Architectural register accessors.  Looks up in the commit
     * rename table to obtain the true physical index of the
     * architected register first, then accesses that physical
     * register.
     */

    RegVal getArchReg(const RegId &reg, ThreadID tid);
    void getArchReg(const RegId &reg, void *val, ThreadID tid);
    void *getWritableArchReg(const RegId &reg, ThreadID tid);

    void setArchReg(const RegId &reg, RegVal val, ThreadID tid);
    void setArchReg(const RegId &reg, const void *val, ThreadID tid);

    /** Sets the commit PC state of a specific thread. */
    void pcState(const PCStateBase &new_pc_state, ThreadID tid);

    /** Reads the commit PC state of a specific thread. */
    const PCStateBase &pcState(ThreadID tid);

    /** Initiates a squash of all in-flight instructions for a given
     * thread.  The source of the squash is an external update of
     * state through the TC.
     */
    void squashFromTC(ThreadID tid);

    /** Function to add instruction onto the head of the list of the
     *  instructions.  Used when new instructions are fetched.
     */
    ListIt addInst(const DynInstPtr &inst);

    /** Function to tell the CPU that an instruction has completed. */
    void instDone(ThreadID tid, const DynInstPtr &inst);

    /** Remove an instruction from the front end of the list.  There's
     *  no restriction on location of the instruction.
     */
    void removeFrontInst(const DynInstPtr &inst);

    /** Remove all instructions that are not currently in the ROB.
     *  There's also an option to not squash delay slot instructions.*/
    void removeInstsNotInROB(ThreadID tid);

    /** Remove all instructions younger than the given sequence number. */
    void removeInstsUntil(const InstSeqNum &seq_num, ThreadID tid);

    /** Removes the instruction pointed to by the iterator. */
    void squashInstIt(const ListIt &instIt, ThreadID tid);

    /** Cleans up all instructions on the remove list. */
    void cleanUpRemovedInsts();

    /** Debug function to print all instructions on the list. */
    void dumpInsts();

  public:
#ifndef NDEBUG
    /** Count of total number of dynamic instructions in flight. */
    int instcount;
#endif

    /** List of all the instructions in flight. */
    std::list<DynInstPtr> instList;

    /** List of all the instructions that will be removed at the end of this
     *  cycle.
     */
    std::queue<ListIt> removeList;

#ifdef DEBUG
    /** Debug structure to keep track of the sequence numbers still in
     * flight.
     */
    std::set<InstSeqNum> snList;
#endif

    /** Records if instructions need to be removed this cycle due to
     *  being retired or squashed.
     */
    bool removeInstsThisCycle;

  protected:
    /** The fetch stage. */
    Fetch fetch;

    /** The decode stage. */
    Decode decode;

    /** The dispatch stage. */
    Rename rename;

    /** The issue/execute/writeback stages. */
    IEW iew;

    /** The commit stage. */
    Commit commit;

    /** The register file. */
    PhysRegFile regFile;

    /** The free list. */
    UnifiedFreeList freeList;

    /** The rename map. */
    UnifiedRenameMap renameMap[MaxThreads];

    /** The commit rename map. */
    UnifiedRenameMap commitRenameMap[MaxThreads];

    /** The re-order buffer. */
    ROB rob;

    /** The stalling slice table. */
    SST sst;

    /** Active Threads List */
    std::list<ThreadID> activeThreads;

    /**
     *  This is a list of threads that are trying to exit. Each thread id
     *  is mapped to a boolean value denoting whether the thread is ready
     *  to exit.
     */
    std::unordered_map<ThreadID, bool> exitingThreads;

    /** Integer Register Scoreboard */
    Scoreboard scoreboard;

    std::vector<TheISA::ISA *> isa;

  public:
    /** Enum to give each stage a specific index, so when calling
     *  activateStage() or deactivateStage(), they can specify which stage
     *  is being activated/deactivated.
     */
    enum StageIdx
    {
        FetchIdx,
        DecodeIdx,
        RenameIdx,
        IEWIdx,
        CommitIdx,
        NumStages
    };

    /** The main time buffer to do backwards communication. */
    TimeBuffer<TimeStruct> timeBuffer;

    /** The fetch stage's instruction queue. */
    TimeBuffer<FetchStruct> fetchQueue;

    /** The decode stage's instruction queue. */
    TimeBuffer<DecodeStruct> decodeQueue;

    /** The rename stage's instruction queue. */
    TimeBuffer<RenameStruct> renameQueue;

    /** The IEW stage's instruction queue. */
    TimeBuffer<IEWStruct> iewQueue;

  private:
    /** The activity recorder; used to tell if the CPU has any
     * activity remaining or if it can go to idle and deschedule
     * itself.
     */
    ActivityRecorder activityRec;

  public:
    /** Records that there was time buffer activity this cycle. */
    void activityThisCycle() { activityRec.activity(); }

    /** Changes a stage's status to active within the activity recorder. */
    void
    activateStage(const StageIdx idx)
    {
        activityRec.activateStage(idx);
    }

    /** Changes a stage's status to inactive within the activity recorder. */
    void
    deactivateStage(const StageIdx idx)
    {
        activityRec.deactivateStage(idx);
    }

    /** Wakes the CPU, rescheduling the CPU if it's not already active. */
    void wakeCPU();

    virtual void wakeup(ThreadID tid) override;

    /** Gets a free thread id. Use if thread ids change across system. */
    ThreadID getFreeTid();

  public:
    /** Returns a pointer to a thread context. */
    gem5::ThreadContext *
    tcBase(ThreadID tid)
    {
        return thread[tid]->getTC();
    }

    /** The global sequence number counter. */
    InstSeqNum globalSeqNum;//[MaxThreads];

    /** Pointer to the checker, which can dynamically verify
     * instruction results at run time.  This can be set to NULL if it
     * is not being used.
     */
    gem5::Checker<DynInstPtr> *checker;

    /** Pointer to the system. */
    System *system;

    /** Pointers to all of the threads in the CPU. */
    std::vector<ThreadState *> thread;

    /** Threads Scheduled to Enter CPU */
    std::list<int> cpuWaitList;

    /** The cycle that the CPU was last running, used for statistics. */
    Cycles lastRunningCycle;

    /** The cycle that the CPU was last activated by a new thread*/
    Tick lastActivatedCycle;

    /** Mapping for system thread id to cpu id */
    std::map<ThreadID, unsigned> threadMap;

    /** Available thread ids in the cpu*/
    std::vector<ThreadID> tids;

    /** CPU pushRequest function, forwards request to LSQ. */
    Fault
    pushRequest(const DynInstPtr& inst, bool isLoad, uint8_t *data,
                unsigned int size, Addr addr, Request::Flags flags,
                uint64_t *res, AtomicOpFunctorPtr amo_op = nullptr,
                const std::vector<bool>& byte_enable=std::vector<bool>())

    {
        return iew.ldstQueue.pushRequest(inst, isLoad, data, size, addr,
                flags, res, std::move(amo_op), byte_enable);
    }

    /** Used by the fetch unit to get a hold of the instruction port. */
    Port &
    getInstPort() override
    {
        return fetch.getInstPort();
    }

    /** Get the dcache port (used to find block size for translations). */
    Port &
    getDataPort() override
    {
        return iew.ldstQueue.getDataPort();
    }

    struct CPUStats : public statistics::Group
    {
        CPUStats(CPU *cpu);

        /** Stat for total number of times the CPU is descheduled. */
        statistics::Scalar timesIdled;
        /** Stat for total number of cycles the CPU spends descheduled. */
        statistics::Scalar idleCycles;
        /** Stat for total number of cycles the CPU spends descheduled due to a
         * quiesce operation or waiting for an interrupt. */
        statistics::Scalar quiesceCycles;
        /** Stat for the number of committed instructions per thread. */
        statistics::Vector committedInsts;
        /** Stat for the number of committed ops (including micro ops) per
         *  thread. */
        statistics::Vector committedOps;
        /** Stat for the CPI per thread. */
        statistics::Formula cpi;
        /** Stat for the total CPI. */
        statistics::Formula totalCpi;
        /** Stat for the IPC per thread. */
        statistics::Formula ipc;
        /** Stat for the total IPC. */
        statistics::Formula totalIpc;

        //number of integer register file accesses
        statistics::Scalar intRegfileReads;
        statistics::Scalar intRegfileWrites;
        //number of float register file accesses
        statistics::Scalar fpRegfileReads;
        statistics::Scalar fpRegfileWrites;
        //number of vector register file accesses
        mutable statistics::Scalar vecRegfileReads;
        statistics::Scalar vecRegfileWrites;
        //number of predicate register file accesses
        mutable statistics::Scalar vecPredRegfileReads;
        statistics::Scalar vecPredRegfileWrites;
        //number of CC register file accesses
        statistics::Scalar ccRegfileReads;
        statistics::Scalar ccRegfileWrites;
        //number of misc
        statistics::Scalar miscRegfileReads;
        statistics::Scalar miscRegfileWrites;
        statistics::Scalar dvrLoadsObserved;
        statistics::Scalar dvrStrideCandidates;
        statistics::Scalar dvrDiscoveryStarts;
        statistics::Scalar dvrDiscoveryCompletions;
        statistics::Scalar dvrDiscoveryTimeouts;
        statistics::Scalar dvrDiscoveryAbandons;
        statistics::Scalar dvrNestedRootStarts;
        statistics::Scalar dvrNestedStarts;
        statistics::Scalar dvrNestedCompletions;
        statistics::Scalar dvrNestedTimeouts;
        statistics::Scalar dvrNestedDepthRejects;
        statistics::Scalar dvrNestedParentResets;
        statistics::Scalar dvrNestedContextsBuilt;
        statistics::Scalar dvrNestedProgramsBuilt;
        statistics::Scalar dvrNestedVRATAllocations;
        statistics::Scalar dvrNestedVIRExecutions;
        statistics::Scalar dvrNestedHelpersGenerated;
        statistics::Scalar dvrNestedHelpersIssued;
        statistics::Scalar dvrNestedHelpersCompleted;
        statistics::Scalar dvrNestedReplayAttempts;
        statistics::Scalar dvrNestedReplayTargetsGenerated;
        statistics::Scalar dvrNestedReplayFallbacks;
        statistics::Scalar dvrNestedDependentGenerated;
        statistics::Scalar dvrDiscoveredInstructions;
        statistics::Scalar dvrTaintedInstructions;
        statistics::Scalar dvrDependentLoads;
        statistics::Scalar dvrDiscoveriesWithFLR;
        statistics::Scalar dvrBackwardBranches;
        statistics::Scalar dvrLoopBoundsFound;
        statistics::Scalar dvrDiscoveriesWithBounds;
        statistics::Scalar dvrLoopBoundMatches;
        statistics::Scalar dvrLoopBoundFallbacks;
        statistics::Scalar dvrLaneCountSamples;
        statistics::Scalar dvrTotalActiveLanes;
        statistics::Scalar dvrPrefetchesGenerated;
        statistics::Scalar dvrPrefetchesIssued;
        statistics::Scalar dvrPrefetchesCompleted;
        statistics::Scalar dvrPrefetchesDropped;
        statistics::Scalar dvrPrefetchTranslationFaults;
        statistics::Scalar dvrSourcePrefetchTranslationFaults;
        statistics::Scalar dvrDependentPrefetchTranslationFaults;
        statistics::Scalar dvrAddressRelationsTrained;
        statistics::Scalar dvrDependentPrefetchesGenerated;
        statistics::Scalar dvrDependentPrefetchesIssued;
        statistics::Scalar dvrDependentPrefetchesCompleted;
        statistics::Scalar dvrRecordedUops;
        statistics::Scalar dvrRecorderOverflows;
        statistics::Scalar dvrVectorProgramsBuilt;
        statistics::Scalar dvrVRATAllocations;
        statistics::Scalar dvrVIRChunkIssues;
        statistics::Scalar dvrVIRChunkExecutions;
        statistics::Scalar dvrDivergentBranches;
        statistics::Scalar dvrReconvergences;
        statistics::Scalar dvrPredicateGenerationAbandons;
        statistics::Scalar dvrHelperTimeouts;
        statistics::Scalar dvrReconvergenceStackOverflows;
        statistics::Scalar dvrHelpersSuppressed;
        statistics::Scalar dvrPredicateSelections;
        statistics::Scalar dvrDistinctPredicatePaths;
        statistics::Scalar dvrPredicateMisses;
        statistics::Scalar dvrSourcePrefetchesIssued;
        statistics::Scalar dvrSourcePrefetchesCompleted;
        statistics::Scalar dvrPrefetchQueuePeak;
        statistics::Scalar dvrPrefetchesSuppressedMainThread;
        statistics::Scalar dvrPrefetchesRejectedBackpressure;
        statistics::Scalar dvrPrefetchesSuperseded;
        statistics::Scalar dvrPrefetchesPossiblyUseful;
        statistics::Scalar dvrPrefetchesLate;
        statistics::Scalar dvrReplaySupportedUops;
        statistics::Scalar dvrReplayUnsupportedUops;
        statistics::Scalar dvrReplayUnstableInputs;
        statistics::Scalar dvrReplayAttempts;
        statistics::Scalar dvrReplayTargetsGenerated;
        statistics::Scalar dvrReplayFallbacks;
        statistics::Scalar dvrQualityIssuedBytes;
        statistics::Scalar dvrQualityCompletedBytes;
        statistics::Scalar dvrQualityDemandAddressesObserved;

        // Vector Runahead statistics.
        statistics::Scalar vrRoundsEntered;
        statistics::Scalar vrGathersIssued;
        statistics::Scalar vrPrefetchesIssued;
        statistics::Scalar vrSourcePrefetchesIssued;
        statistics::Scalar vrDependentPrefetchesGenerated;
        statistics::Scalar vrDependentPrefetchesIssued;
        statistics::Scalar vrPrefetchesCompleted;
        statistics::Scalar vrPrefetchesDropped;
        statistics::Scalar vrPrefetchTranslationFaults;
        statistics::Scalar vrTaintedInstructions;
        statistics::Scalar vrTerminatedRecurringStride;
        statistics::Scalar vrTerminatedTerminator;
        statistics::Scalar vrTerminatedAllInvalid;
        statistics::Scalar vrTerminatedTimeout;
        statistics::Scalar vrPrefetchQueuePeak;
        statistics::Scalar vrVRATAllocations;
        statistics::Scalar vrRDQEnqueues;
        statistics::Scalar vrRDQReleases;
        statistics::Scalar vrInvalidLanes;
        statistics::Scalar vrPrefetchRetries;
    } cpuStats;

  public:
    // hardware transactional memory
    void htmSendAbortSignal(ThreadID tid, uint64_t htm_uid,
                            HtmFailureFaultCause cause) override;

  public:
    /** Call this function when ROB encounters a full-window stall. The CPU
     *  will enter PRE at the next cycle if PRE is enabled.
     */
    void enterPRE();

    /** Call this method when the stalling load returns. The CPU will trigger
     *  an exception that helps exiting PRE at the next cycle.
     */
    void exitPRE();

    /** Returns whether PRE is enabled. */
    bool isPREEnabled() const { return enablePRE; }

    /** Returns whether the CPU is in PRE. */
    bool isInPRE() const { return inPRE; };

    /** 用主线程 load 地址训练 DVR 的 RPT。 */
    void observeDVRLoad(const DynInstPtr &inst, Addr address);

    /**
     * Vector Runahead: called from the LSQ for every PRE-mode load whose
     * address is known.  Trains the VR stride detector and, on confidence 3,
     * enters vector-runahead mode (paper Section III-C).
     */
    void observeVRLoad(const DynInstPtr &inst, Addr address);

    /** Called from rename for every instruction in the runahead stream while
     *  in vector-runahead mode: taint-track, capture the chain and enforce
     *  the four termination conditions (paper Section III-J). */
    void observeVRInstruction(const DynInstPtr &inst);

    /** Enter vector-runahead mode for the striding load that just reached
     *  confidence 3, vectorizing it into an N-lane gather. */
    void enterVR(const DynInstPtr &inst, Addr address, int64_t stride);

    /** Leave vector-runahead mode and clear its state. */
    void exitVR();

    /** Returns whether the CPU is in vector-runahead mode. */
    bool isInVR() const { return inVR; };

    /** Enqueue the N lanes of one vector gather (striding or dependent). */
    void issueVRGather(Addr pc, Addr base, int64_t stride, unsigned lanes,
                       unsigned level, bool source);

    /** Drain one entry of the VR prefetch queue into the L1D timing port,
     *  mirroring DVR's software-prefetch engine. */
    void serviceVRPrefetchQueue();

    /** Replay the captured chain rooted at the trigger, substituting the
     *  returned striding value, and generate the dependent gather(s). */
    void replayVRChain(RegVal source_value,
                       const VRPrefetchSenderState &state);

    /** Track the high-water mark of the VR prefetch queue. */
    void updateVRPrefetchQueuePeak();
    void invalidateVRLane(unsigned lane);
    void replayPendingVRResponses();

    struct DVRReplayTemplate
    {
        unsigned count = 0;
        int8_t triggerDestination = -1;
        std::array<DVRInstructionRecorder::Uop,
                   DVRInstructionRecorder::MaxUops> uops = {};
        DVRLoopBoundDetector::RegisterSnapshot initialRegs = {};
        bool valid = false;
    };

    struct DVRPredicateGeneration
    {
        uint64_t generation = 0;
        unsigned expectedLanes = 0;
        unsigned terminalLanes = 0;
        std::array<uint64_t, 2> terminalMask = {};
        DVRLanePredicateTracker tracker;
        bool divergenceCounted = false;
        bool reported = false;
    };

    struct DVRPrefetchSenderState : public Packet::SenderState
    {
        static constexpr unsigned MaxRelations = 4;
        bool source;
        bool nested;
        unsigned relationCount;
        std::array<int64_t, MaxRelations> scales;
        std::array<int64_t, MaxRelations> offsets;
        std::array<RegVal, MaxRelations> masks;
        std::array<RegVal, MaxRelations> patterns;
        std::shared_ptr<const DVRReplayTemplate> replay;
        std::shared_ptr<DVRPredicateGeneration> predicate;
        unsigned lane;
        ThreadID tid;

        DVRPrefetchSenderState(bool is_source, bool is_nested,
            unsigned relation_count,
            const std::array<int64_t, MaxRelations> &relation_scales,
            const std::array<int64_t, MaxRelations> &relation_offsets,
            const std::array<RegVal, MaxRelations> &relation_masks,
            const std::array<RegVal, MaxRelations> &relation_patterns,
            std::shared_ptr<const DVRReplayTemplate> replay_template,
            std::shared_ptr<DVRPredicateGeneration> predicate_generation,
            unsigned lane_id,
            ThreadID thread);
    };

    /** 处理 DVR 软件预取生成的响应。 */
    void completeDVRPrefetch(PacketPtr pkt);

    /** 处理 VR gather 预取响应；source 响应携带数据用于依赖链回放。 */
    void completeVRPrefetch(PacketPtr pkt);

  private:
    /** Whether PRE is enabled. */
    bool enablePRE;

    /** Whether the CPU is in PRE. */
    bool inPRE;

    /** DVR 发现开关及其主要状态结构。 */
    bool enableDVR;
    DVRStrideDetector dvrStrideDetector;
    DVRDiscoveryController dvrDiscovery;
    DVRNestedController dvrNestedController;
    // Only request-level counters are connected here. Strict cache quality
    // needs tag/fill/victim callbacks; the 1x1 shadow is therefore unused.
    DVRQualityTracker dvrQualityTracker;
    DVRVectorTaintTracker dvrTaintTracker;
    DVRLoopBoundDetector dvrLoopBoundDetector;
    DVRInstructionRecorder dvrInstructionRecorder;
    DVRVectorRenameTable dvrVectorRenameTable;
    DVRVectorInstructionRegister dvrVectorInstructionRegister;
    DVRLoopBoundDetector::RegisterSnapshot dvrDiscoveryStartRegs = {};
    unsigned dvrMaxLanes;
    unsigned dvrHelperMaxUops;

    /**
     * Vector Runahead state.  The stride detector, taint vector, round
     * context, VRAT (reused DVRVectorRenameTable) and the VR prefetch queue.
     */
    bool enableVR;
    bool inVR;
    VRStrideDetector vrStrideDetector;
    VRVectorTaintTracker vrTaint;
    VRRound vrRound;
    DVRInstructionRecorder vrChain;
    DVRVectorRenameTable vrRAT;
    DVRLoopBoundDetector::RegisterSnapshot vrInitialRegs = {};
    unsigned vrVectorLanes;
    unsigned vrUnrollLength;
    unsigned vrPipelineDepth;
    unsigned vrTimeoutInstructions;
    unsigned vrUnrollsIssued = 0;
    std::deque<VRPrefetchEntry> vrPrefetchQueue;
    struct VRPendingResponse
    {
        RegVal value;
        bool source;
        unsigned level;
        unsigned lane;
        unsigned nextUop;
        int8_t valueReg;
        ThreadID tid;
    };
    std::deque<VRPendingResponse> vrPendingResponses;
    std::deque<unsigned> vrRDQ;
    uint64_t vrPrefetchQueuePeak = 0;
    uint64_t vrActiveLaneMask = 0;
    unsigned vrMaxRDQEntries = 192;
    unsigned vrCycles = 0;

    struct DVRPrefetchAddress
    {
        Addr address;
        Addr pc;
        ThreadID tid;
        bool source;
        bool nested = false;
        unsigned relationCount = 0;
        std::array<int64_t, DVRPrefetchSenderState::MaxRelations> scales = {};
        std::array<int64_t, DVRPrefetchSenderState::MaxRelations> offsets = {};
        std::array<RegVal, DVRPrefetchSenderState::MaxRelations> masks = {};
        std::array<RegVal, DVRPrefetchSenderState::MaxRelations> patterns = {};
        std::shared_ptr<const DVRReplayTemplate> replay;
        std::shared_ptr<DVRPredicateGeneration> predicate;
        unsigned lane = 0;
    };
    std::deque<DVRPrefetchAddress> dvrPrefetchQueue;
    struct DVRAddressRelation
    {
        bool hasPrevious = false;
        bool trained = false;
        RegVal previousValue = 0;
        Addr previousAddress = 0;
        int64_t scale = 0;
        int64_t offset = 0;
        RegVal stableMask = ~RegVal(0);
        RegVal pattern = 0;
        unsigned samples = 0;
    };
    std::unordered_map<Addr, DVRAddressRelation> dvrAddressRelations;
    std::unordered_map<Addr, std::vector<Addr>> dvrTriggerRelations;
    std::unordered_map<Addr, unsigned> dvrOutstandingPrefetchLines;
    std::unordered_map<Addr, Tick> dvrCompletedPrefetchLines;
    uint64_t dvrPrefetchQueuePeak = 0;
    uint64_t dvrNextPredicateGeneration = 1;
    std::shared_ptr<DVRPredicateGeneration> dvrActivePredicateGeneration;
    Addr dvrCurrentTriggerPC = 0;
    uint8_t dvrSelectedRelationSlots = 0;
    RegVal dvrInitiatingLoadValue = 0;
    struct DVRPendingNestedCandidate
    {
        bool valid = false;
        Addr pc = 0;
        InstSeqNum sequence = 0;
        Addr address = 0;
        int64_t stride = 0;
    } dvrPendingNestedCandidate;

    /**
     * Child discovery owns every mutable mechanism state.  In particular it
     * never reuses or resets the root recorder/taint/VRAT/VIR objects.
     */
    struct DVRNestedExecutionContext
    {
        bool active = false;
        DVRNestedController::DiscoveryId id = 0;
        ThreadID tid = 0;
        InstSeqNum triggerSequence = 0;
        Addr triggerPC = 0;
        Addr triggerAddress = 0;
        int64_t stride = 0;
        RegVal initiatingValue = 0;
        DVRVectorTaintTracker taint;
        DVRLoopBoundDetector loopBound;
        DVRInstructionRecorder recorder;
        DVRVectorRenameTable vrat;
        DVRVectorInstructionRegister vir;
        DVRLoopBoundDetector::RegisterSnapshot startRegs = {};

        void reset()
        {
            active = false;
            id = 0;
            triggerSequence = 0;
            triggerPC = 0;
            triggerAddress = 0;
            stride = 0;
            initiatingValue = 0;
            taint.reset();
            loopBound.reset();
            recorder.reset();
            vrat.reset();
            vir.reset();
            startRegs = {};
        }
    } dvrNestedContext;

    void captureDVRRegisterSnapshot(
        ThreadID tid, const DynInstPtr &committing_inst,
        DVRLoopBoundDetector::RegisterSnapshot &snapshot);
    void launchDVRStridePrefetches(ThreadID tid, Addr current_address,
                                   Addr pc, int64_t stride, unsigned lanes,
                                   const DVRLoopBoundDetector::RegisterSnapshot
                                       &finish_regs);
    void completeDVRNestedContext(
        const DynInstPtr &committing_inst,
        const DVRLoopBoundDetector::RegisterSnapshot &finish_regs);
    void launchDVRNestedPrefetches(
        unsigned lanes,
        const DVRLoopBoundDetector::RegisterSnapshot &finish_regs);
    void serviceDVRPrefetchQueue();
    Addr dvrPrefetchLine(Addr address) const;
    void accountDVRDemand(Addr address);
    void updateDVRPrefetchQueuePeak();
    void retireDVRPredicateLane(
        const std::shared_ptr<DVRPredicateGeneration> &generation,
        unsigned lane, bool has_value, RegVal value = 0);
    void finishDVRPredicateGeneration(
        const std::shared_ptr<DVRPredicateGeneration> &generation,
        bool replaced);
    bool replayDVRSource(const DVRPrefetchSenderState &state,
                         RegVal source_value);
    void trainDVRAddressRelation(Addr trigger_pc, Addr flr_pc,
                                 RegVal source_value,
                                 Addr dependent_address);

    /** Checkpoints of the rename map when entering PRE. */
    std::vector<PhysRegIdPtr> checkpointRenameMap[CCRegClass + 1];
    std::vector<PhysRegIdPtr> checkpointFreeList[CCRegClass + 1];
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_CPU_HH__
