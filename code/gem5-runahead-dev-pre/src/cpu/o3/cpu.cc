/*
 * Copyright (c) 2011-2012, 2014, 2016, 2017, 2019-2020 ARM Limited
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
 * Copyright (c) 2004-2006 The Regents of The University of Michigan
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

#include "cpu/o3/cpu.hh"

#include "mem/packet_access.hh"

#include "config/the_isa.hh"
#include "cpu/activity.hh"
#include "cpu/checker/cpu.hh"
#include "cpu/checker/thread_context.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/limits.hh"
#include "cpu/o3/thread_context.hh"
#include "cpu/simple_thread.hh"
#include "cpu/thread_context.hh"
#include "debug/Activity.hh"
#include "debug/Drain.hh"
#include "debug/O3CPU.hh"
#include "debug/Quiesce.hh"
#include "enums/MemoryMode.hh"
#include "sim/cur_tick.hh"
#include "sim/full_system.hh"
#include "sim/process.hh"
#include "sim/stat_control.hh"
#include "sim/system.hh"

namespace gem5
{

bool MJ::enable;
Tick MJ::lastTick;
MJ::MJ(const char *stage, const char *event)
{
    if (!enable)
        return;
    if (curTick() != lastTick) {
        lastTick = curTick();
        std::cout << std::endl;
    }
    std::cout << "[" << stage << "] @" << curTick() << " " << event;
}

struct BaseCPUParams;

namespace o3
{

CPU::DVRPrefetchSenderState::DVRPrefetchSenderState(
    bool is_source, bool is_nested, unsigned relation_count,
    const std::array<int64_t, MaxRelations> &relation_scales,
    const std::array<int64_t, MaxRelations> &relation_offsets,
    const std::array<RegVal, MaxRelations> &relation_masks,
    const std::array<RegVal, MaxRelations> &relation_patterns,
    std::shared_ptr<const DVRReplayTemplate> replay_template,
    std::shared_ptr<DVRPredicateGeneration> predicate_generation,
    unsigned lane_id,
    ThreadID thread)
    : source(is_source), nested(is_nested), relationCount(relation_count),
      scales(relation_scales), offsets(relation_offsets),
      masks(relation_masks), patterns(relation_patterns),
      replay(std::move(replay_template)),
      predicate(std::move(predicate_generation)), lane(lane_id), tid(thread)
{
}

CPU::CPU(const BaseO3CPUParams &params)
    : BaseCPU(params),
      mmu(params.mmu),
      tickEvent([this]{ tick(); }, "O3CPU tick",
                false, Event::CPU_Tick_Pri),
      threadExitEvent([this]{ exitThreads(); }, "O3CPU exit threads",
                false, Event::CPU_Exit_Pri),
#ifndef NDEBUG
      instcount(0),
#endif
      removeInstsThisCycle(false),
      fetch(this, params),
      decode(this, params),
      rename(this, params),
      iew(this, params),
      commit(this, params),

      regFile(params.numPhysIntRegs,
              params.numPhysFloatRegs,
              params.numPhysVecRegs,
              params.numPhysVecPredRegs,
              params.numPhysCCRegs,
              params.isa[0]->regClasses()),

      freeList(name() + ".freelist", &regFile),

      rob(this, params),

      sst(this, params),

      scoreboard(name() + ".scoreboard", regFile.totalNumPhysRegs()),

      isa(numThreads, NULL),

      timeBuffer(params.backComSize, params.forwardComSize),
      fetchQueue(params.backComSize, params.forwardComSize),
      decodeQueue(params.backComSize, params.forwardComSize),
      renameQueue(params.backComSize, params.forwardComSize),
      iewQueue(params.backComSize, params.forwardComSize),
      activityRec(name(), NumStages,
                  params.backComSize + params.forwardComSize,
                  params.activity),

      globalSeqNum(1),
      system(params.system),
      lastRunningCycle(curCycle()),
      cpuStats(this),
      enablePRE(params.enablePRE),
      inPRE(false),
      enableDVR(params.enableDVR),
      dvrMode(params.dvrMode),
      dvrStrideDetector(params.dvrRPTEntries),
      dvrDiscovery(params.dvrDiscoveryMaxInsts),
      dvrNestedController(params.dvrDiscoveryMaxInsts),
      dvrNestedDiscoveryMode(
          params.dvrNDMThreshold, params.dvrNDMMaxInsts),
      dvrQualityTracker(1, 1),
      dvrMaxLanes(params.dvrMaxLanes),
      dvrHelperMaxUops(params.dvrHelperMaxUops),
      dvrEnableDependentPrefetch(params.dvrEnableDependentPrefetch)
{
    dvrIssueWidth = params.issueWidth;
    dvrFetchWidth = params.fetchWidth;
    dvrDecodeWidth = params.decodeWidth;
    // Table-1 has two load/store pipelines.  Demand memory operations consume
    // these first; DVR may use only the residual capacity.
    dvrLSUWidth = 2;
    fatal_if(FullSystem && params.numThreads > 1,
            "SMT is not supported in O3 in full system mode currently.");

    fatal_if(!FullSystem && params.numThreads < params.workload.size(),
            "More workload items (%d) than threads (%d) on CPU %s.",
            params.workload.size(), params.numThreads, name());

    if (!params.switched_out) {
        _status = Running;
    } else {
        _status = SwitchedOut;
    }

    if (params.checker) {
        BaseCPU *temp_checker = params.checker;
        checker = dynamic_cast<Checker<DynInstPtr> *>(temp_checker);
        checker->setIcachePort(&fetch.getInstPort());
        checker->setSystem(params.system);
    } else {
        checker = NULL;
    }

    if (!FullSystem) {
        thread.resize(numThreads);
        tids.resize(numThreads);
    }

    // The stages also need their CPU pointer setup.  However this
    // must be done at the upper level CPU because they have pointers
    // to the upper level CPU, and not this CPU.

    // Set up Pointers to the activeThreads list for each stage
    fetch.setActiveThreads(&activeThreads);
    decode.setActiveThreads(&activeThreads);
    rename.setActiveThreads(&activeThreads);
    iew.setActiveThreads(&activeThreads);
    commit.setActiveThreads(&activeThreads);

    // Give each of the stages the time buffer they will use.
    fetch.setTimeBuffer(&timeBuffer);
    decode.setTimeBuffer(&timeBuffer);
    rename.setTimeBuffer(&timeBuffer);
    iew.setTimeBuffer(&timeBuffer);
    commit.setTimeBuffer(&timeBuffer);

    // Also setup each of the stages' queues.
    fetch.setFetchQueue(&fetchQueue);
    decode.setFetchQueue(&fetchQueue);
    commit.setFetchQueue(&fetchQueue);
    decode.setDecodeQueue(&decodeQueue);
    rename.setDecodeQueue(&decodeQueue);
    rename.setRenameQueue(&renameQueue);
    iew.setRenameQueue(&renameQueue);
    iew.setIEWQueue(&iewQueue);
    commit.setIEWQueue(&iewQueue);
    commit.setRenameQueue(&renameQueue);

    commit.setIEWStage(&iew);
    rename.setIEWStage(&iew);
    rename.setCommitStage(&commit);

    ThreadID active_threads;
    if (FullSystem) {
        active_threads = 1;
    } else {
        active_threads = params.workload.size();

        if (active_threads > MaxThreads) {
            panic("Workload Size too large. Increase the 'MaxThreads' "
                  "constant in cpu/o3/limits.hh or edit your workload size.");
        }
    }

    // Make Sure That this a Valid Architeture
    assert(numThreads);
    const auto &regClasses = params.isa[0]->regClasses();

    assert(params.numPhysIntRegs >=
            numThreads * regClasses.at(IntRegClass).numRegs());
    assert(params.numPhysFloatRegs >=
            numThreads * regClasses.at(FloatRegClass).numRegs());
    assert(params.numPhysVecRegs >=
            numThreads * regClasses.at(VecRegClass).numRegs());
    assert(params.numPhysVecPredRegs >=
            numThreads * regClasses.at(VecPredRegClass).numRegs());
    assert(params.numPhysCCRegs >=
            numThreads * regClasses.at(CCRegClass).numRegs());

    if (numThreads > 1 && enablePRE) {
        fatal("PRE supports single thread only.\n");
    }

    // Just make this a warning and go ahead anyway, to keep from having to
    // add checks everywhere.
    warn_if(regClasses.at(CCRegClass).numRegs() == 0 &&
            params.numPhysCCRegs != 0,
            "Non-zero number of physical CC regs specified, even though\n"
            "    ISA does not use them.");

    rename.setScoreboard(&scoreboard);
    iew.setScoreboard(&scoreboard);

    // Setup the rename map for whichever stages need it.
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        isa[tid] = dynamic_cast<TheISA::ISA *>(params.isa[tid]);
        commitRenameMap[tid].init(regClasses, &regFile, &freeList);
        renameMap[tid].init(regClasses, &regFile, &freeList);
    }

    // Initialize rename map to assign physical registers to the
    // architectural registers for active threads only.
    for (ThreadID tid = 0; tid < active_threads; tid++) {
        for (auto type = (RegClassType)0; type <= CCRegClass;
                type = (RegClassType)(type + 1)) {
            for (RegIndex ridx = 0; ridx < regClasses.at(type).numRegs();
                    ++ridx) {
                // Note that we can't use the rename() method because we don't
                // want special treatment for the zero register at this point
                RegId rid = RegId(type, ridx);
                PhysRegIdPtr phys_reg = freeList.getReg(type);
                renameMap[tid].setEntry(rid, phys_reg);
                commitRenameMap[tid].setEntry(rid, phys_reg);
            }
        }
    }

    rename.setRenameMap(renameMap);
    commit.setRenameMap(commitRenameMap);
    rename.setFreeList(&freeList);

    // Setup the ROB for whichever stages need it.
    commit.setROB(&rob);

    // Setup PRE utilities
    decode.setSST(&sst);
    rename.setSST(&sst);
    commit.setSST(&sst);

    lastActivatedCycle = 0;

    DPRINTF(O3CPU, "Creating O3CPU object.\n");

    // Setup any thread state.
    thread.resize(numThreads);

    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        if (FullSystem) {
            // SMT is not supported in FS mode yet.
            assert(numThreads == 1);
            thread[tid] = new ThreadState(this, 0, NULL);
        } else {
            if (tid < params.workload.size()) {
                DPRINTF(O3CPU, "Workload[%i] process is %#x", tid,
                        thread[tid]);
                thread[tid] = new ThreadState(this, tid, params.workload[tid]);
            } else {
                //Allocate Empty thread so M5 can use later
                //when scheduling threads to CPU
                Process* dummy_proc = NULL;

                thread[tid] = new ThreadState(this, tid, dummy_proc);
            }
        }

        gem5::ThreadContext *tc;

        // Setup the TC that will serve as the interface to the threads/CPU.
        auto *o3_tc = new ThreadContext;

        tc = o3_tc;

        // If we're using a checker, then the TC should be the
        // CheckerThreadContext.
        if (params.checker) {
            tc = new CheckerThreadContext<ThreadContext>(o3_tc, checker);
        }

        o3_tc->cpu = this;
        o3_tc->thread = thread[tid];

        // Give the thread the TC.
        thread[tid]->tc = tc;

        // Add the TC to the CPU's list of TC's.
        threadContexts.push_back(tc);
    }

    // O3CPU always requires an interrupt controller.
    if (!params.switched_out && interrupts.empty()) {
        fatal("O3CPU %s has no interrupt controller.\n"
              "Ensure createInterruptController() is called.\n", name());
    }

    // Initiate my journal.
    MJ::enable = params.enableMJ;
    MJ::lastTick = 0;
}

void
CPU::regProbePoints()
{
    BaseCPU::regProbePoints();

    ppInstAccessComplete = new ProbePointArg<PacketPtr>(
            getProbeManager(), "InstAccessComplete");
    ppDataAccessComplete = new ProbePointArg<
        std::pair<DynInstPtr, PacketPtr>>(
                getProbeManager(), "DataAccessComplete");

    fetch.regProbePoints();
    rename.regProbePoints();
    iew.regProbePoints();
    commit.regProbePoints();
}

CPU::CPUStats::CPUStats(CPU *cpu)
    : statistics::Group(cpu),
      ADD_STAT(timesIdled, statistics::units::Count::get(),
               "Number of times that the entire CPU went into an idle state "
               "and unscheduled itself"),
      ADD_STAT(idleCycles, statistics::units::Cycle::get(),
               "Total number of cycles that the CPU has spent unscheduled due "
               "to idling"),
      ADD_STAT(quiesceCycles, statistics::units::Cycle::get(),
               "Total number of cycles that CPU has spent quiesced or waiting "
               "for an interrupt"),
      ADD_STAT(committedInsts, statistics::units::Count::get(),
               "Number of Instructions Simulated"),
      ADD_STAT(committedOps, statistics::units::Count::get(),
               "Number of Ops (including micro ops) Simulated"),
      ADD_STAT(cpi, statistics::units::Rate<
                    statistics::units::Cycle, statistics::units::Count>::get(),
               "CPI: Cycles Per Instruction"),
      ADD_STAT(totalCpi, statistics::units::Rate<
                    statistics::units::Cycle, statistics::units::Count>::get(),
               "CPI: Total CPI of All Threads"),
      ADD_STAT(ipc, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "IPC: Instructions Per Cycle"),
      ADD_STAT(totalIpc, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "IPC: Total IPC of All Threads"),
      ADD_STAT(intRegfileReads, statistics::units::Count::get(),
               "Number of integer regfile reads"),
      ADD_STAT(intRegfileWrites, statistics::units::Count::get(),
               "Number of integer regfile writes"),
      ADD_STAT(fpRegfileReads, statistics::units::Count::get(),
               "Number of floating regfile reads"),
      ADD_STAT(fpRegfileWrites, statistics::units::Count::get(),
               "Number of floating regfile writes"),
      ADD_STAT(vecRegfileReads, statistics::units::Count::get(),
               "number of vector regfile reads"),
      ADD_STAT(vecRegfileWrites, statistics::units::Count::get(),
               "number of vector regfile writes"),
      ADD_STAT(vecPredRegfileReads, statistics::units::Count::get(),
               "number of predicate regfile reads"),
      ADD_STAT(vecPredRegfileWrites, statistics::units::Count::get(),
               "number of predicate regfile writes"),
      ADD_STAT(ccRegfileReads, statistics::units::Count::get(),
               "number of cc regfile reads"),
      ADD_STAT(ccRegfileWrites, statistics::units::Count::get(),
               "number of cc regfile writes"),
      ADD_STAT(miscRegfileReads, statistics::units::Count::get(),
               "number of misc regfile reads"),
      ADD_STAT(miscRegfileWrites, statistics::units::Count::get(),
               "number of misc regfile writes"),
      ADD_STAT(dvrLoadsObserved, statistics::units::Count::get(),
               "DVR main-thread loads observed by the RPT"),
      ADD_STAT(dvrStrideCandidates, statistics::units::Count::get(),
               "DVR confident striding-load observations"),
      ADD_STAT(dvrDiscoveryStarts, statistics::units::Count::get(),
               "DVR 发现阶段在提交时启动的次数"),
      ADD_STAT(dvrDiscoveryCompletions, statistics::units::Count::get(),
               "由 trigger load 结束的 DVR 发现阶段数"),
      ADD_STAT(dvrDiscoveryTimeouts, statistics::units::Count::get(),
               "因指令数上限结束的 DVR 发现阶段数"),
      ADD_STAT(dvrDiscoveryAbandons, statistics::units::Count::get(),
               "DVR armed triggers discarded after speculative squash"),
      ADD_STAT(dvrDiscoveryRollbacks, statistics::units::Count::get(),
               "Discovery checkpoints discarded by an O3 squash"),
      ADD_STAT(dvrNestedRootStarts, statistics::units::Count::get(),
               "Root discoveries mirrored into the nested controller"),
      ADD_STAT(dvrNestedStarts, statistics::units::Count::get(),
               "Committed inner striding loads that start nested discovery"),
      ADD_STAT(dvrNestedCompletions, statistics::units::Count::get(),
               "Nested discoveries completed at inner-trigger recurrence"),
      ADD_STAT(dvrNestedTimeouts, statistics::units::Count::get(),
               "Nested discovery frames terminated by commit budget"),
      ADD_STAT(dvrNestedDepthRejects, statistics::units::Count::get(),
               "Nested discovery candidates rejected at maximum depth"),
      ADD_STAT(dvrNestedParentResets, statistics::units::Count::get(),
               "Active nested contexts discarded when their root ends"),
      ADD_STAT(dvrNestedContextsBuilt, statistics::units::Count::get(),
               "Nested child contexts with independent mechanism state"),
      ADD_STAT(dvrNestedProgramsBuilt, statistics::units::Count::get(),
               "Nested child trigger-to-FLR vector programs built"),
      ADD_STAT(dvrNestedVRATAllocations, statistics::units::Count::get(),
               "Physical vector mappings allocated by child VRATs"),
      ADD_STAT(dvrNestedVIRExecutions, statistics::units::Count::get(),
               "VIR chunks executed by child contexts"),
      ADD_STAT(dvrNestedHelpersGenerated, statistics::units::Count::get(),
               "Source helper requests generated by nested contexts"),
      ADD_STAT(dvrNestedHelpersIssued, statistics::units::Count::get(),
               "Nested helper memory requests accepted by L1D"),
      ADD_STAT(dvrNestedHelpersCompleted, statistics::units::Count::get(),
               "Nested helper memory responses completed"),
      ADD_STAT(dvrNestedReplayAttempts, statistics::units::Count::get(),
               "Nested source responses offered to child uop replay"),
      ADD_STAT(dvrNestedReplayTargetsGenerated,
               statistics::units::Count::get(),
               "Dependent targets generated by child recorded-uop replay"),
      ADD_STAT(dvrNestedReplayFallbacks, statistics::units::Count::get(),
               "Nested source responses falling back to affine relations"),
      ADD_STAT(dvrNestedDependentGenerated, statistics::units::Count::get(),
               "All dependent helper targets generated by nested contexts"),
      ADD_STAT(dvrNestedFlattenBatches, statistics::units::Count::get(),
               "Nested helper batches flattened from outer x inner loops"),
      ADD_STAT(dvrNestedOuterInstances, statistics::units::Count::get(),
               "Outer-loop instances consumed by nested flatten batches"),
      ADD_STAT(dvrNestedInnerLanes, statistics::units::Count::get(),
               "Inner lanes inferred across nested flatten batches"),
      ADD_STAT(dvrNestedFlattenedLanes, statistics::units::Count::get(),
               "Actual flattened nested lanes, capped at 128 per batch"),
      ADD_STAT(dvrNestedVariableLaneBatches,
               statistics::units::Count::get(),
               "Nested batches containing independently different bounds"),
      ADD_STAT(dvrNDMAttempts, statistics::units::Count::get(),
               "Completed short inner loops entering NDM control"),
      ADD_STAT(dvrNDMOuterFound, statistics::units::Count::get(),
               "Committed distinct outer strides accepted by NDM"),
      ADD_STAT(dvrNDMFallbacks, statistics::units::Count::get(),
               "NDM control generations returning to ordinary inner DVR"),
      ADD_STAT(dvrNDMTimeouts, statistics::units::Count::get(),
               "NDM control generations reaching the commit budget"),
      ADD_STAT(dvrNDMBranchInversions, statistics::units::Count::get(),
               "NDM inner backward branches inverted for outer search"),
      ADD_STAT(dvrNDMIRCaptures, statistics::units::Count::get(),
               "NDM instruction-register branch captures"),
      ADD_STAT(dvrNDMILRCaptures, statistics::units::Count::get(),
               "NDM inner-loop-register captures"),
      ADD_STAT(dvrNDMLCRCaptures, statistics::units::Count::get(),
               "NDM loop-control-register captures"),
      ADD_STAT(dvrNDMOuterInvocations, statistics::units::Count::get(),
               "NDM outer invocations accepted after branch inversion"),
      ADD_STAT(dvrResourceConflicts, statistics::units::Count::get(),
               "DVR attempts blocked by main-thread resource ownership"),
      ADD_STAT(dvrIssueBudgetConflicts, statistics::units::Count::get(),
               "DVR attempts blocked because no residual issue slot exists"),
      ADD_STAT(dvrALUBudgetConflicts, statistics::units::Count::get(),
               "DVR attempts blocked because address ALU capacity is full"),
      ADD_STAT(dvrLSUBudgetConflicts, statistics::units::Count::get(),
               "DVR attempts blocked because demand loads used all LSU slots"),
      ADD_STAT(dvrHelperIssueCycles, statistics::units::Count::get(),
               "Cycles in which a residual DVR issue slot was consumed"),
      ADD_STAT(dvrHelperFetchCycles, statistics::units::Cycle::get(),
               "Cycles in which the DVR helper fetched captured uops"),
      ADD_STAT(dvrHelperDecodeCycles, statistics::units::Cycle::get(),
               "Cycles in which the DVR helper decoded captured uops"),
      ADD_STAT(dvrHelperReadyUops, statistics::units::Count::get(),
               "Captured helper uops made ready for issue"),
      ADD_STAT(dvrHelperComputeCycles, statistics::units::Cycle::get(),
               "Cycles in which captured helper compute uops executed"),
      ADD_STAT(dvrHelperComputeConflicts, statistics::units::Count::get(),
               "Helper compute cycles blocked by main ALU occupancy"),
      ADD_STAT(dvrHelperFURequests, statistics::units::Count::get(),
               "Helper compute uops requesting a native O3 FU"),
      ADD_STAT(dvrHelperFUGrants, statistics::units::Count::get(),
               "Helper compute uops granted a native O3 FU"),
      ADD_STAT(dvrHelperFUStalls, statistics::units::Count::get(),
               "Helper compute uops stalled by native FU availability"),
      ADD_STAT(dvrHelperALUOps, statistics::units::Count::get(),
               "Captured helper ALU/control uops profiled"),
      ADD_STAT(dvrHelperShiftOps, statistics::units::Count::get(),
               "Captured helper shift uops profiled"),
      ADD_STAT(dvrHelperMultiplyOps, statistics::units::Count::get(),
               "Captured helper multiply uops profiled"),
      ADD_STAT(dvrHelperLSUOps, statistics::units::Count::get(),
               "Captured helper load/store uops profiled"),
      ADD_STAT(dvrMainIssueSlotsUsed, statistics::units::Count::get(),
               "Demand instructions executed before DVR arbitration"),
      ADD_STAT(dvrMainALUSlotsUsed, statistics::units::Count::get(),
               "Demand non-memory instructions executed before DVR"),
      ADD_STAT(dvrMainLSUSlotsUsed, statistics::units::Count::get(),
               "Demand memory instructions executed before DVR"),
      ADD_STAT(dvrFetchActiveCycles, statistics::units::Cycle::get(),
               "Cycles with demand fetch activity"),
      ADD_STAT(dvrDecodeActiveCycles, statistics::units::Cycle::get(),
               "Cycles with demand decode activity"),
      ADD_STAT(dvrDiscoveredInstructions, statistics::units::Count::get(),
               "Committed instructions recorded by completed discoveries"),
      ADD_STAT(dvrTaintedInstructions, statistics::units::Count::get(),
               "Discovery instructions with a tainted integer source"),
      ADD_STAT(dvrDependentLoads, statistics::units::Count::get(),
               "Discovery loads whose address input is tainted"),
      ADD_STAT(dvrDiscoveriesWithFLR, statistics::units::Count::get(),
               "Completed discoveries with a nonzero Final Load Register"),
      ADD_STAT(dvrBackwardBranches, statistics::units::Count::get(),
               "Backward conditional branches seen during discovery"),
      ADD_STAT(dvrLoopBoundsFound, statistics::units::Count::get(),
               "Loop branches enclosing the trigger-to-FLR chain"),
      ADD_STAT(dvrDiscoveriesWithBounds, statistics::units::Count::get(),
               "Completed discoveries with an inferred loop boundary"),
      ADD_STAT(dvrLoopBoundMatches, statistics::units::Count::get(),
               "Loop bounds matched by the two register checkpoints"),
      ADD_STAT(dvrLoopBoundFallbacks, statistics::units::Count::get(),
               "Loop bounds that fell back to the maximum lane count"),
      ADD_STAT(dvrLaneCountSamples, statistics::units::Count::get(),
               "Completed DVR discoveries assigned an active lane count"),
      ADD_STAT(dvrTotalActiveLanes, statistics::units::Count::get(),
               "Sum of active lanes selected across DVR discoveries"),
      ADD_STAT(dvrPrefetchesGenerated, statistics::units::Count::get(),
               "DVR stride-lane prefetch addresses generated"),
      ADD_STAT(dvrPrefetchesIssued, statistics::units::Count::get(),
               "DVR prefetch timing requests accepted by L1D"),
      ADD_STAT(dvrPrefetchesCompleted, statistics::units::Count::get(),
               "DVR prefetch responses consumed by the CPU"),
      ADD_STAT(dvrPrefetchesDropped, statistics::units::Count::get(),
               "DVR prefetches dropped due to replacement or backpressure"),
      ADD_STAT(dvrPrefetchTranslationFaults, statistics::units::Count::get(),
               "DVR prefetch virtual-address translation faults"),
      ADD_STAT(dvrSourcePrefetchTranslationFaults,
               statistics::units::Count::get(),
               "DVR source virtual-address translation faults"),
      ADD_STAT(dvrDependentPrefetchTranslationFaults,
               statistics::units::Count::get(),
               "DVR dependent virtual-address translation faults"),
      ADD_STAT(dvrAddressRelationsTrained, statistics::units::Count::get(),
               "DVR trigger-value to FLR-address affine relations trained"),
      ADD_STAT(dvrDependentPrefetchesGenerated,
               statistics::units::Count::get(),
               "DVR indirect target prefetches generated from source data"),
      ADD_STAT(dvrDependentPrefetchesIssued, statistics::units::Count::get(),
               "DVR dependent prefetches accepted by L1D"),
      ADD_STAT(dvrDependentPrefetchesCompleted,
               statistics::units::Count::get(),
               "DVR dependent prefetch responses completed"),
      ADD_STAT(dvrRecordedUops, statistics::units::Count::get(),
               "Trigger-to-FLR uops retained by the DVR recorder"),
      ADD_STAT(dvrRecorderOverflows, statistics::units::Count::get(),
               "Discoveries whose trigger-to-FLR slice exceeded eight uops"),
      ADD_STAT(dvrVectorProgramsBuilt, statistics::units::Count::get(),
               "Recorded slices materialized as DVR vector programs"),
      ADD_STAT(dvrVRATAllocations, statistics::units::Count::get(),
               "16-lane physical mappings allocated by the DVR VRAT"),
      ADD_STAT(dvrVIRChunkIssues, statistics::units::Count::get(),
               "Recorded uop chunks issued through the DVR VIR"),
      ADD_STAT(dvrVIRChunkExecutions, statistics::units::Count::get(),
               "Recorded uop chunks executed through the DVR VIR"),
      ADD_STAT(dvrDivergentBranches, statistics::units::Count::get(),
               "DVR value-predicate generations whose returned lane values "
               "split the active mask"),
      ADD_STAT(dvrReconvergences, statistics::units::Count::get(),
               "Completed divergent DVR predicate generations whose lane "
               "masks reconverged"),
      ADD_STAT(dvrVIRUnsupportedControlFlow,
               statistics::units::Count::get(),
               "VIR programs terminated because a lane target was outside "
               "the captured recorder"),
      ADD_STAT(dvrVIRNormalTerminatedLanes,
               statistics::units::Count::get(),
               "VIR lanes that reached the end of the captured program"),
      ADD_STAT(dvrVIREarlyExitLanes, statistics::units::Count::get(),
               "VIR lanes that exited through a captured branch"),
      ADD_STAT(dvrVIRExternalPathLanes, statistics::units::Count::get(),
               "VIR lanes that selected a recorder-external target"),
      ADD_STAT(dvrVIRUnsupportedSemanticLanes,
               statistics::units::Count::get(),
               "VIR lanes stopped by an unsupported recorded semantic"),
      ADD_STAT(dvrVIRSourceValueExecutions,
               statistics::units::Count::get(),
               "Per-lane VIR continuations started by source responses"),
      ADD_STAT(dvrVIRSourceValueBranches,
               statistics::units::Count::get(),
               "Branches executed after a real source value was installed"),
      ADD_STAT(dvrVIRSourceValueExternalLanes,
               statistics::units::Count::get(),
               "Source-value VIR lanes selecting recorder-external targets"),
      ADD_STAT(dvrVIRSourceValueSemanticFailures,
               statistics::units::Count::get(),
               "Source-value VIR lanes stopped by unsupported semantics"),
      ADD_STAT(dvrVIRSourceValueTerminations,
               statistics::units::Count::get(),
               "Source-value VIR lanes reaching a terminal path"),
      ADD_STAT(dvrVIRContinuationContexts,
               statistics::units::Count::get(),
               "Persistent multi-lane VIR contexts created per helper"),
      ADD_STAT(dvrVIRContinuationResumes,
               statistics::units::Count::get(),
               "Source responses resumed through persistent VIR contexts"),
      ADD_STAT(dvrVIRContinuationFallbacks,
               statistics::units::Count::get(),
               "Source responses using temporary VIR continuation fallback"),
      ADD_STAT(dvrVIRContinuationPCGroups,
               statistics::units::Count::get(),
               "Current-PC groups issued by persistent VIR contexts"),
      ADD_STAT(dvrVIRContinuationGroupedLanes,
               statistics::units::Count::get(),
               "Lane-uops issued through persistent VIR PC groups"),
      ADD_STAT(dvrVIRContinuationMaxGroupWidth,
               statistics::units::Count::get(),
               "Maximum lane width observed in a persistent VIR PC group"),
      ADD_STAT(dvrPredicateGenerationAbandons,
               statistics::units::Count::get(),
               "DVR predicate generations replaced before all lanes "
               "reached a terminal outcome"),
      ADD_STAT(dvrHelperTimeouts, statistics::units::Count::get(),
               "DVR helpers terminated by the helper-uop budget"),
      ADD_STAT(dvrReconvergenceStackOverflows,
               statistics::units::Count::get(),
               "DVR helpers terminated by an eight-entry stack overflow"),
      ADD_STAT(dvrHelpersSuppressed, statistics::units::Count::get(),
               "DVR helper launches suppressed by invalid or terminated VIR"),
      ADD_STAT(dvrControlFallbackSourceLaunches,
               statistics::units::Count::get(),
               "Source helpers launched after VIR-only control-flow rejection"),
      ADD_STAT(dvrPredicateSelections, statistics::units::Count::get(),
               "Dependent paths selected by learned value predicates"),
      ADD_STAT(dvrDistinctPredicatePaths, statistics::units::Count::get(),
               "Distinct predicate relation slots exercised"),
      ADD_STAT(dvrPredicateMisses, statistics::units::Count::get(),
               "Source values that matched no learned dependent path"),
      ADD_STAT(dvrSourcePrefetchesIssued, statistics::units::Count::get(),
               "DVR source (stride-lane) prefetches accepted by L1D"),
      ADD_STAT(dvrSourcePrefetchesCompleted, statistics::units::Count::get(),
               "DVR source (stride-lane) prefetch responses completed"),
      ADD_STAT(dvrPrefetchQueuePeak, statistics::units::Count::get(),
               "Peak number of waiting DVR helper memory requests"),
      ADD_STAT(dvrPrefetchesSuppressedMainThread,
               statistics::units::Count::get(),
               "DVR requests suppressed because the main-thread data port "
               "was already occupied"),
      ADD_STAT(dvrPrefetchesRejectedBackpressure,
               statistics::units::Count::get(),
               "DVR requests rejected after an available-port probe"),
      ADD_STAT(dvrPrefetchesSuperseded, statistics::units::Count::get(),
               "Queued DVR requests discarded by a newer helper launch"),
      ADD_STAT(dvrPrefetchesPossiblyUseful, statistics::units::Count::get(),
               "Demand loads to a line after a DVR prefetch completed"),
      ADD_STAT(dvrPrefetchesLate, statistics::units::Count::get(),
               "Demand loads to a line while a DVR prefetch was outstanding"),
      ADD_STAT(dvrReplaySupportedUops, statistics::units::Count::get(),
               "Supported post-trigger uops in DVR replay templates"),
      ADD_STAT(dvrReplayUnsupportedUops, statistics::units::Count::get(),
               "Unsupported post-trigger uops in DVR replay templates"),
      ADD_STAT(dvrReplayUnstableInputs, statistics::units::Count::get(),
               "Replay templates rejected due to changing external inputs"),
      ADD_STAT(dvrReplayAttempts, statistics::units::Count::get(),
               "Source responses offered to the scalar DVR uop replay path"),
      ADD_STAT(dvrReplayTargetsGenerated, statistics::units::Count::get(),
               "Dependent targets generated by real recorded-uop replay"),
      ADD_STAT(dvrReplayFallbacks, statistics::units::Count::get(),
               "Source responses falling back from uop replay to affine paths"),
      ADD_STAT(dvrQualityIssuedBytes, statistics::units::Byte::get(),
               "Bytes in DVR helper requests accepted by L1D"),
      ADD_STAT(dvrQualityCompletedBytes, statistics::units::Byte::get(),
               "Bytes in completed DVR helper responses"),
      ADD_STAT(dvrQualityDemandAddressesObserved,
               statistics::units::Count::get(),
               "Committed demand-load addresses observed; not cache hits")
{
    // Register any of the O3CPU's stats here.
    timesIdled
        .prereq(timesIdled);

    idleCycles
        .prereq(idleCycles);

    quiesceCycles
        .prereq(quiesceCycles);

    // Number of Instructions simulated
    // --------------------------------
    // Should probably be in Base CPU but need templated
    // MaxThreads so put in here instead
    committedInsts
        .init(cpu->numThreads)
        .flags(statistics::total);

    committedOps
        .init(cpu->numThreads)
        .flags(statistics::total);

    cpi
        .precision(6);
    cpi = cpu->baseStats.numCycles / committedInsts;

    totalCpi
        .precision(6);
    totalCpi = cpu->baseStats.numCycles / sum(committedInsts);

    ipc
        .precision(6);
    ipc = committedInsts / cpu->baseStats.numCycles;

    totalIpc
        .precision(6);
    totalIpc = sum(committedInsts) / cpu->baseStats.numCycles;

    intRegfileReads
        .prereq(intRegfileReads);

    intRegfileWrites
        .prereq(intRegfileWrites);

    fpRegfileReads
        .prereq(fpRegfileReads);

    fpRegfileWrites
        .prereq(fpRegfileWrites);

    vecRegfileReads
        .prereq(vecRegfileReads);

    vecRegfileWrites
        .prereq(vecRegfileWrites);

    vecPredRegfileReads
        .prereq(vecPredRegfileReads);

    vecPredRegfileWrites
        .prereq(vecPredRegfileWrites);

    ccRegfileReads
        .prereq(ccRegfileReads);

    ccRegfileWrites
        .prereq(ccRegfileWrites);

    miscRegfileReads
        .prereq(miscRegfileReads);

    miscRegfileWrites
        .prereq(miscRegfileWrites);
}

void
CPU::tick()
{
    DPRINTF(O3CPU, "\n\nO3CPU: Ticking main, O3CPU.\n");
    assert(!switchedOut());
    assert(drainState() != DrainState::Drained);

    ++baseStats.numCycles;
    updateCycleCounters(BaseCPU::CPU_STATE_ON);
    dvrHelperIssuesThisCycle = 0;
    dvrHelperComputeIssuesThisCycle = 0;
    dvrMainIssuesThisCycle = 0;
    dvrMainALUIssuesThisCycle = 0;
    dvrMainLSUIssuesThisCycle = 0;

//    activity = false;

    //Tick each of the stages
    fetch.tick();

    decode.tick();

    rename.tick();

    iew.tick();

    // The helper has an independent front-end.  It advances after the
    // demand pipeline so the main thread retains priority for shared
    // fetch/decode/issue resources, while its own ready queue is visible to
    // the residual issue arbitration below.
    const unsigned helper_frontend_work =
        dvrHelperThread.advanceFrontend(dvrFetchWidth, dvrDecodeWidth);
    if (helper_frontend_work != 0) {
        if (dvrHelperThread.fetchRemaining != 0 ||
            dvrHelperThread.decodeRemaining != 0)
            ++cpuStats.dvrHelperFetchCycles;
        if (dvrHelperThread.readyUops != 0)
            ++cpuStats.dvrHelperDecodeCycles;
        cpuStats.dvrHelperReadyUops += dvrHelperThread.readyUops;
    }

    const unsigned helper_compute = issueDVRHelperCompute();
    if (helper_compute != 0)
        ++cpuStats.dvrHelperComputeCycles;
    else if (dvrHelperThread.computePending() &&
             dvrHelperThread.readyUops != 0)
        ++cpuStats.dvrHelperComputeConflicts;

    cpuStats.dvrMainIssueSlotsUsed += dvrMainIssuesThisCycle;
    cpuStats.dvrMainALUSlotsUsed += dvrMainALUIssuesThisCycle;
    cpuStats.dvrMainLSUSlotsUsed += dvrMainLSUIssuesThisCycle;
    if (dvrMainIssuesThisCycle != 0) {
        ++cpuStats.dvrFetchActiveCycles;
        ++cpuStats.dvrDecodeActiveCycles;
    }

    // The main thread gets the first opportunity to use the LSQ data port.
    // A DVR helper probes the port only after IEW has issued this cycle's
    // demand accesses, and is discarded rather than retried on contention.
    serviceDVRPrefetchQueue();

    commit.tick();

    // A Discovery can finish in commit.tick() and enqueue a helper request
    // after the normal pre-commit helper arbitration point.  Give that
    // newly-created queue one same-cycle, residual service opportunity.
    serviceDVRPrefetchQueue();

    // Now advance the time buffers
    timeBuffer.advance();

    fetchQueue.advance();
    decodeQueue.advance();
    renameQueue.advance();
    iewQueue.advance();

    activityRec.advance();

    if (removeInstsThisCycle) {
        cleanUpRemovedInsts();
    }

    if (!tickEvent.scheduled()) {
        if (_status == SwitchedOut) {
            DPRINTF(O3CPU, "Switched out!\n");
            // increment stat
            lastRunningCycle = curCycle();
        } else if (!activityRec.active() || _status == Idle) {
            DPRINTF(O3CPU, "Idle!\n");
            lastRunningCycle = curCycle();
            cpuStats.timesIdled++;
        } else {
            schedule(tickEvent, clockEdge(Cycles(1)));
            DPRINTF(O3CPU, "Scheduling next tick!\n");
        }
    }

    if (!FullSystem)
        updateThreadPriority();

    tryDrain();
}

void
CPU::init()
{
    BaseCPU::init();

    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        // Set noSquashFromTC so that the CPU doesn't squash when initially
        // setting up registers.
        thread[tid]->noSquashFromTC = true;
    }

    // Clear noSquashFromTC.
    for (int tid = 0; tid < numThreads; ++tid)
        thread[tid]->noSquashFromTC = false;

    commit.setThreads(thread);
}

void
CPU::startup()
{
    BaseCPU::startup();

    fetch.startupStage();
    decode.startupStage();
    iew.startupStage();
    rename.startupStage();
    commit.startupStage();
}

void
CPU::activateThread(ThreadID tid)
{
    std::list<ThreadID>::iterator isActive =
        std::find(activeThreads.begin(), activeThreads.end(), tid);

    DPRINTF(O3CPU, "[tid:%i] Calling activate thread.\n", tid);
    assert(!switchedOut());

    if (isActive == activeThreads.end()) {
        DPRINTF(O3CPU, "[tid:%i] Adding to active threads list\n", tid);

        activeThreads.push_back(tid);
    }
}

void
CPU::deactivateThread(ThreadID tid)
{
    // hardware transactional memory
    // shouldn't deactivate thread in the middle of a transaction
    assert(!commit.executingHtmTransaction(tid));

    //Remove From Active List, if Active
    std::list<ThreadID>::iterator thread_it =
        std::find(activeThreads.begin(), activeThreads.end(), tid);

    DPRINTF(O3CPU, "[tid:%i] Calling deactivate thread.\n", tid);
    assert(!switchedOut());

    if (thread_it != activeThreads.end()) {
        DPRINTF(O3CPU,"[tid:%i] Removing from active threads list\n",
                tid);
        activeThreads.erase(thread_it);
    }

    fetch.deactivateThread(tid);
    commit.deactivateThread(tid);
}

Counter
CPU::totalInsts() const
{
    Counter total(0);

    ThreadID size = thread.size();
    for (ThreadID i = 0; i < size; i++)
        total += thread[i]->numInst;

    return total;
}

Counter
CPU::totalOps() const
{
    Counter total(0);

    ThreadID size = thread.size();
    for (ThreadID i = 0; i < size; i++)
        total += thread[i]->numOp;

    return total;
}

void
CPU::activateContext(ThreadID tid)
{
    assert(!switchedOut());

    // Needs to set each stage to running as well.
    activateThread(tid);

    // We don't want to wake the CPU if it is drained. In that case,
    // we just want to flag the thread as active and schedule the tick
    // event from drainResume() instead.
    if (drainState() == DrainState::Drained)
        return;

    // If we are time 0 or if the last activation time is in the past,
    // schedule the next tick and wake up the fetch unit
    if (lastActivatedCycle == 0 || lastActivatedCycle < curTick()) {
        scheduleTickEvent(Cycles(0));

        // Be sure to signal that there's some activity so the CPU doesn't
        // deschedule itself.
        activityRec.activity();
        fetch.wakeFromQuiesce();

        Cycles cycles(curCycle() - lastRunningCycle);
        // @todo: This is an oddity that is only here to match the stats
        if (cycles != 0)
            --cycles;
        cpuStats.quiesceCycles += cycles;

        lastActivatedCycle = curTick();

        _status = Running;

        BaseCPU::activateContext(tid);
    }
}

void
CPU::suspendContext(ThreadID tid)
{
    DPRINTF(O3CPU,"[tid:%i] Suspending Thread Context.\n", tid);
    assert(!switchedOut());

    deactivateThread(tid);

    // If this was the last thread then unschedule the tick event.
    if (activeThreads.size() == 0) {
        unscheduleTickEvent();
        lastRunningCycle = curCycle();
        _status = Idle;
    }

    DPRINTF(Quiesce, "Suspending Context\n");

    BaseCPU::suspendContext(tid);
}

void
CPU::haltContext(ThreadID tid)
{
    //For now, this is the same as deallocate
    DPRINTF(O3CPU,"[tid:%i] Halt Context called. Deallocating\n", tid);
    assert(!switchedOut());

    deactivateThread(tid);
    removeThread(tid);

    // If this was the last thread then unschedule the tick event.
    if (activeThreads.size() == 0) {
        if (tickEvent.scheduled())
        {
            unscheduleTickEvent();
        }
        lastRunningCycle = curCycle();
        _status = Idle;
    }
    updateCycleCounters(BaseCPU::CPU_STATE_SLEEP);
}

void
CPU::insertThread(ThreadID tid)
{
    DPRINTF(O3CPU,"[tid:%i] Initializing thread into CPU");
    // Will change now that the PC and thread state is internal to the CPU
    // and not in the ThreadContext.
    gem5::ThreadContext *src_tc;
    if (FullSystem)
        src_tc = system->threads[tid];
    else
        src_tc = tcBase(tid);

    //Bind Int Regs to Rename Map
    const auto &regClasses = isa[tid]->regClasses();

    for (auto type = (RegClassType)0; type <= CCRegClass;
            type = (RegClassType)(type + 1)) {
        for (RegIndex idx = 0; idx < regClasses.at(type).numRegs(); idx++) {
            PhysRegIdPtr phys_reg = freeList.getReg(type);
            renameMap[tid].setEntry(RegId(type, idx), phys_reg);
            scoreboard.setReg(phys_reg);
        }
    }

    //Copy Thread Data Into RegFile
    //copyFromTC(tid);

    //Set PC/NPC/NNPC
    pcState(src_tc->pcState(), tid);

    src_tc->setStatus(gem5::ThreadContext::Active);

    activateContext(tid);

    //Reset ROB/IQ/LSQ Entries
    commit.rob->resetEntries();
}

void
CPU::removeThread(ThreadID tid)
{
    DPRINTF(O3CPU,"[tid:%i] Removing thread context from CPU.\n", tid);

    // Copy Thread Data From RegFile
    // If thread is suspended, it might be re-allocated
    // copyToTC(tid);


    // @todo: 2-27-2008: Fix how we free up rename mappings
    // here to alleviate the case for double-freeing registers
    // in SMT workloads.

    // clear all thread-specific states in each stage of the pipeline
    // since this thread is going to be completely removed from the CPU
    commit.clearStates(tid);
    fetch.clearStates(tid);
    decode.clearStates(tid);
    rename.clearStates(tid);
    iew.clearStates(tid);

    // Flush out any old data from the time buffers.
    for (int i = 0; i < timeBuffer.getSize(); ++i) {
        timeBuffer.advance();
        fetchQueue.advance();
        decodeQueue.advance();
        renameQueue.advance();
        iewQueue.advance();
    }

    // at this step, all instructions in the pipeline should be already
    // either committed successfully or squashed. All thread-specific
    // queues in the pipeline must be empty.
    assert(iew.instQueue.getCount(tid) == 0);
    assert(iew.ldstQueue.getCount(tid) == 0);
    assert(commit.rob->isEmpty(tid));

    // Reset ROB/IQ/LSQ Entries

    // Commented out for now.  This should be possible to do by
    // telling all the pipeline stages to drain first, and then
    // checking until the drain completes.  Once the pipeline is
    // drained, call resetEntries(). - 10-09-06 ktlim
/*
    if (activeThreads.size() >= 1) {
        commit.rob->resetEntries();
        iew.resetEntries();
    }
*/
}

