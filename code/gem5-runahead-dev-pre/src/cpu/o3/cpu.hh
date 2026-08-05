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

#include <array>
#include <cstdio>
#include <iostream>
#include <list>
#include <memory>
#include <queue>
#include <deque>
#include <set>
#include <unordered_map>
#include <unordered_set>
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
    ~CPU() override;

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
        statistics::Scalar dvrDiscoveryInnermostSwitches;
        statistics::Scalar dvrDiscoveryCompletions;
        statistics::Scalar dvrDiscoveryTimeouts;
        statistics::Scalar dvrDiscoveryAbandons;
        statistics::Scalar dvrDiscoveryRollbacks;
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
        statistics::Scalar dvrNestedFlattenBatches;
        statistics::Scalar dvrNestedOuterInstances;
        statistics::Scalar dvrNestedInnerLanes;
        statistics::Scalar dvrNestedFlattenedLanes;
        statistics::Scalar dvrNestedFlattenInvariantChecks;
        statistics::Scalar dvrNestedFlattenInvariantFailures;
        statistics::Scalar dvrNestedFlattenExpectedLanes;
        statistics::Scalar dvrNestedVariableLaneBatches;
        statistics::Scalar dvrNDMAttempts;
        statistics::Scalar dvrNDMOuterFound;
        statistics::Scalar dvrNDMFallbacks;
        statistics::Scalar dvrNDMTimeouts;
        statistics::Scalar dvrNDMBranchInversions;
        statistics::Scalar dvrNDMIRCaptures;
        statistics::Scalar dvrNDMILRCaptures;
        statistics::Scalar dvrNDMLCRCaptures;
        statistics::Scalar dvrNDMOuterInvocations;
        statistics::Scalar dvrResourceConflicts;
        statistics::Scalar dvrIssueBudgetConflicts;
        statistics::Scalar dvrALUBudgetConflicts;
        statistics::Scalar dvrLSUBudgetConflicts;
        statistics::Scalar dvrHelperIssueCycles;
        statistics::Scalar dvrHelperFetchCycles;
        statistics::Scalar dvrHelperDecodeCycles;
        statistics::Scalar dvrHelperUopsBecameReady;
        statistics::Scalar dvrHelperUopsIssued;
        statistics::Scalar dvrHelperDynUopsDecoded;
        statistics::Scalar dvrHelperDynUopsIssued;
        statistics::Scalar dvrHelperDynUopsCompleted;
        statistics::Scalar dvrHelperDecodedCacheHits;
        statistics::Scalar dvrHelperDecodedCacheMisses;
        statistics::Scalar dvrHelperFetchBlockedByMain;
        statistics::Scalar dvrHelperDecodeBlockedByMain;
        statistics::Scalar dvrHelperVIRCapacityStalls;
        statistics::Scalar dvrHelperVRATPrograms;
        statistics::Scalar dvrHelperVRATWrites;
        statistics::Scalar dvrHelperReadyUopCycles;
        statistics::Scalar dvrHelperReadyOccupancySamples;
        statistics::Formula dvrHelperReadyOccupancy;
        statistics::Scalar dvrHelperComputeCycles;
        statistics::Scalar dvrHelperComputeConflicts;
        statistics::Scalar dvrHelperFURequests;
        statistics::Scalar dvrHelperFUGrants;
        statistics::Scalar dvrHelperFUStalls;
        statistics::Scalar dvrHelperIssueQueueStalls;
        statistics::Scalar dvrHelperScoreboardWaitCycles;
        statistics::Scalar dvrVectorALUChunkIssues;
        statistics::Scalar dvrVectorAddChunkIssues;
        statistics::Scalar dvrVectorShiftChunkIssues;
        statistics::Scalar dvrVectorMultiplyChunkIssues;
        statistics::Scalar dvrVectorChunkRequests;
        statistics::Scalar dvrVectorizerSourceLanes;
        statistics::Scalar dvrVectorizerDependentLanes;
        statistics::Scalar dvrVIRActiveMaskChecks;
        statistics::Scalar dvrVIRActiveMaskFailures;
        statistics::Scalar dvrVectorFUConflictCycles;
        statistics::Scalar dvrVectorLatencyCycles;
        statistics::Scalar dvrVectorComputeWaitCycles;
        statistics::Scalar dvrVectorActiveLanes;
        statistics::Formula dvrVectorUtilization;
        statistics::Scalar dvrHelperALUOps;
        statistics::Scalar dvrHelperShiftOps;
        statistics::Scalar dvrHelperMultiplyOps;
        statistics::Scalar dvrHelperLSUOps;
        statistics::Scalar dvrMainIssueSlotsUsed;
        statistics::Scalar dvrMainALUSlotsUsed;
        statistics::Scalar dvrMainLSUSlotsUsed;
        statistics::Scalar dvrFetchActiveCycles;
        statistics::Scalar dvrDecodeActiveCycles;
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
        statistics::Scalar dvrVIRUnsupportedControlFlow;
        statistics::Scalar dvrVIRNormalTerminatedLanes;
        statistics::Scalar dvrVIREarlyExitLanes;
        statistics::Scalar dvrVIRExternalPathLanes;
        statistics::Scalar dvrVIRUnsupportedSemanticLanes;
        statistics::Scalar dvrVIRSourceValueExecutions;
        statistics::Scalar dvrVIRSourceValueBranches;
        statistics::Scalar dvrVIRSourceValueExternalLanes;
        statistics::Scalar dvrVIRSourceValueSemanticFailures;
        statistics::Scalar dvrVIRSourceValueTerminations;
        statistics::Scalar dvrVIRContinuationContexts;
        statistics::Scalar dvrVIRContinuationResumes;
        statistics::Scalar dvrVIRContinuationFallbacks;
        statistics::Scalar dvrVIRContinuationPCGroups;
        statistics::Scalar dvrVIRContinuationGroupedLanes;
        statistics::Scalar dvrVIRContinuationMaxGroupWidth;
        statistics::Scalar dvrPredicateGenerationAbandons;
        statistics::Scalar dvrHelperTimeouts;
        statistics::Scalar dvrReconvergenceStackOverflows;
        statistics::Scalar dvrHelpersSuppressed;
        statistics::Scalar dvrControlFallbackSourceLaunches;
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
        statistics::Scalar dvrOutstandingPrefetchLineSamples;
        statistics::Scalar dvrOutstandingPrefetchLineSum;
        statistics::Scalar dvrOutstandingPrefetchLinePeak;
        statistics::Scalar dvrReplaySupportedUops;
        statistics::Scalar dvrReplayUnsupportedUops;
        statistics::Scalar dvrReplayUnstableInputs;
        statistics::Scalar dvrReplayAttempts;
        statistics::Scalar dvrReplayTargetsGenerated;
        statistics::Scalar dvrReplayFallbacks;
        statistics::Scalar dvrAlternatePathLookups;
        statistics::Scalar dvrAlternatePathHits;
        statistics::Scalar dvrAlternatePathCompleteHits;
        statistics::Scalar dvrAlternatePathLiveInRejects;
        statistics::Scalar dvrAlternatePathIncompleteRejects;
        statistics::Scalar dvrAlternatePathUopsReplayed;
        statistics::Scalar dvrAlternatePathDependentTargets;
        statistics::Scalar dvrAlternatePathDemandCovered;
        statistics::Scalar dvrReconvergenceResumeSuccesses;
        statistics::Scalar dvrQualityIssuedBytes;
        statistics::Scalar dvrQualityCompletedBytes;
        statistics::Scalar dvrQualityDemandAddressesObserved;
        statistics::Scalar dvrDependentDemandLoads;
        statistics::Scalar dvrDependentDemandCovered;
        statistics::Scalar dvrDependentDemandLate;
        statistics::Scalar dvrHelperLaunchAttempts;
        statistics::Scalar dvrHelperLaunchAdmitted;
        statistics::Scalar dvrHelperLaunchCapacityDrops;
        statistics::Scalar dvrHelperLaunchZeroLanes;
        statistics::Scalar dvrPrefetchesDeduplicated;
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
    /** IEW dispatch 阶段观察指令并启动/推进 DVR Discovery。 */
    void observeDVRDispatch(const DynInstPtr &inst);

    /**
     * Private vector register file for one helper program.  It is deliberately
     * separate from the main O3 physical register file: DVR is a transient,
     * in-order subthread and must not create architectural rename/commit state.
     */
    struct DVRHelperVectorRegisterFile
    {
        static constexpr unsigned MaxLanes = DVRLanePredicateTracker::MaxLanes;
        static constexpr unsigned NumArchitecturalRegs =
            DVRLoopBoundDetector::MaxArchitecturalIntRegs;
        static constexpr unsigned NumPhysicalRegs = 64;

        struct PhysicalRegister
        {
            std::array<RegVal, MaxLanes> values = {};
            std::array<Tick, MaxLanes> ready = {};
            std::array<uint64_t, 2> valid = {};
        };

        std::array<int16_t, NumArchitecturalRegs> vrat = {};
        std::array<PhysicalRegister, NumPhysicalRegs> physical = {};
        unsigned nextPhysical = 0;

        void reset()
        {
            nextPhysical = 0;
            for (unsigned reg = 0; reg < NumArchitecturalRegs; ++reg)
                vrat[reg] = -1;
            for (auto &entry : physical) {
                entry.values.fill(0);
                entry.ready.fill(0);
                entry.valid = {};
            }
        }

        void initialize(const DVRLoopBoundDetector::RegisterSnapshot &regs)
        {
            reset();
            for (unsigned arch = 0; arch < NumArchitecturalRegs; ++arch) {
                vrat[arch] = nextPhysical++;
                auto &entry = physical[vrat[arch]];
                for (unsigned lane = 0; lane < MaxLanes; ++lane) {
                    entry.values[lane] = regs[arch];
                    entry.valid[lane / 64] |= uint64_t(1) << (lane % 64);
                }
            }
        }

        int16_t rename(unsigned arch)
        {
            if (arch >= NumArchitecturalRegs)
                return -1;
            if (nextPhysical >= NumPhysicalRegs)
                nextPhysical = 0;
            vrat[arch] = nextPhysical++;
            physical[vrat[arch]] = PhysicalRegister();
            return vrat[arch];
        }

        RegVal read(unsigned arch, unsigned lane) const
        {
            if (arch >= NumArchitecturalRegs || lane >= MaxLanes ||
                vrat[arch] < 0)
                return 0;
            return physical[vrat[arch]].values[lane];
        }

        Tick readyAt(unsigned arch, unsigned lane) const
        {
            if (arch >= NumArchitecturalRegs || lane >= MaxLanes ||
                vrat[arch] < 0)
                return 0;
            return physical[vrat[arch]].ready[lane];
        }

        void write(unsigned arch, unsigned lane, RegVal value, Tick ready_tick)
        {
            if (arch >= NumArchitecturalRegs || lane >= MaxLanes ||
                vrat[arch] < 0)
                return;
            auto &entry = physical[vrat[arch]];
            entry.values[lane] = value;
            entry.ready[lane] = ready_tick;
            entry.valid[lane / 64] |= uint64_t(1) << (lane % 64);
        }
    };

    struct DVRReplayTemplate
    {
        Addr triggerPC = 0;
        // Complete captured stream consumed by the persistent VIR.  The
        // scalar replay prefix ends at the final dependent load so post-FLR
        // loop-control state cannot invalidate a valid address chain.
        unsigned count = 0;
        unsigned scalarCount = 0;
        int8_t triggerDestination = -1;
        bool continuePastFLR = false;
        std::array<DVRInstructionRecorder::Uop,
                   DVRInstructionRecorder::MaxUops> uops = {};
        DVRLoopBoundDetector::RegisterSnapshot initialRegs = {};
        std::shared_ptr<DVRHelperVectorRegisterFile> helperRegs;
        std::shared_ptr<DVRVectorInstructionRegister> continuation;
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

  private:
    /** Whether PRE is enabled. */
    bool enablePRE;

    /** Whether the CPU is in PRE. */
    bool inPRE;

    /** DVR 发现开关及其主要状态结构。 */
    bool enableDVR;
    std::string dvrMode;
    DVRStrideDetector dvrStrideDetector;
    DVRDiscoveryController dvrDiscovery;
    DVRNestedController dvrNestedController;
    DVRNestedDiscoveryMode dvrNestedDiscoveryMode;
    // Only request-level counters are connected here. Strict cache quality
    // needs tag/fill/victim callbacks; the 1x1 shadow is therefore unused.
    DVRQualityTracker dvrQualityTracker;
    DVRVectorTaintTracker dvrTaintTracker;
    DVRLoopBoundDetector dvrLoopBoundDetector;
    DVRInstructionRecorder dvrInstructionRecorder;
    DVRVectorRenameTable dvrVectorRenameTable;
    DVRVectorInstructionRegister dvrVectorInstructionRegister;
    DVRLoopBoundDetector::RegisterSnapshot dvrDiscoveryStartRegs = {};
    std::set<InstSeqNum> dvrDispatchTainted;
    std::set<InstSeqNum> dvrDispatchDependentLoads;
    // Control-flow uops are retained in the replay metadata even when their
    // operands are not tainted, so branches between FLR and LCR are visible
    // to the VIR path.
    std::set<InstSeqNum> dvrDispatchRecorded;
    unsigned dvrMaxLanes;
    unsigned dvrHelperMaxUops;
    bool dvrEnableDependentPrefetch;
    bool dvrVectorChunkModel;
    bool dvrVectorUnlimitedFU;
    unsigned dvrVectorElementBits;
    struct DVRTraceSink
    {
        FILE *workload = nullptr;
        FILE *dependency = nullptr;
        FILE *vectorization = nullptr;
        FILE *events = nullptr;
        bool enabled() const { return workload != nullptr; }
    } dvrTrace;
    void dvrTraceWorkload(const char *kind, Tick tick, InstSeqNum seq,
                          Addr pc, Addr address);
    void dvrTraceDependency(const char *kind, Tick tick, Addr trigger_pc,
                            Addr pc, Addr address, uint32_t taint,
                            int lanes = 0);
    void dvrTraceVector(const char *kind, Tick tick, Addr pc, Addr address,
                        int lanes, int invocation = -1);
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

    /**
     * Independent in-order DVR helper subthread.
     *
     * The helper owns a PC, replay-uop stream, frontend window, scoreboard,
     * and issue queue.  It is stepped once per CPU cycle after the main
     * thread has used the LSQ data port.  It does not modify the architectural
     * register state; only its memory requests are allowed to enter the
     * shared cache hierarchy.
     */
    struct DVRHelperThread
    {
        enum class State { Idle, Fetch, Decode, Running, Draining };
        State state = State::Idle;
        uint64_t id = 0;
        Addr triggerPC = 0;
        unsigned programUops = 0;
        unsigned maxUops = 0;
        unsigned workUnits = 0;
        unsigned nextLane = 0;
        unsigned issuedUops = 0;
        unsigned outstanding = 0;
        unsigned fetchRemaining = 0;
        unsigned decodeRemaining = 0;
        unsigned readyUops = 0;
        unsigned aluRemaining = 0;
        unsigned shiftRemaining = 0;
        unsigned multiplyRemaining = 0;
        Tick computeReadyTick = 0;
        bool vectorChunkModel = false;
        Addr helperPC = 0;

        enum class ComputeKind { Alu, Add, Shift, Multiply };
        struct DVRDynUop
        {
            enum class State { Decoded, Ready, Issued, WaitingMemory,
                               Completed };
            std::shared_ptr<const DVRReplayTemplate> program;
            StaticInstPtr staticInst;
            unsigned uopIndex = 0;
            OpClass opClass = SimdAluOp;
            int8_t source0 = -1;
            int8_t source1 = -1;
            int8_t destination = -1;
            Addr pc = 0;
            std::array<uint64_t, 2> activeMask = {};
            std::vector<unsigned> lanes;
            unsigned chunksRemaining = 1;
            Tick issueCycle = 0;
            Tick completeCycle = 0;
            State state = State::Decoded;
        };
        static constexpr unsigned VIRCapacity = 8;
        struct ReplayGeneration
        {
            std::shared_ptr<const DVRReplayTemplate> program;
            unsigned workUnits = 0;
            unsigned unit = 0;
            unsigned uop = 0;
        };
        struct ReplayLaneContext
        {
            struct ReconvergenceFrame
            {
                Addr pc = 0;
                Addr deferredPC = 0;
                bool alternatePath = false;
            };
            static constexpr unsigned ReconvergenceEntries = 8;
            std::shared_ptr<const DVRReplayTemplate> program;
            std::array<RegVal, DVRLoopBoundDetector::MaxArchitecturalIntRegs>
                regs = {};
            std::array<Tick, DVRLoopBoundDetector::MaxArchitecturalIntRegs>
                readyCycle = {};
            std::shared_ptr<DVRPredicateGeneration> predicate;
            std::array<int64_t, DVRPrefetchSenderState::MaxRelations> scales = {};
            std::array<int64_t, DVRPrefetchSenderState::MaxRelations> offsets = {};
            std::array<RegVal, DVRPrefetchSenderState::MaxRelations> masks = {};
            std::array<RegVal, DVRPrefetchSenderState::MaxRelations> patterns = {};
            unsigned relationCount = 0;
            unsigned lane = 0;
            ThreadID tid = 0;
            Addr triggerPC = 0;
            unsigned uopIndex = 1;
            Addr lanePC = 0;
            std::array<ReconvergenceFrame, ReconvergenceEntries>
                reconvergenceStack = {};
            unsigned reconvergenceDepth = 0;
            unsigned helperUops = 0;
            bool nested = false;
            bool active = true;
        };
        struct IssueEntry
        {
            ComputeKind kind = ComputeKind::Alu;
            int8_t source0 = -1;
            int8_t source1 = -1;
            int8_t destination = -1;
            Addr pc = 0;
            Addr nextPC = 0;
            Tick readyTick = 0;
        };
        static constexpr unsigned IssueQueueCapacity = 8;
        static constexpr unsigned ScoreboardRegisters = 32;
        std::deque<IssueEntry> issueQueue;
        std::deque<ReplayGeneration> replayGenerations;
        std::deque<ReplayLaneContext> replayLanes;
        std::deque<DVRDynUop> virBuffer;
        // Decoded instructions belong to the helper context and never enter
        // the main O3 fetch queue.
        std::unordered_map<Addr, StaticInstPtr> decodedUopCache;
        std::array<Tick, ScoreboardRegisters> readyCycle = {};

        void reset()
        {
            state = State::Idle;
            id = 0;
            triggerPC = 0;
            programUops = 0;
            maxUops = 0;
            workUnits = 0;
            nextLane = 0;
            issuedUops = 0;
            outstanding = 0;
            fetchRemaining = 0;
            decodeRemaining = 0;
            readyUops = 0;
            aluRemaining = 0;
            shiftRemaining = 0;
            multiplyRemaining = 0;
            computeReadyTick = 0;
            vectorChunkModel = false;
            helperPC = 0;
            issueQueue.clear();
            replayGenerations.clear();
            replayLanes.clear();
            virBuffer.clear();
            decodedUopCache.clear();
            readyCycle.fill(0);
        }

        void begin(uint64_t helper_id, Addr pc, unsigned uops,
                   unsigned units, unsigned budget,
                   const DVRInstructionRecorder::ResourceCounts &resources,
                   bool vector_model,
                   std::shared_ptr<const DVRReplayTemplate> replay)
        {
            id = helper_id;
            triggerPC = pc;
            programUops = uops;
            maxUops = budget;
            workUnits = units;
            helperPC = pc;
            nextLane = 0;
            issuedUops = 0;
            outstanding = 0;
            // Each helper work unit executes the captured program.  A work
            // unit is either a scalar lane or a 512-bit vector chunk.
            const uint64_t total_uops = uint64_t(programUops) * units;
            fetchRemaining = std::min<uint64_t>(total_uops, maxUops);
            decodeRemaining = 0;
            readyUops = 0;
            aluRemaining = replay && vector_model ? 0 : resources.alu;
            shiftRemaining = replay && vector_model ? 0 : resources.shift;
            multiplyRemaining = replay && vector_model ? 0 : resources.multiply;
            vectorChunkModel = vector_model;
            issueQueue.clear();
            replayGenerations.clear();
            replayLanes.clear();
            virBuffer.clear();
            decodedUopCache.clear();
            readyCycle.fill(0);
            state = fetchRemaining == 0 ? State::Idle : State::Fetch;
        }

        void extend(Addr pc, unsigned uops, unsigned units,
                    const DVRInstructionRecorder::ResourceCounts &resources,
                    bool vector_model,
                    std::shared_ptr<const DVRReplayTemplate> replay)
        {
            if (state == State::Idle) {
                begin(id, pc, uops, units, maxUops, resources, vector_model,
                      replay);
                return;
            }

            // A discovery may complete while an older generation is still
            // draining.  Preserve both generations in the bounded helper
            // stream instead of resetting the helper's frontend and issue
            // budget to the newest one.
            const uint64_t generation_uops =
                std::min<uint64_t>(uint64_t(uops) * units, maxUops);
            fetchRemaining += generation_uops;
            maxUops += generation_uops;
            if (!(replay && vector_model)) {
                aluRemaining += resources.alu;
                shiftRemaining += resources.shift;
                multiplyRemaining += resources.multiply;
            }
            vectorChunkModel = vector_model;
            if (state == State::Draining || state == State::Decode)
                state = State::Fetch;
        }

        unsigned advanceFrontend(unsigned fetch_width, unsigned decode_width)
        {
            if (state == State::Idle || state == State::Draining)
                return 0;

            unsigned fetched = 0;
            if (fetchRemaining != 0) {
                fetched = std::min(fetch_width, fetchRemaining);
                fetchRemaining -= fetched;
                decodeRemaining += fetched;
            }

            unsigned decoded = std::min(decode_width, decodeRemaining);
            decodeRemaining -= decoded;
            readyUops += decoded;

            if (fetchRemaining == 0 && decodeRemaining == 0 &&
                readyUops == 0 && outstanding == 0) {
                state = State::Idle;
            } else if (readyUops != 0) {
                state = State::Running;
            } else if (fetchRemaining != 0) {
                state = State::Fetch;
            } else {
                state = State::Decode;
            }
            return fetched + decoded;
        }

        unsigned advanceCompute(unsigned alu_width, unsigned shift_width,
                                unsigned multiply_width)
        {
            if (state == State::Idle || state == State::Draining)
                return 0;
            const unsigned alu = std::min(alu_width, aluRemaining);
            const unsigned shift = std::min(shift_width, shiftRemaining);
            const unsigned multiply =
                std::min(multiply_width, multiplyRemaining);
            aluRemaining -= alu;
            shiftRemaining -= shift;
            multiplyRemaining -= multiply;
            return alu + shift + multiply;
        }

        bool computePending() const
        {
            return aluRemaining != 0 || shiftRemaining != 0 ||
                   multiplyRemaining != 0 || !issueQueue.empty() ||
                   !replayGenerations.empty() || !replayLanes.empty();
        }

        bool enqueueReplayLane(const DVRPrefetchSenderState &sender,
                               RegVal source_value,
                               unsigned &became_ready)
        {
            became_ready = 0;
            if (!sender.replay || sender.replay->count <= 1 ||
                !sender.replay->helperRegs ||
                sender.replay->triggerDestination < 0 ||
                sender.replay->triggerDestination >=
                    DVRLoopBoundDetector::MaxArchitecturalIntRegs)
                return false;
            ReplayLaneContext lane_context;
            lane_context.program = sender.replay;
            lane_context.regs = sender.replay->initialRegs;
            lane_context.regs[0] = 0;
            lane_context.regs[sender.replay->triggerDestination] = source_value;
            lane_context.predicate = sender.predicate;
            lane_context.scales = sender.scales;
            lane_context.offsets = sender.offsets;
            lane_context.masks = sender.masks;
            lane_context.patterns = sender.patterns;
            lane_context.relationCount = sender.relationCount;
            lane_context.lane = sender.lane;
            lane_context.tid = sender.tid;
            lane_context.triggerPC = sender.replay->triggerPC;
            lane_context.lanePC = sender.replay->count > 1 ?
                sender.replay->uops[1].pc : 0;
            lane_context.nested = sender.nested;
            sender.replay->helperRegs->write(
                sender.replay->triggerDestination, sender.lane,
                source_value, 0);
            replayLanes.push_back(std::move(lane_context));
            became_ready = readyUops == 0 ? 1 : 0;
            if (became_ready != 0) {
                readyUops = 1;
                state = State::Running;
            }
            return true;
        }

        unsigned retireCompletedVIR(Tick now)
        {
            unsigned retired = 0;
            for (auto it = virBuffer.begin(); it != virBuffer.end();) {
                if (it->state == DVRDynUop::State::Issued &&
                    it->completeCycle <= now) {
                    it->state = DVRDynUop::State::Completed;
                    ++retired;
                    it = virBuffer.erase(it);
                } else {
                    ++it;
                }
            }
            return retired;
        }

        void refillIssueQueue()
        {
            if (!vectorChunkModel)
                return;
            while (issueQueue.size() < IssueQueueCapacity) {
                if (!replayGenerations.empty()) {
                    auto &generation = replayGenerations.front();
                    if (!generation.program || generation.workUnits == 0) {
                        replayGenerations.pop_front();
                        continue;
                    }
                    if (generation.uop >= generation.program->count) {
                        generation.uop = 0;
                        if (++generation.unit >= generation.workUnits)
                            replayGenerations.pop_front();
                        continue;
                    }
                    const auto &uop = generation.program->uops[
                        generation.uop++];
                    helperPC = uop.pc;
                    // Load uops are represented by the real source/dependent
                    // request queue.  Their compute-side address uops remain
                    // in this stream, while the request itself uses LSQ.
                    const bool address_uop =
                        uop.semantic ==
                        DVRInstructionRecorder::Uop::Semantic::LoadAddress;
                    if (uop.load && !address_uop)
                        continue;
                    IssueEntry entry;
                    entry.source0 = uop.source0;
                    entry.source1 = uop.source1;
                    entry.destination = uop.destination;
                    entry.pc = uop.pc;
                    entry.nextPC = uop.control ?
                        (uop.branchTaken ? uop.branchTargetPC :
                         uop.fallthroughPC) : uop.pc + 4;
                    if (uop.semantic ==
                            DVRInstructionRecorder::Uop::Semantic::ShiftLeft ||
                        uop.semantic ==
                            DVRInstructionRecorder::Uop::Semantic::ShiftRightLogical ||
                        uop.semantic ==
                            DVRInstructionRecorder::Uop::Semantic::ShiftRightArithmetic ||
                        uop.semantic ==
                            DVRInstructionRecorder::Uop::Semantic::ShiftLeftImmediate ||
                        uop.semantic ==
                            DVRInstructionRecorder::Uop::Semantic::ShiftLeftWordImmediate ||
                        uop.semantic ==
                            DVRInstructionRecorder::Uop::Semantic::ShiftRightLogicalImmediate ||
                        uop.semantic ==
                            DVRInstructionRecorder::Uop::Semantic::ShiftRightArithmeticImmediate ||
                        uop.semantic ==
                            DVRInstructionRecorder::Uop::Semantic::ShiftRightLogicalWordImmediate ||
                        uop.semantic ==
                            DVRInstructionRecorder::Uop::Semantic::ShiftRightArithmeticWordImmediate) {
                        entry.kind = ComputeKind::Shift;
                    } else if (
                        uop.semantic ==
                            DVRInstructionRecorder::Uop::Semantic::Multiply ||
                        uop.semantic ==
                            DVRInstructionRecorder::Uop::Semantic::MultiplyWord) {
                        entry.kind = ComputeKind::Multiply;
                    } else if (
                        uop.semantic ==
                            DVRInstructionRecorder::Uop::Semantic::Add ||
                        uop.semantic ==
                            DVRInstructionRecorder::Uop::Semantic::Sub ||
                        uop.semantic ==
                            DVRInstructionRecorder::Uop::Semantic::AddWord ||
                        uop.semantic ==
                            DVRInstructionRecorder::Uop::Semantic::SubWord ||
                        uop.semantic ==
                            DVRInstructionRecorder::Uop::Semantic::AddImmediate ||
                        uop.semantic ==
                            DVRInstructionRecorder::Uop::Semantic::AddWordImmediate ||
                        uop.semantic ==
                            DVRInstructionRecorder::Uop::Semantic::LoadAddress ||
                        uop.semantic ==
                            DVRInstructionRecorder::Uop::Semantic::LoadByteSigned ||
                        uop.semantic ==
                            DVRInstructionRecorder::Uop::Semantic::LoadHalfSigned ||
                        uop.semantic ==
                            DVRInstructionRecorder::Uop::Semantic::LoadWordSigned ||
                        uop.semantic ==
                            DVRInstructionRecorder::Uop::Semantic::LoadWordUnsigned ||
                        uop.semantic ==
                            DVRInstructionRecorder::Uop::Semantic::LoadDouble) {
                        // Address generation and integer add/sub use the
                        // dedicated vector-add class, not the generic SIMD
                        // ALU class.
                        entry.kind = ComputeKind::Add;
                    } else {
                        entry.kind = ComputeKind::Alu;
                    }
                    issueQueue.push_back(entry);
                    continue;
                }

                if (aluRemaining == 0 && shiftRemaining == 0 &&
                    multiplyRemaining == 0)
                    break;
                IssueEntry entry;
                entry.source0 = 0;
                entry.destination = 0;
                entry.pc = helperPC;
                entry.nextPC = helperPC + 4;
                if (aluRemaining != 0) {
                    entry.kind = ComputeKind::Alu;
                    --aluRemaining;
                } else if (shiftRemaining != 0) {
                    entry.kind = ComputeKind::Shift;
                    --shiftRemaining;
                } else {
                    entry.kind = ComputeKind::Multiply;
                    --multiplyRemaining;
                }
                issueQueue.push_back(entry);
            }
        }

        bool issueQueueReady(Tick now) const
        {
            if (!vectorChunkModel || issueQueue.empty())
                return false;
            const auto &entry = issueQueue.front();
            const bool source0_ready = entry.source0 < 0 ||
                entry.source0 >= ScoreboardRegisters ||
                readyCycle[entry.source0] <= now;
            const bool source1_ready = entry.source1 < 0 ||
                entry.source1 >= ScoreboardRegisters ||
                readyCycle[entry.source1] <= now;
            return entry.readyTick <= now && source0_ready && source1_ready;
        }

        bool issueQueueScoreboardBlocked(Tick now) const
        {
            if (issueQueue.empty())
                return false;
            const auto &entry = issueQueue.front();
            return (entry.source0 >= 0 &&
                    entry.source0 < ScoreboardRegisters &&
                    readyCycle[entry.source0] > now) ||
                   (entry.source1 >= 0 &&
                    entry.source1 < ScoreboardRegisters &&
                    readyCycle[entry.source1] > now);
        }

        bool canIssue(bool source_request = false) const
        {
            return state == State::Running && readyUops != 0 &&
                   issuedUops < maxUops &&
                   (source_request || !computePending());
        }

        unsigned wakeForMemoryRequest()
        {
            if (computePending())
                return 0;
            unsigned became_ready = 0;
            if (state == State::Idle || state == State::Draining ||
                (state == State::Running && readyUops == 0 &&
                 fetchRemaining == 0 && decodeRemaining == 0)) {
                state = State::Running;
                readyUops = std::max(readyUops, 1u);
                became_ready = 1;
            }
            // External source responses can create replay loads after the
            // captured frontend budget has been consumed.  Give each such
            // queued request one bounded issue credit.
            if (issuedUops >= maxUops)
                maxUops = issuedUops + 1;
            return became_ready;
        }

        void issueCompute(Tick ready_tick)
        {
            assert(readyUops != 0);
            assert(vectorChunkModel);
            assert(!issueQueue.empty());
            const auto entry = issueQueue.front();
            issueQueue.pop_front();
            ++issuedUops;
            --readyUops;
            if (entry.destination >= 0 &&
                entry.destination < ScoreboardRegisters)
                readyCycle[entry.destination] = ready_tick;
            helperPC = entry.nextPC;
            computeReadyTick = std::max(computeReadyTick, ready_tick);
            if (issuedUops >= maxUops && readyUops == 0 &&
                fetchRemaining == 0 && decodeRemaining == 0 &&
                outstanding == 0 && !computePending())
                state = State::Draining;
        }

        void issueReplayChunk(Tick ready_tick)
        {
            assert(vectorChunkModel);
            assert(readyUops != 0);
            ++issuedUops;
            --readyUops;
            computeReadyTick = std::max(computeReadyTick, ready_tick);
            if (issuedUops >= maxUops && readyUops == 0 &&
                fetchRemaining == 0 && decodeRemaining == 0 &&
                outstanding == 0 && replayLanes.empty())
                state = State::Draining;
        }

        unsigned refillComputeReady()
        {
            refillIssueQueue();
            if (!computePending() || readyUops != 0 ||
                fetchRemaining != 0 || decodeRemaining != 0)
                return 0;
            state = State::Running;
            readyUops = 1;
            return 1;
        }

        bool computeComplete(Tick now) const
        {
            return computeReadyTick <= now;
        }

        void issue()
        {
            ++issuedUops;
            assert(readyUops != 0);
            --readyUops;
            ++outstanding;
            if (issuedUops >= maxUops && readyUops == 0 &&
                fetchRemaining == 0 && decodeRemaining == 0)
                state = State::Draining;
        }

        void complete()
        {
            if (outstanding != 0)
                --outstanding;
            if (state == State::Draining && outstanding == 0)
                state = State::Idle;
        }

        bool active() const { return state != State::Idle; }

        StaticInstPtr decodeUop(Addr pc, const StaticInstPtr &decoded,
                                bool &cache_hit)
        {
            auto found = decodedUopCache.find(pc);
            if (found != decodedUopCache.end()) {
                cache_hit = true;
                return found->second;
            }
            cache_hit = false;
            if (decoded)
                decodedUopCache.emplace(pc, decoded);
            return decoded;
        }
    } dvrHelperThread;
    // Main O3 stages run first every cycle.  Helpers may consume at most one
    // residual issue/LSU slot after IEW has completed.
    unsigned dvrHelperIssuesThisCycle = 0;
    unsigned dvrHelperComputeIssuesThisCycle = 0;
    unsigned dvrMainIssuesThisCycle = 0;
    unsigned dvrMainALUIssuesThisCycle = 0;
    unsigned dvrMainLSUIssuesThisCycle = 0;
    unsigned dvrIssueWidth = 1;
    unsigned dvrFetchWidth = 1;
    unsigned dvrDecodeWidth = 1;
    unsigned dvrLSUWidth = 1;
    unsigned dvrReplayMaxGroupWidth = 0;
    static constexpr unsigned DvrHelperIssueWidth = 4;
    static constexpr unsigned DvrVectorBits = 512;
    // Keep a small number of generations in flight.  Without this bound,
    // dispatch can discover every loop iteration faster than the single
    // residual LSU slot drains source lanes, burying dependent targets behind
    // an unbounded source queue.
    static constexpr unsigned DvrMaxQueuedPrefetches = 256;
    std::deque<DVRPrefetchAddress> dvrPrefetchQueue;
    // Source values are lane-specific even when they share a cache line, so
    // source requests are deduplicated by byte address.  Dependent loads only
    // need one request per cache line.
    std::unordered_set<Addr> dvrQueuedPrefetchAddresses;
    std::unordered_set<Addr> dvrOutstandingPrefetchAddresses;
    std::unordered_set<Addr> dvrQueuedDependentLines;
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
        Addr minAddress = 0;
        Addr maxAddress = 0;
        unsigned samples = 0;
    };
    struct DVRRelationKey
    {
        Addr triggerPC;
        Addr flrPC;

        bool operator==(const DVRRelationKey &other) const
        {
            return triggerPC == other.triggerPC && flrPC == other.flrPC;
        }
    };
    struct DVRRelationKeyHash
    {
        size_t operator()(const DVRRelationKey &key) const
        {
            const size_t lhs = std::hash<Addr>{}(key.triggerPC);
            const size_t rhs = std::hash<Addr>{}(key.flrPC);
            return lhs ^ (rhs + size_t(0x9e3779b9) + (lhs << 6) +
                          (lhs >> 2));
        }
    };
    std::unordered_map<DVRRelationKey, DVRAddressRelation,
                       DVRRelationKeyHash> dvrAddressRelations;
    std::unordered_map<Addr, std::vector<Addr>> dvrTriggerRelations;
    std::unordered_map<Addr, unsigned> dvrOutstandingPrefetchLines;
    uint64_t dvrOutstandingPrefetchLinePeakValue = 0;
    std::unordered_map<Addr, Tick> dvrCompletedPrefetchLines;
    std::unordered_set<Addr> dvrDependentLoadPCs;
    std::unordered_map<Addr, unsigned> dvrDependentOutstandingLines;
    std::unordered_set<Addr> dvrDependentCompletedLines;
    std::unordered_set<Addr> dvrAlternateDependentLines;
    struct DVRAlternatePathKey
    {
        Addr branchPC = 0;
        Addr targetPC = 0;
        Addr reconvergencePC = 0;
        ContextID addressSpaceID = 0;

        bool operator==(const DVRAlternatePathKey &other) const
        {
            return branchPC == other.branchPC && targetPC == other.targetPC &&
                reconvergencePC == other.reconvergencePC &&
                addressSpaceID == other.addressSpaceID;
        }
    };
    struct DVRAlternatePathKeyHash
    {
        size_t operator()(const DVRAlternatePathKey &key) const
        {
            size_t hash = std::hash<Addr>{}(key.branchPC);
            hash ^= std::hash<Addr>{}(key.targetPC) +
                size_t(0x9e3779b9) + (hash << 6) + (hash >> 2);
            hash ^= std::hash<Addr>{}(key.reconvergencePC) +
                size_t(0x9e3779b9) + (hash << 6) + (hash >> 2);
            hash ^= std::hash<ContextID>{}(key.addressSpaceID) +
                size_t(0x9e3779b9) + (hash << 6) + (hash >> 2);
            return hash;
        }
    };
    struct DVRAlternatePath
    {
        std::vector<DVRInstructionRecorder::Uop> uops;
        uint32_t liveInRegisters = 0;
        uint64_t codeVersion = 0;
        bool complete = false;
    };
    std::unordered_map<DVRAlternatePathKey, DVRAlternatePath,
                       DVRAlternatePathKeyHash> dvrAlternatePathCache;
    uint64_t dvrPrefetchQueuePeak = 0;
    uint64_t dvrNextPredicateGeneration = 1;
    uint64_t dvrNextHelperId = 1;
    std::shared_ptr<DVRPredicateGeneration> dvrActivePredicateGeneration;
    Addr dvrCurrentTriggerPC = 0;
    Addr dvrCurrentTriggerAddress = 0;
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
    DVRPendingNestedCandidate dvrCommittedNestedCandidate;

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

    /**
     * Completed dynamic outer invocations waiting to be flattened.  An
     * invocation is added only after its closing recurrence has committed,
     * so every base has an independently inferred inner-lane count.
     */
    struct DVRNestedInvocationBatch
    {
        Addr triggerPC = 0;
        std::array<Addr, 16> bases = {};
        std::array<unsigned, 16> innerLanes = {};
        std::array<int64_t, 16> innerStrides = {};
        unsigned count = 0;

        void reset()
        {
            triggerPC = 0;
            bases = {};
            innerLanes = {};
            innerStrides = {};
            count = 0;
        }
    } dvrNestedInvocationBatch;

    void captureDVRRegisterSnapshot(
        ThreadID tid, const DynInstPtr &committing_inst,
        DVRLoopBoundDetector::RegisterSnapshot &snapshot);
    void beginDVRDiscoveryAtDispatch(
        const DynInstPtr &inst, const DVRStrideDetector::Candidate &candidate,
        bool restart);
    void launchDVRStridePrefetches(ThreadID tid, Addr current_address,
                                   Addr pc, int64_t stride, unsigned lanes,
                                   const DVRLoopBoundDetector::RegisterSnapshot
                                   &finish_regs);
    void launchDVRVectorRunahead(ThreadID tid, Addr current_address,
                                 Addr pc, int64_t stride);
    void completeDVRNestedContext(
        const DynInstPtr &committing_inst,
        const DVRLoopBoundDetector::RegisterSnapshot &finish_regs);
    void launchDVRNestedPrefetches(
        const DVRLoopBoundDetector::RegisterSnapshot &finish_regs);
    void serviceDVRPrefetchQueue();

  public:
    /** IEW reports demand execution before the lower-priority helper runs. */
    void recordDVRMainThreadIssue(bool memory)
    {
        ++dvrMainIssuesThisCycle;
        if (memory)
            ++dvrMainLSUIssuesThisCycle;
        else
            ++dvrMainALUIssuesThisCycle;
    }

  private:
    void startDVRHelper(Addr trigger_pc, unsigned program_uops,
                        unsigned lanes,
                        const DVRInstructionRecorder::ResourceCounts
                        &resources = {},
                        std::shared_ptr<const DVRReplayTemplate> replay =
                            nullptr);
    unsigned dvrHelperWorkUnits(unsigned lanes) const
    {
        if (!dvrVectorChunkModel)
            return lanes;
        const unsigned elements_per_chunk = dvrElementsPerChunk();
        return (lanes + elements_per_chunk - 1) / elements_per_chunk;
    }
    unsigned dvrElementsPerChunk() const
    {
        return DvrVectorBits / dvrVectorElementBits;
    }
    unsigned issueDVRHelperCompute();
    unsigned issueDVRReplayLanes(unsigned slots);
    Addr dvrPrefetchLine(Addr address) const;
    void accountDVRDemand(Addr address);
    void updateDVRPrefetchQueuePeak();
    unsigned dvrPrefetchBytes(const DVRPrefetchAddress &prefetch) const;
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
    void recordDVRAlternatePaths(const DVRInstructionRecorder &recorder,
                                 ContextID address_space_id);
    void augmentDVRAlternatePaths(DVRInstructionRecorder &recorder,
                                  ContextID address_space_id,
                                  const DVRLoopBoundDetector::RegisterSnapshot
                                      &initial_regs);

    /** Checkpoints of the rename map when entering PRE. */
    std::vector<PhysRegIdPtr> checkpointRenameMap[CCRegClass + 1];
    std::vector<PhysRegIdPtr> checkpointFreeList[CCRegClass + 1];
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_CPU_HH__