Fault
CPU::getInterrupts()
{
    // Check if there are any outstanding interrupts
    return interrupts[0]->getInterrupt();
}

void
CPU::processInterrupts(const Fault &interrupt)
{
    // Check for interrupts here.  For now can copy the code that
    // exists within isa_fullsys_traits.hh.  Also assume that thread 0
    // is the one that handles the interrupts.
    // @todo: Possibly consolidate the interrupt checking code.
    // @todo: Allow other threads to handle interrupts.

    assert(interrupt != NoFault);
    interrupts[0]->updateIntrInfo();

    DPRINTF(O3CPU, "Interrupt %s being handled\n", interrupt->name());
    trap(interrupt, 0, nullptr);
}

void
CPU::trap(const Fault &fault, ThreadID tid, const StaticInstPtr &inst)
{
    // Pass the thread's TC into the invoke method.
    fault->invoke(threadContexts[tid], inst);
}

void
CPU::serializeThread(CheckpointOut &cp, ThreadID tid) const
{
    thread[tid]->serialize(cp);
}

void
CPU::unserializeThread(CheckpointIn &cp, ThreadID tid)
{
    thread[tid]->unserialize(cp);
}

DrainState
CPU::drain()
{
    // Deschedule any power gating event (if any)
    deschedulePowerGatingEvent();

    // If the CPU isn't doing anything, then return immediately.
    if (switchedOut())
        return DrainState::Drained;

    DPRINTF(Drain, "Draining...\n");

    // We only need to signal a drain to the commit stage as this
    // initiates squashing controls the draining. Once the commit
    // stage commits an instruction where it is safe to stop, it'll
    // squash the rest of the instructions in the pipeline and force
    // the fetch stage to stall. The pipeline will be drained once all
    // in-flight instructions have retired.
    commit.drain();

    // Wake the CPU and record activity so everything can drain out if
    // the CPU was not able to immediately drain.
    if (!isCpuDrained())  {
        // If a thread is suspended, wake it up so it can be drained
        for (auto t : threadContexts) {
            if (t->status() == gem5::ThreadContext::Suspended){
                DPRINTF(Drain, "Currently suspended so activate %i \n",
                        t->threadId());
                t->activate();
                // As the thread is now active, change the power state as well
                activateContext(t->threadId());
            }
        }

        wakeCPU();
        activityRec.activity();

        DPRINTF(Drain, "CPU not drained\n");

        return DrainState::Draining;
    } else {
        DPRINTF(Drain, "CPU is already drained\n");
        if (tickEvent.scheduled())
            deschedule(tickEvent);

        // Flush out any old data from the time buffers.  In
        // particular, there might be some data in flight from the
        // fetch stage that isn't visible in any of the CPU buffers we
        // test in isCpuDrained().
        for (int i = 0; i < timeBuffer.getSize(); ++i) {
            timeBuffer.advance();
            fetchQueue.advance();
            decodeQueue.advance();
            renameQueue.advance();
            iewQueue.advance();
        }

        drainSanityCheck();
        return DrainState::Drained;
    }
}

bool
CPU::tryDrain()
{
    if (drainState() != DrainState::Draining || !isCpuDrained())
        return false;

    if (tickEvent.scheduled())
        deschedule(tickEvent);

    DPRINTF(Drain, "CPU done draining, processing drain event\n");
    signalDrainDone();

    return true;
}

void
CPU::drainSanityCheck() const
{
    assert(isCpuDrained());
    fetch.drainSanityCheck();
    decode.drainSanityCheck();
    rename.drainSanityCheck();
    iew.drainSanityCheck();
    commit.drainSanityCheck();
}

bool
CPU::isCpuDrained() const
{
    bool drained(true);

    if (!instList.empty() || !removeList.empty()) {
        DPRINTF(Drain, "Main CPU structures not drained.\n");
        drained = false;
    }

    if (!fetch.isDrained()) {
        DPRINTF(Drain, "Fetch not drained.\n");
        drained = false;
    }

    if (!decode.isDrained()) {
        DPRINTF(Drain, "Decode not drained.\n");
        drained = false;
    }

    if (!rename.isDrained()) {
        DPRINTF(Drain, "Rename not drained.\n");
        drained = false;
    }

    if (!iew.isDrained()) {
        DPRINTF(Drain, "IEW not drained.\n");
        drained = false;
    }

    if (!commit.isDrained()) {
        DPRINTF(Drain, "Commit not drained.\n");
        drained = false;
    }

    return drained;
}

void CPU::commitDrained(ThreadID tid) { fetch.drainStall(tid); }

void
CPU::drainResume()
{
    if (switchedOut())
        return;

    DPRINTF(Drain, "Resuming...\n");
    verifyMemoryMode();

    fetch.drainResume();
    commit.drainResume();

    _status = Idle;
    for (ThreadID i = 0; i < thread.size(); i++) {
        if (thread[i]->status() == gem5::ThreadContext::Active) {
            DPRINTF(Drain, "Activating thread: %i\n", i);
            activateThread(i);
            _status = Running;
        }
    }

    assert(!tickEvent.scheduled());
    if (_status == Running)
        schedule(tickEvent, nextCycle());

    // Reschedule any power gating event (if any)
    schedulePowerGatingEvent();
}

void
CPU::switchOut()
{
    DPRINTF(O3CPU, "Switching out\n");
    BaseCPU::switchOut();

    activityRec.reset();

    _status = SwitchedOut;

    if (checker)
        checker->switchOut();
}

void
CPU::takeOverFrom(BaseCPU *oldCPU)
{
    BaseCPU::takeOverFrom(oldCPU);

    fetch.takeOverFrom();
    decode.takeOverFrom();
    rename.takeOverFrom();
    iew.takeOverFrom();
    commit.takeOverFrom();

    assert(!tickEvent.scheduled());

    auto *oldO3CPU = dynamic_cast<CPU *>(oldCPU);
    if (oldO3CPU)
        globalSeqNum = oldO3CPU->globalSeqNum;

    lastRunningCycle = curCycle();
    _status = Idle;
}

void
CPU::verifyMemoryMode() const
{
    if (!system->isTimingMode()) {
        fatal("The O3 CPU requires the memory system to be in "
              "'timing' mode.\n");
    }
}

RegVal
CPU::readMiscRegNoEffect(int misc_reg, ThreadID tid) const
{
    return isa[tid]->readMiscRegNoEffect(misc_reg);
}

RegVal
CPU::readMiscReg(int misc_reg, ThreadID tid)
{
    cpuStats.miscRegfileReads++;
    return isa[tid]->readMiscReg(misc_reg);
}

void
CPU::setMiscRegNoEffect(int misc_reg, RegVal val, ThreadID tid)
{
    isa[tid]->setMiscRegNoEffect(misc_reg, val);
}

void
CPU::setMiscReg(int misc_reg, RegVal val, ThreadID tid)
{
    cpuStats.miscRegfileWrites++;
    isa[tid]->setMiscReg(misc_reg, val);
}

RegVal
CPU::getReg(PhysRegIdPtr phys_reg)
{
    switch (phys_reg->classValue()) {
      case IntRegClass:
        cpuStats.intRegfileReads++;
        break;
      case FloatRegClass:
        cpuStats.fpRegfileReads++;
        break;
      case CCRegClass:
        cpuStats.ccRegfileReads++;
        break;
      case VecRegClass:
      case VecElemClass:
        cpuStats.vecRegfileReads++;
        break;
      case VecPredRegClass:
        cpuStats.vecPredRegfileReads++;
        break;
      default:
        break;
    }
    return regFile.getReg(phys_reg);
}

void
CPU::getReg(PhysRegIdPtr phys_reg, void *val)
{
    switch (phys_reg->classValue()) {
      case IntRegClass:
        cpuStats.intRegfileReads++;
        break;
      case FloatRegClass:
        cpuStats.fpRegfileReads++;
        break;
      case CCRegClass:
        cpuStats.ccRegfileReads++;
        break;
      case VecRegClass:
      case VecElemClass:
        cpuStats.vecRegfileReads++;
        break;
      case VecPredRegClass:
        cpuStats.vecPredRegfileReads++;
        break;
      default:
        break;
    }
    regFile.getReg(phys_reg, val);
}

void *
CPU::getWritableReg(PhysRegIdPtr phys_reg)
{
    switch (phys_reg->classValue()) {
      case VecRegClass:
        cpuStats.vecRegfileReads++;
        break;
      case VecPredRegClass:
        cpuStats.vecPredRegfileReads++;
        break;
      default:
        break;
    }
    return regFile.getWritableReg(phys_reg);
}

void
CPU::setReg(PhysRegIdPtr phys_reg, RegVal val)
{
    switch (phys_reg->classValue()) {
      case IntRegClass:
        cpuStats.intRegfileWrites++;
        break;
      case FloatRegClass:
        cpuStats.fpRegfileWrites++;
        break;
      case CCRegClass:
        cpuStats.ccRegfileWrites++;
        break;
      case VecRegClass:
      case VecElemClass:
        cpuStats.vecRegfileWrites++;
        break;
      case VecPredRegClass:
        cpuStats.vecPredRegfileWrites++;
        break;
      default:
        break;
    }
    regFile.setReg(phys_reg, val);
}

void
CPU::setReg(PhysRegIdPtr phys_reg, const void *val)
{
    switch (phys_reg->classValue()) {
      case IntRegClass:
        cpuStats.intRegfileWrites++;
        break;
      case FloatRegClass:
        cpuStats.fpRegfileWrites++;
        break;
      case CCRegClass:
        cpuStats.ccRegfileWrites++;
        break;
      case VecRegClass:
      case VecElemClass:
        cpuStats.vecRegfileWrites++;
        break;
      case VecPredRegClass:
        cpuStats.vecPredRegfileWrites++;
        break;
      default:
        break;
    }
    regFile.setReg(phys_reg, val);
}

RegVal
CPU::getArchReg(const RegId &reg, ThreadID tid)
{
    PhysRegIdPtr phys_reg = commitRenameMap[tid].lookup(reg);
    return regFile.getReg(phys_reg);
}

void
CPU::getArchReg(const RegId &reg, void *val, ThreadID tid)
{
    PhysRegIdPtr phys_reg = commitRenameMap[tid].lookup(reg);
    regFile.getReg(phys_reg, val);
}

void *
CPU::getWritableArchReg(const RegId &reg, ThreadID tid)
{
    PhysRegIdPtr phys_reg = commitRenameMap[tid].lookup(reg);
    return regFile.getWritableReg(phys_reg);
}

void
CPU::setArchReg(const RegId &reg, RegVal val, ThreadID tid)
{
    PhysRegIdPtr phys_reg = commitRenameMap[tid].lookup(reg);
    regFile.setReg(phys_reg, val);
}

void
CPU::setArchReg(const RegId &reg, const void *val, ThreadID tid)
{
    PhysRegIdPtr phys_reg = commitRenameMap[tid].lookup(reg);
    regFile.setReg(phys_reg, val);
}

const PCStateBase &
CPU::pcState(ThreadID tid)
{
    return commit.pcState(tid);
}

void
CPU::pcState(const PCStateBase &val, ThreadID tid)
{
    commit.pcState(val, tid);
}

void
CPU::squashFromTC(ThreadID tid)
{
    thread[tid]->noSquashFromTC = true;
    commit.generateTCEvent(tid);
}

CPU::ListIt
CPU::addInst(const DynInstPtr &inst)
{
    instList.push_back(inst);

    return --(instList.end());
}

void
CPU::instDone(ThreadID tid, const DynInstPtr &inst)
{
    // Keep an instruction count.
    if (!inst->isMicroop() || inst->isLastMicroop()) {
        thread[tid]->numInst++;
        thread[tid]->threadStats.numInsts++;
        cpuStats.committedInsts[tid]++;

        if (enableDVR && !inPRE && !inst->isPRE()) {
            const bool was_discovering = dvrDiscovery.isDiscovering();
            if (was_discovering &&
                inst->seqNum > dvrDiscovery.triggerSeq()) {
                const bool dispatch_tainted =
                    dvrDispatchTainted.erase(inst->seqNum) != 0;
                const bool dispatch_dependent =
                    dvrDispatchDependentLoads.erase(inst->seqNum) != 0;
                if (dispatch_tainted)
                    dvrInstructionRecorder.record(inst);
                if (dispatch_dependent) {
                    dvrLoopBoundDetector.updateFinalLoad(
                        dvrTaintTracker.flr());
                    if (inst->effAddrValid()) {
                        trainDVRAddressRelation(
                            dvrCurrentTriggerPC, inst->pcState().instAddr(),
                            dvrInitiatingLoadValue, inst->effAddr);
                    }
                }
                const auto loop_observation =
                    dvrLoopBoundDetector.observe(inst);
                if (loop_observation.backwardBranch)
                    ++cpuStats.dvrBackwardBranches;
                if (loop_observation.boundFound)
                    ++cpuStats.dvrLoopBoundsFound;
            }
            const auto result = dvrDiscovery.observeCommit(
                inst->pcState().instAddr(), inst->seqNum);
            const auto nested_result = dvrNestedController.observeCommit(
                inst->pcState().instAddr(), inst->seqNum);
            const auto ndm_result = dvrNestedDiscoveryMode.observeCommit();
            if (dvrNestedDiscoveryMode.active() && inst->isCondCtrl() &&
                inst->isDirectCtrl()) {
                const bool captured =
                    dvrNestedDiscoveryMode.observeInnerBranch(
                        inst->pcState().instAddr(),
                        inst->branchTarget()->instAddr(),
                        // The generic PCState interface does not expose the
                        // ISA-specific npc accessor.  RV64C is decoded into
                        // the recorder with a fixed-width fall-through, so
                        // use the same conservative architectural next-PC
                        // model for the NDM control record.
                        inst->pcState().instAddr() + 4,
                        inst->pcState().branching());
                if (captured) {
                    ++cpuStats.dvrNDMBranchInversions;
                    ++cpuStats.dvrNDMIRCaptures;
                }
            }
            // This is the NDM shadow's outer-path scan.  It intentionally
            // consumes committed effective addresses, not the affine address
            // reconstructed from a stride candidate.
            if (dvrNestedDiscoveryMode.active() && inst->isLoad() &&
                inst->effAddrValid()) {
                const auto outer = dvrNestedDiscoveryMode.observeOuterLoad(
                    inst->pcState().instAddr(), inst->effAddr);
                if (outer.event ==
                    DVRNestedDiscoveryMode::Event::OuterAccepted) {
                    ++cpuStats.dvrNDMOuterFound;
                }
            }
            if (was_discovering &&
                inst->seqNum == dvrDiscovery.triggerSeq()) {
                for (int dest = 0; dest < inst->numDestRegs(); ++dest) {
                    if (inst->destRegIdx(dest).classValue() == IntRegClass) {
                        dvrInitiatingLoadValue = getReg(
                            inst->renamedDestIdx(dest));
                        break;
                    }
                }
            }
            if (ndm_result.event ==
                DVRNestedDiscoveryMode::Event::TimedOut) {
                ++cpuStats.dvrNDMTimeouts;
                ++cpuStats.dvrNDMFallbacks;
            }
            if (nested_result.event ==
                DVRNestedController::Event::Completed) {
                ++cpuStats.dvrNestedCompletions;
            } else if (nested_result.event ==
                       DVRNestedController::Event::TimedOut) {
                ++cpuStats.dvrNestedTimeouts;
            }
            // Finalize the old child before this same recurring trigger can
            // commit as the first instruction of a new child generation.
            if (nested_result.event ==
                    DVRNestedController::Event::Completed &&
                dvrNestedContext.active &&
                nested_result.id == dvrNestedContext.id) {
                DVRLoopBoundDetector::RegisterSnapshot finish_regs = {};
                captureDVRRegisterSnapshot(tid, inst, finish_regs);
                completeDVRNestedContext(inst, finish_regs);
            }

            // The candidate was detected speculatively in IEW.  It becomes a
            // real child context only when that exact dynamic load commits.
            if (dvrPendingNestedCandidate.valid &&
                dvrPendingNestedCandidate.sequence == inst->seqNum &&
                dvrPendingNestedCandidate.pc == inst->pcState().instAddr()) {
                dvrCommittedNestedCandidate = dvrPendingNestedCandidate;
                if (dvrNestedController.depth() == 1) {
                    const auto parent = dvrNestedController.currentId();
                    if (parent) {
                        const auto nested = dvrNestedController.startNested(
                            *parent, dvrPendingNestedCandidate.pc,
                            dvrPendingNestedCandidate.sequence);
                        if (nested.event ==
                            DVRNestedController::Event::Started) {
                            ++cpuStats.dvrNestedStarts;
                            dvrNestedContext.reset();
                            dvrNestedContext.active = true;
                            dvrNestedContext.id = nested.id;
                            dvrNestedContext.tid = tid;
                            dvrNestedContext.triggerSequence = inst->seqNum;
                            dvrNestedContext.triggerPC =
                                dvrPendingNestedCandidate.pc;
                            dvrNestedContext.triggerAddress =
                                dvrPendingNestedCandidate.address;
                            dvrNestedContext.stride =
                                dvrPendingNestedCandidate.stride;
                            // This table contains observed, committed dynamic
                            // invocations only.  Do not manufacture adjacent
                            // outer-loop bases from the learned stride.
                            dvrNestedContext.taint.begin(inst);
                            dvrNestedContext.loopBound.begin(
                                dvrNestedContext.triggerPC);
                            dvrNestedContext.recorder.begin(inst);
                            for (int dest = 0; dest < inst->numDestRegs(); ++dest) {
                                if (inst->destRegIdx(dest).classValue() ==
                                    IntRegClass) {
                                    dvrNestedContext.initiatingValue = getReg(
                                        inst->renamedDestIdx(dest));
                                    break;
                                }
                            }
                            captureDVRRegisterSnapshot(tid, inst,
                                dvrNestedContext.startRegs);
                            ++cpuStats.dvrNestedContextsBuilt;
                        } else if (nested.event ==
                                   DVRNestedController::Event::RejectedDepth) {
                            ++cpuStats.dvrNestedDepthRejects;
                        }
                    }
                } else if (dvrNestedController.depth() >=
                           DVRNestedController::MaxDepth) {
                    ++cpuStats.dvrNestedDepthRejects;
                }
                dvrPendingNestedCandidate = {};
            } else if (dvrPendingNestedCandidate.valid &&
                       inst->seqNum > dvrPendingNestedCandidate.sequence) {
                // Candidate was squashed and can never commit now.
                dvrPendingNestedCandidate = {};
            }
            if (dvrNestedContext.active &&
                       inst->seqNum > dvrNestedContext.triggerSequence) {
                const auto child_observation =
                    dvrNestedContext.taint.observe(inst);
                if (child_observation.taintedInstruction)
                    dvrNestedContext.recorder.record(inst);
                if (child_observation.dependentLoad) {
                    dvrNestedContext.loopBound.updateFinalLoad(
                        dvrNestedContext.taint.flr());
                    if (inst->effAddrValid()) {
                        trainDVRAddressRelation(
                            dvrNestedContext.triggerPC,
                            inst->pcState().instAddr(),
                            dvrNestedContext.initiatingValue,
                            inst->effAddr);
                    }
                }
                dvrNestedContext.loopBound.observe(inst);
            }
            if (nested_result.event ==
                    DVRNestedController::Event::TimedOut &&
                dvrNestedContext.active &&
                nested_result.id == dvrNestedContext.id) {
                dvrNestedContext.reset();
            }
            switch (result.event) {
              case DVRDiscoveryController::Event::Started:
                ++cpuStats.dvrDiscoveryStarts;
                if (dvrNestedController.active()) {
                    ++cpuStats.dvrNestedParentResets;
                    dvrNestedController.reset();
                    dvrNestedContext.reset();
                }
                if (dvrMode == "nested" && dvrNestedController.startRoot(
                        result.triggerPC, inst->seqNum).event ==
                    DVRNestedController::Event::Started) {
                    ++cpuStats.dvrNestedRootStarts;
                }
                dvrTaintTracker.begin(inst);
                dvrLoopBoundDetector.begin(result.triggerPC);
                dvrInstructionRecorder.begin(inst);
                // NDM is an outer-loop generation and may span several
                // ordinary discovery generations.  Starting the next
                // dispatch-time Discovery must not erase its captured
                // IR/ILR/LCR or outer-invocation plan.
                if (!dvrNestedDiscoveryMode.active())
                    dvrNestedDiscoveryMode.reset();
                dvrCommittedNestedCandidate = {};
                dvrCurrentTriggerPC = result.triggerPC;
                dvrCurrentTriggerAddress = inst->effAddrValid() ?
                    inst->effAddr : 0;
                dvrInitiatingLoadValue = 0;
                for (int dest = 0; dest < inst->numDestRegs(); ++dest) {
                    if (inst->destRegIdx(dest).classValue() == IntRegClass) {
                        dvrInitiatingLoadValue = getReg(
                            inst->renamedDestIdx(dest));
                        break;
                    }
                }
                captureDVRRegisterSnapshot(
                    tid, inst, dvrDiscoveryStartRegs);
                DPRINTF(O3CPU,
                        "DVR discovery start pc=%#x stride=%lld sn=%llu\n",
                        result.triggerPC,
                        static_cast<long long>(result.stride), inst->seqNum);
                break;
              case DVRDiscoveryController::Event::Completed: {
                ++cpuStats.dvrDiscoveryCompletions;
                cpuStats.dvrDiscoveredInstructions += result.instructions;
                if (dvrTaintTracker.flr() != 0)
                    ++cpuStats.dvrDiscoveriesWithFLR;
                if (dvrLoopBoundDetector.hasBound())
                    ++cpuStats.dvrDiscoveriesWithBounds;
                DVRLoopBoundDetector::RegisterSnapshot finish_regs = {};
                captureDVRRegisterSnapshot(tid, inst, finish_regs);
                const auto inference = dvrLoopBoundDetector.infer(
                    dvrDiscoveryStartRegs, finish_regs, dvrMaxLanes);
                // Every completed root discovery is a dynamic inner-loop
                // invocation with its own start address and bound.  Once NDM
                // has found the outer stride, pair that exact data with the
                // next committed outer base from its FIFO.
                if (dvrNestedDiscoveryMode.state() ==
                        DVRNestedDiscoveryMode::State::OuterFound &&
                    inference.matched && inference.lanes != 0 &&
                    dvrCurrentTriggerAddress != 0 &&
                    dvrTaintTracker.flr() != 0 &&
                    dvrNestedDiscoveryMode.recordOuterInvocation(
                        dvrCurrentTriggerAddress, inference.lanes,
                        result.triggerPC, dvrTaintTracker.flr(),
                        inference.increment)) {
                    ++cpuStats.dvrNDMOuterInvocations;
                }
                const auto ndm_started = dvrNestedDiscoveryMode.start(
                    result.triggerPC, inference.increment, inference.lanes);
                if (ndm_started.event ==
                    DVRNestedDiscoveryMode::Event::Started) {
                    ++cpuStats.dvrNDMAttempts;
                    int8_t induction_reg = -1;
                    int8_t bound_reg = -1;
                    RegVal induction_value = 0;
                    RegVal bound_value = 0;
                    if (dvrLoopBoundDetector.boundSource0Reg() >= 0 &&
                        dvrLoopBoundDetector.boundSource1Reg() >= 0) {
                        const unsigned source0 =
                            dvrLoopBoundDetector.boundSource0Reg();
                        const unsigned source1 =
                            dvrLoopBoundDetector.boundSource1Reg();
                        if (dvrDiscoveryStartRegs[source0] ==
                            finish_regs[source0]) {
                            bound_reg = static_cast<int8_t>(source0);
                            induction_reg = static_cast<int8_t>(source1);
                        } else {
                            bound_reg = static_cast<int8_t>(source1);
                            induction_reg = static_cast<int8_t>(source0);
                        }
                        bound_value = finish_regs[bound_reg];
                        induction_value = finish_regs[induction_reg];
                    }
                    dvrNestedDiscoveryMode.captureLoopRegisters(
                        induction_reg, bound_reg, induction_value,
                        bound_value, inference.remaining);
                    if (induction_reg >= 0)
                        ++cpuStats.dvrNDMILRCaptures;
                    if (bound_reg >= 0)
                        ++cpuStats.dvrNDMLCRCaptures;
                }
                ++cpuStats.dvrLaneCountSamples;
                cpuStats.dvrTotalActiveLanes += inference.lanes;
                if (inference.matched)
                    ++cpuStats.dvrLoopBoundMatches;
                else
                    ++cpuStats.dvrLoopBoundFallbacks;
                cpuStats.dvrRecordedUops += dvrInstructionRecorder.size();
                bool helper_allowed = dvrMode != "discovery" &&
                                      inference.matched &&
                                      inference.lanes != 0 &&
                                      dvrInstructionRecorder.size() > 1 &&
                                      dvrTaintTracker.flr() != 0 &&
                                      !dvrInstructionRecorder.overflow();
                if (dvrInstructionRecorder.overflow()) {
                    ++cpuStats.dvrRecorderOverflows;
                    helper_allowed = false;
                }
                bool vir_control_fallback = false;
                if (helper_allowed) {
                    ++cpuStats.dvrVectorProgramsBuilt;
                    cpuStats.dvrVRATAllocations +=
                        dvrVectorRenameTable.build(
                            dvrInstructionRecorder, inference.lanes);
                    const auto vir_result =
                        dvrVectorInstructionRegister.execute(
                            dvrInstructionRecorder, inference.lanes,
                            dvrHelperMaxUops, dvrDiscoveryStartRegs);
                    cpuStats.dvrVIRChunkIssues +=
                        vir_result.chunkIssues;
                    cpuStats.dvrVIRChunkExecutions +=
                        vir_result.chunkExecutions;
                    cpuStats.dvrDivergentBranches +=
                        vir_result.divergentBranches;
                    cpuStats.dvrReconvergences +=
                        vir_result.reconvergences;
                    cpuStats.dvrVIRNormalTerminatedLanes +=
                        vir_result.normalTerminatedLanes;
                    cpuStats.dvrVIREarlyExitLanes +=
                        vir_result.earlyExitLanes;
                    cpuStats.dvrVIRExternalPathLanes +=
                        vir_result.externalPathLanes;
                    cpuStats.dvrVIRUnsupportedSemanticLanes +=
                        vir_result.unsupportedSemanticLanes;
                    if (vir_result.unsupportedControlFlow) {
                        ++cpuStats.dvrVIRUnsupportedControlFlow;
                        helper_allowed = false;
                        vir_control_fallback = true;
                    }
                    if (vir_result.timedOut) {
                        ++cpuStats.dvrHelperTimeouts;
                        helper_allowed = false;
                        vir_control_fallback = false;
                    }
                    if (vir_result.stackOverflow) {
                        ++cpuStats.dvrReconvergenceStackOverflows;
                        helper_allowed = false;
                        vir_control_fallback = false;
                    }
                }
                const bool launch_source_fallback =
                    vir_control_fallback && !dvrInstructionRecorder.overflow();
                bool ndm_launched = false;
                if (helper_allowed &&
                    dvrNestedDiscoveryMode.readyToVectorize()) {
                    dvrNestedContext.reset();
                    dvrNestedContext.active = true;
                    dvrNestedContext.tid = tid;
                    dvrNestedContext.triggerPC = result.triggerPC;
                    dvrNestedContext.triggerAddress =
                        dvrCurrentTriggerAddress;
                    dvrNestedContext.stride = inference.increment;
                    dvrNestedContext.taint = dvrTaintTracker;
                    dvrNestedContext.recorder = dvrInstructionRecorder;
                    dvrNestedContext.startRegs = dvrDiscoveryStartRegs;
                    launchDVRNestedPrefetches(finish_regs);
                    dvrNestedDiscoveryMode.finishVectorization();
                    dvrNestedContext.reset();
                    ndm_launched = true;
                }
                if (!ndm_launched &&
                    (helper_allowed || launch_source_fallback) &&
                    inst->effAddrValid()) {
                    if (launch_source_fallback)
                        ++cpuStats.dvrControlFallbackSourceLaunches;
                    launchDVRStridePrefetches(
                        tid, inst->effAddr, result.triggerPC,
                        result.stride, inference.lanes, finish_regs);
                } else if (dvrTaintTracker.flr() != 0) {
                    ++cpuStats.dvrHelpersSuppressed;
                }
                DPRINTF(O3CPU,
                        "DVR discovery complete pc=%#x stride=%lld "
                        "insts=%u flr=%#x taint=%#x loop=%#x->%#x "
                        "bound=%#x increment=%lld remaining=%llu lanes=%u\n",
                        result.triggerPC,
                        static_cast<long long>(result.stride),
                        result.instructions, dvrTaintTracker.flr(),
                        dvrTaintTracker.bits(),
                        dvrLoopBoundDetector.branchPC(),
                        dvrLoopBoundDetector.targetPC(), inference.bound,
                        static_cast<long long>(inference.increment),
                        static_cast<unsigned long long>(inference.remaining),
                        inference.lanes);
                dvrTaintTracker.reset();
                dvrLoopBoundDetector.reset();
                dvrInstructionRecorder.reset();
                dvrVectorRenameTable.reset();
                dvrVectorInstructionRegister.reset();
                dvrCommittedNestedCandidate = {};
                if (dvrNestedController.depth() > 1) {
                    ++cpuStats.dvrNestedParentResets;
                    dvrNestedController.reset();
                    dvrNestedContext.reset();
                } else if (const auto root = dvrNestedController.rootId()) {
                    dvrNestedController.complete(*root,
                        inst->pcState().instAddr());
                }
                break;
              }
              case DVRDiscoveryController::Event::TimedOut:
                ++cpuStats.dvrDiscoveryTimeouts;
                DPRINTF(O3CPU,
                        "DVR discovery timeout pc=%#x stride=%lld insts=%u\n",
                        result.triggerPC,
                        static_cast<long long>(result.stride),
                        result.instructions);
                dvrTaintTracker.reset();
                dvrLoopBoundDetector.reset();
                dvrInstructionRecorder.reset();
                if (!dvrNestedDiscoveryMode.active())
                    dvrNestedDiscoveryMode.reset();
                dvrCommittedNestedCandidate = {};
                if (dvrNestedController.active()) {
                    ++cpuStats.dvrNestedParentResets;
                    dvrNestedController.reset();
                    dvrNestedContext.reset();
                }
                break;
              case DVRDiscoveryController::Event::Abandoned:
                ++cpuStats.dvrDiscoveryAbandons;
                dvrTaintTracker.reset();
                dvrLoopBoundDetector.reset();
                dvrInstructionRecorder.reset();
                if (!dvrNestedDiscoveryMode.active())
                    dvrNestedDiscoveryMode.reset();
                dvrCommittedNestedCandidate = {};
                if (dvrNestedController.active()) {
                    ++cpuStats.dvrNestedParentResets;
                    dvrNestedController.reset();
                    dvrNestedContext.reset();
                }
                break;
              case DVRDiscoveryController::Event::None:
                break;
            }
        }

        // Check for instruction-count-based events.
        thread[tid]->comInstEventQueue.serviceEvents(thread[tid]->numInst);
    }
    thread[tid]->numOp++;
    thread[tid]->threadStats.numOps++;
    cpuStats.committedOps[tid]++;

    probeInstCommit(inst->staticInst, inst->pcState().instAddr());
}

void
CPU::captureDVRRegisterSnapshot(
    ThreadID tid, const DynInstPtr &committing_inst,
    DVRLoopBoundDetector::RegisterSnapshot &snapshot)
{
    const unsigned num_int_regs = std::min<unsigned>(
        isa[tid]->regClasses().at(IntRegClass).numRegs(), snapshot.size());
    for (unsigned idx = 0; idx < num_int_regs; ++idx)
        snapshot[idx] = getArchReg(RegId(IntRegClass, idx), tid);

    // instDone() 紧接在 Commit 更新 commitRenameMap 之前执行。
    for (int idx = 0; idx < committing_inst->numDestRegs(); ++idx) {
        const RegId &dest = committing_inst->destRegIdx(idx);
        if (dest.classValue() == IntRegClass && dest.index() < snapshot.size())
            snapshot[dest.index()] = getReg(
                committing_inst->renamedDestIdx(idx));
    }
}

void
CPU::launchDVRStridePrefetches(ThreadID tid, Addr current_address,
                               Addr pc, int64_t stride, unsigned lanes,
                               const DVRLoopBoundDetector::RegisterSnapshot
                                   &finish_regs)
{
    lanes = std::min(lanes, DVRLanePredicateTracker::MaxLanes);
    // Dispatch-started discoveries can complete faster than the helper port
    // drains its queue.  Preserve the oldest committed helper instead of
    // replacing it on every subsequent discovery.
    if (!dvrPrefetchQueue.empty())
        return;
    for (const auto &queued : dvrPrefetchQueue) {
        if (queued.source && queued.predicate) {
            retireDVRPredicateLane(
                queued.predicate, queued.lane, false);
        }
    }
    finishDVRPredicateGeneration(dvrActivePredicateGeneration, true);
    cpuStats.dvrPrefetchesSuperseded += dvrPrefetchQueue.size();
    cpuStats.dvrPrefetchesDropped += dvrPrefetchQueue.size();
    dvrPrefetchQueue.clear();

    std::array<int64_t, DVRPrefetchSenderState::MaxRelations> scales = {};
    std::array<int64_t, DVRPrefetchSenderState::MaxRelations> offsets = {};
    std::array<RegVal, DVRPrefetchSenderState::MaxRelations> masks = {};
    std::array<RegVal, DVRPrefetchSenderState::MaxRelations> patterns = {};
    unsigned relation_count = 0;
    const auto trigger_it = dvrTriggerRelations.find(pc);
    if (trigger_it != dvrTriggerRelations.end()) {
        for (const Addr flr_pc : trigger_it->second) {
            const auto relation_it = dvrAddressRelations.find(flr_pc);
            if (relation_it == dvrAddressRelations.end() ||
                !relation_it->second.trained)
                continue;
            const auto &relation = relation_it->second;
            scales[relation_count] = relation.scale;
            offsets[relation_count] = relation.offset;
            masks[relation_count] = relation.stableMask;
            patterns[relation_count] = relation.pattern & relation.stableMask;
            if (++relation_count == DVRPrefetchSenderState::MaxRelations)
                break;
        }
    }
    if (relation_count > 1) {
        // stableMask 记录训练过程中保持不变的位。
        // 只保留两条路径都稳定且取值不同的位，避免谓词过严。
        const auto stable_masks = masks;
        const auto stable_patterns = patterns;
        for (unsigned lhs = 0; lhs < relation_count; ++lhs) {
            RegVal discriminating = 0;
            for (unsigned rhs = 0; rhs < relation_count; ++rhs) {
                if (lhs == rhs)
                    continue;
                discriminating |= stable_masks[lhs] & stable_masks[rhs] &
                    (stable_patterns[lhs] ^ stable_patterns[rhs]);
            }
            masks[lhs] = discriminating;
            patterns[lhs] = stable_patterns[lhs] & discriminating;
        }
    }

    std::shared_ptr<DVRPredicateGeneration> predicate;
    if (relation_count > 1) {
        predicate = std::make_shared<DVRPredicateGeneration>();
        predicate->generation = dvrNextPredicateGeneration++;
        predicate->expectedLanes = lanes;
        predicate->tracker.begin(lanes, relation_count, masks, patterns);
        dvrActivePredicateGeneration = predicate;
    } else {
        dvrActivePredicateGeneration.reset();
    }

    auto replay = std::make_shared<DVRReplayTemplate>();
    /*
     * Discovery continues to the next trigger so it can learn the loop
     * boundary, but DVR replay must stop at the FLR.  Instructions consuming
     * the FLR value (for example a scalar reduction accumulator) belong to
     * the main thread and are not address-generation inputs.
     */
    replay->count = 0;
    for (unsigned index = 1; index < dvrInstructionRecorder.size(); ++index) {
        if (dvrInstructionRecorder[index].load)
            replay->count = index + 1;
    }
    replay->initialRegs = dvrDiscoveryStartRegs;
    if (replay->count != 0) {
        replay->triggerDestination = dvrInstructionRecorder[0].destination;
        replay->valid = replay->count > 1 &&
            replay->triggerDestination > 0 &&
            replay->triggerDestination <
                DVRLoopBoundDetector::MaxArchitecturalIntRegs;
        uint32_t defined_regs = uint32_t(1);
        if (replay->triggerDestination > 0 &&
            replay->triggerDestination <
                DVRLoopBoundDetector::MaxArchitecturalIntRegs) {
            defined_regs |= uint32_t(1) << replay->triggerDestination;
        }
        bool unstable_input = false;
        for (unsigned index = 0; index < replay->count; ++index) {
            replay->uops[index] = dvrInstructionRecorder[index];
            if (index == 0)
                continue;
            uint32_t external_sources =
                replay->uops[index].intSources & ~defined_regs;
            while (external_sources) {
                const unsigned reg = __builtin_ctz(external_sources);
                external_sources &= external_sources - 1;
                if (dvrDiscoveryStartRegs[reg] != finish_regs[reg]) {
                    unstable_input = true;
                    replay->valid = false;
                }
            }
            if (replay->uops[index].semantic ==
                DVRInstructionRecorder::Uop::Semantic::Unsupported) {
                ++cpuStats.dvrReplayUnsupportedUops;
                replay->valid = false;
            } else {
                ++cpuStats.dvrReplaySupportedUops;
            }
            defined_regs |= replay->uops[index].intDestinations;
        }
        if (unstable_input)
            ++cpuStats.dvrReplayUnstableInputs;
    }
    if (replay->count > 1) {
        replay->continuation =
            std::make_shared<DVRVectorInstructionRegister>();
        replay->continuation->initializeSourceContinuation(
            replay->uops, replay->count, lanes, replay->initialRegs);
        ++cpuStats.dvrVIRContinuationContexts;
    }

    // Source stride prefetches are useful even when the committed slice did
    // not contain a replayable load uop.  Keep the helper alive for source
    // lanes; replay->count only controls dependent replay semantics.
    startDVRHelper(pc, std::max(1u, replay->count), lanes,
                   dvrInstructionRecorder.resourceCounts());
    for (unsigned lane = 1; lane <= lanes; ++lane) {
        const Addr address = current_address + stride * lane;
        DVRPrefetchAddress prefetch;
        prefetch.address = address;
        prefetch.pc = pc;
        prefetch.tid = tid;
        prefetch.source = true;
        prefetch.relationCount = relation_count;
        prefetch.scales = scales;
        prefetch.offsets = offsets;
        prefetch.masks = masks;
        prefetch.patterns = patterns;
        prefetch.replay = replay;
        prefetch.predicate = predicate;
        prefetch.lane = lane - 1;
        dvrPrefetchQueue.push_back(prefetch);
        ++cpuStats.dvrPrefetchesGenerated;
    }
    updateDVRPrefetchQueuePeak();
}

void
CPU::launchDVRVectorRunahead(ThreadID tid, Addr current_address,
                             Addr pc, int64_t stride)
{
    const unsigned lanes = std::min(dvrMaxLanes,
                                    DVRLanePredicateTracker::MaxLanes);
    dvrPrefetchQueue.clear();
    DVRInstructionRecorder::ResourceCounts vector_resources;
    vector_resources.lsu = 1;
    startDVRHelper(pc, 1, lanes, vector_resources);
    for (unsigned lane = 1; lane <= lanes; ++lane) {
        DVRPrefetchAddress prefetch;
        prefetch.address = current_address + stride * lane;
        prefetch.pc = pc;
        prefetch.tid = tid;
        prefetch.source = false;
        prefetch.lane = lane - 1;
        dvrPrefetchQueue.push_back(prefetch);
        ++cpuStats.dvrPrefetchesGenerated;
    }
    updateDVRPrefetchQueuePeak();
}

void
CPU::completeDVRNestedContext(
    const DynInstPtr &committing_inst,
    const DVRLoopBoundDetector::RegisterSnapshot &finish_regs)
{
    if (!dvrNestedContext.active)
        return;

    const auto inference = dvrNestedContext.loopBound.infer(
        dvrNestedContext.startRegs, finish_regs, dvrMaxLanes);
    // NDM records only a completed child with its own inferred bound.  The
    // old prototype copied the initiating short-loop bound here, which paired
    // one invocation's bound with another invocation's base.
    const unsigned ndm_invocation_lanes = inference.matched ? inference.lanes : 0;
    if (dvrNestedDiscoveryMode.state() ==
            DVRNestedDiscoveryMode::State::OuterFound &&
        ndm_invocation_lanes != 0 && dvrNestedContext.taint.flr() != 0 &&
        dvrNestedDiscoveryMode.recordOuterInvocation(
            dvrNestedContext.triggerAddress, ndm_invocation_lanes,
            dvrNestedContext.triggerPC, dvrNestedContext.taint.flr(),
            inference.increment)) {
        ++cpuStats.dvrNDMOuterInvocations;
    }
    bool helper_allowed = dvrNestedContext.recorder.size() > 1 &&
        dvrNestedContext.taint.flr() != 0 && inference.matched &&
        !dvrNestedContext.recorder.overflow();
    if (helper_allowed) {
        ++cpuStats.dvrNestedProgramsBuilt;
        cpuStats.dvrNestedVRATAllocations += dvrNestedContext.vrat.build(
            dvrNestedContext.recorder, inference.lanes);
        const auto vir_result = dvrNestedContext.vir.execute(
            dvrNestedContext.recorder, inference.lanes,
            dvrHelperMaxUops, dvrNestedContext.startRegs);
        cpuStats.dvrNestedVIRExecutions += vir_result.chunkExecutions;
        cpuStats.dvrDivergentBranches += vir_result.divergentBranches;
        cpuStats.dvrReconvergences += vir_result.reconvergences;
        cpuStats.dvrVIRNormalTerminatedLanes +=
            vir_result.normalTerminatedLanes;
        cpuStats.dvrVIREarlyExitLanes += vir_result.earlyExitLanes;
        cpuStats.dvrVIRExternalPathLanes += vir_result.externalPathLanes;
        cpuStats.dvrVIRUnsupportedSemanticLanes +=
            vir_result.unsupportedSemanticLanes;
        if (vir_result.unsupportedControlFlow) {
            ++cpuStats.dvrVIRUnsupportedControlFlow;
            helper_allowed = false;
        }
        helper_allowed = !vir_result.timedOut &&
            !vir_result.stackOverflow && helper_allowed;
    }
    if (helper_allowed) {
        // When NDM has accepted an outer stride, every child completion is
        // one independently bounded outer invocation.  Do not flatten until
        // NDM has observed at least two such invocations; this is the point
        // at which the paper's outer-loop vector is formed.
        if (dvrNestedInvocationBatch.triggerPC != 0 &&
            dvrNestedInvocationBatch.triggerPC !=
                dvrNestedContext.triggerPC) {
            dvrNestedInvocationBatch.reset();
        }
        dvrNestedInvocationBatch.triggerPC = dvrNestedContext.triggerPC;
        if (dvrNestedInvocationBatch.count <
            dvrNestedInvocationBatch.bases.size()) {
            const unsigned slot = dvrNestedInvocationBatch.count++;
            dvrNestedInvocationBatch.bases[slot] =
                dvrNestedContext.triggerAddress;
            dvrNestedInvocationBatch.innerLanes[slot] = inference.lanes;
            dvrNestedInvocationBatch.innerStrides[slot] = inference.increment;
        }
        // A single completed invocation is not Nested DVR.  Wait until at
        // least two independently bounded invocations can be combined.
        const bool ndm_allows_flatten =
            !dvrNestedDiscoveryMode.active() ||
            dvrNestedDiscoveryMode.readyToVectorize();
        if (dvrNestedInvocationBatch.count >= 2 && ndm_allows_flatten) {
            launchDVRNestedPrefetches(finish_regs);
            if (dvrNestedDiscoveryMode.readyToVectorize())
                dvrNestedDiscoveryMode.finishVectorization();
        }
    }

    DPRINTF(O3CPU,
            "Nested DVR context complete id=%llu trigger=%#x flr=%#x "
            "uops=%u lanes=%u helper=%d\n",
            static_cast<unsigned long long>(dvrNestedContext.id),
            dvrNestedContext.triggerPC, dvrNestedContext.taint.flr(),
            dvrNestedContext.recorder.size(), inference.lanes,
            helper_allowed);
    dvrNestedContext.reset();
}

void
CPU::launchDVRNestedPrefetches(
    const DVRLoopBoundDetector::RegisterSnapshot &finish_regs)
{
    auto replay = std::make_shared<DVRReplayTemplate>();
    replay->count = 0;
    for (unsigned index = 1;
         index < dvrNestedContext.recorder.size(); ++index) {
        if (dvrNestedContext.recorder[index].load)
            replay->count = index + 1;
    }
    replay->initialRegs = dvrNestedContext.startRegs;
    if (replay->count != 0) {
        replay->triggerDestination =
            dvrNestedContext.recorder[0].destination;
        replay->valid = replay->count > 1 &&
            replay->triggerDestination > 0 &&
            replay->triggerDestination <
                DVRLoopBoundDetector::MaxArchitecturalIntRegs;
        uint32_t defined_regs = uint32_t(1);
        if (replay->triggerDestination > 0 &&
            replay->triggerDestination <
                DVRLoopBoundDetector::MaxArchitecturalIntRegs) {
            defined_regs |= uint32_t(1) << replay->triggerDestination;
        }
        for (unsigned index = 0; index < replay->count; ++index) {
            replay->uops[index] = dvrNestedContext.recorder[index];
            if (index == 0)
                continue;
            uint32_t external = replay->uops[index].intSources &
                ~defined_regs;
            while (external) {
                const unsigned reg = __builtin_ctz(external);
                external &= external - 1;
                if (dvrNestedContext.startRegs[reg] != finish_regs[reg])
                    replay->valid = false;
            }
            if (replay->uops[index].semantic ==
                DVRInstructionRecorder::Uop::Semantic::Unsupported) {
                replay->valid = false;
            }
            defined_regs |= replay->uops[index].intDestinations;
        }
    }

    std::array<int64_t, DVRPrefetchSenderState::MaxRelations> scales = {};
    std::array<int64_t, DVRPrefetchSenderState::MaxRelations> offsets = {};
    std::array<RegVal, DVRPrefetchSenderState::MaxRelations> masks = {};
    std::array<RegVal, DVRPrefetchSenderState::MaxRelations> patterns = {};
    unsigned relation_count = 0;
    const auto trigger = dvrTriggerRelations.find(
        dvrNestedContext.triggerPC);
    if (trigger != dvrTriggerRelations.end()) {
        for (const Addr flr_pc : trigger->second) {
            const auto found = dvrAddressRelations.find(flr_pc);
            if (found == dvrAddressRelations.end() ||
                !found->second.trained)
                continue;
            scales[relation_count] = found->second.scale;
            offsets[relation_count] = found->second.offset;
            masks[relation_count] = found->second.stableMask;
            patterns[relation_count] = found->second.pattern &
                found->second.stableMask;
            if (++relation_count ==
                DVRPrefetchSenderState::MaxRelations)
                break;
        }
    }

    // Once NDM has completed its inner-loop control capture, its bounded
    // outer-invocation plan is authoritative.  The legacy CPU batch remains
    // a compatibility path for ordinary multi-invocation mode, but it must
    // never replace the NDM plan.  In particular, the address delta inside
    // each invocation is the inferred inner induction increment, not the
    // stride of the outer discovery candidate.
    const bool use_ndm_plan = dvrNestedDiscoveryMode.readyToVectorize();
    const unsigned invocations = use_ndm_plan ?
        std::min<unsigned>(dvrNestedDiscoveryMode.outerInvocationCount(), 8) :
        std::min<unsigned>(dvrNestedInvocationBatch.count,
                           dvrNestedInvocationBatch.bases.size());
    const auto &ndm_invocations = dvrNestedDiscoveryMode.outerInvocations();
    unsigned flattened = 0;
    unsigned total_inner_lanes = 0;
    bool variable_lanes = false;
    for (unsigned invocation = 0; invocation < invocations; ++invocation) {
        const unsigned plan_lanes = use_ndm_plan ?
            ndm_invocations[invocation].innerLanes :
            dvrNestedInvocationBatch.innerLanes[invocation];
        const unsigned invocation_lanes = std::min(plan_lanes,
            DVRLanePredicateTracker::MaxLanes);
        total_inner_lanes += invocation_lanes;
        if (invocation != 0 && invocation_lanes !=
            (use_ndm_plan ? ndm_invocations[0].innerLanes :
             dvrNestedInvocationBatch.innerLanes[0])) {
            variable_lanes = true;
        }
        flattened += std::min(invocation_lanes,
            DVRLanePredicateTracker::MaxLanes - flattened);
    }
    ++cpuStats.dvrNestedFlattenBatches;
    cpuStats.dvrNestedOuterInstances += invocations;
    cpuStats.dvrNestedInnerLanes += total_inner_lanes;
    cpuStats.dvrNestedFlattenedLanes += flattened;
    if (variable_lanes)
        ++cpuStats.dvrNestedVariableLaneBatches;
    if (replay->count > 1) {
        replay->continuation =
            std::make_shared<DVRVectorInstructionRegister>();
        replay->continuation->initializeSourceContinuation(
            replay->uops, replay->count, flattened, replay->initialRegs);
        ++cpuStats.dvrVIRContinuationContexts;
    }
    startDVRHelper(dvrNestedContext.triggerPC, replay->count,
                   flattened, dvrNestedContext.recorder.resourceCounts());
    unsigned flat_lane = 0;
    for (unsigned invocation = 0; invocation < invocations; ++invocation) {
      const Addr base = use_ndm_plan ? ndm_invocations[invocation].innerStart :
          dvrNestedInvocationBatch.bases[invocation];
      const unsigned invocation_lanes = use_ndm_plan ?
          ndm_invocations[invocation].innerLanes :
          dvrNestedInvocationBatch.innerLanes[invocation];
      const int64_t inner_stride = use_ndm_plan ?
          ndm_invocations[invocation].innerStride :
          dvrNestedInvocationBatch.innerStrides[invocation];
      for (unsigned lane = 1; lane <= invocation_lanes && flat_lane <
               DVRLanePredicateTracker::MaxLanes; ++lane, ++flat_lane) {
        DVRPrefetchAddress prefetch;
        prefetch.address = base + inner_stride * lane;
        prefetch.pc = dvrNestedContext.triggerPC;
        prefetch.tid = dvrNestedContext.tid;
        prefetch.source = true;
        prefetch.nested = true;
        prefetch.relationCount = relation_count;
        prefetch.scales = scales;
        prefetch.offsets = offsets;
        prefetch.masks = masks;
        prefetch.patterns = patterns;
        prefetch.replay = replay;
        prefetch.lane = flat_lane;
        dvrPrefetchQueue.push_back(prefetch);
        ++cpuStats.dvrPrefetchesGenerated;
        ++cpuStats.dvrNestedHelpersGenerated;
      }
    }
    dvrNestedInvocationBatch.reset();
    updateDVRPrefetchQueuePeak();
}

Addr
CPU::dvrPrefetchLine(Addr address) const
{
    const Addr line_size = cacheLineSize();
    return address & ~(line_size - 1);
}

void
CPU::updateDVRPrefetchQueuePeak()
{
    if (dvrPrefetchQueue.size() > dvrPrefetchQueuePeak) {
        const uint64_t increase =
            dvrPrefetchQueue.size() - dvrPrefetchQueuePeak;
        dvrPrefetchQueuePeak = dvrPrefetchQueue.size();
        cpuStats.dvrPrefetchQueuePeak += increase;
    }
}

void
CPU::accountDVRDemand(Addr address)
{
    const Addr line = dvrPrefetchLine(address);
    const auto completed = dvrCompletedPrefetchLines.find(line);
    if (completed != dvrCompletedPrefetchLines.end()) {
        ++cpuStats.dvrPrefetchesPossiblyUseful;
        dvrCompletedPrefetchLines.erase(completed);
        return;
    }
    const auto outstanding = dvrOutstandingPrefetchLines.find(line);
    if (outstanding != dvrOutstandingPrefetchLines.end() &&
        outstanding->second != 0) {
        ++cpuStats.dvrPrefetchesLate;
    }
}

void
CPU::finishDVRPredicateGeneration(
    const std::shared_ptr<DVRPredicateGeneration> &generation,
    bool replaced)
{
    if (!generation || generation->reported)
        return;

    if (replaced && generation->terminalLanes < generation->expectedLanes) {
        generation->reported = true;
        ++cpuStats.dvrPredicateGenerationAbandons;
        return;
    }
    if (generation->terminalLanes < generation->expectedLanes)
        return;

    generation->reported = true;
    if (generation->tracker.divergent())
        ++cpuStats.dvrReconvergences;
}

void
CPU::retireDVRPredicateLane(
    const std::shared_ptr<DVRPredicateGeneration> &generation,
    unsigned lane, bool has_value, RegVal value)
{
    if (!generation || generation->reported ||
        lane >= generation->expectedLanes) {
        return;
    }

    const uint64_t bit = uint64_t(1) << (lane % 64);
    if (generation->terminalMask[lane / 64] & bit)
        return;
    generation->terminalMask[lane / 64] |= bit;
    ++generation->terminalLanes;

    if (has_value) {
        generation->tracker.observe(lane, value);
        if (!generation->divergenceCounted &&
            generation->tracker.divergent()) {
            generation->divergenceCounted = true;
            ++cpuStats.dvrDivergentBranches;
        }
    }
    finishDVRPredicateGeneration(generation, false);
}

unsigned
CPU::issueDVRHelperCompute()
{
    unsigned slots = dvrIssueWidth;
    if (dvrMainIssuesThisCycle >= slots)
        return 0;
    slots -= dvrMainIssuesThisCycle;
    if (dvrHelperIssuesThisCycle >= slots)
        return 0;
    slots -= dvrHelperIssuesThisCycle;

    unsigned issued = 0;
    auto issue_class = [&](unsigned &remaining, OpClass op_class) {
        while (remaining != 0 && slots != 0) {
            ++cpuStats.dvrHelperFURequests;
            Cycles latency(1);
            if (!iew.tryIssueDVRHelperFU(op_class, latency)) {
                ++cpuStats.dvrHelperFUStalls;
                break;
            }
            --remaining;
            --slots;
            ++issued;
            ++dvrHelperComputeIssuesThisCycle;
            ++cpuStats.dvrHelperFUGrants;
        }
    };

    issue_class(dvrHelperThread.aluRemaining, IntAluOp);
    issue_class(dvrHelperThread.shiftRemaining, IntAluOp);
    issue_class(dvrHelperThread.multiplyRemaining, IntMultOp);
    return issued;
}

void
CPU::serviceDVRPrefetchQueue()
{
    if (dvrPrefetchQueue.empty() || !dvrHelperThread.canIssue())
        return;
    if (dvrHelperIssuesThisCycle >= DvrHelperIssueWidth) {
        ++cpuStats.dvrResourceConflicts;
        ++cpuStats.dvrIssueBudgetConflicts;
        return;
    }
    if (dvrMainIssuesThisCycle + dvrHelperIssuesThisCycle +
            dvrHelperComputeIssuesThisCycle >= dvrIssueWidth) {
        ++cpuStats.dvrResourceConflicts;
        ++cpuStats.dvrIssueBudgetConflicts;
        return;
    }
    if (dvrMainALUIssuesThisCycle >= dvrIssueWidth) {
        ++cpuStats.dvrResourceConflicts;
        ++cpuStats.dvrALUBudgetConflicts;
        return;
    }
    if (dvrMainLSUIssuesThisCycle >= dvrLSUWidth) {
        ++cpuStats.dvrResourceConflicts;
        ++cpuStats.dvrLSUBudgetConflicts;
        return;
    }

    const auto prefetch = dvrPrefetchQueue.front();
    dvrPrefetchQueue.pop_front();
    constexpr unsigned SourceBytes = sizeof(RegVal);
    const unsigned line_offset = prefetch.address & (cacheLineSize() - 1);
    if (prefetch.source && line_offset + SourceBytes > cacheLineSize()) {
        // The helper packet bypasses the architectural load-splitting path.
        // Never consume a partial scalar as a trigger value.
        ++cpuStats.dvrPrefetchesDropped;
        retireDVRPredicateLane(prefetch.predicate, prefetch.lane, false);
        return;
    }
    Request::Flags flags;
    flags.set(Request::PREFETCH | Request::DVR_PREFETCH);
    RequestPtr req = std::make_shared<Request>(
        prefetch.address, prefetch.source ? SourceBytes : 1,
        flags, dataRequestorId(), prefetch.pc,
        thread[prefetch.tid]->contextId());
    req->taskId(context_switch_task_id::Prefetcher);

    const Fault fault = mmu->translateAtomic(
        req, thread[prefetch.tid]->getTC(), BaseMMU::Read);
    if (fault != NoFault) {
        ++cpuStats.dvrPrefetchTranslationFaults;
        if (prefetch.source)
            ++cpuStats.dvrSourcePrefetchTranslationFaults;
        else {
            ++cpuStats.dvrDependentPrefetchTranslationFaults;
        }
        if (prefetch.source)
            retireDVRPredicateLane(
                prefetch.predicate, prefetch.lane, false);
        return;
    }

    /*
     * A source helper must return the loaded bytes for trigger-to-FLR replay.
     * SoftPFReq only guarantees a cache side effect; its response payload is
     * not a scalar load value.  ReadReq still has no architectural consumer
     * here, while Request::PREFETCH preserves helper accounting/priority.
     */
    PacketPtr pkt = new Packet(
        req, prefetch.source ? MemCmd::ReadReq : MemCmd::SoftPFReq);
    pkt->allocate();
    pkt->senderState = new DVRPrefetchSenderState(
        prefetch.source, prefetch.nested, prefetch.relationCount, prefetch.scales,
        prefetch.offsets, prefetch.masks, prefetch.patterns, prefetch.replay,
        prefetch.predicate, prefetch.lane, prefetch.tid);
    auto &port = iew.ldstQueue.getDataPort();
    if (!port.tryTiming(pkt)) {
        ++cpuStats.dvrPrefetchesSuppressedMainThread;
        ++cpuStats.dvrPrefetchesDropped;
        if (prefetch.source)
            retireDVRPredicateLane(
                prefetch.predicate, prefetch.lane, false);
        delete pkt->senderState;
        delete pkt;
        return;
    }
    if (!port.sendTimingReq(pkt)) {
        ++cpuStats.dvrPrefetchesRejectedBackpressure;
        ++cpuStats.dvrPrefetchesDropped;
        if (prefetch.source)
            retireDVRPredicateLane(
                prefetch.predicate, prefetch.lane, false);
        delete pkt->senderState;
        delete pkt;
        return;
    }
    ++cpuStats.dvrPrefetchesIssued;
    ++dvrHelperIssuesThisCycle;
    ++cpuStats.dvrHelperIssueCycles;
    dvrHelperThread.issue();
    dvrQualityTracker.issued(
        reinterpret_cast<uintptr_t>(pkt), dvrPrefetchLine(req->getPaddr()),
        pkt->getSize(), curTick());
    cpuStats.dvrQualityIssuedBytes += pkt->getSize();
    ++dvrOutstandingPrefetchLines[dvrPrefetchLine(prefetch.address)];
    if (!prefetch.source) {
        ++cpuStats.dvrDependentPrefetchesIssued;
    } else {
        ++cpuStats.dvrSourcePrefetchesIssued;
    }
    if (prefetch.nested)
        ++cpuStats.dvrNestedHelpersIssued;
}

void
CPU::completeDVRPrefetch(PacketPtr pkt)
{
    auto *state = dynamic_cast<DVRPrefetchSenderState*>(pkt->senderState);
    assert(state);
    ++cpuStats.dvrPrefetchesCompleted;
    dvrQualityTracker.completed(reinterpret_cast<uintptr_t>(pkt), curTick());
    cpuStats.dvrQualityCompletedBytes += pkt->getSize();
    // observeDVRLoad 使用架构有效虚拟地址，因此质量统计也使用请求虚拟地址。
    const Addr line = dvrPrefetchLine(pkt->req->getVaddr());
    auto outstanding = dvrOutstandingPrefetchLines.find(line);
    if (outstanding != dvrOutstandingPrefetchLines.end()) {
        if (--outstanding->second == 0)
            dvrOutstandingPrefetchLines.erase(outstanding);
    }
    dvrCompletedPrefetchLines[line] = curTick();
    if (state->source && pkt->hasData()) {
        const RegVal value = pkt->getLE<RegVal>();
        retireDVRPredicateLane(state->predicate, state->lane, true, value);
        if (state->replay && state->replay->count > 1 &&
            state->replay->triggerDestination > 0 &&
            state->replay->triggerDestination <
                DVRLoopBoundDetector::MaxArchitecturalIntRegs) {
            DVRVectorInstructionRegister response_vir;
            if (state->replay->continuation)
                ++cpuStats.dvrVIRContinuationResumes;
            else
                ++cpuStats.dvrVIRContinuationFallbacks;
            const auto response = state->replay->continuation ?
                state->replay->continuation->resumeSourceLanes(
                    state->replay->uops, state->replay->count,
                    state->lane, state->replay->triggerDestination, value,
                    dvrHelperMaxUops) :
                response_vir.executeFromSource(
                    state->replay->uops, state->replay->count,
                    state->replay->triggerDestination, value,
                    dvrHelperMaxUops, state->replay->initialRegs);
            ++cpuStats.dvrVIRSourceValueExecutions;
            cpuStats.dvrVIRSourceValueBranches += response.divergentBranches;
            cpuStats.dvrVIRSourceValueExternalLanes +=
                response.externalPathLanes;
            cpuStats.dvrVIRSourceValueSemanticFailures +=
                response.unsupportedSemanticLanes;
            cpuStats.dvrVIRSourceValueTerminations +=
                response.normalTerminatedLanes + response.earlyExitLanes;
            cpuStats.dvrVIRContinuationPCGroups += response.pcGroups;
            cpuStats.dvrVIRContinuationGroupedLanes += response.activeLanes;
            if (response.maxPCGroupLanes != 0)
                cpuStats.dvrVIRContinuationMaxGroupWidth =
                    response.maxPCGroupLanes;
        }
        bool matched = dvrEnableDependentPrefetch &&
                       replayDVRSource(*state, value);
        if (dvrEnableDependentPrefetch && !matched) {
            if (state->replay) {
                ++cpuStats.dvrReplayFallbacks;
                if (state->nested)
                    ++cpuStats.dvrNestedReplayFallbacks;
            }
            for (unsigned index = 0; index < state->relationCount; ++index) {
                if (state->relationCount > 1 &&
                    (value & state->masks[index]) != state->patterns[index])
                    continue;
                DVRPrefetchAddress dependent;
                dependent.address = static_cast<Addr>(
                    state->scales[index] * static_cast<int64_t>(value) +
                    state->offsets[index]);
                // RISC-V user virtual addresses in this SE model are below
                // the canonical 47-bit boundary.  Do not enqueue an affine
                // target that can only become an MMU translation fault.
                if (dependent.address == 0 ||
                    dependent.address >= (Addr(1) << 47)) {
                    continue;
                }
                dependent.pc = pkt->req->getPC();
                dependent.tid = state->tid;
                dependent.source = false;
                dependent.nested = state->nested;
                dvrPrefetchQueue.push_back(dependent);
                updateDVRPrefetchQueuePeak();
                ++cpuStats.dvrDependentPrefetchesGenerated;
                if (state->nested)
                    ++cpuStats.dvrNestedDependentGenerated;
                ++cpuStats.dvrPredicateSelections;
                if (!(dvrSelectedRelationSlots & (uint8_t(1) << index))) {
                    dvrSelectedRelationSlots |= uint8_t(1) << index;
                    ++cpuStats.dvrDistinctPredicatePaths;
                }
                matched = true;
                break;
            }
        }
        if (!matched)
            ++cpuStats.dvrPredicateMisses;
    }
    if (state->source) {
        if (!pkt->hasData())
            retireDVRPredicateLane(
                state->predicate, state->lane, false);
        ++cpuStats.dvrSourcePrefetchesCompleted;
    } else {
        ++cpuStats.dvrDependentPrefetchesCompleted;
    }
    if (state->nested)
        ++cpuStats.dvrNestedHelpersCompleted;
    dvrHelperThread.complete();
    delete state;
    pkt->senderState = nullptr;
    delete pkt;
}

void
CPU::startDVRHelper(
    Addr trigger_pc, unsigned program_uops, unsigned lanes,
    const DVRInstructionRecorder::ResourceCounts &resources)
{
    if (program_uops == 0 || lanes == 0)
        return;

    dvrHelperThread.begin(dvrNextHelperId++, trigger_pc, program_uops,
                          lanes, dvrHelperMaxUops, resources);
    cpuStats.dvrHelperALUOps += resources.alu * lanes;
    cpuStats.dvrHelperShiftOps += resources.shift * lanes;
    cpuStats.dvrHelperMultiplyOps += resources.multiply * lanes;
    cpuStats.dvrHelperLSUOps += resources.lsu * lanes;
    DPRINTF(O3CPU,
            "DVR helper start id=%llu trigger=%#x uops=%u lanes=%u budget=%u\n",
            static_cast<unsigned long long>(dvrHelperThread.id), trigger_pc,
            program_uops, lanes, dvrHelperMaxUops);
}

bool
CPU::replayDVRSource(const DVRPrefetchSenderState &state,
                     RegVal source_value)
{
    if (!state.replay || !state.replay->valid)
        return false;

    ++cpuStats.dvrReplayAttempts;
    if (state.nested)
        ++cpuStats.dvrNestedReplayAttempts;
    auto regs = state.replay->initialRegs;
    regs[0] = 0;
    regs[state.replay->triggerDestination] = source_value;

    for (unsigned index = 1; index < state.replay->count; ++index) {
        const auto &uop = state.replay->uops[index];
        if (uop.source0 < 0 ||
            uop.source0 >= DVRLoopBoundDetector::MaxArchitecturalIntRegs)
            return false;
        const RegVal source0 = regs[uop.source0];
        RegVal source1 = 0;
        if (uop.semantic != DVRInstructionRecorder::Uop::Semantic::Add &&
            uop.semantic != DVRInstructionRecorder::Uop::Semantic::Sub &&
            uop.semantic != DVRInstructionRecorder::Uop::Semantic::And &&
            uop.semantic != DVRInstructionRecorder::Uop::Semantic::Or &&
            uop.semantic != DVRInstructionRecorder::Uop::Semantic::Xor &&
            uop.semantic != DVRInstructionRecorder::Uop::Semantic::ShiftLeft &&
            uop.semantic != DVRInstructionRecorder::Uop::Semantic::ShiftRightLogical &&
            uop.semantic != DVRInstructionRecorder::Uop::Semantic::ShiftRightArithmetic &&
            uop.semantic != DVRInstructionRecorder::Uop::Semantic::Multiply &&
            uop.semantic != DVRInstructionRecorder::Uop::Semantic::AddImmediate &&
            uop.semantic != DVRInstructionRecorder::Uop::Semantic::AndImmediate &&
            uop.semantic != DVRInstructionRecorder::Uop::Semantic::OrImmediate &&
            uop.semantic != DVRInstructionRecorder::Uop::Semantic::XorImmediate &&
            uop.semantic != DVRInstructionRecorder::Uop::Semantic::ShiftLeftImmediate &&
            uop.semantic != DVRInstructionRecorder::Uop::Semantic::ShiftRightLogicalImmediate &&
            uop.semantic != DVRInstructionRecorder::Uop::Semantic::ShiftRightArithmeticImmediate &&
            uop.semantic != DVRInstructionRecorder::Uop::Semantic::LoadAddress &&
            uop.semantic != DVRInstructionRecorder::Uop::Semantic::LoadByteSigned &&
            uop.semantic != DVRInstructionRecorder::Uop::Semantic::LoadHalfSigned &&
            uop.semantic != DVRInstructionRecorder::Uop::Semantic::LoadWordSigned &&
            uop.semantic != DVRInstructionRecorder::Uop::Semantic::LoadWordUnsigned &&
            uop.semantic != DVRInstructionRecorder::Uop::Semantic::LoadDouble &&
            uop.semantic != DVRInstructionRecorder::Uop::Semantic::AddWord &&
            uop.semantic != DVRInstructionRecorder::Uop::Semantic::SubWord) {
            if (uop.source1 < 0 ||
                uop.source1 >=
                    DVRLoopBoundDetector::MaxArchitecturalIntRegs)
                return false;
            source1 = regs[uop.source1];
        }
        RegVal result = 0;
        if (!uop.evaluate(source0, source1, result))
            return false;

        const bool is_replayed_load =
            uop.semantic == DVRInstructionRecorder::Uop::Semantic::LoadAddress ||
            uop.semantic == DVRInstructionRecorder::Uop::Semantic::LoadByteSigned ||
            uop.semantic == DVRInstructionRecorder::Uop::Semantic::LoadHalfSigned ||
            uop.semantic == DVRInstructionRecorder::Uop::Semantic::LoadWordSigned ||
            uop.semantic == DVRInstructionRecorder::Uop::Semantic::LoadWordUnsigned ||
            uop.semantic == DVRInstructionRecorder::Uop::Semantic::LoadDouble;
        if (is_replayed_load) {
            DVRPrefetchAddress dependent;
            dependent.address = static_cast<Addr>(result);
            if (dependent.address == 0 ||
                dependent.address >= (Addr(1) << 47))
                return false;
            dependent.pc = uop.pc;
            dependent.tid = state.tid;
            dependent.source = false;
            dependent.nested = state.nested;
            dvrPrefetchQueue.push_back(dependent);
            updateDVRPrefetchQueuePeak();
            ++cpuStats.dvrDependentPrefetchesGenerated;
            ++cpuStats.dvrReplayTargetsGenerated;
            if (state.nested) {
                ++cpuStats.dvrNestedReplayTargetsGenerated;
                ++cpuStats.dvrNestedDependentGenerated;
            }
            return true;
        }

        if (uop.destination <= 0 ||
            uop.destination >=
                DVRLoopBoundDetector::MaxArchitecturalIntRegs)
            return false;
        regs[uop.destination] = result;
        regs[0] = 0;
    }
    return false;
}

void
CPU::trainDVRAddressRelation(Addr trigger_pc, Addr flr_pc, RegVal source_value,
                             Addr dependent_address)
{
    auto &relation = dvrAddressRelations[flr_pc];
    auto &trigger_relations = dvrTriggerRelations[trigger_pc];
    if (std::find(trigger_relations.begin(), trigger_relations.end(), flr_pc) ==
        trigger_relations.end())
        trigger_relations.push_back(flr_pc);
    if (relation.samples == 0) {
        relation.pattern = source_value;
    } else {
        relation.stableMask &= ~(relation.previousValue ^ source_value);
        relation.pattern &= relation.stableMask;
    }
    ++relation.samples;
    if (relation.hasPrevious && source_value != relation.previousValue) {
        const int64_t value_delta = static_cast<int64_t>(
            source_value - relation.previousValue);
        const int64_t address_delta = static_cast<int64_t>(
            dependent_address - relation.previousAddress);
        if (value_delta != 0 && address_delta % value_delta == 0) {
            const int64_t scale = address_delta / value_delta;
            if (scale != 0 && std::abs(scale) <= 4096) {
                const bool newly_trained = !relation.trained;
                relation.scale = scale;
                relation.offset = static_cast<int64_t>(dependent_address) -
                    scale * static_cast<int64_t>(source_value);
                relation.trained = true;
                if (newly_trained)
                    ++cpuStats.dvrAddressRelationsTrained;
            }
        }
    }
    relation.hasPrevious = true;
    relation.previousValue = source_value;
    relation.previousAddress = dependent_address;
}

void
CPU::removeFrontInst(const DynInstPtr &inst)
{
    DPRINTF(O3CPU, "Removing committed instruction [tid:%i] PC %s "
            "[sn:%lli]\n",
            inst->threadNumber, inst->pcState(), inst->seqNum);

    removeInstsThisCycle = true;

    // Remove the front instruction.
    removeList.push(inst->getInstListIt());
}

void
CPU::removeInstsNotInROB(ThreadID tid)
{
    DPRINTF(O3CPU, "Thread %i: Deleting instructions from instruction"
            " list.\n", tid);

    ListIt end_it;

    bool rob_empty = false;

    if (instList.empty()) {
        return;
    } else if (rob.isEmpty(tid)) {
        DPRINTF(O3CPU, "ROB is empty, squashing all insts.\n");
        end_it = instList.begin();
        rob_empty = true;
    } else {
        end_it = (rob.readTailInst(tid))->getInstListIt();
        DPRINTF(O3CPU, "ROB is not empty, squashing insts not in ROB.\n");
    }

    removeInstsThisCycle = true;

    ListIt inst_it = instList.end();

    inst_it--;

    // Walk through the instruction list, removing any instructions
    // that were inserted after the given instruction iterator, end_it.
    while (inst_it != end_it) {
        assert(!instList.empty());

        squashInstIt(inst_it, tid);

        inst_it--;
    }

    // If the ROB was empty, then we actually need to remove the first
    // instruction as well.
    if (rob_empty) {
        squashInstIt(inst_it, tid);
    }
}

void
CPU::removeInstsUntil(const InstSeqNum &seq_num, ThreadID tid)
{
    assert(!instList.empty());

    removeInstsThisCycle = true;

    ListIt inst_iter = instList.end();

    inst_iter--;

    DPRINTF(O3CPU, "Deleting instructions from instruction "
            "list that are from [tid:%i] and above [sn:%lli] (end=%lli).\n",
            tid, seq_num, (*inst_iter)->seqNum);

    while ((*inst_iter)->seqNum > seq_num) {

        bool break_loop = (inst_iter == instList.begin());

        squashInstIt(inst_iter, tid);

        inst_iter--;

        if (break_loop)
            break;
    }
}

void
CPU::squashInstIt(const ListIt &instIt, ThreadID tid)
{
    if ((*instIt)->threadNumber == tid) {
        DPRINTF(O3CPU, "Squashing instruction, "
                "[tid:%i] [sn:%lli] PC %s\n",
                (*instIt)->threadNumber,
                (*instIt)->seqNum,
                (*instIt)->pcState());

        // Discovery opens at dispatch.  If the trigger is later removed by
        // an O3 squash, commit cannot observe its abandonment; restore the
        // speculative DVR state at this boundary instead.
        if (enableDVR && dvrDiscovery.rollback((*instIt)->seqNum)) {
            ++cpuStats.dvrDiscoveryRollbacks;
            dvrTaintTracker.reset();
            dvrLoopBoundDetector.reset();
            dvrInstructionRecorder.reset();
            dvrVectorRenameTable.reset();
            dvrVectorInstructionRegister.reset();
            dvrNestedController.reset();
            dvrNestedDiscoveryMode.reset();
            dvrNestedContext.reset();
            dvrPendingNestedCandidate = {};
            dvrCommittedNestedCandidate = {};
            dvrDispatchTainted.clear();
            dvrDispatchDependentLoads.clear();
        }

        // Mark it as squashed.
        (*instIt)->setSquashed();

        // @todo: Formulate a consistent method for deleting
        // instructions from the instruction list
        // Remove the instruction from the list.
        removeList.push(instIt);
    }
}

void
CPU::cleanUpRemovedInsts()
{
    while (!removeList.empty()) {
        DPRINTF(O3CPU, "Removing instruction, "
                "[tid:%i] [sn:%lli] PC %s\n",
                (*removeList.front())->threadNumber,
                (*removeList.front())->seqNum,
                (*removeList.front())->pcState());

        instList.erase(removeList.front());

        removeList.pop();
    }

    removeInstsThisCycle = false;
}
/*
void
CPU::removeAllInsts()
{
    instList.clear();
}
*/
void
CPU::dumpInsts()
{
    int num = 0;

    ListIt inst_list_it = instList.begin();

    cprintf("Dumping Instruction List\n");

    while (inst_list_it != instList.end()) {
        cprintf("Instruction:%i\nPC:%#x\n[tid:%i]\n[sn:%lli]\nIssued:%i\n"
                "Squashed:%i\n\n",
                num, (*inst_list_it)->pcState().instAddr(),
                (*inst_list_it)->threadNumber,
                (*inst_list_it)->seqNum, (*inst_list_it)->isIssued(),
                (*inst_list_it)->isSquashed());
        inst_list_it++;
        ++num;
    }
}
/*
void
CPU::wakeDependents(const DynInstPtr &inst)
{
    iew.wakeDependents(inst);
}
*/
void
CPU::wakeCPU()
{
    if (activityRec.active() || tickEvent.scheduled()) {
        DPRINTF(Activity, "CPU already running.\n");
        return;
    }

    DPRINTF(Activity, "Waking up CPU\n");

    Cycles cycles(curCycle() - lastRunningCycle);
    // @todo: This is an oddity that is only here to match the stats
    if (cycles > 1) {
        --cycles;
        cpuStats.idleCycles += cycles;
        baseStats.numCycles += cycles;
    }

    schedule(tickEvent, clockEdge());
}

void
CPU::wakeup(ThreadID tid)
{
    if (thread[tid]->status() != gem5::ThreadContext::Suspended)
        return;

    wakeCPU();

    DPRINTF(Quiesce, "Suspended Processor woken\n");
    threadContexts[tid]->activate();
}

ThreadID
CPU::getFreeTid()
{
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        if (!tids[tid]) {
            tids[tid] = true;
            return tid;
        }
    }

    return InvalidThreadID;
}

void
CPU::updateThreadPriority()
{
    if (activeThreads.size() > 1) {
        //DEFAULT TO ROUND ROBIN SCHEME
        //e.g. Move highest priority to end of thread list
        std::list<ThreadID>::iterator list_begin = activeThreads.begin();

        unsigned high_thread = *list_begin;

        activeThreads.erase(list_begin);

        activeThreads.push_back(high_thread);
    }
}

void
CPU::addThreadToExitingList(ThreadID tid)
{
    DPRINTF(O3CPU, "Thread %d is inserted to exitingThreads list\n", tid);

    // the thread trying to exit can't be already halted
    assert(tcBase(tid)->status() != gem5::ThreadContext::Halted);

    // make sure the thread has not been added to the list yet
    assert(exitingThreads.count(tid) == 0);

    // add the thread to exitingThreads list to mark that this thread is
    // trying to exit. The boolean value in the pair denotes if a thread is
    // ready to exit. The thread is not ready to exit until the corresponding
    // exit trap event is processed in the future. Until then, it'll be still
    // an active thread that is trying to exit.
    exitingThreads.emplace(std::make_pair(tid, false));
}

bool
CPU::isThreadExiting(ThreadID tid) const
{
    return exitingThreads.count(tid) == 1;
}

void
CPU::scheduleThreadExitEvent(ThreadID tid)
{
    assert(exitingThreads.count(tid) == 1);

    // exit trap event has been processed. Now, the thread is ready to exit
    // and be removed from the CPU.
    exitingThreads[tid] = true;

    // we schedule a threadExitEvent in the next cycle to properly clean
    // up the thread's states in the pipeline. threadExitEvent has lower
    // priority than tickEvent, so the cleanup will happen at the very end
    // of the next cycle after all pipeline stages complete their operations.
    // We want all stages to complete squashing instructions before doing
    // the cleanup.
    if (!threadExitEvent.scheduled()) {
        schedule(threadExitEvent, nextCycle());
    }
}

void
CPU::exitThreads()
{
    // there must be at least one thread trying to exit
    assert(exitingThreads.size() > 0);

    // terminate all threads that are ready to exit
    auto it = exitingThreads.begin();
    while (it != exitingThreads.end()) {
        ThreadID thread_id = it->first;
        bool readyToExit = it->second;

        if (readyToExit) {
            DPRINTF(O3CPU, "Exiting thread %d\n", thread_id);
            haltContext(thread_id);
            tcBase(thread_id)->setStatus(gem5::ThreadContext::Halted);
            it = exitingThreads.erase(it);
        } else {
            it++;
        }
    }
}

void
CPU::htmSendAbortSignal(ThreadID tid, uint64_t htm_uid,
        HtmFailureFaultCause cause)
{
    const Addr addr = 0x0ul;
    const int size = 8;
    const Request::Flags flags =
      Request::PHYSICAL|Request::STRICT_ORDER|Request::HTM_ABORT;

    // O3-specific actions
    iew.ldstQueue.resetHtmStartsStops(tid);
    commit.resetHtmStartsStops(tid);

    // notify l1 d-cache (ruby) that core has aborted transaction
    RequestPtr req =
        std::make_shared<Request>(addr, size, flags, _dataRequestorId);

    req->taskId(taskId());
    req->setContext(thread[tid]->contextId());
    req->setHtmAbortCause(cause);

    assert(req->isHTMAbort());

    PacketPtr abort_pkt = Packet::createRead(req);
    uint8_t *memData = new uint8_t[8];
    assert(memData);
    abort_pkt->dataStatic(memData);
    abort_pkt->setHtmTransactional(htm_uid);

    // TODO include correct error handling here
    if (!iew.ldstQueue.getDataPort().sendTimingReq(abort_pkt)) {
        panic("HTM abort signal was not sent to the memory subsystem.");
    }
}

void
CPU::enterPRE()
{
    const auto &regClasses = isa[0]->regClasses();

    for (auto type = (RegClassType)0; type <= CCRegClass;
            type = (RegClassType)(type + 1)) {
        // Clear the usableForPRE bit of registers and only mark those that
        // are in the free list. Only registers that are free when entering
        // PRE can be allocated as destination register during PRE.

        // Checkpoint the rename map.
        for (RegIndex ridx = 0; ridx < regClasses.at(type).numRegs();
                ++ridx) {
            RegId rid = RegId(type, ridx);
            PhysRegIdPtr phys_reg = renameMap[0].lookup(rid);
            phys_reg->setUsableForPRE(false);
            checkpointRenameMap[type].push_back(phys_reg);
        }

        // Checkpoint the free list.
        for (unsigned num = freeList.numFreeRegs(type); num; num--) {
            PhysRegIdPtr phys_reg = freeList.getReg(type);
            freeList.addReg(phys_reg);
            phys_reg->setUsableForPRE(true);
            checkpointFreeList[type].push_back(phys_reg);
        }
    }

    inPRE = true;
}

void
CPU::exitPRE()
{
    const auto &regClasses = isa[0]->regClasses();

    for (auto type = (RegClassType)0; type <= CCRegClass;
            type = (RegClassType)(type + 1)) {

        // Restore the rename map.
        for (RegIndex ridx = 0; ridx < regClasses.at(type).numRegs();
                ++ridx) {
            RegId rid = RegId(type, ridx);
            PhysRegIdPtr phys_reg = checkpointRenameMap[type].at(ridx);
            renameMap[0].setEntry(rid, phys_reg);
        }

        // Restore the free list.
        for (auto num = freeList.numFreeRegs(type); num; num--) {
            freeList.getReg(type);
        }
        for (auto phys_reg : checkpointFreeList[type]) {
            freeList.addReg(phys_reg);
        }

        // Invalidate the checkpoint.
        checkpointRenameMap[type].clear();
        checkpointFreeList[type].clear();
    }

    inPRE = false;
}

void
CPU::observeDVRDispatch(const DynInstPtr &inst)
{
    if (!enableDVR || inPRE || inst->isPRE())
        return;

    const bool is_discovery_end = dvrDiscovery.observeDispatch(
        inst->pcState().instAddr(), inst->seqNum);

    if (inst->isLoad()) {
        const auto candidate = dvrStrideDetector.observeDispatch(
            inst->pcState().instAddr());
        if (candidate) {
            ++cpuStats.dvrStrideCandidates;
            if (dvrMode == "vr") {
                launchDVRVectorRunahead(inst->threadNumber, candidate->address,
                                        candidate->pc, candidate->stride);
            } else if (dvrDiscovery.isDiscovering() ||
                       dvrNestedDiscoveryMode.active()) {
                if (!dvrPendingNestedCandidate.valid ||
                    inst->seqNum < dvrPendingNestedCandidate.sequence) {
                    dvrPendingNestedCandidate = {
                        true, candidate->pc, inst->seqNum,
                        candidate->address, candidate->stride};
                }
            } else {
                dvrDiscovery.arm(*candidate, inst->seqNum);
                ++cpuStats.dvrDiscoveryStarts;
                // Discovery is a dispatch-time mechanism.  Mirror the
                // root generation here as well, so nested child contexts
                // can be attached to the same speculative stream before
                // the trigger load commits.  The old commit-side hook is
                // retained only as a compatibility path for controllers
                // that report an explicit Started event.
                if (dvrMode == "nested" &&
                    dvrNestedController.startRoot(
                        candidate->pc, inst->seqNum).event ==
                        DVRNestedController::Event::Started) {
                    ++cpuStats.dvrNestedRootStarts;
                }
                dvrTaintTracker.begin(inst);
                dvrLoopBoundDetector.begin(candidate->pc);
                dvrInstructionRecorder.begin(inst);
                dvrDispatchTainted.clear();
                dvrDispatchDependentLoads.clear();
                if (!dvrNestedDiscoveryMode.active())
                    dvrNestedDiscoveryMode.reset();
                dvrCommittedNestedCandidate = {};
                dvrCurrentTriggerPC = candidate->pc;
                dvrCurrentTriggerAddress = candidate->address;
                dvrInitiatingLoadValue = 0;
                captureDVRRegisterSnapshot(
                    inst->threadNumber, inst, dvrDiscoveryStartRegs);
            }
        }
    }

    if (dvrDiscovery.isDiscovering() && !is_discovery_end &&
        inst->seqNum > dvrDiscovery.triggerSeq()) {
        const auto observation = dvrTaintTracker.observe(inst);
        if (observation.taintedInstruction) {
            ++cpuStats.dvrTaintedInstructions;
            dvrDispatchTainted.insert(inst->seqNum);
        }
        if (observation.dependentLoad) {
            ++cpuStats.dvrDependentLoads;
            dvrDispatchDependentLoads.insert(inst->seqNum);
        }
    }
}

void
CPU::observeDVRLoad(const DynInstPtr &inst, Addr address)
{
    if (!enableDVR || inPRE || inst->isPRE())
        return;

    accountDVRDemand(address);
    dvrQualityTracker.demandAddressObserved();
    ++cpuStats.dvrQualityDemandAddressesObserved;
    ++cpuStats.dvrLoadsObserved;
    dvrStrideDetector.observe(inst->pcState().instAddr(), address);
    if (dvrDiscovery.isDiscovering() &&
        inst->seqNum == dvrDiscovery.triggerSeq()) {
        for (int dest = 0; dest < inst->numDestRegs(); ++dest) {
            if (inst->destRegIdx(dest).classValue() == IntRegClass) {
                dvrInitiatingLoadValue = getReg(inst->renamedDestIdx(dest));
                break;
            }
        }
    }
}

} // namespace o3
} // namespace gem5
