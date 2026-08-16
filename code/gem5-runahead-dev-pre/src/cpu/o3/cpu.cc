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

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits>
#include <fstream>
#include <string>

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
#include "arch/riscv/decoder.hh"
#include "mem/se_translating_port_proxy.hh"
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
    bool is_source, bool is_nested, bool is_oracle, unsigned relation_count,
    const std::array<int64_t, MaxRelations> &relation_scales,
    const std::array<int64_t, MaxRelations> &relation_offsets,
    const std::array<RegVal, MaxRelations> &relation_masks,
    const std::array<RegVal, MaxRelations> &relation_patterns,
    std::shared_ptr<const DVRReplayTemplate> replay_template,
    std::shared_ptr<DVRHelperVectorRegisterFile> helper_regs,
    std::shared_ptr<DVRPredicateGeneration> predicate_generation,
    unsigned lane_id,
    ThreadID thread)
    : source(is_source), nested(is_nested), oracle(is_oracle),
      relationCount(relation_count),
      scales(relation_scales), offsets(relation_offsets),
      masks(relation_masks), patterns(relation_patterns),
      replay(std::move(replay_template)),
      helperRegs(std::move(helper_regs)),
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
      dvrInstructionPort(this),
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
      dvrEnableDependentPrefetch(params.dvrEnableDependentPrefetch),
      dvrPCMin(params.dvrPCMin),
      dvrPCMax(params.dvrPCMax),
      dvrAllowBoundedFallback(params.dvrAllowBoundedFallback),
      dvrSharedPhysicalBank(params.dvrSharedPhysicalBank),
      oraclePrefetch(params.oraclePrefetch),
      oracleTraceFile(params.oracleTraceFile),
      oracleLookahead(params.oracleLookahead),
      dvrVectorChunkModel(params.dvrVectorChunkModel),
      dvrVectorUnlimitedFU(params.dvrVectorUnlimitedFU),
      dvrDecoupledIssue(params.dvrDecoupledIssue),
      dvrVectorElementBits(params.dvrVectorElementBits),
      dvrVectorIssueInterval(params.dvrVectorIssueInterval)
{
    if (dvrVectorElementBits == 0 ||
        DvrVectorBits % dvrVectorElementBits != 0) {
        fatal("DVR vector element width must divide 512 bits\n");
    }
    if (oraclePrefetch && oracleTraceFile.empty())
        fatal("oraclePrefetch requires oracleTraceFile\n");
    if (oraclePrefetch) {
        std::ifstream trace(oracleTraceFile);
        std::string line;
        while (std::getline(trace, line)) {
            if (line.empty() || line.rfind("tick,", 0) == 0)
                continue;
            const auto comma = line.rfind(',');
            if (comma == std::string::npos)
                continue;
            const std::string address = line.substr(comma + 1);
            try {
                oracleLoadTrace.push_back(
                    static_cast<Addr>(std::stoull(address, nullptr, 0)));
            } catch (...) {
                continue;
            }
        }
        if (oracleLoadTrace.empty())
            fatal("oracle trace contains no load addresses: %s\n",
                  oracleTraceFile);
    }
    if (const char *trace_dir = std::getenv("DVR_TRACE_DIR")) {
        dvrTrace.narrow = std::getenv("DVR_TRACE_NARROW") != nullptr;
        dvrTrace.branchCensus =
            std::getenv("DVR_TRACE_BRANCH_CENSUS") != nullptr;
        char path[4096];
        auto open_trace = [&](const char *name) -> FILE * {
            std::snprintf(path, sizeof(path), "%s/%s", trace_dir, name);
            return std::fopen(path, "w");
        };
        dvrTrace.workload = open_trace("workload.csv");
        dvrTrace.dependency = open_trace("dependency_chain.csv");
        dvrTrace.vectorization = open_trace("vectorization.csv");
        dvrTrace.loopBounds = open_trace("loop_bounds.csv");
        dvrTrace.events = open_trace("events.jsonl");
        if (dvrTrace.workload)
            std::fprintf(dvrTrace.workload, "tick,seq,kind,pc,address\n");
        if (dvrTrace.dependency)
            std::fprintf(dvrTrace.dependency,
                "tick,kind,trigger_pc,pc,address,taint,lanes\n");
        if (dvrTrace.vectorization)
            std::fprintf(dvrTrace.vectorization,
                "tick,kind,pc,address,lanes,invocation\n");
        if (dvrTrace.loopBounds)
            std::fprintf(dvrTrace.loopBounds,
                "tick,reason,trigger_pc,flr,branch_pc,target_pc,source0,"
                "source1,comparison,has_bound,matched,fallback,bound,"
                "increment,remaining,lanes\n");
    }
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

bool
CPU::DVRInstructionFetchPort::recvTimingResp(PacketPtr pkt)
{
    cpu->completeDVRInstructionFetch(pkt);
    return true;
}

bool
CPU::isDVRInstructionFetch(PacketPtr pkt) const
{
    return pkt && dynamic_cast<DVRInstructionFetchState *>(
        pkt->senderState) != nullptr;
}

void
CPU::DVRInstructionFetchPort::recvReqRetry()
{
    cpu->retryDVRInstructionFetch();
}

Port &
CPU::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "dvr_icache_port")
        return dvrInstructionPort;
    return BaseCPU::getPort(if_name, idx);
}

bool
CPU::requestDVRInstructionFetch(ThreadID tid, Addr pc,
                                const StaticInstPtr &captured)
{
    if (!dvrInstructionPort.isConnected())
        return false;

    // A helper lane may revisit the same PC while its cache request is in
    // flight.  Keep one request per PC and let the lane retry after fill.
    if (dvrInstructionFetchPending.count(pc) != 0)
        return true;
    if (dvrInstructionRetryPkt != nullptr)
        return true;
    if (tid >= threadContexts.size() || !threadContexts[tid])
        return false;

    const unsigned line_bytes = cacheLineSize();
    const unsigned line_offset = pc & (line_bytes - 1);
    const unsigned request_bytes = std::min<unsigned>(4,
        line_bytes - line_offset);
    RequestPtr req = std::make_shared<Request>(
        pc, request_bytes, Request::INST_FETCH, instRequestorId(), pc,
        thread[tid]->contextId());
    req->taskId(taskId());
    const Fault fault = mmu->translateFunctional(
        req, thread[tid]->getTC(), BaseMMU::Execute);
    if (fault != NoFault) {
        ++cpuStats.dvrHelperInstructionFetchFaults;
        ++cpuStats.dvrHelperDecodeFallbacks;
        dvrHelperThread.decodedUopCache[pc] = captured;
        return false;
    }

    PacketPtr pkt = new Packet(req, MemCmd::ReadReq);
    pkt->dataDynamic(new uint8_t[request_bytes]);
    auto *state = new DVRInstructionFetchState;
    state->pc = pc;
    state->tid = tid;
    state->captured = captured;
    pkt->senderState = state;
    dvrInstructionFetchPending.insert(pc);
    ++cpuStats.dvrHelperInstructionTimingRequests;
    if (!dvrInstructionPort.sendTimingReq(pkt)) {
        dvrInstructionRetryPkt = pkt;
    }
    return true;
}

void
CPU::completeDVRInstructionFetch(PacketPtr pkt)
{
    auto *state = dynamic_cast<DVRInstructionFetchState *>(
        pkt->senderState);
    if (!state) {
        delete pkt;
        return;
    }

    StaticInstPtr decoded;
    if (pkt->hasData() && pkt->getSize() >= 2 &&
        state->tid < threadContexts.size() && threadContexts[state->tid]) {
        uint32_t raw = 0;
        std::memcpy(&raw, pkt->getConstPtr<uint8_t>(),
                    std::min<unsigned>(pkt->getSize(), sizeof(raw)));
        auto *decoder = dynamic_cast<RiscvISA::Decoder *>(
            threadContexts[state->tid]->getDecoderPtr());
        if (decoder)
            decoded = decoder->decodeRaw(raw, state->pc);
    }
    if (decoded) {
        dvrHelperThread.decodedUopCache[state->pc] = decoded;
        dvrHelperThread.frontendDecoded(state->pc, false, curTick());
        ++cpuStats.dvrHelperInstructionsDecoded;
    } else {
        dvrHelperThread.decodedUopCache[state->pc] = state->captured;
        dvrHelperThread.frontendDecoded(state->pc, true, curTick());
        ++cpuStats.dvrHelperDecodeFallbacks;
    }
    if (state->pc >= 0x11970 && state->pc <= 0x119a0) {
        dvrTraceVector("bfs_uop_fetch_response", curTick(), state->pc,
                       decoded ? 1 : 0,
                       dvrInstructionFetchPending.count(state->pc) ? 1 : 0,
                       0);
    }
    ++cpuStats.dvrHelperInstructionTimingResponses;
    dvrInstructionFetchPending.erase(state->pc);
    pkt->senderState = nullptr;
    delete state;
    delete pkt;
}

void
CPU::retryDVRInstructionFetch()
{
    if (!dvrInstructionRetryPkt)
        return;
    ++cpuStats.dvrHelperInstructionTimingRetries;
    if (dvrInstructionPort.sendTimingReq(dvrInstructionRetryPkt))
        dvrInstructionRetryPkt = nullptr;
}

CPU::~CPU()
{
    if (dvrTrace.workload) std::fclose(dvrTrace.workload);
    if (dvrTrace.dependency) std::fclose(dvrTrace.dependency);
    if (dvrTrace.vectorization) std::fclose(dvrTrace.vectorization);
    if (dvrTrace.loopBounds) std::fclose(dvrTrace.loopBounds);
    if (dvrTrace.events) std::fclose(dvrTrace.events);
}

void
CPU::dvrTraceWorkload(const char *kind, Tick tick, InstSeqNum seq,
                      Addr pc, Addr address)
{
    if (!dvrTrace.enabled()) return;
    if (dvrTrace.narrow) return;
    std::fprintf(dvrTrace.workload, "%llu,%llu,%s,%#llx,%#llx\n",
        static_cast<unsigned long long>(tick),
        static_cast<unsigned long long>(seq), kind,
        static_cast<unsigned long long>(pc),
        static_cast<unsigned long long>(address));
    std::fprintf(dvrTrace.events,
        "{\"category\":\"workload\",\"tick\":%llu,\"seq\":%llu,\"kind\":\"%s\",\"pc\":%llu,\"address\":%llu}\n",
        static_cast<unsigned long long>(tick),
        static_cast<unsigned long long>(seq), kind,
        static_cast<unsigned long long>(pc),
        static_cast<unsigned long long>(address));
}

void
CPU::dvrTraceDependency(const char *kind, Tick tick, Addr trigger_pc,
                        Addr pc, Addr address, uint32_t taint, int lanes)
{
    if (!dvrTrace.enabled()) return;
    if (dvrTrace.narrow) {
        if (!dvrTrace.branchCensus && trigger_pc != 0x11970)
            return;
        if (dvrTrace.branchCensus) {
            const bool keep =
                std::strcmp(kind, "tainted_branch_record") == 0 ||
                std::strcmp(kind, "tainted") == 0 ||
                std::strcmp(kind, "nested_tainted") == 0 ||
                std::strcmp(kind, "nested_flr") == 0 ||
                std::strcmp(kind, "replay_target") == 0 ||
                std::strcmp(kind, "alternate_replay_target") == 0 ||
                std::strcmp(kind, "nested_replay_load") == 0 ||
                std::strcmp(kind, "nested_replay_dedup") == 0 ||
                std::strcmp(kind, "dependent_value") == 0 ||
                std::strcmp(kind, "flr") == 0 ||
                std::strcmp(kind, "nested_flr") == 0 ||
                std::strcmp(kind, "simt_taken_dependent_load") == 0 ||
                std::strcmp(kind, "simt_not_taken_dependent_load") == 0 ||
                std::strcmp(kind, "alternate_path_lookup_hit") == 0 ||
                std::strcmp(kind, "alternate_path_lookup_miss") == 0 ||
                std::strcmp(kind, "replay_branch_evaluated_trigger") == 0 ||
                std::strcmp(kind, "replay_branch_mixed_trigger") == 0;
            if (!keep)
                return;
        }
    }
    std::fprintf(dvrTrace.dependency, "%llu,%s,%#llx,%#llx,%#llx,%#x,%d\n",
        static_cast<unsigned long long>(tick), kind,
        static_cast<unsigned long long>(trigger_pc),
        static_cast<unsigned long long>(pc),
        static_cast<unsigned long long>(address), taint, lanes);
    std::fprintf(dvrTrace.events,
        "{\"category\":\"dependency\",\"tick\":%llu,\"kind\":\"%s\",\"trigger_pc\":%llu,\"pc\":%llu,\"address\":%llu,\"taint\":%u,\"lanes\":%d}\n",
        static_cast<unsigned long long>(tick), kind,
        static_cast<unsigned long long>(trigger_pc),
        static_cast<unsigned long long>(pc),
        static_cast<unsigned long long>(address), taint, lanes);
}

void
CPU::dvrTraceVector(const char *kind, Tick tick, Addr pc, Addr address,
                    int lanes, int invocation)
{
    if (!dvrTrace.enabled()) return;
    if (dvrTrace.narrow) {
        if (dvrTrace.branchCensus) {
            const bool branch = kind &&
                std::strncmp(kind, "replay_branch_", 14) == 0;
            const bool reconvergence = kind &&
                std::strncmp(kind, "reconvergence_", 14) == 0;
            const bool alternate = kind &&
                std::strncmp(kind, "alternate_", 10) == 0;
            const bool replay = kind &&
                std::strncmp(kind, "replay_", 7) == 0;
            const bool vir = kind &&
                std::strcmp(kind, "vir_issue_group") == 0;
            if (!branch && !reconvergence && !alternate && !replay && !vir)
                return;
        }
        const bool scheduler = kind &&
            std::strncmp(kind, "bfs_replay_", 11) == 0;
        const bool bfs_diag = kind &&
            std::strncmp(kind, "bfs_", 4) == 0;
        const bool branch = kind &&
            std::strncmp(kind, "replay_branch_", 14) == 0;
        const bool group = kind &&
            std::strcmp(kind, "vir_issue_group") == 0 &&
            pc >= 0x11970 && pc <= 0x119a0;
        if (!dvrTrace.branchCensus &&
            !scheduler && !branch && !group && !bfs_diag)
            return;
    }
    std::fprintf(dvrTrace.vectorization, "%llu,%s,%#llx,%#llx,%d,%d\n",
        static_cast<unsigned long long>(tick), kind,
        static_cast<unsigned long long>(pc),
        static_cast<unsigned long long>(address), lanes, invocation);
    std::fprintf(dvrTrace.events,
        "{\"category\":\"vectorization\",\"tick\":%llu,\"kind\":\"%s\",\"pc\":%llu,\"address\":%llu,\"lanes\":%d,\"invocation\":%d}\n",
        static_cast<unsigned long long>(tick), kind,
        static_cast<unsigned long long>(pc),
        static_cast<unsigned long long>(address), lanes, invocation);
}

void
CPU::dvrTraceLoopBound(
    Tick tick, const char *reason, Addr trigger_pc, Addr final_load_pc,
    Addr branch_pc, Addr target_pc, int source0, int source1,
    uint8_t comparison, bool has_bound,
    const DVRLoopBoundDetector::Inference *inference)
{
    if (!dvrTrace.enabled() || !dvrTrace.loopBounds)
        return;
    if (dvrTrace.narrow &&
        (dvrTrace.branchCensus || trigger_pc != 0x11970))
        return;
    const bool matched = inference && inference->matched;
    const bool fallback = inference && inference->fallback;
    const uint64_t bound = matched ? inference->bound : 0;
    const int64_t increment = matched ? inference->increment : 0;
    const uint64_t remaining = matched ? inference->remaining : 0;
    const unsigned lanes = inference ? inference->lanes : 0;
    std::fprintf(dvrTrace.loopBounds,
        "%llu,%s,%#llx,%#llx,%#llx,%#llx,%d,%d,%u,%d,%d,%d,%llu,%lld,%llu,%u\n",
        static_cast<unsigned long long>(tick), reason ? reason : "unknown",
        static_cast<unsigned long long>(trigger_pc),
        static_cast<unsigned long long>(final_load_pc),
        static_cast<unsigned long long>(branch_pc),
        static_cast<unsigned long long>(target_pc), source0, source1,
        comparison, has_bound ? 1 : 0, matched ? 1 : 0, fallback ? 1 : 0,
        static_cast<unsigned long long>(bound),
        static_cast<long long>(increment),
        static_cast<unsigned long long>(remaining), lanes);
}

void
CPU::dvrTraceSIMTBranch(
    Tick tick, Addr branch_pc, Addr reconvergence_pc,
    const std::array<uint64_t, 2> &active_mask,
    const std::array<uint64_t, 2> &taken_mask,
    const std::array<uint64_t, 2> &not_taken_mask,
    unsigned groups)
{
    if (!dvrTrace.enabled())
        return;
    if (dvrTrace.narrow && !dvrTrace.branchCensus && branch_pc != 0x1197e)
        return;
    // Keep the full 128-bit masks in the event stream.  The CSV vector trace
    // intentionally remains backward compatible with older post-processing.
    std::fprintf(dvrTrace.events,
        "{\"category\":\"simt_branch\",\"tick\":%llu,"
        "\"branch_pc\":%llu,\"reconvergence_pc\":%llu,"
        "\"active_mask_lo\":%llu,\"active_mask_hi\":%llu,"
        "\"taken_mask_lo\":%llu,\"taken_mask_hi\":%llu,"
        "\"not_taken_mask_lo\":%llu,\"not_taken_mask_hi\":%llu,"
        "\"groups\":%u}\n",
        static_cast<unsigned long long>(tick),
        static_cast<unsigned long long>(branch_pc),
        static_cast<unsigned long long>(reconvergence_pc),
        static_cast<unsigned long long>(active_mask[0]),
        static_cast<unsigned long long>(active_mask[1]),
        static_cast<unsigned long long>(taken_mask[0]),
        static_cast<unsigned long long>(taken_mask[1]),
        static_cast<unsigned long long>(not_taken_mask[0]),
        static_cast<unsigned long long>(not_taken_mask[1]), groups);
}

void
CPU::oracleOnCommittedLoad(Addr address, ThreadID tid)
{
    if (!oraclePrefetch || oracleLoadTrace.empty())
        return;
    const uint64_t current = oracleLoadIndex++;
    const uint64_t end = std::min<uint64_t>(oracleLoadTrace.size(),
        current + 1 + oracleLookahead);
    for (uint64_t index = current + 1; index < end; ++index) {
        const Addr future = oracleLoadTrace[index];
        if (future == 0 || dvrQueuedPrefetchAddresses.count(future) != 0 ||
            dvrOutstandingPrefetchAddresses.count(future) != 0)
            continue;
        if (dvrPrefetchQueue.size() >= DvrMaxQueuedPrefetches)
            break;
        DVRPrefetchAddress prefetch;
        prefetch.address = future;
        prefetch.pc = 0;
        prefetch.tid = tid;
        prefetch.source = false;
        prefetch.oracle = true;
        dvrPrefetchQueue.push_back(prefetch);
        dvrQueuedPrefetchAddresses.insert(future);
        ++cpuStats.oraclePrefetchesGenerated;
    }
    updateDVRPrefetchQueuePeak();
    const Addr line = dvrPrefetchLine(address);
    auto completed = oracleCompletedLines.find(line);
    if (completed != oracleCompletedLines.end()) {
        ++cpuStats.oracleDemandCovered;
        oracleCompletedLines.erase(completed);
    }
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
      ADD_STAT(dvrDiscoveryInnermostSwitches,
               statistics::units::Count::get(),
               "Discovery generations switched to a repeated inner stride"),
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
      ADD_STAT(dvrNestedFlattenInvariantChecks,
               statistics::units::Count::get(),
               "Nested flatten batches checked against the per-batch lane cap"),
      ADD_STAT(dvrNestedFlattenInvariantFailures,
               statistics::units::Count::get(),
               "Nested flatten batches violating the per-batch lane invariant"),
      ADD_STAT(dvrNestedFlattenExpectedLanes,
               statistics::units::Count::get(),
               "Expected nested lanes from per-batch min(128, inner lanes)"),
      ADD_STAT(dvrNestedVariableLaneBatches,
               statistics::units::Count::get(),
               "Nested batches containing independently different bounds"),
      ADD_STAT(dvrNDMAttempts, statistics::units::Count::get(),
               "Completed short inner loops entering NDM control"),
      ADD_STAT(dvrNDMThresholdBypasses, statistics::units::Count::get(),
               "Nested discoveries bypassing NDM because inner lanes reach the threshold"),
      ADD_STAT(dvrNestedOrdinaryDVRLaunches, statistics::units::Count::get(),
               "Ordinary DVR launches selected for nested mode when NDM is unnecessary"),
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
      ADD_STAT(dvrHelperUopsBecameReady, statistics::units::Count::get(),
               "Captured helper uops that became ready"),
      ADD_STAT(dvrHelperUopsIssued, statistics::units::Count::get(),
               "Captured helper uops successfully issued"),
      ADD_STAT(dvrHelperDynUopsDecoded, statistics::units::Count::get(),
               "Helper-owned DVRDynUops admitted to the VIR"),
      ADD_STAT(dvrHelperDynUopsIssued, statistics::units::Count::get(),
               "Helper-owned DVRDynUops issued to a shared FU"),
      ADD_STAT(dvrHelperDynUopsCompleted, statistics::units::Count::get(),
               "Helper-owned DVRDynUops completed"),
      ADD_STAT(dvrHelperDecodedCacheHits, statistics::units::Count::get(),
               "Helper decoded-uop cache hits"),
      ADD_STAT(dvrHelperDecodedCacheMisses, statistics::units::Count::get(),
               "Helper decoded-uop cache misses"),
      ADD_STAT(dvrHelperInstructionFetches, statistics::units::Count::get(),
               "Helper instruction fetch attempts from the live address space"),
      ADD_STAT(dvrHelperInstructionFetchFaults,
               statistics::units::Count::get(),
               "Helper instruction fetches that could not be translated"),
      ADD_STAT(dvrHelperInstructionsDecoded, statistics::units::Count::get(),
               "Helper instructions decoded from fetched bytes"),
      ADD_STAT(dvrHelperDecodeFallbacks, statistics::units::Count::get(),
               "Helper decode fallbacks to captured metadata"),
      ADD_STAT(dvrHelperInstructionTimingRequests,
               statistics::units::Count::get(),
               "Helper instruction-cache timing requests"),
      ADD_STAT(dvrHelperInstructionTimingResponses,
               statistics::units::Count::get(),
               "Helper instruction-cache timing responses"),
      ADD_STAT(dvrHelperInstructionTimingRetries,
               statistics::units::Count::get(),
               "Helper instruction-cache request retries"),
      ADD_STAT(dvrHelperFetchBlockedByMain, statistics::units::Cycle::get(),
               "Helper fetch cycles denied by main-thread priority"),
      ADD_STAT(dvrHelperDecodeBlockedByMain, statistics::units::Cycle::get(),
               "Helper decode cycles denied by main-thread priority"),
      ADD_STAT(dvrHelperVIRCapacityStalls, statistics::units::Count::get(),
               "Helper uops blocked by the finite VIR capacity"),
      ADD_STAT(dvrHelperVRATPrograms, statistics::units::Count::get(),
               "Replay programs initialized with a private helper VRAT"),
      ADD_STAT(dvrHelperSharedVRATPrograms, statistics::units::Count::get(),
               "Replay programs mapped into the shared O3 physical register file"),
      ADD_STAT(dvrHelperVRATWrites, statistics::units::Count::get(),
               "Lane writes into private helper vector registers"),
      ADD_STAT(dvrHelperReadyUopCycles, statistics::units::Count::get(),
               "Sum of helper ready-queue occupancy across sampled cycles"),
      ADD_STAT(dvrHelperReadyOccupancySamples,
               statistics::units::Count::get(),
               "Cycles sampled for helper ready-queue occupancy"),
      ADD_STAT(dvrHelperReadyOccupancy, statistics::units::Rate<
                   statistics::units::Count,
                   statistics::units::Count>::get(),
               "Average helper ready-queue occupancy"),
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
      ADD_STAT(dvrHelperIssueQueueStalls, statistics::units::Count::get(),
               "Helper issue-queue head stalls"),
      ADD_STAT(dvrHelperScoreboardWaitCycles, statistics::units::Cycle::get(),
               "Cycles helper issue waited for a virtual register"),
      ADD_STAT(dvrVectorALUChunkIssues, statistics::units::Count::get(),
               "DVR vector ALU chunks issued"),
      ADD_STAT(dvrVectorAddChunkIssues, statistics::units::Count::get(),
               "DVR vector add-class chunks issued through SimdAddOp"),
      ADD_STAT(dvrVectorShiftChunkIssues, statistics::units::Count::get(),
               "DVR vector shift chunks issued"),
      ADD_STAT(dvrVectorMultiplyChunkIssues, statistics::units::Count::get(),
               "DVR vector multiply chunks issued"),
      ADD_STAT(dvrVectorChunkRequests, statistics::units::Count::get(),
               "DVR vector chunks requesting a shared FU"),
      ADD_STAT(dvrVectorizerSourceLanes, statistics::units::Count::get(),
               "Source lanes materialized by the DVR vectorizer"),
      ADD_STAT(dvrVectorizerDependentLanes, statistics::units::Count::get(),
               "Dependent lanes materialized by the DVR vectorizer"),
      ADD_STAT(dvrVIRActiveMaskChecks, statistics::units::Count::get(),
               "VIR issue groups checked against their active lane mask"),
      ADD_STAT(dvrVIRActiveMaskFailures, statistics::units::Count::get(),
               "VIR issue groups whose active mask disagreed with the lane group"),
      ADD_STAT(dvrVectorFUConflictCycles, statistics::units::Cycle::get(),
               "Cycles with a constrained DVR vector FU conflict"),
      ADD_STAT(dvrVectorLatencyCycles, statistics::units::Cycle::get(),
               "Native FU latency cycles carried by DVR vector chunks"),
      ADD_STAT(dvrVectorComputeWaitCycles, statistics::units::Cycle::get(),
               "Cycles source replay waited for vector compute completion"),
      ADD_STAT(dvrVectorActiveLanes, statistics::units::Count::get(),
               "Sum of active lanes presented to DVR vector chunks"),
      ADD_STAT(dvrVectorUtilization, statistics::units::Ratio::get(),
               "Granted DVR vector chunks per vector chunk request"),
      ADD_STAT(dvrHelperALUOps, statistics::units::Count::get(),
               "Captured helper ALU/control uops profiled"),
      ADD_STAT(dvrHelperShiftOps, statistics::units::Count::get(),
               "Captured helper shift uops profiled"),
      ADD_STAT(dvrHelperMultiplyOps, statistics::units::Count::get(),
               "Captured helper multiply uops profiled"),
      ADD_STAT(dvrHelperLSUOps, statistics::units::Count::get(),
               "Captured helper load/store uops profiled"),
      ADD_STAT(dvrHelperLoadEntriesAllocated,
               statistics::units::Count::get(),
               "Helper load queue entries allocated"),
      ADD_STAT(dvrHelperLoadEntriesCompleted,
               statistics::units::Count::get(),
               "Helper load queue entries completed"),
      ADD_STAT(dvrHelperLoadEntryWritebacks,
               statistics::units::Count::get(),
               "Helper load responses that woke a helper context"),
      ADD_STAT(dvrHelperLoadEntryFaults, statistics::units::Count::get(),
               "Helper load entries terminated by translation fault"),
      ADD_STAT(dvrHelperLoadEntryRetries, statistics::units::Count::get(),
               "Helper load entries retried by data-port backpressure"),
      ADD_STAT(dvrHelperLoadEntryDropped, statistics::units::Count::get(),
               "Helper load entries dropped before a memory request"),
      ADD_STAT(dvrHelperLoadEntryPending, statistics::units::Count::get(),
               "Helper load entries still waiting at simulation end"),
      ADD_STAT(dvrHelperLoadEntryWakeups, statistics::units::Count::get(),
               "Helper replay wakeups from load responses"),
      ADD_STAT(dvrHelperLoadEntryPeak, statistics::units::Count::get(),
               "Peak helper load queue entries in flight"),
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
      ADD_STAT(dvrLoopBackBranchesSeen, statistics::units::Count::get(),
               "Backward conditional branches seen in DVR discoveries"),
      ADD_STAT(dvrLoopBackBranchesWithFLR, statistics::units::Count::get(),
               "Backward conditional branches in discoveries with an FLR"),
      ADD_STAT(dvrLoopBackBranchesWithoutFLR, statistics::units::Count::get(),
               "Backward conditional branches in discoveries without an FLR"),
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
               "Captured trigger/replay metadata uops retained by the DVR recorder"),
      ADD_STAT(dvrRecorderOverflows, statistics::units::Count::get(),
               "Discoveries whose captured replay metadata exceeded the recorder capacity"),
      ADD_STAT(dvrVectorProgramsBuilt, statistics::units::Count::get(),
               "Recorded slices materialized as DVR vector programs"),
      ADD_STAT(dvrVRATAllocations, statistics::units::Count::get(),
               "16-copy (128 scalar-lane) physical mappings allocated by the DVR VRAT"),
      ADD_STAT(dvrVRATControlDivergenceAllocations,
               statistics::units::Count::get(),
               "VRAT vector bundles allocated solely because control-flow diverged"),
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
      ADD_STAT(dvrSIMTBranchGroups, statistics::units::Count::get(),
               "Distinct next-PC groups formed by SIMT data-dependent branches"),
      ADD_STAT(dvrSIMTTakenLanes, statistics::units::Count::get(),
               "Lane outcomes selecting the taken branch path"),
      ADD_STAT(dvrSIMTNotTakenLanes, statistics::units::Count::get(),
               "Lane outcomes selecting the not-taken branch path"),
      ADD_STAT(dvrSIMTMixedBranches, statistics::units::Count::get(),
               "Data-dependent branches with both taken and not-taken lanes"),
      ADD_STAT(dvrSIMTReconvergencePushes, statistics::units::Count::get(),
               "SIMT reconvergence frames pushed for deferred paths"),
      ADD_STAT(dvrSIMTReconvergencePops, statistics::units::Count::get(),
               "SIMT reconvergence frames popped at the join PC"),
      ADD_STAT(dvrSIMTTakenDependentLoads, statistics::units::Count::get(),
               "Dependent loads executed on taken SIMT paths"),
      ADD_STAT(dvrSIMTNotTakenDependentLoads, statistics::units::Count::get(),
               "Dependent loads executed on not-taken SIMT paths"),
      ADD_STAT(dvrReplayContinuePastFLRTemplates,
               statistics::units::Count::get(),
               "Replay templates that continue past FLR to the loop boundary"),
      ADD_STAT(dvrReplayContinuePastFLRLanes,
               statistics::units::Count::get(),
               "Replay lanes admitted to templates that continue past FLR"),
      ADD_STAT(dvrSIMTTakenPathTerminations,
               statistics::units::Count::get(),
               "Taken-path lanes terminated during helper replay"),
      ADD_STAT(dvrSIMTNotTakenPathTerminations,
               statistics::units::Count::get(),
               "Not-taken-path lanes terminated during helper replay"),
      ADD_STAT(dvrSIMTFLRTerminations, statistics::units::Count::get(),
               "Helper lanes terminated at the final dependent load (FLR)"),
      ADD_STAT(dvrSIMTStridePCTerminations, statistics::units::Count::get(),
               "Divergent helper lanes terminated at the next stride PC"),
      ADD_STAT(dvrSIMTTimeoutTerminations, statistics::units::Count::get(),
               "Helper lanes terminated at the 200-uop timeout"),
      ADD_STAT(dvrSIMTStackOverflowDroppedLanes,
               statistics::units::Count::get(),
               "Deferred lanes dropped when the SIMT stack was full"),
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
      ADD_STAT(dvrDebugReached14598, statistics::units::Count::get(),
               "BFS debug generations reaching dependent FLR PC 0x14598"),
      ADD_STAT(dvrDebugContinuedPastFLR, statistics::units::Count::get(),
               "BFS debug lanes continuing past FLR PC 0x14598"),
      ADD_STAT(dvrDebugExecuted1459cConditional,
               statistics::units::Count::get(),
               "BFS debug executions of conditional predicate PC 0x1459c"),
      ADD_STAT(dvrDebug1459cMixedLaneResults,
               statistics::units::Count::get(),
               "BFS debug mixed taken/not-taken results at PC 0x1459c"),
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
      ADD_STAT(dvrOutstandingPrefetchLineSamples,
               statistics::units::Count::get(),
               "Cycles sampled for DVR outstanding prefetched lines"),
      ADD_STAT(dvrOutstandingPrefetchLineSum,
               statistics::units::Count::get(),
               "Sum of DVR outstanding prefetched lines across samples"),
      ADD_STAT(dvrOutstandingPrefetchLinePeak,
               statistics::units::Count::get(),
               "Peak DVR outstanding prefetched cache lines"),
      ADD_STAT(dvrReplaySupportedUops, statistics::units::Count::get(),
               "Supported uops in scalar trigger-to-FLR replay prefixes"),
      ADD_STAT(dvrReplayUnsupportedUops, statistics::units::Count::get(),
               "Unsupported uops in scalar trigger-to-FLR replay prefixes"),
      ADD_STAT(dvrReplayUnstableInputs, statistics::units::Count::get(),
               "Replay templates rejected due to changing external inputs"),
      ADD_STAT(dvrReplayAttempts, statistics::units::Count::get(),
               "Source responses offered to the scalar DVR uop replay path"),
      ADD_STAT(dvrReplayTargetsGenerated, statistics::units::Count::get(),
               "Dependent targets generated by real recorded-uop replay"),
      ADD_STAT(dvrReplayFallbacks, statistics::units::Count::get(),
               "Source responses falling back from uop replay to affine paths"),
      ADD_STAT(dvrAlternatePathLookups, statistics::units::Count::get(),
               "Alternate-path cache lookups for missing recorder branches"),
      ADD_STAT(dvrAlternatePathHits, statistics::units::Count::get(),
               "Alternate-path cache entries found"),
      ADD_STAT(dvrAlternatePathCompleteHits, statistics::units::Count::get(),
               "Complete alternate-path entries admitted for replay"),
      ADD_STAT(dvrAlternatePathCandidates, statistics::units::Count::get(),
               "Tainted conditional branches considered for alternate paths"),
      ADD_STAT(dvrAlternatePathBackwardFiltered,
               statistics::units::Count::get(),
               "Backward loop branches filtered from alternate paths"),
      ADD_STAT(dvrAlternatePathTargetPresent, statistics::units::Count::get(),
               "Alternate targets already present in the recorder"),
      ADD_STAT(dvrAlternatePathCompleteRecords,
               statistics::units::Count::get(),
               "Complete alternate-path records produced"),
      ADD_STAT(dvrAlternatePathIncompleteRecords,
               statistics::units::Count::get(),
               "Incomplete alternate-path records rejected before caching"),
      ADD_STAT(dvrAlternatePathInsertRejects, statistics::units::Count::get(),
               "Complete alternate paths rejected by recorder insertion"),
      ADD_STAT(dvrAlternatePathLiveInRejects, statistics::units::Count::get(),
               "Alternate paths rejected because live-ins are unavailable"),
      ADD_STAT(dvrAlternatePathIncompleteRejects,
               statistics::units::Count::get(),
               "Incomplete alternate-path entries rejected"),
      ADD_STAT(dvrAlternatePathUnsupportedRejects,
               statistics::units::Count::get(),
               "Alternate paths rejected due to unsupported semantics"),
      ADD_STAT(dvrAlternatePathControlRejects,
               statistics::units::Count::get(),
               "Alternate paths rejected due to unresolved control flow"),
      ADD_STAT(dvrAlternatePathDirectJumps, statistics::units::Count::get(),
               "Direct jumps retained as alternate-path control metadata"),
      ADD_STAT(dvrAlternatePathSafeSkips, statistics::units::Count::get(),
               "Unsupported non-stateful uops omitted from alternate paths"),
      ADD_STAT(dvrAlternatePathUopsReplayed, statistics::units::Count::get(),
               "Uops executed from alternate-path cache entries"),
      ADD_STAT(dvrAlternatePathDependentTargets,
               statistics::units::Count::get(),
               "Dependent targets generated while executing alternate paths"),
      ADD_STAT(dvrAlternatePathDemandCovered, statistics::units::Count::get(),
               "Demand loads covered by alternate-path dependent targets"),
      ADD_STAT(dvrReconvergenceResumeSuccesses,
               statistics::units::Count::get(),
               "Alternate paths that resumed at their reconvergence PC"),
      ADD_STAT(dvrQualityIssuedBytes, statistics::units::Byte::get(),
               "Bytes in DVR helper requests accepted by L1D"),
      ADD_STAT(dvrQualityCompletedBytes, statistics::units::Byte::get(),
               "Bytes in completed DVR helper responses"),
      ADD_STAT(dvrQualityDemandAddressesObserved,
               statistics::units::Count::get(),
               "Committed demand-load addresses observed; not cache hits"),
      ADD_STAT(dvrDependentDemandLoads, statistics::units::Count::get(),
               "Main-thread loads at trained DVR dependent-load PCs"),
      ADD_STAT(dvrDependentDemandCovered, statistics::units::Count::get(),
               "Dependent demands preceded by a completed DVR target"),
      ADD_STAT(dvrDependentDemandLate, statistics::units::Count::get(),
               "Dependent demands observed while a DVR target was outstanding"),
      ADD_STAT(dvrHelperLaunchAttempts, statistics::units::Count::get(),
               "Discovery completions attempting a source-helper launch"),
      ADD_STAT(dvrHelperLaunchAdmitted, statistics::units::Count::get(),
               "Source-helper launches admitted with bounded queue space"),
      ADD_STAT(dvrHelperLaunchCapacityDrops, statistics::units::Count::get(),
               "Source-helper launches rejected because the queue was full"),
      ADD_STAT(dvrHelperLaunchZeroLanes, statistics::units::Count::get(),
               "Source-helper launches with no inferred lane"),
      ADD_STAT(dvrPrefetchesDeduplicated, statistics::units::Count::get(),
               "DVR candidates suppressed because an equivalent request was already active"),
      ADD_STAT(oraclePrefetchesGenerated, statistics::units::Count::get(),
               "Future load addresses generated by the trace oracle"),
      ADD_STAT(oraclePrefetchesIssued, statistics::units::Count::get(),
               "Oracle requests accepted by L1D"),
      ADD_STAT(oraclePrefetchesCompleted, statistics::units::Count::get(),
               "Oracle request responses completed"),
      ADD_STAT(oracleDemandCovered, statistics::units::Count::get(),
               "Committed loads preceded by a completed oracle request")
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

    dvrHelperReadyOccupancy
        .precision(6);
    dvrHelperReadyOccupancy.flags(statistics::nozero | statistics::nonan);
    dvrHelperReadyOccupancy =
        dvrHelperReadyUopCycles / dvrHelperReadyOccupancySamples;

    dvrVectorUtilization.flags(statistics::nozero | statistics::nonan);
    dvrVectorUtilization =
        (dvrVectorALUChunkIssues + dvrVectorShiftChunkIssues +
         dvrVectorMultiplyChunkIssues) / dvrVectorChunkRequests;

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
    dvrHelperLSUIssuesThisCycle = 0;
    dvrHelperComputeIssuesThisCycle = 0;
    dvrMainIssuesThisCycle = 0;
    dvrMainALUIssuesThisCycle = 0;
    dvrMainLSUIssuesThisCycle = 0;
    if (dvrVectorChunkModel)
        cpuStats.dvrHelperDynUopsCompleted +=
            dvrHelperThread.retireCompletedVIR(curTick());

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
    const unsigned ready_uops_before = dvrHelperThread.readyUops;
    // The O3 stages have already run.  Their issue count is used as the
    // conservative demand proxy for the shared front-end budget: main-thread
    // work gets first claim, and only the residual fetch/decode width is
    // offered to the independent helper.
    const unsigned main_fetch_claim = std::min(
        dvrFetchWidth, dvrMainIssuesThisCycle);
    const unsigned main_decode_claim = std::min(
        dvrDecodeWidth, dvrMainIssuesThisCycle);
    const unsigned helper_fetch_width = dvrFetchWidth - main_fetch_claim;
    const unsigned helper_decode_width = dvrDecodeWidth - main_decode_claim;
    if (dvrHelperThread.fetchRemaining != 0 && helper_fetch_width == 0)
        ++cpuStats.dvrHelperFetchBlockedByMain;
    if (dvrHelperThread.decodeRemaining != 0 && helper_decode_width == 0)
        ++cpuStats.dvrHelperDecodeBlockedByMain;
    const unsigned helper_frontend_work =
        dvrHelperThread.advanceFrontend(helper_fetch_width,
                                        helper_decode_width);
    // Front-end accounting above is now backed by a real helper fetch.  The
    // helper owns the PC/window cursor; each fetched entry reads the current
    // process bytes and is decoded by the ISA decoder before it can reach the
    // VIR.  The captured Uop remains the dependency metadata and validation
    // record, never the instruction-byte source.
    for (unsigned fetched = 0;
         fetched < dvrHelperThread.lastFetched &&
         dvrHelperThread.frontendReplay &&
         dvrHelperThread.frontendReplay->count != 0; ++fetched) {
        auto &program = dvrHelperThread.frontendReplay;
        if (dvrHelperThread.frontendUopIndex >= program->count)
            dvrHelperThread.frontendUopIndex = 0;
        const auto &frontend_uop =
            program->uops[dvrHelperThread.frontendUopIndex++];
        bool fetch_fault = false;
        bool cache_hit = false;
        dvrHelperThread.frontendFetched(frontend_uop.pc, curTick());
        const StaticInstPtr frontend_decoded = fetchDecodeDVRUop(
            dvrHelperThread.frontendTid, frontend_uop.pc,
            frontend_uop.staticInst, fetch_fault, cache_hit);
        if (frontend_decoded)
            dvrHelperThread.frontendDecoded(
                frontend_uop.pc, fetch_fault, curTick());
        dvrHelperThread.helperPC = frontend_uop.pc;
        // Timing misses are accounted by request/response handlers.  A
        // nullptr here means the helper front-end is waiting on its cache
        // request, not that semantic metadata was used as a fallback.
    }
    const unsigned ready_uops_after = dvrHelperThread.readyUops;
    if (helper_frontend_work != 0) {
        if (dvrHelperThread.fetchRemaining != 0 ||
            dvrHelperThread.decodeRemaining != 0)
            ++cpuStats.dvrHelperFetchCycles;
        if (dvrHelperThread.readyUops != 0)
            ++cpuStats.dvrHelperDecodeCycles;
        if (ready_uops_after >= ready_uops_before)
            cpuStats.dvrHelperUopsBecameReady +=
                ready_uops_after - ready_uops_before;
    }
    if (dvrHelperThread.active()) {
        ++cpuStats.dvrHelperReadyOccupancySamples;
        cpuStats.dvrHelperReadyUopCycles += dvrHelperThread.readyUops;
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

    const uint64_t outstanding_lines = dvrOutstandingPrefetchLines.size();
    ++cpuStats.dvrOutstandingPrefetchLineSamples;
    cpuStats.dvrOutstandingPrefetchLineSum += outstanding_lines;
    if (outstanding_lines > dvrOutstandingPrefetchLinePeakValue) {
        dvrOutstandingPrefetchLinePeakValue = outstanding_lines;
        cpuStats.dvrOutstandingPrefetchLinePeak =
            dvrOutstandingPrefetchLinePeakValue;
    }

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

        if (inst->isLoad() && inst->effAddrValid()) {
            dvrTraceWorkload("load", curTick(), inst->seqNum,
                             inst->pcState().instAddr(), inst->effAddr);
            oracleOnCommittedLoad(inst->effAddr, tid);
        }

        if (enableDVR && !inPRE && !inst->isPRE()) {
            const bool was_discovering = dvrDiscovery.isDiscovering();
            // The second dispatch of the trigger closes Discovery.  Its
            // commit is the delimiter, not part of the trigger-to-FLR body.
            // Keep commit-side VTT/FLR/recorder/LBD updates consistent with
            // the dispatch-side discovery window.
            const bool discovery_body_commit =
                dvrDiscovery.inDiscoveryBody(inst->seqNum);
            if (discovery_body_commit) {
                const bool dispatch_tainted =
                    dvrDispatchTainted.erase(inst->seqNum) != 0;
                const bool dispatch_recorded =
                    dvrDispatchRecorded.erase(inst->seqNum) != 0;
                const bool dispatch_dependent =
                    dvrDispatchDependentLoads.erase(inst->seqNum) != 0;
                if (dispatch_recorded) {
                    dvrInstructionRecorder.record(inst, dispatch_tainted);
                    if (dvrCurrentTriggerPC == 0x1458a &&
                        dvrInstructionRecorder.size() != 0 &&
                        (dvrInstructionRecorder[
                            dvrInstructionRecorder.size() - 1].pc == 0x1459c ||
                         dvrInstructionRecorder[
                            dvrInstructionRecorder.size() - 1].pc == 0x145a4)) {
                        const auto &predicate =
                            dvrInstructionRecorder[
                                dvrInstructionRecorder.size() - 1];
                        const uint32_t flags =
                            (predicate.control ? 1u : 0u) |
                            (predicate.conditional ? 2u : 0u) |
                            (dispatch_tainted ? 4u : 0u);
                        DPRINTF(O3CPU,
                            "DVR predicate record trigger=%#x pc=%#x "
                            "encoding=%#x semantic=%u control=%d "
                            "conditional=%d tainted=%d src0=%d src1=%d "
                            "dst=%d intSources=%#x intDestinations=%#x\n",
                            dvrCurrentTriggerPC, predicate.pc,
                            predicate.encoding,
                            static_cast<unsigned>(predicate.semantic),
                            predicate.control, predicate.conditional,
                            dispatch_tainted, predicate.source0,
                            predicate.source1, predicate.destination,
                            predicate.intSources, predicate.intDestinations);
                        dvrTraceDependency(
                            predicate.pc == 0x145a4 ? "branch_record" :
                                                       "predicate_record", curTick(),
                            dvrCurrentTriggerPC, predicate.pc,
                            predicate.encoding, flags,
                            static_cast<int>(predicate.semantic));
                    }
                    if (dvrInstructionRecorder.size() != 0) {
                        const auto &recorded = dvrInstructionRecorder[
                            dvrInstructionRecorder.size() - 1];
                        if (recorded.control && recorded.conditional &&
                            dispatch_tainted) {
                            const uint32_t flags =
                                (recorded.control ? 1u : 0u) |
                                (recorded.conditional ? 2u : 0u) |
                                (dispatch_tainted ? 4u : 0u);
                            dvrTraceDependency(
                                "tainted_branch_record", curTick(),
                                dvrCurrentTriggerPC, recorded.pc,
                                recorded.encoding, flags,
                                static_cast<int>(recorded.semantic));
                        }
                    }
                    if (dispatch_tainted)
                        dvrTraceDependency("tainted", curTick(),
                            dvrCurrentTriggerPC,
                            inst->pcState().instAddr(),
                            inst->effAddrValid() ? inst->effAddr : 0,
                            dvrTaintTracker.bits());
                }
                if (dispatch_dependent) {
                    // Only a committed dependent load may advance the FLR
                    // that delimits the generated helper program.
                    dvrCommittedFinalLoadPC =
                        inst->pcState().instAddr();
                    dvrLoopBoundDetector.updateFinalLoad(
                        dvrCommittedFinalLoadPC);
                    if (inst->effAddrValid()) {
                        dvrTraceDependency("flr", curTick(),
                            dvrCurrentTriggerPC,
                            inst->pcState().instAddr(), inst->effAddr,
                            dvrTaintTracker.bits());
                        trainDVRAddressRelation(
                            dvrCurrentTriggerPC, inst->pcState().instAddr(),
                            dvrInitiatingLoadValue, inst->effAddr);
                    }
                }
                DVRLoopBoundDetector::RegisterSnapshot branch_regs = {};
                captureDVRRegisterSnapshot(tid, inst, branch_regs);
                const auto loop_observation =
                    dvrLoopBoundDetector.observe(inst, &branch_regs);
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
                uint64_t encoded = 0;
                const size_t encoded_size = inst->staticInst->asBytes(
                    &encoded, sizeof(encoded));
                const Addr fallthrough = inst->pcState().instAddr() +
                    (encoded_size != 0 && (encoded & 0x3) != 0x3 ? 2 : 4);
                const bool captured =
                    dvrNestedDiscoveryMode.observeInnerBranch(
                        inst->pcState().instAddr(),
                        inst->branchTarget()->instAddr(),
                        // The generic PCState interface does not expose the
                        // ISA-specific npc accessor.  RV64C is decoded into
                        // the recorder with a fixed-width fall-through, so
                        // Preserve the actual RVC/RV64 fall-through width.
                        fallthrough,
                        inst->pcState().branching());
                if (captured) {
                    ++cpuStats.dvrNDMBranchInversions;
                    ++cpuStats.dvrNDMIRCaptures;
                    dvrTraceVector("branch_inversion", curTick(),
                        inst->pcState().instAddr(),
                        inst->branchTarget()->instAddr(),
                        dvrNestedDiscoveryMode.innerLaneCount());
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
                    dvrTraceVector("outer_stride", curTick(),
                        inst->pcState().instAddr(), inst->effAddr,
                        dvrNestedDiscoveryMode.innerLaneCount());
                }
            }
            const bool discovery_trigger_commit = was_discovering &&
                inst->seqNum == dvrDiscovery.triggerSeq();
            if (discovery_trigger_commit) {
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
                if (dvrNestedDiscoveryMode.active() &&
                    dvrNestedController.depth() == 0 &&
                    dvrNestedController.startRoot(
                        dvrCurrentTriggerPC,
                        dvrPendingNestedCandidate.sequence).event ==
                        DVRNestedController::Event::Started) {
                    ++cpuStats.dvrNestedRootStarts;
                }
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
                // Keep the child path decodable across an alternate branch;
                // taint remains a separate dependency classification.
                // Preserve the child taint classification in the replay
                // template.  Without this argument nested discovery records
                // every uop as untainted, so the helper cannot distinguish
                // the dependent load from address-materialization loads.
                dvrNestedContext.recorder.record(
                    inst, child_observation.taintedInstruction);
                if (child_observation.taintedInstruction)
                    dvrTraceDependency("nested_tainted", curTick(),
                        dvrNestedContext.triggerPC,
                        inst->pcState().instAddr(),
                        inst->effAddrValid() ? inst->effAddr : 0, 0);
                if (child_observation.dependentLoad) {
                    dvrNestedContext.loopBound.updateFinalLoad(
                        dvrNestedContext.taint.flr());
                    if (inst->effAddrValid()) {
                        dvrTraceDependency("nested_flr", curTick(),
                            dvrNestedContext.triggerPC,
                            inst->pcState().instAddr(), inst->effAddr, 0);
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
                static unsigned dvrTraceDiscoveryStarts = 0;
                if (dvrTraceDiscoveryStarts++ < 32)
                    DPRINTF(O3CPU,
                        "DVR_TRACE discovery_start pc=%#x sn=%llu stride=%lld\n",
                        result.triggerPC, inst->seqNum,
                        static_cast<long long>(result.stride));
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
                dvrCommittedFinalLoadPC = 0;
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
                dvrStrideDetector.endDiscovery();
                const Addr committed_flr = dvrCommittedFinalLoadPC;
                ++cpuStats.dvrDiscoveryCompletions;
                static unsigned dvrTraceDiscoveryCompletions = 0;
                if (dvrTraceDiscoveryCompletions++ < 32)
                    DPRINTF(O3CPU,
                        "DVR_TRACE discovery_complete trigger=%#x flr=%#x\n",
                        result.triggerPC, committed_flr);
                cpuStats.dvrDiscoveredInstructions += result.instructions;
                if (committed_flr != 0)
                    ++cpuStats.dvrDiscoveriesWithFLR;
                for (unsigned index = 0;
                     index < dvrInstructionRecorder.size(); ++index) {
                    const auto &uop = dvrInstructionRecorder[index];
                    if (!uop.conditional || uop.branchTargetPC == 0 ||
                        uop.branchTargetPC >= uop.pc)
                        continue;
                    ++cpuStats.dvrLoopBackBranchesSeen;
                    if (committed_flr != 0)
                        ++cpuStats.dvrLoopBackBranchesWithFLR;
                    else
                        ++cpuStats.dvrLoopBackBranchesWithoutFLR;
                }
                if (dvrLoopBoundDetector.hasBound())
                    ++cpuStats.dvrDiscoveriesWithBounds;
                DVRLoopBoundDetector::RegisterSnapshot finish_regs = {};
                captureDVRRegisterSnapshot(tid, inst, finish_regs);
                const auto inference = dvrLoopBoundDetector.infer(
                    dvrDiscoveryStartRegs, finish_regs, dvrMaxLanes);
                recordDVRDiscoveryGeneration("complete", &inference,
                                              result.triggerPC, result.stride);
                dvrTraceVector("discovery_complete", curTick(),
                    result.triggerPC, committed_flr,
                    inference.matched ? inference.lanes : 0);
                // DVR follows the paper's two-case reconvergence policy:
                // branches at or before FLR use FLR as the termination/join
                // point, while a branch strictly between FLR and the loop
                // back-edge (LCR) uses the loop boundary.  The latter case
                // lets divergent lanes continue to the next stride PC
                // instead of waiting for an already-consumed FLR.
                dvrInstructionRecorder.setReconvergencePC(
                    committed_flr, dvrLoopBoundDetector.branchPC());
                // Publish this complete dynamic path before looking up its
                // opposite direction.  A later discovery can then splice
                // the cached suffix into the same reconvergence point.
                // A discovery without an FLR has no prefetch-producing
                // termination point.  Do not publish its loop/control path
                // as an incomplete alternate entry: it would poison the
                // cache and turn later lookups into guaranteed rejects.
                bool alternate_complete_hit = false;
                if (committed_flr != 0) {
                    // Nested conditionals may only become composable after
                    // a suffix for an inner branch has been inserted into
                    // the alternate-path cache.  Reach a small fixed point
                    // before augmenting the recorder; a single pass leaves
                    // the outer BFS branch permanently incomplete.
                    for (unsigned pass = 0; pass < 3; ++pass)
                        recordDVRAlternatePaths(dvrInstructionRecorder,
                                                inst->contextId());
                    alternate_complete_hit = augmentDVRAlternatePaths(
                        dvrInstructionRecorder, inst->contextId(),
                        finish_regs);
                }
                for (unsigned index = 0;
                     index < dvrInstructionRecorder.size(); ++index) {
                    const auto &uop = dvrInstructionRecorder[index];
                    if (uop.conditional)
                        dvrTraceVector("reconvergence_branch", curTick(),
                            uop.pc, uop.branchTargetPC, inference.lanes);
                }
                // Every completed root discovery is a dynamic inner-loop
                // invocation with its own start address and bound.  Once NDM
                // has found the outer stride, pair that exact data with the
                // next committed outer base from its FIFO.
                if ((dvrNestedDiscoveryMode.state() ==
                         DVRNestedDiscoveryMode::State::OuterFound ||
                     dvrNestedDiscoveryMode.state() ==
                         DVRNestedDiscoveryMode::State::Vectorizing) &&
                    inference.matched && inference.lanes != 0 &&
                    dvrCurrentTriggerAddress != 0 &&
                    committed_flr != 0 &&
                    dvrNestedDiscoveryMode.recordOuterInvocation(
                        dvrCurrentTriggerAddress, inference.lanes,
                        result.triggerPC, committed_flr,
                        inference.increment)) {
                    ++cpuStats.dvrNDMOuterInvocations;
                    dvrTraceVector("invocation", curTick(),
                        result.triggerPC, dvrCurrentTriggerAddress,
                        inference.lanes);
                }
                const bool ndm_eligible = dvrMode == "nested" &&
                    dvrNestedDiscoveryMode.eligible(inference.lanes);
                if (ndm_eligible &&
                    dvrNestedDiscoveryMode.start(
                        result.triggerPC, inference.increment,
                        inference.lanes).event ==
                    DVRNestedDiscoveryMode::Event::Started) {
                    ++cpuStats.dvrNDMAttempts;
                    dvrTraceVector("ndm_start", curTick(), result.triggerPC,
                        0, inference.lanes);
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
                } else if (dvrMode == "nested" && !ndm_eligible &&
                           dvrNestedDiscoveryMode.active()) {
                    // A large inner invocation already provides enough
                    // scalar-equivalent lanes; discard any stale short-loop
                    // NDM plan and use ordinary DVR for this generation.
                    dvrNestedDiscoveryMode.reset();
                }
                if (dvrMode == "nested" && inference.matched &&
                    inference.lanes != 0 && !ndm_eligible) {
                    ++cpuStats.dvrNDMThresholdBypasses;
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
                                      committed_flr != 0 &&
                                      !dvrInstructionRecorder.overflow();
                // Preserve the paper's bounded fallback when no loop bound
                // can be proved.  It is distinct from an exact inference:
                // no NDM/precise-loop admission is granted, but the helper
                // can still issue up to 128 speculative future strides.
                const bool fallback_allowed =
                    dvrAllowBoundedFallback && dvrMode != "discovery" &&
                    !inference.matched &&
                    inference.fallback && inference.lanes != 0 &&
                    dvrInstructionRecorder.size() > 1 &&
                    committed_flr != 0 &&
                    !dvrInstructionRecorder.overflow();
                // A complete opposite-path entry is independently safe to
                // replay: it has a single entry/exit, a known reconvergence,
                // and validated live-ins.  Do not discard it solely because
                // the generic loop-bound detector could not infer the BFS
                // induction variable.  This remains narrower than normal
                // fallback because it is gated by a real cache hit and FLR.
                // A cache-complete alternate path is also useful when the
                // loop detector cannot prove a trip count.  In that case
                // admit one bounded next-stride probe rather than inventing
                // a full 128-lane loop bound.
                const unsigned alternate_continuation_lanes =
                    inference.lanes != 0 ? inference.lanes : 1;
                const bool alternate_continuation_allowed =
                    dvrMode != "discovery" && alternate_complete_hit &&
                    dvrInstructionRecorder.size() > 1 &&
                    committed_flr != 0 &&
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
                    // The ordinary initial VIR pass audits the same
                    // trigger-to-FLR region that the paper vectorizes.  A
                    // branch between FLR and LCR is the documented exception:
                    // retain the full template so divergent lanes can keep
                    // exploring toward the next stride PC.
                    DVRInstructionRecorder vir_program =
                        dvrInstructionRecorder;
                    unsigned initial_vir_size = 0;
                    for (unsigned index = 1;
                         index < vir_program.size(); ++index) {
                        if (vir_program[index].load)
                            initial_vir_size = index + 1;
                    }
                    const Addr lcr_pc = dvrLoopBoundDetector.branchPC();
                    const bool continue_past_flr =
                        vir_program.hasConditionalBetween(
                            committed_flr, lcr_pc);
                    if (!continue_past_flr && initial_vir_size > 1)
                        vir_program.truncate(initial_vir_size);
                    const auto vir_result =
                        dvrVectorInstructionRegister.execute(
                            vir_program, inference.lanes,
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
                    if (vir_result.unsupportedSemanticLanes != 0) {
                        dvrTraceVector(
                            "initial_vir_unsupported", curTick(),
                            vir_result.unsupportedSemanticPC,
                            vir_result.unsupportedSemanticEncoding,
                            vir_result.unsupportedSemanticLanes,
                            vir_result.unsupportedSemantic);
                    }
                    cpuStats.dvrAlternatePathUopsReplayed +=
                        vir_result.alternatePathUops;
                    cpuStats.dvrReconvergenceResumeSuccesses +=
                        vir_result.alternatePathReconvergences;
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
                /*
                 * NDM data-plane admission is deliberately independent of
                 * the ordinary VIR control-flow admission above.  A
                 * captured inner program can be unsuitable for the
                 * single-invocation VIR path (for example because its
                 * branch target leaves the short recorder), while still
                 * being a valid NDM source/replay template.  Gating NDM on
                 * helper_allowed silently drops the outer x inner plan and
                 * leaves all workload-level flatten counters at zero.
                 */
                const bool ndm_capture_valid =
                    dvrMode == "nested" &&
                    dvrNestedDiscoveryMode.readyToVectorize() &&
                    dvrNestedDiscoveryMode.outerInvocationCount() >= 2 &&
                    inference.matched && inference.lanes != 0 &&
                    committed_flr != 0 &&
                    dvrInstructionRecorder.size() > 1 &&
                    !dvrInstructionRecorder.overflow();
                bool ndm_launched = false;
                if (ndm_capture_valid) {
                    dvrNestedContext.reset();
                    dvrNestedContext.active = true;
                    dvrNestedContext.tid = tid;
                    dvrNestedContext.triggerPC = result.triggerPC;
                    dvrNestedContext.triggerAddress =
                        dvrCurrentTriggerAddress;
                    dvrNestedContext.stride = inference.increment;
                    dvrNestedContext.taint = dvrTaintTracker;
                    dvrNestedContext.taint.setFLR(committed_flr);
                    dvrNestedContext.recorder = dvrInstructionRecorder;
                    dvrNestedContext.startRegs = dvrDiscoveryStartRegs;
                    launchDVRNestedPrefetches(finish_regs);
                    dvrNestedDiscoveryMode.finishVectorization();
                    dvrNestedContext.reset();
                    ndm_launched = true;
                }
                // While a short-loop NDM plan is active, defer ordinary
                // single-loop launches so the bounded helper queue can form
                // the outer x inner batch.  When NDM is ineligible or has
                // fallen back, nested mode uses ordinary DVR directly.
                const bool ordinary_dvr_allowed = dvrMode != "nested" ||
                    !dvrNestedDiscoveryMode.active();
                if (ordinary_dvr_allowed && !ndm_launched &&
                    (helper_allowed || fallback_allowed ||
                     launch_source_fallback ||
                     alternate_continuation_allowed) &&
                    inst->effAddrValid()) {
                    if (launch_source_fallback)
                        ++cpuStats.dvrControlFallbackSourceLaunches;
                    if (fallback_allowed)
                        dvrTraceVector("loop_bound_fallback_launch",
                            curTick(), result.triggerPC, inst->effAddr,
                            inference.lanes);
                    if (alternate_continuation_allowed && !helper_allowed)
                        dvrTraceVector("alternate_path_continuation_launch",
                            curTick(), result.triggerPC, inst->effAddr,
                            alternate_continuation_lanes);
                    if (dvrMode == "nested")
                        ++cpuStats.dvrNestedOrdinaryDVRLaunches;
                    launchDVRStridePrefetches(
                        tid, inst->effAddr, result.triggerPC,
                        result.stride,
                        alternate_continuation_allowed && !helper_allowed ?
                            alternate_continuation_lanes : inference.lanes,
                        finish_regs);
                } else if (committed_flr != 0) {
                    ++cpuStats.dvrHelpersSuppressed;
                }
                DPRINTF(O3CPU,
                        "DVR discovery complete pc=%#x stride=%lld "
                        "insts=%u flr=%#x taint=%#x loop=%#x->%#x "
                        "bound=%#x increment=%lld remaining=%llu lanes=%u\n",
                        result.triggerPC,
                        static_cast<long long>(result.stride),
                        result.instructions, committed_flr,
                        dvrTaintTracker.bits(),
                        dvrLoopBoundDetector.branchPC(),
                        dvrLoopBoundDetector.targetPC(), inference.bound,
                        static_cast<long long>(inference.increment),
                        static_cast<unsigned long long>(inference.remaining),
                        inference.lanes);
                dvrTaintTracker.reset();
                dvrCommittedFinalLoadPC = 0;
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
                recordDVRDiscoveryGeneration("timeout", nullptr,
                                              result.triggerPC, result.stride);
                dvrStrideDetector.endDiscovery();
                ++cpuStats.dvrDiscoveryTimeouts;
                dvrTraceVector("discovery_timeout", curTick(),
                    result.triggerPC, 0, result.instructions);
                DPRINTF(O3CPU,
                        "DVR discovery timeout pc=%#x stride=%lld insts=%u\n",
                        result.triggerPC,
                        static_cast<long long>(result.stride),
                        result.instructions);
                dvrTaintTracker.reset();
                dvrCommittedFinalLoadPC = 0;
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
                recordDVRDiscoveryGeneration("abandon", nullptr,
                                              result.triggerPC, result.stride);
                dvrStrideDetector.endDiscovery();
                ++cpuStats.dvrDiscoveryAbandons;
                dvrTraceVector("discovery_abandon", curTick(),
                    result.triggerPC, 0, 0);
                dvrTaintTracker.reset();
                dvrCommittedFinalLoadPC = 0;
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
    ++cpuStats.dvrHelperLaunchAttempts;
    lanes = std::min(lanes, DVRLanePredicateTracker::MaxLanes);
    if (lanes == 0) {
        ++cpuStats.dvrHelperLaunchZeroLanes;
        ++cpuStats.dvrHelpersSuppressed;
        return;
    }

    // Multiple discoveries may be in flight, but admission remains bounded.
    // The old active-helper/ non-empty-queue gate discarded a whole discovery
    // generation even when the queue had room, which reduced source
    // production to a small fraction of the observed dependent demand.
    const unsigned available = DvrMaxQueuedPrefetches -
        std::min<unsigned>(dvrPrefetchQueue.size(), DvrMaxQueuedPrefetches);
    if (available == 0) {
        ++cpuStats.dvrHelperLaunchCapacityDrops;
        ++cpuStats.dvrHelpersSuppressed;
        return;
    }
    if (lanes > available)
        lanes = available;
    ++cpuStats.dvrHelperLaunchAdmitted;
    dvrTraceVector("discovery_launch", curTick(), pc, current_address,
                   lanes);

    std::array<int64_t, DVRPrefetchSenderState::MaxRelations> scales = {};
    std::array<int64_t, DVRPrefetchSenderState::MaxRelations> offsets = {};
    std::array<RegVal, DVRPrefetchSenderState::MaxRelations> masks = {};
    std::array<RegVal, DVRPrefetchSenderState::MaxRelations> patterns = {};
    unsigned relation_count = 0;
    const auto trigger_it = dvrTriggerRelations.find(pc);
    if (trigger_it != dvrTriggerRelations.end()) {
        for (const Addr flr_pc : trigger_it->second) {
            const auto relation_it = dvrAddressRelations.find(
                DVRRelationKey{pc, flr_pc});
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
    replay->triggerPC = pc;
    // The next iteration boundary is the loop back-edge target, not
    // necessarily the trigger load itself.  CC commonly enters its loop
    // body through a header before reaching the stride load; matching only
    // the trigger PC would miss every stride-PC termination.
    replay->stridePC = dvrLoopBoundDetector.targetPC() != 0 ?
        dvrLoopBoundDetector.targetPC() : pc;
    replay->loopControlPC = dvrLoopBoundDetector.branchPC();
    /*
     * Keep the complete captured stream in the replay template.  The FLR is
     * still the discovery marker and the first dependent target, but a
     * control-flow instruction after the FLR must remain visible to VIR: the
     * paper explicitly continues divergent lanes to the next stride PC when
     * an intervening branch separates FLR and LCR.  The eight-uop front-end
     * window is refilled while this template remains resident.
     */
    replay->count = dvrInstructionRecorder.size();
    replay->scalarCount = 0;
    for (unsigned index = 1; index < replay->count; ++index) {
        if (dvrInstructionRecorder[index].load)
            replay->scalarCount = index + 1;
    }
    const Addr lcr_pc = dvrLoopBoundDetector.branchPC();
    replay->continuePastFLR = dvrInstructionRecorder.hasConditionalBetween(
        dvrCommittedFinalLoadPC, lcr_pc);
    if (replay->continuePastFLR)
        ++cpuStats.dvrReplayContinuePastFLRTemplates;
    // Source lanes begin at the next committed trigger occurrence.  The
    // replay register image must come from that same occurrence, otherwise
    // the source address and the non-trigger inputs belong to different
    // dynamic iterations.
    replay->initialRegs = finish_regs;
    if (replay->count != 0) {
        replay->triggerDestination = dvrInstructionRecorder[0].destination;
        replay->valid = replay->scalarCount > 1 &&
            replay->triggerDestination > 0 &&
            replay->triggerDestination <
                DVRLoopBoundDetector::MaxArchitecturalIntRegs;
        uint32_t defined_regs = uint32_t(1);
        if (replay->triggerDestination > 0 &&
            replay->triggerDestination <
                DVRLoopBoundDetector::MaxArchitecturalIntRegs) {
            defined_regs |= uint32_t(1) << replay->triggerDestination;
        }
        // Replay starts from the architectural image captured at the next
        // trigger occurrence (finish_regs), not from the discovery-start
        // image.  Therefore a loop-carried/live-in register changing across
        // discovery is expected and must not invalidate the template.  The
        // old start-vs-finish comparison rejected every Camel template and
        // silently disabled dependent replay.  Keep the validation focused
        // on representable register IDs and unsupported semantics below.
        for (unsigned index = 0; index < replay->count; ++index)
            replay->uops[index] = dvrInstructionRecorder[index];
        if (replay->scalarCount > 1)
            replay->finalLoadPC = replay->uops[replay->scalarCount - 1].pc;
        for (unsigned index = 0; index < replay->scalarCount; ++index) {
            if (index == 0)
                continue;
            uint32_t external_sources =
                replay->uops[index].intSources & ~defined_regs;
            while (external_sources) {
                const unsigned reg = __builtin_ctz(external_sources);
                external_sources &= external_sources - 1;
                if (reg >= DVRLoopBoundDetector::MaxArchitecturalIntRegs)
                    replay->valid = false;
            }
            const auto &replay_uop = replay->uops[index];
            const bool safe_unsupported =
                replay_uop.semantic ==
                    DVRInstructionRecorder::Uop::Semantic::Unsupported &&
                !replay_uop.control && replay_uop.intDestinations == 0;
            const bool direct_jump = replay_uop.control &&
                !replay_uop.conditional && replay_uop.branchTargetPC != 0 &&
                replay_uop.intDestinations == 0;
            const bool terminal_indirect = replay_uop.control &&
                !replay_uop.conditional && replay_uop.branchTargetPC == 0 &&
                replay_uop.intDestinations == 0;
            if (replay_uop.semantic ==
                    DVRInstructionRecorder::Uop::Semantic::Unsupported &&
                !safe_unsupported && !direct_jump && !terminal_indirect) {
                ++cpuStats.dvrReplayUnsupportedUops;
                dvrTraceDependency("replay_unsupported", curTick(), pc,
                    replay_uop.pc, replay_uop.encoding, 0, 0);
                replay->valid = false;
            } else {
                ++cpuStats.dvrReplaySupportedUops;
            }
            defined_regs |= replay->uops[index].intDestinations;
        }
        // No start-vs-finish equality is required for live-ins.  A template
        // that reaches this point has a complete register image in
        // replay->initialRegs and can be evaluated for the source lane.
    }
    if (replay->count > 1) {
        replay->continuation =
            std::make_shared<DVRVectorInstructionRegister>();
        replay->continuation->initializeSourceContinuation(
            replay->uops, replay->count, lanes, replay->initialRegs,
            replay->scalarCount, replay->continuePastFLR);
        ++cpuStats.dvrVIRContinuationContexts;
    }

    // The helper register file is execution state, not discovery metadata.
    // All source lanes of this launch share one private 16-copy VRAT; it is
    // released when its outstanding source/dependent requests and VIR uops
    // retire, instead of accumulating in historical replay templates.
    auto helper_regs = std::make_shared<DVRHelperVectorRegisterFile>();
    if (dvrSharedPhysicalBank)
        helper_regs->configureSharedPhysicalBank(&regFile, &freeList);
    helper_regs->initialize(replay->initialRegs);
    if (replay->triggerDestination >= 0 &&
        replay->triggerDestination <
            DVRLoopBoundDetector::MaxArchitecturalIntRegs &&
        !helper_regs->vectorize(replay->triggerDestination)) {
        ++cpuStats.dvrHelpersSuppressed;
        return;
    }
    assert(helper_regs->conservationValid());
    ++cpuStats.dvrHelperVRATPrograms;
    if (helper_regs->usesSharedPhysicalBank())
        ++cpuStats.dvrHelperSharedVRATPrograms;

    // Source stride prefetches are useful even when the committed slice did
    // not contain a replayable load uop.  Keep the helper alive for source
    // lanes; replay->count only controls dependent replay semantics.
    startDVRHelper(pc, std::max(1u, replay->count), lanes,
                   dvrInstructionRecorder.resourceCounts(), replay, tid);
    for (unsigned lane = 1; lane <= lanes; ++lane) {
        const Addr address = current_address + stride * lane;
        // A source response carries the value at this exact byte address;
        // cache-line deduplication would incorrectly merge different lanes.
        if (dvrQueuedPrefetchAddresses.count(address) != 0 ||
            dvrOutstandingPrefetchAddresses.count(address) != 0) {
            ++cpuStats.dvrPrefetchesDeduplicated;
            retireDVRPredicateLane(predicate, lane - 1, false);
            continue;
        }
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
        prefetch.helperRegs = helper_regs;
        prefetch.predicate = predicate;
        prefetch.lane = lane - 1;
        dvrPrefetchQueue.push_back(prefetch);
        dvrQueuedPrefetchAddresses.insert(prefetch.address);
        ++cpuStats.dvrPrefetchesGenerated;
        ++cpuStats.dvrVectorizerSourceLanes;
        dvrTraceVector("source_lane", curTick(), pc, address, 1,
                       static_cast<int>(prefetch.lane));
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
    dvrQueuedPrefetchAddresses.clear();
    dvrQueuedDependentLines.clear();
    DVRInstructionRecorder::ResourceCounts vector_resources;
    vector_resources.lsu = 1;
        startDVRHelper(pc, 1, lanes, vector_resources, nullptr, tid);
    for (unsigned lane = 1; lane <= lanes; ++lane) {
        DVRPrefetchAddress prefetch;
        prefetch.address = current_address + stride * lane;
        prefetch.pc = pc;
        prefetch.tid = tid;
        prefetch.source = true;
        prefetch.lane = lane - 1;
        dvrPrefetchQueue.push_back(prefetch);
        dvrQueuedPrefetchAddresses.insert(prefetch.address);
        ++cpuStats.dvrPrefetchesGenerated;
        ++cpuStats.dvrVectorizerDependentLanes;
        dvrTraceVector("vr_lane", curTick(), pc, prefetch.address, 1,
                       static_cast<int>(prefetch.lane));
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

    dvrNestedContext.recorder.setReconvergencePC(
        dvrNestedContext.taint.flr(),
        dvrNestedContext.loopBound.branchPC());
    for (unsigned pass = 0; pass < 3; ++pass)
        recordDVRAlternatePaths(dvrNestedContext.recorder,
                                committing_inst->contextId());
    augmentDVRAlternatePaths(dvrNestedContext.recorder,
                             committing_inst->contextId(), finish_regs);

    const auto inference = dvrNestedContext.loopBound.infer(
        dvrNestedContext.startRegs, finish_regs, dvrMaxLanes);
    // NDM records only a completed child with its own inferred bound.  The
    // old prototype copied the initiating short-loop bound here, which paired
    // one invocation's bound with another invocation's base.
    const unsigned ndm_invocation_lanes = inference.matched ? inference.lanes : 0;
    if ((dvrNestedDiscoveryMode.state() ==
             DVRNestedDiscoveryMode::State::OuterFound ||
         dvrNestedDiscoveryMode.state() ==
             DVRNestedDiscoveryMode::State::Vectorizing) &&
        ndm_invocation_lanes != 0 && dvrNestedContext.taint.flr() != 0 &&
        dvrNestedDiscoveryMode.recordOuterInvocation(
            dvrNestedContext.triggerAddress, ndm_invocation_lanes,
            dvrNestedContext.triggerPC, dvrNestedContext.taint.flr(),
            inference.increment)) {
        ++cpuStats.dvrNDMOuterInvocations;
        dvrTraceVector("invocation", curTick(), dvrNestedContext.triggerPC,
            dvrNestedContext.triggerAddress, ndm_invocation_lanes);
    }
    bool helper_allowed = dvrNestedContext.recorder.size() > 1 &&
        dvrNestedContext.taint.flr() != 0 && inference.matched &&
        !dvrNestedContext.recorder.overflow();
    /*
     * The NDM plan is a separate admission path from ordinary single
     * invocation VIR.  Keep a valid child capture available for flattening
     * even when VIR rejected a branch target outside its short recorder.
     */
    const bool ndm_capture_valid =
        dvrMode == "nested" &&
        dvrNestedDiscoveryMode.readyToVectorize() &&
        inference.matched && inference.lanes != 0 &&
        dvrNestedContext.taint.flr() != 0 &&
        dvrNestedContext.recorder.size() > 1 &&
        !dvrNestedContext.recorder.overflow();
    if (helper_allowed || ndm_capture_valid) {
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
        if (vir_result.unsupportedSemanticLanes != 0) {
            dvrTraceVector(
                "nested_vir_unsupported", curTick(),
                vir_result.unsupportedSemanticPC,
                vir_result.unsupportedSemanticEncoding,
                vir_result.unsupportedSemanticLanes,
                vir_result.unsupportedSemantic);
        }
        cpuStats.dvrAlternatePathUopsReplayed +=
            vir_result.alternatePathUops;
        cpuStats.dvrReconvergenceResumeSuccesses +=
            vir_result.alternatePathReconvergences;
        if (vir_result.unsupportedControlFlow) {
            ++cpuStats.dvrVIRUnsupportedControlFlow;
            helper_allowed = false;
        }
        helper_allowed = !vir_result.timedOut &&
            !vir_result.stackOverflow && helper_allowed;
    }
    if (helper_allowed || ndm_capture_valid) {
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
        // A single CPU-observed invocation is not enough for the legacy
        // batching path.  An NDM plan, however, already contains the
        // independently bounded future outer invocations discovered after
        // branch inversion.  Requiring two entries in the legacy batch in
        // that case prevents the authoritative NDM plan from ever reaching
        // launchDVRNestedPrefetches(): the child context is reset after each
        // completion, leaving every generation stuck at count == 1.
        const bool ndm_plan_ready =
            dvrNestedDiscoveryMode.readyToVectorize() &&
            dvrNestedDiscoveryMode.outerInvocationCount() >= 2;
        const bool ndm_allows_flatten =
            !dvrNestedDiscoveryMode.active() ||
            ndm_plan_ready;
        if ((ndm_plan_ready || dvrNestedInvocationBatch.count >= 2) &&
            ndm_allows_flatten) {
            launchDVRNestedPrefetches(finish_regs);
            if (ndm_plan_ready)
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
    replay->triggerPC = dvrNestedContext.triggerPC;
    replay->stridePC = dvrNestedContext.loopBound.targetPC() != 0 ?
        dvrNestedContext.loopBound.targetPC() :
        dvrNestedContext.triggerPC;
    replay->loopControlPC = dvrNestedContext.loopBound.branchPC();
    // Keep branches after FLR in the persistent template for the same
    // divergent-path rule used by ordinary DVR replay.
    replay->count = dvrNestedContext.recorder.size();
    replay->scalarCount = 0;
    for (unsigned index = 1; index < replay->count; ++index) {
        if (dvrNestedContext.recorder[index].load)
            replay->scalarCount = index + 1;
    }
    const Addr lcr_pc = dvrNestedContext.loopBound.branchPC();
    replay->continuePastFLR = dvrNestedContext.recorder.hasConditionalBetween(
        dvrNestedContext.taint.flr(), lcr_pc);
    if (replay->continuePastFLR)
        ++cpuStats.dvrReplayContinuePastFLRTemplates;
    replay->initialRegs = finish_regs;
    if (replay->count != 0) {
        replay->triggerDestination =
            dvrNestedContext.recorder[0].destination;
        replay->valid = replay->scalarCount > 1 &&
            replay->triggerDestination > 0 &&
            replay->triggerDestination <
                DVRLoopBoundDetector::MaxArchitecturalIntRegs;
        uint32_t defined_regs = uint32_t(1);
        if (replay->triggerDestination > 0 &&
            replay->triggerDestination <
                DVRLoopBoundDetector::MaxArchitecturalIntRegs) {
            defined_regs |= uint32_t(1) << replay->triggerDestination;
        }
        for (unsigned index = 0; index < replay->count; ++index)
            replay->uops[index] = dvrNestedContext.recorder[index];
        if (replay->scalarCount > 1)
            replay->finalLoadPC = replay->uops[replay->scalarCount - 1].pc;
        for (unsigned index = 0; index < replay->scalarCount; ++index) {
            if (index == 0)
                continue;
            uint32_t external = replay->uops[index].intSources &
                ~defined_regs;
            while (external) {
                const unsigned reg = __builtin_ctz(external);
                external &= external - 1;
                if (reg >= DVRLoopBoundDetector::MaxArchitecturalIntRegs)
                    replay->valid = false;
            }
            const auto &replay_uop = replay->uops[index];
            const bool safe_unsupported =
                replay_uop.semantic ==
                    DVRInstructionRecorder::Uop::Semantic::Unsupported &&
                !replay_uop.control && replay_uop.intDestinations == 0;
            const bool direct_jump = replay_uop.control &&
                !replay_uop.conditional && replay_uop.branchTargetPC != 0 &&
                replay_uop.intDestinations == 0;
            if (replay_uop.semantic ==
                    DVRInstructionRecorder::Uop::Semantic::Unsupported &&
                !safe_unsupported && !direct_jump) {
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
            const auto found = dvrAddressRelations.find(
                DVRRelationKey{dvrNestedContext.triggerPC, flr_pc});
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
        std::min<unsigned>(dvrNestedDiscoveryMode.outerInvocationCount(), 16) :
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
    ++cpuStats.dvrNestedFlattenInvariantChecks;
    const unsigned expected_flattened =
        std::min<unsigned>(DVRLanePredicateTracker::MaxLanes,
                           total_inner_lanes);
    cpuStats.dvrNestedFlattenExpectedLanes += expected_flattened;
    if (flattened != expected_flattened)
        ++cpuStats.dvrNestedFlattenInvariantFailures;
    // Record the exact outer-vector width used by this batch.  This is
    // distinct from the flattened scalar-lane count: the paper permits up
    // to 16 outer invocations, each contributing inner lanes, with the
    // final stream truncated to 128 scalar-equivalent lanes.
    dvrTraceVector("flatten_batch", curTick(), dvrNestedContext.triggerPC,
                   0, flattened, static_cast<int>(invocations));
    // A flattened NDM batch is one bounded helper program.  Do not admit a
    // prefix when the helper queue cannot hold the whole batch: a partial
    // outer x inner vector would break the recorded flatten invariant and,
    // worse, let later discoveries bury its source responses behind an
    // unbounded queue.  This is the same admission discipline as ordinary
    // DVR stride launches, applied before any helper state is created.
    const unsigned available = DvrMaxQueuedPrefetches -
        std::min<unsigned>(dvrPrefetchQueue.size(), DvrMaxQueuedPrefetches);
    if (flattened == 0 || available < flattened) {
        ++cpuStats.dvrHelperLaunchCapacityDrops;
        ++cpuStats.dvrHelpersSuppressed;
        return;
    }
    if (replay->count > 1) {
        replay->continuation =
            std::make_shared<DVRVectorInstructionRegister>();
        replay->continuation->initializeSourceContinuation(
            replay->uops, replay->count, flattened, replay->initialRegs,
            replay->scalarCount, replay->continuePastFLR);
        ++cpuStats.dvrVIRContinuationContexts;
    }
    auto helper_regs = std::make_shared<DVRHelperVectorRegisterFile>();
    if (dvrSharedPhysicalBank)
        helper_regs->configureSharedPhysicalBank(&regFile, &freeList);
    helper_regs->initialize(replay->initialRegs);
    if (replay->triggerDestination >= 0 &&
        replay->triggerDestination <
            DVRLoopBoundDetector::MaxArchitecturalIntRegs &&
        !helper_regs->vectorize(replay->triggerDestination)) {
        ++cpuStats.dvrHelpersSuppressed;
        return;
    }
    assert(helper_regs->conservationValid());
    ++cpuStats.dvrHelperVRATPrograms;
    if (helper_regs->usesSharedPhysicalBank())
        ++cpuStats.dvrHelperSharedVRATPrograms;
    startDVRHelper(dvrNestedContext.triggerPC, replay->count,
                   flattened, dvrNestedContext.recorder.resourceCounts(),
                   replay, dvrNestedContext.tid);
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
        const Addr source_address = base + inner_stride * lane;
        if (dvrQueuedPrefetchAddresses.count(source_address) != 0 ||
            dvrOutstandingPrefetchAddresses.count(source_address) != 0) {
            ++cpuStats.dvrPrefetchesDeduplicated;
            continue;
        }
        DVRPrefetchAddress prefetch;
        prefetch.address = source_address;
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
        prefetch.helperRegs = helper_regs;
        prefetch.lane = flat_lane;
        dvrPrefetchQueue.push_back(prefetch);
        dvrQueuedPrefetchAddresses.insert(prefetch.address);
        ++cpuStats.dvrPrefetchesGenerated;
        ++cpuStats.dvrNestedHelpersGenerated;
        ++cpuStats.dvrVectorizerSourceLanes;
        dvrTraceVector("nested_source_lane", curTick(), prefetch.pc,
            prefetch.address, 1, static_cast<int>(flat_lane));
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

unsigned
CPU::dvrPrefetchBytes(const DVRPrefetchAddress &prefetch) const
{
    if (!prefetch.replay || prefetch.replay->count == 0)
        return 1;

    const DVRInstructionRecorder::Uop *load_uop = nullptr;
    if (prefetch.source) {
        load_uop = &prefetch.replay->uops[0];
    } else {
        for (unsigned index = 0; index < prefetch.replay->count; ++index) {
            const auto &uop = prefetch.replay->uops[index];
            if (uop.pc == prefetch.pc && uop.load) {
                load_uop = &uop;
                break;
            }
        }
    }
    if (!load_uop || load_uop->loadBytes == 0)
        return 1;
    return std::min<unsigned>(load_uop->loadBytes, sizeof(RegVal));
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
CPU::issueDVRReplayLanes(unsigned slots)
{
    unsigned bfs11970_active = 0;
    unsigned bfs11970_ready = 0;
    unsigned bfs11970_blocked = 0;
    Addr bfs11970_pc = 0;
    if (dvrHelperThread.debugBfsContext) {
        auto context_it = dvrHelperThread.replayContextLanes.find(
            dvrHelperThread.debugBfsContext.get());
        if (context_it != dvrHelperThread.replayContextLanes.end())
        for (const auto *lane : context_it->second) {
          if (!lane->active || !lane->nested || lane->triggerPC != 0x11970)
            continue;
          ++bfs11970_active;
          bfs11970_pc = lane->lanePC;
          if (lane->reconvergenceBlocked)
            ++bfs11970_blocked;
          if (lane->program && lane->uopIndex < lane->program->count) {
            const auto &uop = lane->program->uops[lane->uopIndex];
            const auto ready = [&](int8_t reg) {
                if (reg < 0 || reg >=
                        DVRLoopBoundDetector::MaxArchitecturalIntRegs)
                    return true;
                return lane->helperRegs ?
                    lane->helperRegs->readyAt(reg, lane->lane) <= curTick() :
                    lane->readyCycle[reg] <= curTick();
            };
            if (ready(uop.source0) && ready(uop.source1))
                ++bfs11970_ready;
          }
        }
    }
    if (bfs11970_active != 0) {
        const unsigned reason =
            (slots == 0 ? 1u : 0u) |
            (dvrHelperThread.readyUops == 0 ? 2u : 0u) |
            (dvrHelperThread.virBuffer.size() >=
             DVRHelperThread::VIRCapacity ? 4u : 0u);
        dvrTraceVector("bfs_replay_scheduler", curTick(), bfs11970_pc,
                       dvrHelperThread.readyUops, bfs11970_active,
                       static_cast<int>(reason | (bfs11970_blocked << 8) |
                                       (bfs11970_ready << 16)));
    }
    if (!dvrVectorChunkModel || slots == 0 ||
        dvrHelperThread.replayReadyContexts.empty())
        return 0;

    if (dvrHelperThread.virBuffer.size() >=
        DVRHelperThread::VIRCapacity) {
        ++cpuStats.dvrHelperVIRCapacityStalls;
        return 0;
    }

    if (dvrHelperThread.replayReadyContexts.empty() ||
        dvrHelperThread.readyUops == 0)
        return 0;

    using Lane = CPU::DVRHelperThread::ReplayLaneContext;
    const auto executingAlternatePath = [](const Lane &lane) {
        if (!lane.reconvergence)
            return false;
        for (unsigned depth = 0;
             depth < lane.reconvergence->depth; ++depth) {
            if (lane.reconvergence->stack[depth].alternatePath)
                return true;
        }
        return false;
    };
    const auto findPC = [](const std::shared_ptr<const DVRReplayTemplate> &program,
                           Addr pc) {
        if (!program || pc == 0)
            return std::numeric_limits<unsigned>::max();
        for (unsigned index = 1; index < program->count; ++index) {
            if (program->uops[index].pc == pc)
                return index;
        }
        return std::numeric_limits<unsigned>::max();
    };
    const auto laneSourcesReady = [this](const Lane &lane) {
        if (!lane.program || lane.uopIndex >= lane.program->count)
            return false;
        const auto &uop = lane.program->uops[lane.uopIndex];
        const auto ready = [&](int8_t reg) {
            if (reg < 0 || reg >=
                    DVRLoopBoundDetector::MaxArchitecturalIntRegs)
                return true;
            return lane.helperRegs ?
                lane.helperRegs->readyAt(reg, lane.lane) <= curTick() :
                lane.readyCycle[reg] <= curTick();
        };
        return ready(uop.source0) && ready(uop.source1);
    };
    const auto lanesForContext = [this](
        const std::shared_ptr<CPU::DVRHelperThread::ReplayReconvergenceState>
            &state) -> const std::vector<Lane *> * {
        if (!state)
            return nullptr;
        auto it = dvrHelperThread.replayContextLanes.find(state.get());
        return it == dvrHelperThread.replayContextLanes.end() ?
            nullptr : &it->second;
    };
    if (dvrHelperThread.debugBfsContext) {
        const auto *context_lanes = lanesForContext(
            dvrHelperThread.debugBfsContext);
        if (context_lanes)
        for (const auto *lane : *context_lanes) {
          if (lane->nested && lane->triggerPC == 0x11970) {
            dvrTraceVector("bfs_replay_lane_state", curTick(),
                           lane->lanePC, lane->uopIndex,
                           lane->active ? 1 : 0,
                           static_cast<int>(lane->lane));
          }
        }
    }
        const auto refreshCurrentGroup =
        [&](const std::shared_ptr<CPU::DVRHelperThread::ReplayReconvergenceState>
                &state) {
            if (!state)
                return;
            const auto *context_lanes = lanesForContext(state);
            if (!context_lanes)
                return;
            const Addr preferred_pc = state->currentValid ?
                state->currentPC : 0;
            const auto preferred_mask = state->currentMask;
            state->currentPC = 0;
            state->currentMask = {};
            state->currentValid = false;
            // The stack head is the architectural SIMT scheduler state.  At
            // a pop, preserve its PC/mask and only remove lanes that have
            // terminated while the deferred path was parked.
            if (preferred_pc != 0) {
                for (const auto *lane : *context_lanes) {
                    if (!lane->active || lane->reconvergenceBlocked ||
                        lane->lanePC != preferred_pc)
                        continue;
                    const uint64_t bit = uint64_t(1) << (lane->lane % 64);
                    if (!(preferred_mask[lane->lane / 64] & bit))
                        continue;
                    state->currentMask[lane->lane / 64] |= bit;
                }
                if (state->currentMask[0] != 0 ||
                    state->currentMask[1] != 0) {
                    state->currentPC = preferred_pc;
                    state->currentValid = true;
                    return;
                }
            }
            for (const auto *lane : *context_lanes) {
                if (!lane->active || lane->reconvergenceBlocked ||
                    lane->lanePC == 0)
                    continue;
                if (state->currentPC == 0)
                    state->currentPC = lane->lanePC;
                if (lane->lanePC != state->currentPC)
                    continue;
                state->currentMask[lane->lane / 64] |=
                    uint64_t(1) << (lane->lane % 64);
            }
            state->currentValid = state->currentPC != 0;
        };
    const auto retireTerminatedLanes = [&](const std::shared_ptr<
        CPU::DVRHelperThread::ReplayReconvergenceState> &context) {
        auto context_it = context ?
            dvrHelperThread.replayContextLanes.find(context.get()) :
            dvrHelperThread.replayContextLanes.end();
        if (context_it == dvrHelperThread.replayContextLanes.end())
            return;
        auto &context_lanes = context_it->second;
        for (auto lane_it = context_lanes.begin();
             lane_it != context_lanes.end();) {
            Lane *lane = *lane_it;
            if (lane->active) {
                ++lane_it;
                continue;
            }
            switch (lane->termination) {
              case Lane::TerminationReason::FLR:
                ++cpuStats.dvrSIMTFLRTerminations;
                break;
              case Lane::TerminationReason::StridePC:
                ++cpuStats.dvrSIMTStridePCTerminations;
                break;
              case Lane::TerminationReason::Timeout:
                ++cpuStats.dvrSIMTTimeoutTerminations;
                break;
              default:
                break;
            }
            lane_it = context_lanes.erase(lane_it);
        }
        if (context_lanes.empty()) {
            dvrHelperThread.replayContextLanes.erase(context_it);
            dvrHelperThread.replayReadyContextSet.erase(context.get());
            dvrHelperThread.replayReadyContexts.erase(
                std::remove_if(dvrHelperThread.replayReadyContexts.begin(),
                               dvrHelperThread.replayReadyContexts.end(),
                    [&](const auto &entry) { return entry.get() == context.get(); }),
                dvrHelperThread.replayReadyContexts.end());
        }
    };
    // Reconvergence is group state.  At the termination PC, pop one frame
    // and reactivate exactly the lanes named by its 128-bit mask.
    std::shared_ptr<CPU::DVRHelperThread::ReplayReconvergenceState>
        reconvergence;
    // The queue itself is the fairness unit.  Select one generation per
    // helper issue attempt and rotate it to the back; no global lane scan is
    // needed here.  A context with no ready lane simply yields this attempt
    // and will be retried after the next source wakeup/rotation.
    while (!dvrHelperThread.replayReadyContexts.empty()) {
        auto context = dvrHelperThread.replayReadyContexts.front();
        dvrHelperThread.replayReadyContexts.pop_front();
        if (!lanesForContext(context)) {
            dvrHelperThread.replayReadyContextSet.erase(context.get());
            continue;
        }
        dvrHelperThread.replayReadyContexts.push_back(context);
        reconvergence = context;
        break;
    }
    const auto *selected_lanes = lanesForContext(reconvergence);
    if (reconvergence) {
        refreshCurrentGroup(reconvergence);
        if (bfs11970_active != 0) {
            unsigned in_group = 0;
            unsigned ready_group = 0;
            for (const auto *candidate : selected_lanes ?
                 *selected_lanes : std::vector<Lane *>{}) {
                if (!candidate->active || !candidate->nested ||
                    candidate->triggerPC != 0x11970 ||
                    !reconvergence->currentValid ||
                    candidate->reconvergenceBlocked ||
                    candidate->lanePC != reconvergence->currentPC)
                    continue;
                const uint64_t bit = uint64_t(1) <<
                    (candidate->lane % 64);
                if (!(reconvergence->currentMask[candidate->lane / 64] & bit))
                    continue;
                ++in_group;
                if (laneSourcesReady(*candidate))
                    ++ready_group;
            }
            dvrTraceVector("bfs_replay_scheduler_selected", curTick(),
                           reconvergence->currentPC,
                           reconvergence->depth, in_group,
                           static_cast<int>(ready_group));
        }
        while (reconvergence->depth != 0) {
            const auto frame = reconvergence->stack[
                reconvergence->depth - 1];
            bool at_reconvergence = false;
            bool current_path_active = false;
            for (const auto *lane : selected_lanes ?
                 *selected_lanes : std::vector<Lane *>{}) {
                if (!lane->active)
                    continue;
                const uint64_t lane_bit = uint64_t(1) << (lane->lane % 64);
                const bool deferred = frame.mask[lane->lane / 64] & lane_bit;
                if (!lane->reconvergenceBlocked &&
                    lane->lanePC == frame.reconvergencePC) {
                    at_reconvergence = true;
                }
                if (!lane->reconvergenceBlocked && !deferred)
                    current_path_active = true;
            }
            if (!at_reconvergence && current_path_active)
                break;
            --reconvergence->depth;
            const unsigned resumed_lanes =
                __builtin_popcountll(frame.mask[0]) +
                __builtin_popcountll(frame.mask[1]);
            dvrTraceVector("reconvergence_pop", curTick(), frame.pc,
                           frame.reconvergencePC, resumed_lanes,
                           static_cast<int>(reconvergence->depth));
            ++cpuStats.dvrSIMTReconvergencePops;
            for (auto *lane : selected_lanes ?
                 *selected_lanes : std::vector<Lane *>{}) {
                if (!lane->active)
                    continue;
                const uint64_t bit = uint64_t(1) << (lane->lane % 64);
                if (frame.mask[lane->lane / 64] & bit) {
                    lane->reconvergenceBlocked = false;
                    lane->lanePC = frame.pc;
                    lane->simtPath = frame.takenPath ? 1 : 2;
                    lane->simtDivergent = true;
                } else if (lane->lanePC == frame.reconvergencePC) {
                    // The SIMT stack selects the deferred mask after a
                    // reconvergence point. Lanes from the path just
                    // completed must not issue the next PC concurrently.
                    lane->reconvergenceBlocked = true;
                }
            }
            reconvergence->currentPC = frame.pc;
            reconvergence->currentMask = frame.mask;
            reconvergence->currentValid = frame.pc != 0;
            ++cpuStats.dvrReconvergences;
            if (frame.alternatePath)
                ++cpuStats.dvrReconvergenceResumeSuccesses;
        }
        // Once the final frame is popped, all surviving lanes are at a
        // common control-flow point.  Divergence is a property of the active
        // SIMT path, not a lifetime bit on the lane object.
        if (reconvergence->depth == 0) {
            reconvergence->renamedDestinations.clear();
            for (auto *lane : selected_lanes ?
                 *selected_lanes : std::vector<Lane *>{}) {
                if (lane->active && !lane->reconvergenceBlocked) {
                    // A template with a branch between FLR and LCR remains
                    // in the paper's divergent-generation mode after the
                    // alternate frame is popped.  The LCR itself is the
                    // termination boundary, so clearing this bit here would
                    // make the final branch look scalar and strand the
                    // generation past its intended next-stride stop.
                    if (!lane->continuePastFLR) {
                        lane->simtDivergent = false;
                        lane->simtPath = 0;
                    }
                }
            }
        }
    }
    const auto inCurrentGroup = [&](const Lane &lane) {
        if (!reconvergence || !reconvergence->currentValid ||
            !lane.active || lane.reconvergence != reconvergence ||
            lane.reconvergenceBlocked ||
            lane.lanePC != reconvergence->currentPC)
            return false;
        const uint64_t bit = uint64_t(1) << (lane.lane % 64);
        return (reconvergence->currentMask[lane.lane / 64] & bit) != 0;
    };
    const Lane *seed = nullptr;
    std::vector<Lane *> group;
    for (auto *lane : selected_lanes ?
         *selected_lanes : std::vector<Lane *>{}) {
        if (!lane->active || !lane->program)
            continue;
        if (lane->helperUops >= dvrHelperMaxUops) {
            lane->active = false;
            lane->termination = Lane::TerminationReason::Timeout;
            continue;
        }
        if (lane->triggerPC == 0x11970 && lane->lanePC == 0x11978) {
            dvrTraceVector("bfs_replay_gate", curTick(), lane->lanePC,
                           reconvergence ? reconvergence->currentPC : 0,
                           inCurrentGroup(*lane) ? 1 : 0,
                           static_cast<int>(lane->lane));
        }
        if (!inCurrentGroup(*lane))
            continue;
        // A VIR issue group and its SIMT stack are one helper-generation
        // context.  Without this guard, the first active generation supplies
        // the stack while a later generation supplies the seed lane; its
        // deferred frame can then never be popped by the matching lanes.
        unsigned pc_index = findPC(lane->program, lane->lanePC);
        if (pc_index == std::numeric_limits<unsigned>::max()) {
            // PCv is an independent helper PC. A divergent target that was
            // not observed by scalar Discovery is fetched and decoded through
            // the normal helper front-end, then appended to this generation's
            // shared runtime stream.
            bool fetch_fault = false;
            bool cache_hit = false;
            const StaticInstPtr decoded = fetchDecodeDVRUop(
                lane->tid, lane->lanePC, nullptr, fetch_fault, cache_hit);
            if (!decoded)
                continue;
            DVRInstructionRecorder::Uop dynamic;
            if (!DVRInstructionRecorder::decodeStatic(
                    decoded, lane->lanePC, dynamic)) {
                lane->active = false;
                lane->termination = Lane::TerminationReason::External;
                ++cpuStats.dvrVIRExternalPathLanes;
                continue;
            }
            dynamic.alternatePath = true;
            dynamic.tainted = true;
            dynamic.reconvergencePC =
                lane->continuePastFLR && lane->loopControlPC != 0 ?
                lane->loopControlPC :
                (lane->finalLoadPC != 0 ? lane->finalLoadPC :
                                         lane->stridePC);
            auto runtime = reconvergence ?
                reconvergence->runtimeProgram : nullptr;
            if (!runtime) {
                runtime = std::make_shared<DVRReplayTemplate>(
                    *lane->program);
                if (reconvergence)
                    reconvergence->runtimeProgram = runtime;
            }
            if (runtime->count >= DVRInstructionRecorder::MaxUops) {
                lane->active = false;
                lane->termination = Lane::TerminationReason::External;
                ++cpuStats.dvrVIRExternalPathLanes;
                continue;
            }
            runtime->uops[runtime->count++] = dynamic;
            for (auto *candidate : selected_lanes ?
                 *selected_lanes : std::vector<Lane *>{})
                candidate->program = runtime;
            lane->program = runtime;
            pc_index = findPC(lane->program, lane->lanePC);
            dvrTraceVector("replay_dynamic_pc_fetch", curTick(),
                           lane->lanePC, dynamic.control,
                           dynamic.load, static_cast<int>(lane->lane));
            if (pc_index == std::numeric_limits<unsigned>::max()) {
                lane->active = false;
                lane->termination = Lane::TerminationReason::External;
                ++cpuStats.dvrVIRExternalPathLanes;
                continue;
            }
        }
        // Remove stateless unsupported instructions from the executable
        // dependency slice before they consume VIR/FU bandwidth.  Typical
        // examples are architectural NOPs between the source and FLR.
        while (pc_index < lane->program->count) {
            const auto &candidate = lane->program->uops[pc_index];
            if (candidate.semantic !=
                    DVRInstructionRecorder::Uop::Semantic::Unsupported ||
                candidate.control || candidate.intDestinations != 0) {
                break;
            }
            ++lane->helperUops;
            if (lane->helperUops >= dvrHelperMaxUops ||
                pc_index + 1 >= lane->program->count) {
                lane->active = false;
                break;
            }
            lane->lanePC = lane->program->uops[++pc_index].pc;
        }
        if (!lane->active)
            continue;
        lane->uopIndex = pc_index;
        const auto &uop = lane->program->uops[lane->uopIndex];
        const unsigned lane_id = lane->lane;
        const auto &helper_regs = lane->helperRegs;
        const bool source0_ready = uop.source0 < 0 ||
            uop.source0 >= DVRLoopBoundDetector::MaxArchitecturalIntRegs ||
            (helper_regs ? helper_regs->readyAt(uop.source0, lane_id) :
             lane->readyCycle[uop.source0]) <= curTick();
        const bool source1_ready = uop.source1 < 0 ||
            uop.source1 >= DVRLoopBoundDetector::MaxArchitecturalIntRegs ||
            (helper_regs ? helper_regs->readyAt(uop.source1, lane_id) :
             lane->readyCycle[uop.source1]) <= curTick();
        if (!source0_ready || !source1_ready)
            continue;
        const unsigned copy = lane_id /
            DVRHelperThread::LanesPerVIRCopy;
        const bool copy_issued = copy < DVRHelperThread::VIRCopies &&
            dvrHelperThread.virCopies[copy].issued;
        if (copy >= DVRHelperThread::VIRCopies ||
            copy_issued) {
            if (lane->triggerPC == 0x11970 &&
                lane->lanePC == 0x11978) {
                dvrTraceVector("bfs_replay_blocked_copy", curTick(),
                               lane->lanePC, copy,
                               copy_issued ? 1 : 0,
                               static_cast<int>(lane->lane));
            }
            continue;
        }
        if (!seed) {
            seed = lane;
        } else if (lane->program != seed->program ||
                   lane->helperRegs != seed->helperRegs ||
                   lane->uopIndex != seed->uopIndex ||
                   copy != seed->lane / DVRHelperThread::LanesPerVIRCopy ||
                   lane->program->uops[lane->uopIndex].pc !=
                       seed->program->uops[seed->uopIndex].pc) {
            continue;
        }
        group.push_back(lane);
        if (group.size() == DVRHelperThread::LanesPerVIRCopy)
            break;
    }
    if (group.empty()) {
        retireTerminatedLanes(reconvergence);
        return 0;
    }

    const auto &uop = seed->program->uops[seed->uopIndex];
    const bool bfs_debug_generation = seed->nested &&
        seed->triggerPC == 0x1458a;
    if (bfs_debug_generation && uop.pc == 0x14598) {
        ++cpuStats.dvrDebugReached14598;
        dvrTraceVector("debug_reached_14598", curTick(), uop.pc, 0,
                       static_cast<int>(group.size()),
                       static_cast<int>(seed->lane));
    }
    const Addr branch_reconvergence = uop.reconvergencePC;
    std::array<Addr, 128> branch_next_pc = {};
    std::array<bool, 128> branch_next_valid = {};
    std::array<std::array<uint64_t, 2>, 128> branch_masks = {};
    std::array<Addr, 128> branch_group_pc = {};
    std::array<bool, 128> branch_group_taken = {};
    std::array<uint64_t, 2> branch_active_mask = {};
    std::array<uint64_t, 2> branch_taken_mask = {};
    std::array<uint64_t, 2> branch_not_taken_mask = {};
    unsigned branch_groups = 0;
    unsigned selected_branch_group = 0;
    bool branch_outcomes_complete = !uop.conditional;
    if (uop.conditional) {
        // Branch grouping is over the whole helper generation, not merely
        // the eight-lane AVX issue chunk selected for this cycle.
        unsigned active_branch_lanes = 0;
        bool all_branch_lanes_ready = true;
        for (const auto *candidate : selected_lanes ?
             *selected_lanes : std::vector<Lane *>{}) {
            if (!inCurrentGroup(*candidate) ||
                candidate->program != seed->program ||
                candidate->uopIndex != seed->uopIndex)
                continue;
            ++active_branch_lanes;
            all_branch_lanes_ready &= laneSourcesReady(*candidate);
        }
        // Keep a trace point even when the branch cannot be admitted yet.
        // This distinguishes "the replay template has no branch" from
        // "the branch is present but its dependent operands never became
        // ready", which is the critical BFS diagnostic.
        dvrTraceVector("replay_branch_candidate", curTick(), uop.pc,
                       branch_reconvergence, active_branch_lanes,
                       uop.tainted ? 1 : 0);
        // SIMT branch grouping is atomic over the active group.  Waiting for
        // all operands prevents a partial ready mask from being committed as
        // if it represented the whole vector.
        if (active_branch_lanes == 0 || !all_branch_lanes_ready)
            return 0;
        for (auto *candidate : selected_lanes ?
             *selected_lanes : std::vector<Lane *>{}) {
            if (!inCurrentGroup(*candidate) ||
                candidate->program != seed->program ||
                candidate->uopIndex != seed->uopIndex)
                continue;
            Lane *lane = candidate;
            const auto &lane_uop = lane->program->uops[lane->uopIndex];
            const RegVal source0 = lane_uop.source0 >= 0 &&
                lane_uop.source0 < DVRLoopBoundDetector::MaxArchitecturalIntRegs ?
                (lane->helperRegs ?
                 lane->helperRegs->read(lane_uop.source0, lane->lane) :
                 lane->regs[lane_uop.source0]) : 0;
            const RegVal source1 = lane_uop.source1 >= 0 &&
                lane_uop.source1 < DVRLoopBoundDetector::MaxArchitecturalIntRegs ?
                (lane->helperRegs ?
                 lane->helperRegs->read(lane_uop.source1, lane->lane) :
                 lane->regs[lane_uop.source1]) : 0;
            if (!laneSourcesReady(*lane))
                continue;
            bool taken = lane_uop.branchTaken;
            // Taint is a lane property during replay.  An alternate-path
            // uop may not have been seen in the scalar discovery stream, but
            // consuming a vectorized source is precisely the VTT propagation
            // rule and must make its predicate/load data-dependent.
            const bool lane_tainted = lane_uop.tainted ||
                (lane->helperRegs &&
                 ((lane_uop.source0 >= 0 &&
                   lane->helperRegs->isVectorized(lane_uop.source0,
                                                   lane->lane)) ||
                  (lane_uop.source1 >= 0 &&
                   lane->helperRegs->isVectorized(lane_uop.source1,
                                                   lane->lane))));
            const bool evaluated = !lane_tainted ||
                lane_uop.evaluateBranch(source0, source1, taken);
            if (!evaluated)
                continue;
            const Addr next = taken ? lane_uop.branchTargetPC :
                lane_uop.fallthroughPC;
            if (dvrTrace.branchCensus && dvrTrace.events) {
                std::fprintf(dvrTrace.events,
                    "{\"category\":\"dependency\","
                    "\"kind\":\"replay_branch_operand\","
                    "\"tick\":%llu,\"trigger_pc\":%llu,"
                    "\"pc\":%llu,\"lane\":%u,"
                    "\"source0\":%llu,\"source1\":%llu,"
                    "\"tainted\":%u,\"taken\":%u,"
                    "\"next_pc\":%llu}\n",
                    static_cast<unsigned long long>(curTick()),
                    static_cast<unsigned long long>(seed->triggerPC),
                    static_cast<unsigned long long>(uop.pc), lane->lane,
                    static_cast<unsigned long long>(source0),
                    static_cast<unsigned long long>(source1),
                    lane_tainted ? 1u : 0u, taken ? 1u : 0u,
                    static_cast<unsigned long long>(next));
            }
            branch_active_mask[lane->lane / 64] |=
                uint64_t(1) << (lane->lane % 64);
            if (taken) {
                branch_taken_mask[lane->lane / 64] |=
                    uint64_t(1) << (lane->lane % 64);
            } else {
                branch_not_taken_mask[lane->lane / 64] |=
                    uint64_t(1) << (lane->lane % 64);
            }
            branch_next_pc[lane->lane] = next;
            branch_next_valid[lane->lane] = true;
            if (bfs_debug_generation && uop.pc == 0x1459c) {
                const uint32_t predicate_flags =
                    (lane_uop.control ? 1u : 0u) |
                    (lane_uop.conditional ? 2u : 0u) |
                    (lane_tainted ? 4u : 0u);
                DPRINTF(O3CPU,
                    "DVR predicate lane trigger=%#x pc=%#x lane=%u "
                    "src0=r%d:%#llx src1=r%d:%#llx tainted=%d "
                    "taken=%d next=%#x active=%#llx:%#llx\n",
                    seed->triggerPC, uop.pc, lane->lane,
                    lane_uop.source0,
                    static_cast<unsigned long long>(source0),
                    lane_uop.source1,
                    static_cast<unsigned long long>(source1),
                    lane_tainted, taken, next,
                    static_cast<unsigned long long>(branch_active_mask[1]),
                    static_cast<unsigned long long>(branch_active_mask[0]));
                dvrTraceDependency("predicate_lane", curTick(),
                    seed->triggerPC, uop.pc, source0, predicate_flags,
                    static_cast<int>(lane->lane));
            }
            unsigned path = 0;
            while (path < branch_groups && branch_group_pc[path] != next)
                ++path;
            if (path == branch_groups && branch_groups < 128) {
                branch_group_pc[branch_groups++] = next;
                branch_group_taken[path] = taken;
            }
            if (path < 128)
                branch_masks[path][lane->lane / 64] |=
                    uint64_t(1) << (lane->lane % 64);
            if (lane == seed)
                selected_branch_group = path;
        }
        unsigned evaluated_branch_lanes = 0;
        for (const auto *candidate : selected_lanes ?
             *selected_lanes : std::vector<Lane *>{}) {
            if (!inCurrentGroup(*candidate) ||
                candidate->program != seed->program ||
                candidate->uopIndex != seed->uopIndex)
                continue;
            if (branch_next_valid[candidate->lane])
                ++evaluated_branch_lanes;
        }
        branch_outcomes_complete = evaluated_branch_lanes ==
            active_branch_lanes;
        const unsigned taken_lanes =
            __builtin_popcountll(branch_taken_mask[0]) +
            __builtin_popcountll(branch_taken_mask[1]);
        const unsigned not_taken_lanes =
            __builtin_popcountll(branch_not_taken_mask[0]) +
            __builtin_popcountll(branch_not_taken_mask[1]);
        if (bfs_debug_generation && uop.pc == 0x1459c &&
            uop.conditional) {
            ++cpuStats.dvrDebugExecuted1459cConditional;
            dvrTraceVector("debug_executed_1459c", curTick(), uop.pc,
                           branch_reconvergence,
                           static_cast<int>(evaluated_branch_lanes),
                           static_cast<int>(branch_groups));
            if (taken_lanes != 0 && not_taken_lanes != 0) {
                ++cpuStats.dvrDebug1459cMixedLaneResults;
                dvrTraceVector("debug_1459c_mixed", curTick(), uop.pc,
                               branch_reconvergence,
                               static_cast<int>(taken_lanes),
                               static_cast<int>(not_taken_lanes));
            }
        }
        dvrTraceVector("replay_branch_evaluated", curTick(), uop.pc,
                       branch_reconvergence, evaluated_branch_lanes,
                       static_cast<int>(branch_groups));
        dvrTraceDependency("replay_branch_evaluated_trigger", curTick(),
                           seed->triggerPC, uop.pc, branch_reconvergence,
                           evaluated_branch_lanes, branch_groups);
        cpuStats.dvrSIMTTakenLanes += taken_lanes;
        cpuStats.dvrSIMTNotTakenLanes += not_taken_lanes;
        cpuStats.dvrSIMTBranchGroups += branch_groups;
        dvrTraceSIMTBranch(curTick(), uop.pc, branch_reconvergence,
                           branch_active_mask, branch_taken_mask,
                           branch_not_taken_mask, branch_groups);
        if (taken_lanes != 0 && not_taken_lanes != 0) {
            ++cpuStats.dvrSIMTMixedBranches;
            dvrTraceDependency("replay_branch_mixed_trigger", curTick(),
                               seed->triggerPC, uop.pc,
                               branch_reconvergence, taken_lanes,
                               not_taken_lanes);
        }
    }
    bool decoded_cache_hit = false;
    bool instruction_fetch_fault = false;
    const StaticInstPtr decoded_inst = fetchDecodeDVRUop(
        seed->tid, uop.pc, uop.staticInst, instruction_fetch_fault,
        decoded_cache_hit);
    if (!decoded_inst) {
        if (seed->triggerPC == 0x11970 && uop.pc >= 0x11970 &&
            uop.pc <= 0x119a0) {
            dvrTraceVector("bfs_uop_fetch_wait", curTick(), uop.pc,
                           dvrInstructionFetchPending.count(uop.pc) ? 1 : 0,
                           dvrInstructionRetryPkt ? 1 : 0,
                           static_cast<int>(seed->lane));
        }
        // The independent instruction-cache request is still outstanding.
        // Do not consume a FU slot or remove the lanes; they will become
        // eligible after completeDVRInstructionFetch fills the decode cache.
        return 0;
    }
    dvrHelperThread.retireFrontend(uop.pc);
    if (instruction_fetch_fault)
        DPRINTF(O3CPU, "DVR helper fetch fault at pc=%#x; using captured "
                "semantic metadata\n", uop.pc);
    using Semantic = DVRInstructionRecorder::Uop::Semantic;
    const bool is_shift =
        uop.semantic == Semantic::ShiftLeft ||
        uop.semantic == Semantic::ShiftRightLogical ||
        uop.semantic == Semantic::ShiftRightArithmetic ||
        uop.semantic == Semantic::ShiftLeftImmediate ||
        uop.semantic == Semantic::ShiftLeftWordImmediate ||
        uop.semantic == Semantic::ShiftRightLogicalImmediate ||
        uop.semantic == Semantic::ShiftRightArithmeticImmediate ||
        uop.semantic == Semantic::ShiftRightLogicalWordImmediate ||
        uop.semantic == Semantic::ShiftRightArithmeticWordImmediate;
    const bool is_add =
        uop.semantic == Semantic::Add ||
        uop.semantic == Semantic::Sub ||
        uop.semantic == Semantic::AddWord ||
        uop.semantic == Semantic::SubWord ||
        uop.semantic == Semantic::AddImmediate ||
        uop.semantic == Semantic::AddWordImmediate ||
        uop.semantic == Semantic::LoadAddress ||
        uop.semantic == Semantic::LoadByteSigned ||
        uop.semantic == Semantic::LoadByteUnsigned ||
        uop.semantic == Semantic::LoadHalfSigned ||
        uop.semantic == Semantic::LoadHalfUnsigned ||
        uop.semantic == Semantic::LoadWordSigned ||
        uop.semantic == Semantic::LoadWordUnsigned ||
        uop.semantic == Semantic::LoadDouble;
    const OpClass op_class = is_shift ? SimdShiftOp :
        ((uop.semantic == Semantic::Multiply ||
          uop.semantic == Semantic::MultiplyWord) ? SimdMultOp :
         (is_add ? SimdAddOp : SimdAluOp));
    ++cpuStats.dvrHelperFURequests;
    ++cpuStats.dvrVectorChunkRequests;
    Cycles latency(1);
    if (!dvrVectorUnlimitedFU &&
        curTick() < dvrVectorNextIssueTick) {
        ++cpuStats.dvrHelperFUStalls;
        ++cpuStats.dvrVectorFUConflictCycles;
        return 0;
    }
    if (!dvrVectorUnlimitedFU && !iew.tryIssueDVRHelperFU(op_class, latency)) {
        ++cpuStats.dvrHelperFUStalls;
        ++cpuStats.dvrVectorFUConflictCycles;
        return 0;
    }

    // Commit next PCs for every active lane in the complete branch group,
    // rather than only for the eight lanes selected for this VIR copy.
    if (uop.conditional && branch_outcomes_complete) {
        for (auto *candidate : selected_lanes ?
             *selected_lanes : std::vector<Lane *>{}) {
            if (!candidate->active ||
                candidate->program != seed->program ||
                candidate->uopIndex != seed->uopIndex ||
                !branch_next_valid[candidate->lane])
                continue;
            const Addr next_pc = branch_next_pc[candidate->lane];
            candidate->lanePC = next_pc;
            ++candidate->helperUops;
            if (next_pc == 0) {
                candidate->active = false;
                candidate->termination =
                    Lane::TerminationReason::External;
                continue;
            }
            const bool loop_backedge =
                uop.branchTargetPC != 0 &&
                uop.branchTargetPC < uop.pc;
            const bool at_lcr = candidate->continuePastFLR &&
                candidate->loopControlPC != 0 &&
                uop.pc == candidate->loopControlPC;
            if ((next_pc == candidate->stridePC ||
                 (candidate->continuePastFLR && (loop_backedge || at_lcr))) &&
                (candidate->simtDivergent || branch_groups > 1 ||
                 candidate->continuePastFLR)) {
                candidate->active = false;
                candidate->termination =
                    Lane::TerminationReason::StridePC;
                continue;
            }
            const unsigned next_index = findPC(candidate->program, next_pc);
            if (next_index == std::numeric_limits<unsigned>::max()) {
                // PCv is independent of the Discovery stream. Leave the
                // lane active with the new PC; the next scheduler pass will
                // fetch/decode the alternate target and append it to the
                // generation-local runtime program.
                candidate->uopIndex = candidate->program->count;
                continue;
            }
            candidate->uopIndex = next_index;
            if (candidate->helperUops >= dvrHelperMaxUops) {
                candidate->active = false;
                candidate->termination =
                    Lane::TerminationReason::Timeout;
            }
        }
    }

    // Commit the SIMT split only after this branch instruction has actually
    // been admitted to the VIR. Deferred groups are stored in reverse order,
    // making the stack head the next path resumed at the common termination
    // PC, exactly as in the paper's reconvergence stack.
    const bool branch_divergent = uop.conditional &&
        branch_outcomes_complete && branch_groups > 1;
    if (branch_divergent &&
        branch_reconvergence != 0 && reconvergence) {
        for (unsigned path = branch_groups; path-- > 0;) {
            if (path == selected_branch_group)
                continue;
            if (reconvergence->depth >=
                DVRHelperThread::ReplayReconvergenceState::Entries) {
                const unsigned dropped =
                    __builtin_popcountll(branch_masks[path][0]) +
                    __builtin_popcountll(branch_masks[path][1]);
                ++cpuStats.dvrReconvergenceStackOverflows;
                cpuStats.dvrSIMTStackOverflowDroppedLanes += dropped;
                dvrTraceVector("reconvergence_overflow", curTick(),
                               uop.pc, branch_reconvergence, dropped,
                               static_cast<int>(reconvergence->depth));
                // The paper bounds the hardware stack at eight entries but
                // does not define an architectural overflow protocol.  DVR
                // is speculative, so preserve the current path and all
                // already-represented frames, while masking off only the
                // deferred lanes for this unrepresentable path.  This is the
                // bounded recovery used by the helper; the main thread is
                // unaffected and remains the correctness fallback.
                for (auto *lane : selected_lanes ?
                     *selected_lanes : std::vector<Lane *>{}) {
                    const uint64_t bit = uint64_t(1) << (lane->lane % 64);
                    if (branch_masks[path][lane->lane / 64] & bit) {
                        lane->active = false;
                        lane->termination = Lane::TerminationReason::External;
                    }
                }
                continue;
            }
            auto &frame = reconvergence->stack[reconvergence->depth++];
            frame.currentPC = branch_group_pc[selected_branch_group];
            frame.currentMask = branch_masks[selected_branch_group];
            frame.reconvergencePC = branch_reconvergence;
            frame.pc = branch_group_pc[path];
            frame.mask = branch_masks[path];
            frame.alternatePath = true;
            frame.takenPath = branch_group_taken[path];
            const unsigned deferred_lanes =
                __builtin_popcountll(frame.mask[0]) +
                __builtin_popcountll(frame.mask[1]);
            dvrTraceVector("reconvergence_push", curTick(), frame.pc,
                           frame.reconvergencePC, deferred_lanes,
                           static_cast<int>(reconvergence->depth));
            ++cpuStats.dvrSIMTReconvergencePushes;
            for (auto *lane : selected_lanes ?
                 *selected_lanes : std::vector<Lane *>{}) {
                if (frame.mask[lane->lane / 64] &
                    (uint64_t(1) << (lane->lane % 64)))
                    lane->reconvergenceBlocked = true;
            }
        }
        // Deferred lanes are blocked by the frames above; rebuild the
        // explicit current mask so lanes terminated at a stride PC or an
        // external target are not retained in the selected group.
        refreshCurrentGroup(reconvergence);
        ++cpuStats.dvrDivergentBranches;
    }
    if (uop.conditional && branch_outcomes_complete && branch_groups != 0) {
        for (auto *lane : selected_lanes ?
             *selected_lanes : std::vector<Lane *>{}) {
            if (!lane->active || !branch_next_valid[lane->lane])
                continue;
            lane->simtPath = branch_group_taken[
                std::find(branch_group_pc.begin(),
                          branch_group_pc.begin() + branch_groups,
                          branch_next_pc[lane->lane]) -
                branch_group_pc.begin()] ? 1 : 2;
            if (branch_divergent)
                lane->simtDivergent = true;
        }
    }
    // This is the helper's actual same-PC vector issue group.  Keep the
    // continuation counters tied to the execution-driven lane path rather
    // than only to the legacy response-side VIR evaluator.
    ++cpuStats.dvrVIRContinuationPCGroups;
    cpuStats.dvrVIRContinuationGroupedLanes += group.size();
    if (group.size() > dvrReplayMaxGroupWidth) {
        dvrReplayMaxGroupWidth = group.size();
        cpuStats.dvrVIRContinuationMaxGroupWidth = group.size();
    }

    const Tick ready_tick = curTick() +
        static_cast<uint64_t>(latency) * clockPeriod();
    if (!dvrVectorUnlimitedFU) {
        dvrVectorNextIssueTick = curTick() +
            uint64_t(std::max(1u, dvrVectorIssueInterval)) * clockPeriod();
    }
    DVRHelperThread::DVRDynUop dyn_uop;
    dyn_uop.program = seed->program;
    dyn_uop.helperRegs = seed->helperRegs;
    dyn_uop.staticInst = decoded_inst;
    dyn_uop.uopIndex = seed->uopIndex;
    dyn_uop.opClass = op_class;
    dyn_uop.source0 = uop.source0;
    dyn_uop.source1 = uop.source1;
    dyn_uop.destination = uop.destination;
    dyn_uop.pc = uop.pc;
    dyn_uop.copy = seed->lane / DVRHelperThread::LanesPerVIRCopy;
    dyn_uop.issueCycle = curTick();
    dyn_uop.state = DVRHelperThread::DVRDynUop::State::Ready;
    dyn_uop.lanes.reserve(group.size());
    dyn_uop.source0Physical.reserve(group.size());
    dyn_uop.source1Physical.reserve(group.size());
    for (Lane *lane : group) {
        dyn_uop.lanes.push_back(lane->lane);
        if (lane->lane < DVRHelperVectorRegisterFile::MaxLanes)
            dyn_uop.activeMask[lane->lane / 64] |=
                uint64_t(1) << (lane->lane % 64);
        const int16_t source0_phys =
            lane->helperRegs && uop.source0 >= 0 ?
            lane->helperRegs->physicalIndex(
                uop.source0, lane->lane) : -1;
        const int16_t source1_phys =
            lane->helperRegs && uop.source1 >= 0 ?
            lane->helperRegs->physicalIndex(
                uop.source1, lane->lane) : -1;
        dyn_uop.source0Physical.push_back(source0_phys);
        dyn_uop.source1Physical.push_back(source1_phys);
        if (lane->helperRegs) {
            lane->helperRegs->retainPhysical(source0_phys);
            lane->helperRegs->retainPhysical(source1_phys);
        }
    }
    ++cpuStats.dvrVIRActiveMaskChecks;
    const unsigned mask_lanes =
        __builtin_popcountll(dyn_uop.activeMask[0]) +
        __builtin_popcountll(dyn_uop.activeMask[1]);
    if (mask_lanes != group.size())
        ++cpuStats.dvrVIRActiveMaskFailures;
    auto &copy = dvrHelperThread.virCopies.at(dyn_uop.copy);
    copy.activeMask = dyn_uop.activeMask;
    copy.readyMask = dyn_uop.activeMask;
    copy.issuedMask = dyn_uop.activeMask;
    copy.executedMask = {};
    copy.completedMask = {};
    copy.deadSourceMask = {};
    copy.deadSource0Mask = {};
    copy.deadSource1Mask = {};
    copy.pc = uop.pc;
    copy.uopIndex = seed->uopIndex;
    copy.inFlight = 1;
    copy.active = true;
    copy.issued = true;
    copy.executed = false;
    copy.deadSource = false;
    dvrTraceVector("vir_issue_group", curTick(), uop.pc, 0,
                   static_cast<int>(group.size()),
                   static_cast<int>(seed->lane));
    dvrHelperThread.virBuffer.push_back(std::move(dyn_uop));
    auto &issued_dyn_uop = dvrHelperThread.virBuffer.back();
    ++cpuStats.dvrHelperDynUopsDecoded;
    issued_dyn_uop.state = DVRHelperThread::DVRDynUop::State::Issued;
    ++cpuStats.dvrHelperDynUopsIssued;
    dvrHelperThread.issueReplayChunk(ready_tick);
    --slots;
    ++cpuStats.dvrHelperUopsIssued;
    ++cpuStats.dvrHelperFUGrants;
    ++dvrHelperComputeIssuesThisCycle;
    cpuStats.dvrVectorLatencyCycles += static_cast<uint64_t>(latency);
    if (is_add)
        ++cpuStats.dvrVectorAddChunkIssues;
    if (is_shift)
        ++cpuStats.dvrVectorShiftChunkIssues;
    else if (uop.semantic == DVRInstructionRecorder::Uop::Semantic::Multiply ||
             uop.semantic == DVRInstructionRecorder::Uop::Semantic::MultiplyWord)
        ++cpuStats.dvrVectorMultiplyChunkIssues;
    else
        ++cpuStats.dvrVectorALUChunkIssues;

    // Rename a vector destination exactly once for this dynamic uop.  The
    // old source names were retained above, so destination==source and WAW
    // cases preserve the old values until this VIR copy retires.
    bool source_vectorized = false;
    if (seed->helperRegs) {
        for (Lane *lane : group) {
            source_vectorized |= uop.source0 >= 0 &&
                seed->helperRegs->isVectorized(uop.source0, lane->lane);
            source_vectorized |= uop.source1 >= 0 &&
                seed->helperRegs->isVectorized(uop.source1, lane->lane);
        }
    }
    // Paper 4.2.1 requires a vector destination when either a source is
    // already vectorized or the current lane group is on a divergent path.
    // The latter matters for control-only diamonds whose arithmetic inputs
    // remain scalar but whose results differ by lane.
    bool control_divergence = false;
    for (Lane *lane : group)
        control_divergence |= lane->simtDivergent;
    const bool destination_vectorized = uop.destination > 0 &&
        uop.destination < DVRLoopBoundDetector::MaxArchitecturalIntRegs &&
        seed->helperRegs &&
        seed->helperRegs->isVectorized(uop.destination);
    const bool vector_destination = uop.destination > 0 &&
        uop.destination < DVRLoopBoundDetector::MaxArchitecturalIntRegs &&
        seed->helperRegs && (source_vectorized || control_divergence) &&
        !destination_vectorized;
    // A scalar instruction that overwrites a vectorized architectural
    // register must demote it to a fresh scalar mapping.  Otherwise the
    // scalar result would overwrite the vector bundle in VRAT.
    const bool scalar_overwrite = uop.destination > 0 &&
        uop.destination < DVRLoopBoundDetector::MaxArchitecturalIntRegs &&
        seed->helperRegs && !vector_destination &&
        seed->helperRegs->isVectorized(uop.destination);
    // Rename once for the complete active path, not once per eight-lane
    // VIR copy.  Different paths receive different keys and therefore get
    // independent masked mappings.
    std::array<uint64_t, 2> rename_mask = {};
    for (auto &candidate : dvrHelperThread.replayLanes) {
        if (!candidate.active || candidate.reconvergence != reconvergence ||
            candidate.program != seed->program ||
            candidate.uopIndex != seed->uopIndex ||
            candidate.lanePC != uop.pc ||
            candidate.reconvergenceBlocked ||
            candidate.simtPath != seed->simtPath)
            continue;
        rename_mask[candidate.lane / 64] |=
            uint64_t(1) << (candidate.lane % 64);
    }
    if (rename_mask[0] == 0 && rename_mask[1] == 0) {
        for (Lane *lane : group)
            rename_mask[lane->lane / 64] |=
                uint64_t(1) << (lane->lane % 64);
    }
    const uint64_t mask_hash = rename_mask[0] ^
        ((rename_mask[1] << 17) | (rename_mask[1] >> 47));
    const uint64_t rename_key = (static_cast<uint64_t>(uop.pc) << 8) ^
        static_cast<uint64_t>(uop.destination & 0xff) ^
        mask_hash ^ (static_cast<uint64_t>(seed->simtPath) << 56);
    const bool first_destination_rename =
        (vector_destination || scalar_overwrite) && reconvergence &&
        reconvergence->renamedDestinations.insert(rename_key).second;
    if (first_destination_rename) {
        const auto old_physical =
            seed->helperRegs->physicalMappings(uop.destination);
        bool renamed = false;
        if (vector_destination) {
            if (control_divergence && !source_vectorized)
                ++cpuStats.dvrVRATControlDivergenceAllocations;
            renamed = seed->helperRegs->renameVectorMasked(
                uop.destination, rename_mask);
        } else {
            renamed = seed->helperRegs->renameScalarMasked(
                uop.destination, rename_mask) >= 0;
        }
        if (!renamed) {
            ++cpuStats.dvrVIRSourceValueSemanticFailures;
            for (Lane *lane : group)
                lane->active = false;
        } else {
            // Only names no longer referenced by any lane are dead.  A
            // deferred path may still legitimately retain part of the old
            // mapping after this masked rename.
            const auto current_physical =
                seed->helperRegs->physicalMappings(uop.destination);
            std::vector<int16_t> dead_physical;
            for (const auto old : old_physical)
                if (std::find(current_physical.begin(),
                              current_physical.end(), old) ==
                    current_physical.end())
                    dead_physical.push_back(old);
            dvrHelperThread.markDeadSources(
                seed->helperRegs, dead_physical);
        }
        assert(seed->helperRegs->conservationValid());
    }
    for (Lane *lane : group) {
        const auto &lane_uop = lane->program->uops[lane->uopIndex];
        const RegVal source0 = lane_uop.source0 >= 0 &&
            lane_uop.source0 < DVRLoopBoundDetector::MaxArchitecturalIntRegs ?
            (lane->helperRegs ?
             lane->helperRegs->read(lane_uop.source0, lane->lane) :
             lane->regs[lane_uop.source0]) : 0;
        const RegVal source1 = lane_uop.source1 >= 0 &&
            lane_uop.source1 < DVRLoopBoundDetector::MaxArchitecturalIntRegs ?
            (lane->helperRegs ?
             lane->helperRegs->read(lane_uop.source1, lane->lane) :
             lane->regs[lane_uop.source1]) : 0;
        RegVal result = 0;
        bool valid = true;
        if (lane_uop.conditional) {
            // The full branch group was committed after FU admission above.
            // Its helper-uop budget was charged once for every lane during
            // the group-wide commit; charging the selected VIR copy again
            // would make the budget depend on the eight-lane issue grouping.
            continue;
        }
        valid = lane_uop.evaluate(source0, source1, result);
        if (!valid) {
            lane->active = false;
            lane->termination = Lane::TerminationReason::External;
            ++cpuStats.dvrVIRSourceValueSemanticFailures;
            continue;
        }

        const bool is_load = lane_uop.load;
        const bool effective_tainted = lane_uop.tainted ||
            (lane->helperRegs &&
             ((lane_uop.source0 >= 0 &&
               lane->helperRegs->isVectorized(lane_uop.source0,
                                               lane->lane)) ||
              (lane_uop.source1 >= 0 &&
               lane->helperRegs->isVectorized(lane_uop.source1,
                                               lane->lane))));
        // Only a load whose address is tainted by the trigger is a
        // dependent replay target.  Earlier loads in the captured slice can
        // be ordinary helper loads used to materialize the address; treating
        // the first load as the target prematurely terminates every lane and
        // collapses all requests onto that common base address.
        if (is_load && !effective_tainted) {
            // This is a non-dependent helper load in the captured prefix.
            // Its demand value is not available to the replay interpreter;
            // skip it and continue through the recorded address chain.
            lane->lanePC = lane->uopIndex + 1 < lane->program->count ?
                lane->program->uops[lane->uopIndex + 1].pc : 0;
            lane->uopIndex = findPC(lane->program, lane->lanePC);
            ++lane->helperUops;
            if (lane->lanePC == 0 || lane->helperUops >= dvrHelperMaxUops) {
                lane->active = false;
                lane->termination = lane->helperUops >= dvrHelperMaxUops ?
                    Lane::TerminationReason::Timeout :
                    Lane::TerminationReason::External;
            }
            continue;
        }
        if (is_load && effective_tainted) {
            if (lane->nested) {
                const Addr line = dvrPrefetchLine(result);
                dvrTraceDependency("nested_replay_load", curTick(),
                    lane->triggerPC, lane_uop.pc, result,
                    static_cast<int>(lane->lane),
                    static_cast<int>(line));
            }
            if (result == 0 || result >= (Addr(1) << 47)) {
                lane->active = false;
                lane->termination = Lane::TerminationReason::External;
                ++cpuStats.dvrPredicateMisses;
                continue;
            }
            if (lane->simtPath == 1) {
                ++cpuStats.dvrSIMTTakenDependentLoads;
                dvrTraceDependency("simt_taken_dependent_load", curTick(),
                    lane->triggerPC, lane_uop.pc, result, 1,
                    static_cast<int>(lane->lane));
            } else if (lane->simtPath == 2) {
                ++cpuStats.dvrSIMTNotTakenDependentLoads;
                dvrTraceDependency("simt_not_taken_dependent_load", curTick(),
                    lane->triggerPC, lane_uop.pc, result, 1,
                    static_cast<int>(lane->lane));
            }
            // NDM flattening can combine invocations with different learned
            // relation sets.  Validate against the exact replay load PC
            // first, rather than applying the aggregate sender predicate to
            // every flattened lane.
            bool relation_match = lane->relationCount == 0;
            const auto exact_relation = dvrAddressRelations.find(
                DVRRelationKey{lane->triggerPC, lane_uop.pc});
            if (exact_relation != dvrAddressRelations.end() &&
                exact_relation->second.trained) {
                const auto &relation = exact_relation->second;
                const RegVal trigger_value = lane->helperRegs ?
                    lane->helperRegs->read(
                        seed->program->triggerDestination, lane->lane) :
                    lane->regs[seed->program->triggerDestination];
                const bool mask_match =
                    (trigger_value & relation.stableMask) ==
                    (relation.pattern & relation.stableMask);
                const int64_t expected = relation.scale *
                    static_cast<int64_t>(trigger_value) + relation.offset;
                relation_match = mask_match &&
                    expected == static_cast<int64_t>(result) &&
                    result >= relation.minAddress &&
                    result <= relation.maxAddress;
            } else {
                // The replayed load address is already computed from the
                // recorded dependency slice.  An aggregate relation may
                // belong to a different load PC (or be absent for pointer
                // chasing), so do not reject this exact semantic result just
                // because no affine training record exists.
                relation_match = true;
                for (unsigned relation = 0; relation < lane->relationCount;
                     ++relation) {
                    if (lane->relationCount > 1 &&
                        ((lane->helperRegs ?
                          lane->helperRegs->read(
                              seed->program->triggerDestination, lane->lane) :
                          lane->regs[seed->program->triggerDestination]) &
                         lane->masks[relation]) != lane->patterns[relation])
                        continue;
                    const int64_t expected = lane->scales[relation] *
                        static_cast<int64_t>(
                            lane->helperRegs ?
                            lane->helperRegs->read(
                                seed->program->triggerDestination, lane->lane) :
                            lane->regs[seed->program->triggerDestination]) +
                        lane->offsets[relation];
                    if (expected == static_cast<int64_t>(result)) {
                        relation_match = true;
                        break;
                    }
                }
            }
            // A valid recorded replay has computed this address through the
            // actual trigger-to-FLR dependency chain.  The aggregate affine
            // relation is only a legacy fallback/predicate aid; it can differ
            // across NDM invocations and can also alias across ordinary
            // discovery generations.  It must not veto the semantic replay.
            if (!relation_match && lane->program && lane->program->valid) {
                // Once a valid recorded trigger-to-load program exists, its
                // per-lane computed address is authoritative.  The affine
                // relation is only a launch predicate; using its historical
                // min/max range here would reject pointer-chasing values
                // such as BFS's 0x1197a load even though the replay chain
                // produced a valid address for this lane.
                relation_match = true;
            }
            if (!relation_match) {
                lane->active = false;
                lane->termination = Lane::TerminationReason::External;
                ++cpuStats.dvrPredicateMisses;
                continue;
            }
            if (!dvrEnableDependentPrefetch) {
                lane->active = false;
                lane->termination = Lane::TerminationReason::FLR;
                continue;
            }
            if (result < 4096 || result >= (Addr(1) << 47)) {
                lane->active = false;
                lane->termination = Lane::TerminationReason::External;
                ++cpuStats.dvrPredicateMisses;
                continue;
            }
            const Addr line = dvrPrefetchLine(result);
            const bool final_load = lane->finalLoadPC == 0 ||
                lane_uop.pc == lane->finalLoadPC;
            // Every tainted load before FLR is an address-producing load:
            // issue it as a gather and return its value to this same lane.
            // The FLR also returns a value when the paper requires replay to
            // continue through a branch after FLR.
            const bool value_response = !final_load ||
                lane->continuePastFLR;
            if (bfs_debug_generation && lane_uop.pc == 0x14598 &&
                value_response) {
                ++cpuStats.dvrDebugContinuedPastFLR;
                dvrTraceVector("debug_continued_past_flr", curTick(),
                               lane_uop.pc, result, 1,
                               static_cast<int>(lane->lane));
            }
            if ((value_response ||
                 (dvrQueuedDependentLines.count(line) == 0 &&
                  dvrDependentOutstandingLines.count(line) == 0 &&
                  dvrDependentCompletedLines.count(line) == 0)) &&
                dvrPrefetchQueue.size() < DvrMaxQueuedPrefetches) {
                DVRPrefetchAddress dependent;
                dependent.address = static_cast<Addr>(result);
                dependent.pc = lane_uop.pc;
                dependent.tid = lane->tid;
                dependent.readyTick = ready_tick;
                dependent.source = false;
                dependent.valueResponse = value_response;
                dependent.nested = lane->nested;
                dependent.relationCount = lane->relationCount;
                dependent.scales = lane->scales;
                dependent.offsets = lane->offsets;
                dependent.masks = lane->masks;
                dependent.patterns = lane->patterns;
                dependent.replay = lane->program;
                dependent.helperRegs = lane->helperRegs;
                dependent.predicate = lane->predicate;
                dependent.lane = lane->lane;
                dvrPrefetchQueue.push_front(dependent);
                dvrQueuedPrefetchAddresses.insert(result);
                dvrQueuedDependentLines.insert(line);
                updateDVRPrefetchQueuePeak();
                ++cpuStats.dvrDependentPrefetchesGenerated;
                ++cpuStats.dvrReplayTargetsGenerated;
                ++cpuStats.dvrVectorizerDependentLanes;
                if (lane_uop.alternatePath || executingAlternatePath(*lane)) {
                    ++cpuStats.dvrAlternatePathDependentTargets;
                    dvrAlternateDependentLines.insert(line);
                    // Trace only admitted alternate-path work.  This keeps
                    // the compact trace count aligned with the strict
                    // alternate-uop accounting instead of counting every
                    // lane in a shared VIR issue group.
                    dvrTraceVector("alternate_path_uop", curTick(),
                        lane_uop.pc, 0, 1,
                        static_cast<int>(lane->lane));
                    dvrTraceDependency("alternate_replay_target", curTick(),
                        lane->triggerPC, lane_uop.pc, result,
                        lane->predicate ? 1 : 0, lane->lane);
                }
                if (lane->nested) {
                    ++cpuStats.dvrNestedReplayTargetsGenerated;
                    ++cpuStats.dvrNestedDependentGenerated;
                }
                ++cpuStats.dvrPredicateSelections;
                dvrTraceDependency("replay_target", curTick(),
                    lane->triggerPC, lane_uop.pc, result,
                    lane->predicate ? 1 : 0, lane->lane);
            } else {
                ++cpuStats.dvrPrefetchesDeduplicated;
                if (lane->nested) {
                    dvrTraceDependency("nested_replay_dedup", curTick(),
                        lane->triggerPC, lane_uop.pc, result,
                        static_cast<int>(lane->lane),
                        static_cast<int>(line));
                }
            }
            // A normal chain ends at its first tainted load.  When discovery
            // recorded a conditional between FLR and the loop-control
            // reconvergence point, the paper requires the helper to keep the
            // load result in the lane state and execute both paths.  Ending
            // here would kill the alternate path before it can issue its
            // dependent load and would make stride-PC termination impossible.
            if (!value_response) {
                if (lane_uop.alternatePath &&
                    lane_uop.alternateResumePC != 0)
                    ++cpuStats.dvrReconvergenceResumeSuccesses;
                lane->active = false;
                lane->termination = Lane::TerminationReason::FLR;
                continue;
            }
            // A value-returning dependent request owns the lane until its
            // response arrives.  Park it outside the current SIMT group;
            // resumeDependentLane() restores the load destination and
            // successor PC.  In particular, do not write the computed
            // address into the load destination here.
            lane->lanePC = 0;
            continue;
        }

        if (lane_uop.destination >= 0 &&
            lane_uop.destination < DVRLoopBoundDetector::MaxArchitecturalIntRegs) {
            // A destination fed by a vectorized source gets the paper's
            // 16-copy VRAT mapping before its first lane result is written.
            // The trigger load is already vectorized during helper launch.
            lane->regs[lane_uop.destination] = result;
            lane->readyCycle[lane_uop.destination] = ready_tick;
            if (lane->helperRegs)
                lane->helperRegs->write(
                    lane_uop.destination, lane->lane, result, ready_tick);
            ++cpuStats.dvrHelperVRATWrites;
        }
        lane->regs[0] = 0;
        ++lane->helperUops;
        // A cached direct JAL/C.J must use its validated target rather than
        // incorrectly falling through into a different suffix.
        if (lane_uop.alternatePath && lane_uop.alternateResumePC != 0) {
            lane->lanePC = lane_uop.alternateResumePC;
        } else {
            lane->lanePC = lane_uop.control && !lane_uop.conditional &&
            lane_uop.branchTargetPC != 0 ? lane_uop.branchTargetPC :
            (lane->uopIndex + 1 < lane->program->count ?
             lane->program->uops[lane->uopIndex + 1].pc : 0);
        }
        if (lane->lanePC == 0 || lane->helperUops >= dvrHelperMaxUops) {
            lane->active = false;
            lane->termination = lane->helperUops >= dvrHelperMaxUops ?
                Lane::TerminationReason::Timeout :
                Lane::TerminationReason::External;
        } else
            lane->uopIndex = findPC(lane->program, lane->lanePC);
    }
    issued_dyn_uop.completeCycle = ready_tick;
    // Completion is accounted for by retireCompletedVIR() at completeCycle;
    // the uop remains in the VIR while its modeled FU latency elapses.
    // Account for every lane that terminated in this issue group.  The path
    // tag lets CC diagnostics distinguish a normal/unsupported exit from a
    // path that actually reached a dependent load (counted above).
    for (Lane *lane : group) {
        if (lane->active)
            continue;
        if (lane->simtPath == 1)
            ++cpuStats.dvrSIMTTakenPathTerminations;
        else if (lane->simtPath == 2)
            ++cpuStats.dvrSIMTNotTakenPathTerminations;
    }
    retireTerminatedLanes(reconvergence);
    refreshCurrentGroup(reconvergence);
    if (reconvergence) {
        unsigned active_lanes = 0;
        const auto context_it = dvrHelperThread.replayContextLanes.find(
            reconvergence.get());
        if (context_it != dvrHelperThread.replayContextLanes.end()) {
            for (const auto *lane : context_it->second)
                active_lanes += lane->active ? 1 : 0;
        }
        // A VIR issue consumes the helper's one ready credit.  Keep the
        // generation in the context-level ready queue after its current-PC
        // group advances; otherwise the context can retain active lanes and
        // a valid current mask but never receive another scheduler turn.
        if (active_lanes != 0)
            dvrHelperThread.activateReplayContext(reconvergence);
        if (seed && seed->triggerPC == 0x11970)
            dvrTraceVector("bfs_replay_refresh", curTick(),
                           reconvergence->currentPC,
                           reconvergence->currentMask[0], active_lanes,
                           static_cast<int>(reconvergence->depth));
    }
    return 1;
}

unsigned
CPU::issueDVRHelperCompute()
{
    unsigned slots = dvrDecoupledIssue ? DvrHelperIssueWidth : dvrIssueWidth;
    if (!dvrDecoupledIssue) {
        if (dvrMainIssuesThisCycle >= slots)
            return 0;
        slots -= dvrMainIssuesThisCycle;
    }
    if (dvrHelperIssuesThisCycle >= slots)
        return 0;
    slots -= dvrHelperIssuesThisCycle;

    if (dvrVectorChunkModel) {
        cpuStats.dvrHelperUopsBecameReady +=
            dvrHelperThread.refillComputeReady();
        if (!dvrHelperThread.replayReadyContexts.empty())
            return issueDVRReplayLanes(slots);
        dvrHelperThread.refillIssueQueue();
        if (dvrHelperThread.issueQueue.empty())
            return 0;
        if (dvrHelperThread.readyUops == 0 ||
            !dvrHelperThread.issueQueueReady(curTick())) {
            ++cpuStats.dvrHelperIssueQueueStalls;
            if (dvrHelperThread.issueQueueScoreboardBlocked(curTick())) {
                ++cpuStats.dvrHelperScoreboardWaitCycles;
            }
            return 0;
        }

        const auto kind = dvrHelperThread.issueQueue.front().kind;
        const OpClass op_class = kind ==
                DVRHelperThread::ComputeKind::Add ? SimdAddOp :
            kind == DVRHelperThread::ComputeKind::Alu ? SimdAluOp :
            kind == DVRHelperThread::ComputeKind::Shift ? SimdShiftOp :
                SimdMultOp;
        ++cpuStats.dvrHelperFURequests;
        ++cpuStats.dvrVectorChunkRequests;
        Cycles latency(1);
        const bool interval_ready = dvrVectorUnlimitedFU ||
            curTick() >= dvrVectorNextIssueTick;
        const bool granted = interval_ready &&
            (dvrVectorUnlimitedFU || iew.tryIssueDVRHelperFU(op_class, latency));
        if (!granted) {
            ++cpuStats.dvrHelperFUStalls;
            ++cpuStats.dvrVectorFUConflictCycles;
            return 0;
        }

        const Tick ready_tick = curTick() +
            static_cast<uint64_t>(latency) * clockPeriod();
        if (!dvrVectorUnlimitedFU) {
            dvrVectorNextIssueTick = curTick() +
                uint64_t(std::max(1u, dvrVectorIssueInterval)) * clockPeriod();
        }
        dvrHelperThread.issueCompute(ready_tick);
        --slots;
        ++dvrHelperComputeIssuesThisCycle;
        ++cpuStats.dvrHelperFUGrants;
        ++cpuStats.dvrHelperUopsIssued;
        cpuStats.dvrVectorLatencyCycles +=
            static_cast<uint64_t>(latency);
        if (kind == DVRHelperThread::ComputeKind::Add)
            ++cpuStats.dvrVectorAddChunkIssues;
        if (kind == DVRHelperThread::ComputeKind::Alu ||
            kind == DVRHelperThread::ComputeKind::Add)
            ++cpuStats.dvrVectorALUChunkIssues;
        else if (kind == DVRHelperThread::ComputeKind::Shift)
            ++cpuStats.dvrVectorShiftChunkIssues;
        else
            ++cpuStats.dvrVectorMultiplyChunkIssues;
        return 1;
    }

    unsigned issued = 0;
    auto issue_class = [&](unsigned &remaining, OpClass op_class) {
        while (remaining != 0 && slots != 0) {
            ++cpuStats.dvrHelperFURequests;
            Cycles latency(1);
            const bool granted = iew.tryIssueDVRHelperFU(op_class, latency);
            if (!granted) {
                ++cpuStats.dvrHelperFUStalls;
                break;
            }
            --remaining;
            --slots;
            ++issued;
            ++dvrHelperComputeIssuesThisCycle;
            ++cpuStats.dvrHelperFUGrants;
            ++cpuStats.dvrHelperUopsIssued;
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
    // A vector gather is a stream of independent scalar requests.  Issue
    // several requests per cycle when the shared LSU and helper budgets allow
    // it; the request-level routine stops on backpressure or an unready head.
    unsigned attempts = 0;
    while (!dvrPrefetchQueue.empty() &&
           dvrHelperIssuesThisCycle < DvrHelperIssueWidth &&
           attempts++ < DvrHelperIssueWidth) {
        const unsigned issued_before = dvrHelperIssuesThisCycle;
        serviceDVRPrefetchRequest();
        if (dvrHelperIssuesThisCycle == issued_before)
            break;
    }
}

void
CPU::serviceDVRPrefetchRequest()
{
    if (dvrPrefetchQueue.empty())
        return;

    // Each dependent request carries the completion time of the helper
    // DynUop that produced its address.  Source requests have readyTick=0
    // and can use the LSU immediately; unrelated lanes are not serialized
    // by another lane's vector computation.
    if (dvrPrefetchQueue.front().readyTick > curTick())
        return;
    // Source and dependent requests already admitted to the helper LQ carry
    // their own dependency state.  Do not serialize the whole memory queue
    // behind the latest vector compute completion; only the lane/uop that
    // produced a dependent address must wait for its own readiness.
    cpuStats.dvrHelperUopsBecameReady +=
        dvrHelperThread.wakeForMemoryRequest();
    // The request's readyTick is the helper-LSU dependency check.  A source
    // has readyTick=0; a dependent target receives the completion tick of
    // its vector producer.  Once that per-request condition holds, unrelated
    // lanes still present in the helper must not block this memory uop.
    if (!dvrHelperThread.canIssue(true))
        return;
    if (dvrHelperIssuesThisCycle >= DvrHelperIssueWidth) {
        ++cpuStats.dvrResourceConflicts;
        ++cpuStats.dvrIssueBudgetConflicts;
        return;
    }
    if (!dvrDecoupledIssue) {
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
    }
    // Decoupled issue separates helper compute scheduling from the main
    // issue-width budget, but memory requests still share the LSU/data port.
    // Main-thread LSU operations were recorded first in IEW, so this retains
    // strict main-thread priority in both issue modes.
    if (dvrMainLSUIssuesThisCycle + dvrHelperLSUIssuesThisCycle >=
            dvrLSUWidth) {
        ++cpuStats.dvrResourceConflicts;
        ++cpuStats.dvrLSUBudgetConflicts;
        return;
    }
    const auto prefetch = dvrPrefetchQueue.front();
    if (dvrHelperLoadQueueOccupancy >= DvrHelperLoadQueueCapacity) {
        ++cpuStats.dvrHelperVIRCapacityStalls;
        return;
    }
    if (!iew.hasFreeDVRHelperLoadEntry(prefetch.tid)) {
        ++cpuStats.dvrPrefetchesSuppressedMainThread;
        return;
    }

    dvrPrefetchQueue.pop_front();
    dvrQueuedPrefetchAddresses.erase(prefetch.address);
    if (!prefetch.source)
        dvrQueuedDependentLines.erase(dvrPrefetchLine(prefetch.address));
    if (prefetch.source && prefetch.predicate &&
        prefetch.predicate->sourceTranslationFaulted) {
        // A contiguous source stream has already crossed an unmapped page;
        // mask the not-yet-issued lanes instead of probing the same invalid
        // suffix repeatedly.
        ++cpuStats.dvrPrefetchesDropped;
        retireDVRPredicateLane(prefetch.predicate, prefetch.lane, false);
        return;
    }
    const uint64_t helper_load_id = dvrNextHelperLoadId++;
    dvrHelperLoadEntries.emplace(helper_load_id, DVRHelperLoadEntry{
        prefetch.address, 0, prefetch.pc, prefetch.lane, prefetch.tid,
        prefetch.source, prefetch.nested, DVRHelperLoadState::Allocated,
        curTick(), 0, 0});
    ++cpuStats.dvrHelperLoadEntriesAllocated;
    cpuStats.dvrHelperLoadEntryPending = dvrHelperLoadEntries.size();
    dvrHelperLoadEntryPeakValue = std::max<uint64_t>(
        dvrHelperLoadEntryPeakValue, dvrHelperLoadEntries.size());
    cpuStats.dvrHelperLoadEntryPeak = dvrHelperLoadEntryPeakValue;
    auto finish_helper_entry = [&](DVRHelperLoadState state) {
        auto entry = dvrHelperLoadEntries.find(helper_load_id);
        if (entry == dvrHelperLoadEntries.end())
            return;
        entry->second.state = state;
        if (state == DVRHelperLoadState::TranslationFault)
            ++cpuStats.dvrHelperLoadEntryFaults;
        else if (state == DVRHelperLoadState::Retry)
            ++cpuStats.dvrHelperLoadEntryRetries;
        else if (state == DVRHelperLoadState::Dropped)
            ++cpuStats.dvrHelperLoadEntryDropped;
        dvrHelperLoadEntries.erase(entry);
        cpuStats.dvrHelperLoadEntryPending = dvrHelperLoadEntries.size();
    };
    const unsigned request_bytes = dvrPrefetchBytes(prefetch);
    const unsigned line_offset = prefetch.address & (cacheLineSize() - 1);
    if (line_offset + request_bytes > cacheLineSize()) {
        // Helper packets bypass the architectural LSQ load-splitting path.
        // Never submit an instruction-sized scalar or dependent load that
        // crosses a cache line; it would violate BaseCache's single-block
        // packet contract.  A later aligned lane can still provide coverage.
        ++cpuStats.dvrPrefetchesDropped;
        finish_helper_entry(DVRHelperLoadState::Dropped);
        if (prefetch.source)
            retireDVRPredicateLane(prefetch.predicate, prefetch.lane, false);
        return;
    }
    Request::Flags flags;
    flags.set(Request::PREFETCH | Request::DVR_PREFETCH);
    flags.set(prefetch.source ? Request::DVR_SOURCE : Request::DVR_DEPENDENT);
    RequestPtr req = std::make_shared<Request>(
        prefetch.address, request_bytes,
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
        // Keep the exact virtual address and provenance available in the
        // workload trace.  A speculative fault can be a real out-of-range
        // lane, a bad dependent value, or a valid high-address stack lane;
        // aggregate counters alone cannot distinguish these cases.
        dvrTraceDependency("translation_fault", curTick(), prefetch.pc,
                           prefetch.pc, prefetch.address,
                           prefetch.source ? 1 : 0, prefetch.lane);
        if (prefetch.source && prefetch.predicate)
            prefetch.predicate->sourceTranslationFaulted = true;
        finish_helper_entry(DVRHelperLoadState::TranslationFault);
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
    const bool value_response = !prefetch.source &&
        (prefetch.valueResponse ||
         (prefetch.replay && prefetch.replay->continuePastFLR));
    PacketPtr pkt = new Packet(
        req, (prefetch.source || value_response) ?
            MemCmd::ReadReq : MemCmd::SoftPFReq);
    pkt->allocate();
    auto *sender_state = new DVRPrefetchSenderState(
        prefetch.source, prefetch.nested, prefetch.oracle,
        prefetch.relationCount, prefetch.scales,
        prefetch.offsets, prefetch.masks, prefetch.patterns, prefetch.replay,
        prefetch.helperRegs,
        prefetch.predicate, prefetch.lane, prefetch.tid);
    sender_state->helperLoadId = helper_load_id;
    sender_state->issueTick = curTick();
    sender_state->valueResponse = value_response;
    pkt->senderState = sender_state;
    auto &port = iew.ldstQueue.getDataPort();
    if (!port.tryTiming(pkt)) {
        ++cpuStats.dvrPrefetchesSuppressedMainThread;
        finish_helper_entry(DVRHelperLoadState::Retry);
        // Main-thread priority is arbitration, not a reason to lose the
        // helper request.  Retain the bounded queue entry and retry it on a
        // later cycle; otherwise dependent replay is systematically erased
        // by ordinary LSU traffic.
        dvrPrefetchQueue.push_front(prefetch);
        dvrQueuedPrefetchAddresses.insert(prefetch.address);
        if (!prefetch.source && !prefetch.oracle)
            dvrQueuedDependentLines.insert(dvrPrefetchLine(prefetch.address));
        delete pkt->senderState;
        pkt->senderState = nullptr;
        delete pkt;
        return;
    }
    if (!port.sendTimingReq(pkt)) {
        ++cpuStats.dvrPrefetchesRejectedBackpressure;
        finish_helper_entry(DVRHelperLoadState::Retry);
        dvrPrefetchQueue.push_front(prefetch);
        dvrQueuedPrefetchAddresses.insert(prefetch.address);
        if (!prefetch.source && !prefetch.oracle)
            dvrQueuedDependentLines.insert(dvrPrefetchLine(prefetch.address));
        delete pkt->senderState;
        pkt->senderState = nullptr;
        delete pkt;
        return;
    }
    if (auto entry = dvrHelperLoadEntries.find(helper_load_id);
        entry != dvrHelperLoadEntries.end()) {
        entry->second.physicalAddress = req->getPaddr();
        entry->second.issueTick = curTick();
        entry->second.state = DVRHelperLoadState::WaitingResponse;
    }
    ++cpuStats.dvrPrefetchesIssued;
    ++dvrHelperLoadQueueOccupancy;
    ++dvrHelperIssuesThisCycle;
    ++dvrHelperLSUIssuesThisCycle;
    ++cpuStats.dvrHelperIssueCycles;
    ++cpuStats.dvrHelperUopsIssued;
    dvrHelperThread.issue();
    dvrQualityTracker.issued(
        reinterpret_cast<uintptr_t>(pkt), dvrPrefetchLine(req->getPaddr()),
        pkt->getSize(), curTick());
    cpuStats.dvrQualityIssuedBytes += pkt->getSize();
    ++dvrOutstandingPrefetchLines[dvrPrefetchLine(prefetch.address)];
    dvrOutstandingPrefetchAddresses.insert(prefetch.address);
    if (!prefetch.source) {
        if (prefetch.oracle)
            ++cpuStats.oraclePrefetchesIssued;
        else
            ++cpuStats.dvrDependentPrefetchesIssued;
        if (!prefetch.oracle)
            ++dvrDependentOutstandingLines[dvrPrefetchLine(prefetch.address)];
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
    auto helper_entry = dvrHelperLoadEntries.find(state->helperLoadId);
    if (helper_entry != dvrHelperLoadEntries.end()) {
        helper_entry->second.state = DVRHelperLoadState::Writeback;
        helper_entry->second.responseTick = curTick();
        ++cpuStats.dvrHelperLoadEntryWritebacks;
    }
    assert(dvrHelperLoadQueueOccupancy != 0);
    --dvrHelperLoadQueueOccupancy;
    ++cpuStats.dvrPrefetchesCompleted;
    if (state->oracle) {
        ++cpuStats.oraclePrefetchesCompleted;
        oracleCompletedLines.insert(dvrPrefetchLine(pkt->req->getVaddr()));
    }
    dvrQualityTracker.completed(reinterpret_cast<uintptr_t>(pkt), curTick());
    cpuStats.dvrQualityCompletedBytes += pkt->getSize();
    // observeDVRLoad 使用架构有效虚拟地址，因此质量统计也使用请求虚拟地址。
    const Addr line = dvrPrefetchLine(pkt->req->getVaddr());
    dvrOutstandingPrefetchAddresses.erase(pkt->req->getVaddr());
    auto outstanding = dvrOutstandingPrefetchLines.find(line);
    if (outstanding != dvrOutstandingPrefetchLines.end()) {
        if (--outstanding->second == 0)
            dvrOutstandingPrefetchLines.erase(outstanding);
    }
    if (!state->source && !state->oracle) {
        auto dependent_outstanding = dvrDependentOutstandingLines.find(line);
        if (dependent_outstanding != dvrDependentOutstandingLines.end()) {
            if (--dependent_outstanding->second == 0) {
                dvrDependentOutstandingLines.erase(dependent_outstanding);
                dvrDependentCompletedLines.insert(line);
            }
        }
    }
    dvrCompletedPrefetchLines[line] = curTick();
    if (!state->source && state->valueResponse && pkt->hasData()) {
        RegVal raw = 0;
        const unsigned bytes = std::min<unsigned>(pkt->getSize(),
                                                  sizeof(RegVal));
        const uint8_t *data = pkt->getPtr<uint8_t>();
        for (unsigned index = 0; index < bytes; ++index)
            raw |= static_cast<RegVal>(data[index]) << (index * 8);
        RegVal value = raw;
        if (state->replay) {
            using Semantic = DVRInstructionRecorder::Uop::Semantic;
            for (unsigned index = 1; index < state->replay->count; ++index) {
                if (state->replay->uops[index].pc != pkt->req->getPC())
                    continue;
                switch (state->replay->uops[index].semantic) {
                  case Semantic::LoadByteSigned:
                    value = static_cast<RegVal>(static_cast<int64_t>(
                        static_cast<int8_t>(raw))); break;
                  case Semantic::LoadHalfSigned:
                    value = static_cast<RegVal>(static_cast<int64_t>(
                        static_cast<int16_t>(raw))); break;
                  case Semantic::LoadWordSigned:
                    value = static_cast<RegVal>(static_cast<int64_t>(
                        static_cast<int32_t>(raw))); break;
                  case Semantic::LoadWordUnsigned:
                    value = static_cast<RegVal>(
                        static_cast<uint32_t>(raw)); break;
                  case Semantic::LoadByteUnsigned:
                    value = static_cast<RegVal>(
                        static_cast<uint8_t>(raw)); break;
                  case Semantic::LoadHalfUnsigned:
                    value = static_cast<RegVal>(
                        static_cast<uint16_t>(raw)); break;
                  default:
                    break;
                }
                break;
            }
        }
        dvrTraceDependency("dependent_value", curTick(),
                           state->replay ? state->replay->triggerPC :
                           pkt->req->getPC(), pkt->req->getPC(), value,
                           0, state->lane);
        unsigned became_ready = 0;
        const bool resumed = dvrHelperThread.resumeDependentLane(
            *state, pkt->req->getPC(), value, became_ready);
        if (state->replay && state->replay->triggerPC == 0x11970 &&
            pkt->req->getPC() == 0x1197a) {
            auto *lane = dvrHelperThread.findReplayLane(*state);
            // taint=continuePastFLR, lanes=active, invocation=uop index;
            // address is the post-response replay cursor.  This single
            // event distinguishes a missing response, a failed lane match,
            // and a cursor that failed to advance beyond the FLR.
            dvrTraceDependency(
                resumed ? "bfs_flr_resume" : "bfs_flr_resume_miss",
                curTick(), 0x11970, pkt->req->getPC(),
                lane ? lane->lanePC : 0,
                lane && lane->continuePastFLR ? 1 : 0,
                lane ? static_cast<int>(lane->lane) :
                    static_cast<int>(state->lane));
            if (lane) {
                dvrTraceVector("bfs_flr_cursor", curTick(),
                               lane->lanePC, lane->uopIndex,
                               lane->active ? 1 : 0,
                               static_cast<int>(lane->lane));
            }
        }
        if (resumed) {
            cpuStats.dvrHelperUopsBecameReady += became_ready;
            ++cpuStats.dvrVIRContinuationResumes;
        }
    }
    if (state->source && pkt->hasData()) {
        ++cpuStats.dvrHelperLoadEntryWakeups;
        // Read exactly the architectural width requested by the trigger and
        // apply RISC-V load-value semantics before installing the value in
        // the helper VRAT.  Reading a fixed RegVal here would incorrectly
        // zero/sign extend LB/LH/LW/LWU responses from an eight-byte packet.
        RegVal raw = 0;
        const unsigned bytes = std::min<unsigned>(pkt->getSize(),
                                                  sizeof(RegVal));
        const uint8_t *data = pkt->getPtr<uint8_t>();
        for (unsigned index = 0; index < bytes; ++index)
            raw |= static_cast<RegVal>(data[index]) << (index * 8);
        RegVal value = raw;
        if (state->replay && state->replay->count != 0) {
            using Semantic = DVRInstructionRecorder::Uop::Semantic;
            switch (state->replay->uops[0].semantic) {
              case Semantic::LoadByteSigned:
                value = static_cast<RegVal>(static_cast<int64_t>(
                    static_cast<int8_t>(raw)));
                break;
              case Semantic::LoadByteUnsigned:
                value = static_cast<RegVal>(static_cast<uint8_t>(raw));
                break;
              case Semantic::LoadHalfSigned:
                value = static_cast<RegVal>(static_cast<int64_t>(
                    static_cast<int16_t>(raw)));
                break;
              case Semantic::LoadHalfUnsigned:
                value = static_cast<RegVal>(static_cast<uint16_t>(raw));
                break;
              case Semantic::LoadWordSigned:
                value = static_cast<RegVal>(static_cast<int64_t>(
                    static_cast<int32_t>(raw)));
                break;
              case Semantic::LoadWordUnsigned:
                value = static_cast<RegVal>(static_cast<uint32_t>(raw));
                break;
              case Semantic::LoadDouble:
                value = raw;
                break;
              default:
                break;
            }
        }
        bool helper_replay_enqueued = false;
        dvrTraceDependency("source_value", curTick(), pkt->req->getPC(),
                           pkt->req->getPC(), static_cast<Addr>(value),
                           0, state->lane);
        if (state->nested) {
            dvrTraceDependency(
                "nested_source_replay_meta", curTick(),
                state->replay ? state->replay->triggerPC :
                    pkt->req->getPC(), pkt->req->getPC(),
                state->replay ? state->replay->count : 0,
                state->replay && state->replay->valid ? 1 : 0,
                state->replay && state->replay->continuePastFLR ? 1 : 0);
        }
        retireDVRPredicateLane(state->predicate, state->lane, true, value);
        if (dvrVectorChunkModel && state->replay &&
            state->replay->count > 1) {
            if (state->replay->continuePastFLR)
                ++cpuStats.dvrReplayContinuePastFLRLanes;
            ++cpuStats.dvrReplayAttempts;
            if (state->nested)
                ++cpuStats.dvrNestedReplayAttempts;
            unsigned became_ready = 0;
            helper_replay_enqueued = dvrHelperThread.enqueueReplayLane(
                *state, value, became_ready);
            if (state->nested) {
                dvrTraceDependency(
                    "nested_replay_enqueue", curTick(),
                    state->replay ? state->replay->triggerPC :
                        pkt->req->getPC(), pkt->req->getPC(),
                    helper_replay_enqueued ? 1 : 0,
                    became_ready, state->lane);
            }
            cpuStats.dvrHelperUopsBecameReady += became_ready;
            ++cpuStats.dvrVIRContinuationResumes;
        }
        if (!helper_replay_enqueued && state->replay &&
            state->replay->count > 1 &&
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
            // Source-response continuation is the normal path for a
            // dependent chain.  Its SIMT branch results must contribute to
            // the same global divergence/reconvergence counters as the
            // initial VIR pass.
            cpuStats.dvrDivergentBranches += response.divergentBranches;
            cpuStats.dvrReconvergences += response.reconvergences;
            cpuStats.dvrVIRSourceValueBranches += response.divergentBranches;
            cpuStats.dvrVIRSourceValueExternalLanes +=
                response.externalPathLanes;
            cpuStats.dvrVIRSourceValueSemanticFailures +=
                response.unsupportedSemanticLanes;
            if (response.unsupportedSemanticLanes != 0) {
                dvrTraceVector(
                    "source_vir_unsupported", curTick(),
                    response.unsupportedSemanticPC,
                    response.unsupportedSemanticEncoding,
                    response.unsupportedSemanticLanes,
                    response.unsupportedSemantic);
            }
            cpuStats.dvrAlternatePathUopsReplayed +=
                response.alternatePathUops;
            cpuStats.dvrReconvergenceResumeSuccesses +=
                response.alternatePathReconvergences;
            cpuStats.dvrVIRSourceValueTerminations +=
                response.normalTerminatedLanes + response.earlyExitLanes;
            cpuStats.dvrVIRContinuationPCGroups += response.pcGroups;
            cpuStats.dvrVIRContinuationGroupedLanes += response.activeLanes;
            if (response.maxPCGroupLanes != 0)
                cpuStats.dvrVIRContinuationMaxGroupWidth =
                    response.maxPCGroupLanes;
        }
        bool matched = helper_replay_enqueued;
        if (!matched)
            matched = dvrEnableDependentPrefetch &&
                replayDVRSource(*state, value);
        // Once a recorded replay template exists, an invalid or rejected
        // template must terminate this lane.  Falling back to the older
        // affine relation here can emit an address unrelated to the current
        // dynamic chain and turns replay uncertainty into MMU faults.
        if (dvrEnableDependentPrefetch && !matched && !state->replay) {
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
                if (dependent.address < 4096 ||
                    dependent.address >= (Addr(1) << 47)) {
                    continue;
                }
                const Addr line = dvrPrefetchLine(dependent.address);
                if (dvrQueuedDependentLines.count(line) != 0 ||
                    dvrDependentOutstandingLines.count(line) != 0 ||
                    dvrDependentCompletedLines.count(line) != 0) {
                    ++cpuStats.dvrPrefetchesDeduplicated;
                    matched = true;
                    break;
                }
                dependent.pc = pkt->req->getPC();
                dependent.tid = state->tid;
                dependent.source = false;
                dependent.nested = state->nested;
                ++cpuStats.dvrDependentPrefetchesGenerated;
                dvrTraceVector("dependent_prefetch", curTick(),
                    dependent.pc, dependent.address, 1,
                    static_cast<int>(state->lane));
                if (dvrPrefetchQueue.size() >= DvrMaxQueuedPrefetches) {
                    ++cpuStats.dvrPrefetchesDropped;
                    continue;
                }
                // Dependent targets are the useful end of the chain.  Give
                // them priority over not-yet-issued source lanes while still
                // respecting the finite queue bound.
                dvrPrefetchQueue.push_front(dependent);
                dvrQueuedPrefetchAddresses.insert(dependent.address);
                dvrQueuedDependentLines.insert(line);
                updateDVRPrefetchQueuePeak();
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
    } else if (!state->oracle) {
        ++cpuStats.dvrDependentPrefetchesCompleted;
    }
    if (state->nested)
        ++cpuStats.dvrNestedHelpersCompleted;
    if (helper_entry != dvrHelperLoadEntries.end()) {
        helper_entry->second.state = DVRHelperLoadState::Completed;
        ++cpuStats.dvrHelperLoadEntriesCompleted;
        dvrHelperLoadEntries.erase(helper_entry);
        cpuStats.dvrHelperLoadEntryPending = dvrHelperLoadEntries.size();
    }
    dvrHelperThread.complete();
    if (!dvrHelperThread.active() && !dvrPrefetchQueue.empty()) {
        // Source responses can arrive after the captured program's frontend
        // has drained.  Keep dependent replay requests live by waking a
        // bounded memory-only continuation instead of leaving them stranded
        // behind an Idle helper state.
        DVRInstructionRecorder::ResourceCounts memory_only;
        startDVRHelper(pkt->req->getPC(), 1, 1, memory_only, nullptr,
                       state->tid);
    }
    delete state;
    pkt->senderState = nullptr;
    delete pkt;
}

void
CPU::startDVRHelper(
    Addr trigger_pc, unsigned program_uops, unsigned lanes,
    const DVRInstructionRecorder::ResourceCounts &resources,
    std::shared_ptr<const DVRReplayTemplate> replay, ThreadID tid)
{
    if (program_uops == 0 || lanes == 0)
        return;

    const unsigned work_units = dvrHelperWorkUnits(lanes);
    DVRInstructionRecorder::ResourceCounts helper_resources = resources;
    helper_resources.alu *= work_units;
    helper_resources.shift *= work_units;
    helper_resources.multiply *= work_units;
    helper_resources.lsu *= work_units;
    if (dvrHelperThread.active()) {
        dvrHelperThread.extend(trigger_pc, program_uops, work_units,
                               helper_resources, dvrVectorChunkModel, replay);
    } else {
        dvrHelperThread.begin(dvrNextHelperId++, trigger_pc, program_uops,
                              work_units, dvrHelperMaxUops, helper_resources,
                              dvrVectorChunkModel, replay, tid);
    }
    if (dvrVectorChunkModel)
        cpuStats.dvrVectorActiveLanes += lanes;
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
    if (!state.replay || !state.replay->valid ||
        state.replay->scalarCount <= 1)
        return false;

    ++cpuStats.dvrReplayAttempts;
    if (state.nested)
        ++cpuStats.dvrNestedReplayAttempts;
    auto regs = state.replay->initialRegs;
    regs[0] = 0;
    regs[state.replay->triggerDestination] = source_value;

    bool alternate_active = false;
    bool alternate_selected = false;
    Addr alternate_reconvergence = 0;
    unsigned index = 1;
    unsigned steps = 0;
    while (index < state.replay->scalarCount &&
           steps++ < state.replay->count * 2) {
        const auto &uop = state.replay->uops[index];
        if (uop.alternatePath)
            ++cpuStats.dvrAlternatePathUopsReplayed;
        if (uop.source0 >=
            DVRLoopBoundDetector::MaxArchitecturalIntRegs)
            return false;
        const RegVal source0 = uop.source0 >= 0 ? regs[uop.source0] : 0;
        RegVal source1 = 0;
        const bool uses_source1 =
            uop.semantic == DVRInstructionRecorder::Uop::Semantic::Add ||
            uop.semantic == DVRInstructionRecorder::Uop::Semantic::Sub ||
            uop.semantic == DVRInstructionRecorder::Uop::Semantic::And ||
            uop.semantic == DVRInstructionRecorder::Uop::Semantic::Or ||
            uop.semantic == DVRInstructionRecorder::Uop::Semantic::Xor ||
            uop.semantic == DVRInstructionRecorder::Uop::Semantic::ShiftLeft ||
            uop.semantic == DVRInstructionRecorder::Uop::Semantic::ShiftRightLogical ||
            uop.semantic == DVRInstructionRecorder::Uop::Semantic::ShiftRightArithmetic ||
            uop.semantic == DVRInstructionRecorder::Uop::Semantic::Multiply ||
            uop.semantic == DVRInstructionRecorder::Uop::Semantic::AddWord ||
            uop.semantic == DVRInstructionRecorder::Uop::Semantic::SubWord ||
            uop.conditional;
        if (uses_source1) {
            if (uop.conditional && uop.source1 < 0) {
                source1 = 0;
            } else if (uop.source1 < 0 ||
                uop.source1 >=
                    DVRLoopBoundDetector::MaxArchitecturalIntRegs)
                return false;
            else
                source1 = regs[uop.source1];
        }

        if (uop.conditional) {
            bool taken = uop.branchTaken;
            if (uop.tainted && !uop.evaluateBranch(source0, source1, taken))
                return false;
            const Addr next_pc = taken ? uop.branchTargetPC :
                                         uop.fallthroughPC;
            if (next_pc == 0)
                return false;
            const Addr captured_target = uop.branchTaken ?
                                          uop.branchTargetPC :
                                          uop.fallthroughPC;
            if (next_pc != captured_target) {
                alternate_active = true;
                alternate_selected = true;
                alternate_reconvergence = uop.reconvergencePC;
            }
            unsigned next_index = state.replay->count;
            for (unsigned candidate = 1; candidate < state.replay->count;
                 ++candidate) {
                if (state.replay->uops[candidate].pc == next_pc) {
                    next_index = candidate;
                    break;
                }
            }
            if (next_index == state.replay->count)
                return false;
            index = next_index;
            continue;
        }

        if (uop.control && !uop.conditional && uop.branchTargetPC != 0) {
            if (uop.intDestinations != 0)
                return false;
            const Addr next_pc = uop.branchTargetPC;
            if (alternate_active && alternate_reconvergence != 0 &&
                next_pc == alternate_reconvergence) {
                ++cpuStats.dvrReconvergenceResumeSuccesses;
                alternate_active = false;
                alternate_reconvergence = 0;
            }
            unsigned next_index = state.replay->count;
            for (unsigned candidate = 1; candidate < state.replay->count;
                 ++candidate) {
                if (state.replay->uops[candidate].pc == next_pc) {
                    next_index = candidate;
                    break;
                }
            }
            if (next_index == state.replay->count)
                return false;
            index = next_index;
            continue;
        }

        // Stores, fences and other unsupported operations with no integer
        // destination cannot change a later replay address.  They are
        // intentionally omitted from the prefetch-only helper path.
        if (uop.semantic == DVRInstructionRecorder::Uop::Semantic::Unsupported &&
            !uop.control && uop.intDestinations == 0) {
            ++index;
            continue;
        }
        RegVal result = 0;
        if (!uop.evaluate(source0, source1, result))
            return false;
        const bool is_replayed_load =
            uop.semantic == DVRInstructionRecorder::Uop::Semantic::LoadAddress ||
            uop.semantic == DVRInstructionRecorder::Uop::Semantic::LoadByteSigned ||
            uop.semantic == DVRInstructionRecorder::Uop::Semantic::LoadByteUnsigned ||
            uop.semantic == DVRInstructionRecorder::Uop::Semantic::LoadHalfSigned ||
            uop.semantic == DVRInstructionRecorder::Uop::Semantic::LoadHalfUnsigned ||
            uop.semantic == DVRInstructionRecorder::Uop::Semantic::LoadWordSigned ||
            uop.semantic == DVRInstructionRecorder::Uop::Semantic::LoadWordUnsigned ||
            uop.semantic == DVRInstructionRecorder::Uop::Semantic::LoadDouble;
        if (is_replayed_load) {
            const auto relation_it = dvrAddressRelations.find(
                DVRRelationKey{state.replay->triggerPC, uop.pc});
            if (relation_it != dvrAddressRelations.end() &&
                relation_it->second.trained) {
                const auto &relation = relation_it->second;
                const int64_t expected = relation.scale *
                    static_cast<int64_t>(source_value) + relation.offset;
                if (expected < 0 || static_cast<Addr>(expected) != result ||
                    result < relation.minAddress ||
                    result > relation.maxAddress) {
                    ++cpuStats.dvrReplayUnstableInputs;
                    return false;
                }
            }
            DVRPrefetchAddress dependent;
            dependent.address = static_cast<Addr>(result);
            if (dependent.address < 4096 ||
                dependent.address >= (Addr(1) << 47))
                return false;
            const Addr line = dvrPrefetchLine(dependent.address);
            if (dvrQueuedDependentLines.count(line) != 0 ||
                dvrDependentOutstandingLines.count(line) != 0 ||
                dvrDependentCompletedLines.count(line) != 0) {
                ++cpuStats.dvrPrefetchesDeduplicated;
                return true;
            }
            dependent.pc = uop.pc;
            dependent.tid = state.tid;
            dependent.source = false;
            dependent.nested = state.nested;
            dependent.replay = state.replay;
            dependent.predicate = state.predicate;
            dependent.lane = state.lane;
            ++cpuStats.dvrDependentPrefetchesGenerated;
            if (dvrPrefetchQueue.size() >= DvrMaxQueuedPrefetches) {
                ++cpuStats.dvrPrefetchesDropped;
                return true;
            }
            dvrTraceDependency("replay_target", curTick(),
                               state.replay->uops[0].pc, uop.pc,
                               dependent.address, state.predicate ? 1 : 0,
                               state.lane);
            dvrPrefetchQueue.push_front(dependent);
            dvrQueuedPrefetchAddresses.insert(dependent.address);
            dvrQueuedDependentLines.insert(line);
            updateDVRPrefetchQueuePeak();
            ++cpuStats.dvrReplayTargetsGenerated;
            if (uop.alternatePath || alternate_selected) {
                ++cpuStats.dvrAlternatePathDependentTargets;
                dvrAlternateDependentLines.insert(line);
            }
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
        const unsigned next_index = index + 1;
        if (alternate_active && alternate_reconvergence != 0 &&
            next_index < state.replay->count &&
            state.replay->uops[next_index].pc == alternate_reconvergence) {
            ++cpuStats.dvrReconvergenceResumeSuccesses;
            alternate_active = false;
            alternate_reconvergence = 0;
        }
        index = next_index;
    }
    return false;
}

StaticInstPtr
CPU::fetchDecodeDVRUop(ThreadID tid, Addr pc,
                       const StaticInstPtr &captured,
                       bool &fetch_fault, bool &cache_hit)
{
    fetch_fault = false;
    cache_hit = false;
    auto cached = dvrHelperThread.decodedUopCache.find(pc);
    if (cached != dvrHelperThread.decodedUopCache.end()) {
        cache_hit = true;
        ++cpuStats.dvrHelperDecodedCacheHits;
        return cached->second;
    }

    ++cpuStats.dvrHelperDecodedCacheMisses;
    if (tid >= threadContexts.size() || !threadContexts[tid]) {
        fetch_fault = true;
        ++cpuStats.dvrHelperInstructionFetchFaults;
        ++cpuStats.dvrHelperDecodeFallbacks;
        return captured;
    }

    ++cpuStats.dvrHelperInstructionFetches;
    if (dvrInstructionPort.isConnected()) {
        // A timing miss is intentionally represented by nullptr.  The
        // caller keeps the replay lane active and retries this PC after the
        // response populates decodedUopCache.
        if (requestDVRInstructionFetch(tid, pc, captured))
            return nullptr;
        auto fallback = dvrHelperThread.decodedUopCache.find(pc);
        if (fallback != dvrHelperThread.decodedUopCache.end())
            return fallback->second;
        return captured;
    }

    // The helper has its own PC and front-end timing, but uses the process's
    // instruction address space for the actual bytes.  This is a functional
    // read, analogous to the SE fetch path; it does not touch architectural
    // registers or the main fetch decoder state.
    SETranslatingPortProxy proxy(threadContexts[tid]);
    uint16_t half = 0;
    if (!proxy.tryReadBlob(pc, &half, sizeof(half))) {
        fetch_fault = true;
        ++cpuStats.dvrHelperInstructionFetchFaults;
        ++cpuStats.dvrHelperDecodeFallbacks;
        return captured;
    }

    uint32_t raw = half;
    if ((half & 0x3) == 0x3) {
        if (!proxy.tryReadBlob(pc, &raw, sizeof(raw))) {
            fetch_fault = true;
            ++cpuStats.dvrHelperInstructionFetchFaults;
            ++cpuStats.dvrHelperDecodeFallbacks;
            return captured;
        }
    }

    StaticInstPtr decoded;
    auto *decoder = dynamic_cast<RiscvISA::Decoder *>(
        threadContexts[tid]->getDecoderPtr());
    if (decoder)
        decoded = decoder->decodeRaw(raw, pc);

    if (!decoded) {
        ++cpuStats.dvrHelperDecodeFallbacks;
        return captured;
    }
    dvrHelperThread.decodedUopCache.emplace(pc, decoded);
    ++cpuStats.dvrHelperInstructionsDecoded;
    return decoded;
}

void
CPU::trainDVRAddressRelation(Addr trigger_pc, Addr flr_pc, RegVal source_value,
                             Addr dependent_address)
{
    dvrDependentLoadPCs.insert(flr_pc);
    auto &relation = dvrAddressRelations[DVRRelationKey{trigger_pc, flr_pc}];
    auto &trigger_relations = dvrTriggerRelations[trigger_pc];
    if (std::find(trigger_relations.begin(), trigger_relations.end(), flr_pc) ==
        trigger_relations.end())
        trigger_relations.push_back(flr_pc);
    if (relation.samples == 0) {
        relation.pattern = source_value;
        relation.minAddress = dependent_address;
        relation.maxAddress = dependent_address;
    } else {
        relation.stableMask &= ~(relation.previousValue ^ source_value);
        relation.pattern &= relation.stableMask;
        relation.minAddress = std::min(relation.minAddress, dependent_address);
        relation.maxAddress = std::max(relation.maxAddress, dependent_address);
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
                dvrTraceDependency("relation_trained", curTick(),
                    trigger_pc, flr_pc, dependent_address, 0);
            }
        }
    }
    relation.hasPrevious = true;
    relation.previousValue = source_value;
    relation.previousAddress = dependent_address;
}

void
CPU::recordDVRAlternatePaths(const DVRInstructionRecorder &recorder,
                             ContextID address_space_id)
{
    // A recorder initially fills reconvergencePC with the first committed
    // successor after a branch.  That is useful as a path extraction bound,
    // but it is not a stable join key: the successor is the taken target in
    // one discovery and the fall-through PC in another.  Treat such an
    // immediate successor as an unresolved join (zero) for cache identity;
    // a real FLR/CFG join remains part of the key.
    const auto cache_reconvergence =
        [](const DVRInstructionRecorder::Uop &uop) {
            const auto near = [](Addr lhs, Addr rhs) {
                return lhs != 0 && rhs != 0 &&
                    (lhs >= rhs ? lhs - rhs : rhs - lhs) <= 4;
            };
            if (near(uop.reconvergencePC, uop.branchTargetPC) ||
                near(uop.reconvergencePC, uop.fallthroughPC))
                return Addr(0);
            return uop.reconvergencePC;
        };
    for (unsigned branch = 0; branch < recorder.size(); ++branch) {
        const auto &op = recorder[branch];
        if (!op.conditional || !op.tainted)
            continue;
        ++cpuStats.dvrAlternatePathCandidates;
        // A backward conditional is loop control (the taken edge starts the
        // next iteration), not a diamond alternate path.  Its loop bound is
        // handled by the discovery/loop-bound machinery; caching it here
        // would manufacture a suffix with no stable post-dominator.
        if (op.branchTargetPC != 0 && op.branchTargetPC < op.pc) {
            ++cpuStats.dvrAlternatePathBackwardFiltered;
            continue;
        }

        const Addr reconvergence = op.reconvergencePC;
        const Addr target = op.branchTaken ? op.branchTargetPC :
                                             op.fallthroughPC;
        if (op.pc == 0 || target == 0 || reconvergence == 0)
            continue;

        DVRAlternatePath path;
        unsigned end = recorder.size();
        for (unsigned index = branch + 1; index < recorder.size(); ++index) {
            if (recorder[index].pc == reconvergence) {
                end = index;
                break;
            }
        }
        path.complete = end < recorder.size() && end > branch + 1;
        // Include a boundary FLR in the cached suffix.  Without the load,
        // an alternate branch can replay arithmetic but cannot emit a target.
        unsigned path_end = end;
        Addr alternate_resume = 0;
        if (path.complete && recorder[end].load) {
            path_end = end + 1;
            if (path_end < recorder.size())
                alternate_resume = recorder[path_end].pc;
        }
        uint32_t defined = 0;
        uint32_t live_out = 0;
        if (path.complete) {
            for (unsigned index = end; index < recorder.size(); ++index)
                live_out |= recorder[index].intSources;
        }
        if (path.complete) {
            const auto captured_pc = [&](Addr pc) {
                if (pc == reconvergence)
                    return true;
                for (unsigned candidate = branch + 1; candidate < path_end;
                     ++candidate) {
                    if (recorder[candidate].pc == pc)
                        return true;
                }
                for (const auto &cached_uop : path.uops) {
                    if (cached_uop.pc == pc)
                        return true;
                }
                return false;
            };
            for (unsigned index = branch + 1; index < path_end; ++index) {
                const auto &path_uop = recorder[index];
                // A cached alternate path may contain another direct
                // conditional branch.  The SIMT VIR/reconvergence stack can
                // execute that branch, so rejecting every control uop here
                // made all discovered paths permanently incomplete.  Reject
                // only unsupported semantics and controls whose target
                // cannot be represented in the captured PC stream (typically
                // indirect/unresolved control flow).
                const bool direct_jump = path_uop.control &&
                    !path_uop.conditional && path_uop.branchTargetPC != 0;
                if (direct_jump && captured_pc(path_uop.branchTargetPC)) {
                    ++cpuStats.dvrAlternatePathDirectJumps;
                    auto cached_uop = path_uop;
                    cached_uop.alternatePath = true;
                    path.uops.push_back(cached_uop);
                    continue;
                }
                if (path_uop.semantic ==
                    DVRInstructionRecorder::Uop::Semantic::Unsupported) {
                    // Non-register-writing operations (stores, fences and
                    // cache hints) do not contribute to a prefetch address
                    // and can be omitted from a speculative alternate path.
                    // An unsupported operation that writes a register used
                    // later would change control/address state, so reject
                    // that path instead of guessing its semantics.
                    bool writes_live_value = false;
                    if (path_uop.intDestinations != 0) {
                        for (unsigned later = index + 1;
                             later < path_end; ++later) {
                            if (path_uop.intDestinations &
                                recorder[later].intSources) {
                                writes_live_value = true;
                                break;
                            }
                        }
                    }
                    if (path_uop.control || writes_live_value ||
                        (path_uop.intDestinations & live_out) != 0) {
                        ++cpuStats.dvrAlternatePathUnsupportedRejects;
                        path.complete = false;
                        break;
                    }
                    ++cpuStats.dvrAlternatePathSafeSkips;
                    continue;
                }
                if (path_uop.control && path_uop.conditional) {
                    // A nested conditional can be replayed only when both
                    // of its possible successors are represented.  The
                    // current dynamic suffix supplies one side; compose the
                    // missing side from an earlier *complete* cache entry.
                    // This is deliberately one-level composition: a child
                    // entry must already have passed its own control/live-in
                    // validation, so an incomplete cache entry is never
                    // upgraded merely by being referenced here.
                    const auto append_cached_target = [&](Addr target) {
                        if (target == 0 || captured_pc(target))
                            return target != 0;
                        const DVRAlternatePathKey nested_key{
                            path_uop.pc, target, reconvergence,
                            address_space_id};
                        const auto nested = dvrAlternatePathCache.find(
                            nested_key);
                        if (nested == dvrAlternatePathCache.end() ||
                            !nested->second.complete ||
                            nested->second.uops.empty() ||
                            path.uops.size() + nested->second.uops.size() >
                                DVRInstructionRecorder::MaxUops) {
                            return false;
                        }
                        path.liveInRegisters |=
                            nested->second.liveInRegisters & ~defined;
                        for (const auto &nested_uop : nested->second.uops) {
                            defined |= nested_uop.intDestinations;
                            auto cached_uop = nested_uop;
                            cached_uop.alternatePath = true;
                            path.uops.push_back(cached_uop);
                        }
                        dvrTraceVector("alternate_path_nested_compose",
                            curTick(), path_uop.pc, target,
                            nested->second.uops.size(),
                            static_cast<int>(address_space_id));
                        return captured_pc(target);
                    };
                    // Place the conditional in the template before adding a
                    // cache-composed suffix.  Its PC selects the correct
                    // successor at execution; no fall-through ordering is
                    // used to choose a branch target.
                    path.liveInRegisters |= path_uop.intSources & ~defined;
                    defined |= path_uop.intDestinations;
                    auto cached_branch = path_uop;
                    cached_branch.alternatePath = true;
                    path.uops.push_back(cached_branch);
                    const bool target_ok = append_cached_target(
                        path_uop.branchTargetPC);
                    const bool fallthrough_ok = append_cached_target(
                        path_uop.fallthroughPC);
                    if (!target_ok || !fallthrough_ok) {
                        ++cpuStats.dvrAlternatePathControlRejects;
                        path.complete = false;
                        break;
                    }
                    continue;
                }
                path.liveInRegisters |= path_uop.intSources & ~defined;
                defined |= path_uop.intDestinations;
                auto cached_uop = path_uop;
                cached_uop.alternatePath = true;
                path.uops.push_back(cached_uop);
            }
            if (path.complete && !path.uops.empty())
                path.uops.back().alternateResumePC = alternate_resume;
            if (path.uops.empty())
                path.complete = false;
        }

        DVRAlternatePathKey key{op.pc, target, cache_reconvergence(op),
                                address_space_id};
        path.codeVersion = 0;
        // Keep the full cache key in the compact dependency trace.  The
        // vector trace only has room for branch/target, while reconvergence
        // and address-space identity are part of the correctness key.
        dvrTraceDependency("alternate_path_record_key", curTick(),
            op.pc, target, cache_reconvergence(op),
            static_cast<int>(address_space_id));
        dvrTraceVector(path.complete ? "alternate_path_record_complete" :
                       "alternate_path_record_incomplete", curTick(),
                       op.pc, target, path.uops.size(),
                       static_cast<int>(address_space_id));
        // Incomplete paths are diagnostic artifacts only.  Keeping them in
        // the cache makes a later lookup appear to hit, then guarantees an
        // admission reject even when a usable path could be learned later.
        if (!path.complete) {
            ++cpuStats.dvrAlternatePathIncompleteRecords;
            continue;
        }
        ++cpuStats.dvrAlternatePathCompleteRecords;
        const auto existing = dvrAlternatePathCache.find(key);
        if (existing == dvrAlternatePathCache.end() ||
            (!existing->second.complete && path.complete))
            dvrAlternatePathCache[key] = std::move(path);
    }
}

bool
CPU::augmentDVRAlternatePaths(
    DVRInstructionRecorder &recorder, ContextID address_space_id,
    const DVRLoopBoundDetector::RegisterSnapshot &initial_regs)
{
    const auto cache_reconvergence =
        [](const DVRInstructionRecorder::Uop &uop) {
            const auto near = [](Addr lhs, Addr rhs) {
                return lhs != 0 && rhs != 0 &&
                    (lhs >= rhs ? lhs - rhs : rhs - lhs) <= 4;
            };
            if (near(uop.reconvergencePC, uop.branchTargetPC) ||
                near(uop.reconvergencePC, uop.fallthroughPC))
                return Addr(0);
            return uop.reconvergencePC;
        };
    bool admitted_complete_path = false;
    struct BranchOccurrence
    {
        unsigned index;
        DVRInstructionRecorder::Uop op;
    };
    std::vector<BranchOccurrence> branches;
    for (unsigned index = 0; index < recorder.size(); ++index) {
        if (recorder[index].conditional && recorder[index].tainted)
            branches.push_back({index, recorder[index]});
    }
    // Insert from the end of the captured stream.  Insertion before a
    // reconvergence point shifts later occurrences, but cannot invalidate an
    // earlier occurrence.  This matters when a loop executes the same branch
    // PC multiple times in one discovery: selecting the first matching PC
    // can splice an alternate suffix into the wrong dynamic iteration.
    for (auto occurrence = branches.rbegin(); occurrence != branches.rend();
         ++occurrence) {
        const auto &op = occurrence->op;
        if (!op.conditional || op.reconvergencePC == 0)
            continue;
        if (op.branchTargetPC != 0 && op.branchTargetPC < op.pc)
            continue;

        const unsigned branch = occurrence->index;
        if (branch >= recorder.size() || recorder[branch].pc != op.pc)
            continue;

        const Addr alternate_target = op.branchTaken ? op.fallthroughPC :
                                                       op.branchTargetPC;
        if (alternate_target == 0)
            continue;

        ++cpuStats.dvrAlternatePathCandidates;

        bool target_present = alternate_target == op.reconvergencePC;
        for (unsigned index = branch + 1; index < recorder.size(); ++index) {
            if (recorder[index].pc == op.reconvergencePC)
                break;
            if (recorder[index].pc == alternate_target) {
                target_present = true;
                break;
            }
        }
        if (target_present) {
            ++cpuStats.dvrAlternatePathTargetPresent;
            continue;
        }

        ++cpuStats.dvrAlternatePathLookups;
        const DVRAlternatePathKey key{op.pc, alternate_target,
                                      cache_reconvergence(op),
                                      address_space_id};
        auto found = dvrAlternatePathCache.find(key);
        // A single-path discovery may terminate at its path-local FLR
        // rather than a shared CFG join.  Preserve branch, target and
        // address-space identity, but allow a complete candidate with the
        // same path family to supply the missing suffix.  The candidate's
        // own reconvergence metadata remains attached to its uops and is
        // validated by the replay lane before resuming.
        if (found == dvrAlternatePathCache.end()) {
            for (auto candidate = dvrAlternatePathCache.begin();
                 candidate != dvrAlternatePathCache.end(); ++candidate) {
                if (candidate->first.branchPC == op.pc &&
                    candidate->first.targetPC == alternate_target &&
                    candidate->first.addressSpaceID == address_space_id &&
                    candidate->second.complete) {
                    found = candidate;
                    break;
                }
            }
        }
        if (found == dvrAlternatePathCache.end()) {
            dvrTraceDependency("alternate_path_lookup_miss", curTick(),
                op.pc, alternate_target, op.reconvergencePC,
                static_cast<int>(address_space_id));
            continue;
        }
        dvrTraceDependency("alternate_path_lookup_hit", curTick(),
            op.pc, alternate_target, op.reconvergencePC,
            static_cast<int>(address_space_id));
        ++cpuStats.dvrAlternatePathHits;
        const auto &path = found->second;
        if (!path.complete) {
            ++cpuStats.dvrAlternatePathIncompleteRejects;
            // Keep this trace intentionally at cache-lookup granularity:
            // it identifies the branch/alternate target actually relevant to
            // BFS/Camel admission without dumping every discovery uop.
            dvrTraceVector("alternate_path_incomplete_hit", curTick(),
                           op.pc, alternate_target, path.uops.size(),
                           static_cast<int>(address_space_id));
            continue;
        }
        bool live_in_valid = true;
        for (unsigned reg = 0;
             reg < DVRLoopBoundDetector::MaxArchitecturalIntRegs; ++reg) {
            if ((path.liveInRegisters & (uint32_t(1) << reg)) &&
                reg >= initial_regs.size()) {
                live_in_valid = false;
                break;
            }
        }
        if (!live_in_valid) {
            ++cpuStats.dvrAlternatePathLiveInRejects;
            continue;
        }
        if (!recorder.insertBeforePC(op.reconvergencePC, path.uops)) {
            ++cpuStats.dvrAlternatePathInsertRejects;
            continue;
        }
        ++cpuStats.dvrAlternatePathCompleteHits;
        admitted_complete_path = true;
        dvrTraceVector("alternate_path_complete_hit", curTick(), op.pc,
                       alternate_target, path.uops.size(),
                       static_cast<int>(address_space_id));
    }
    return admitted_complete_path;
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
        if (enableDVR)
            dvrStrideDetector.squashDiscovery((*instIt)->seqNum);
        if (enableDVR)
            dvrTaintTracker.squash((*instIt)->seqNum);
        if (enableDVR && dvrDiscovery.rollback((*instIt)->seqNum)) {
            ++cpuStats.dvrDiscoveryRollbacks;
            dvrStrideDetector.endDiscovery();
            dvrTaintTracker.reset();
            dvrCommittedFinalLoadPC = 0;
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
            dvrDispatchRecorded.clear();
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
CPU::beginDVRDiscoveryAtDispatch(
    const DynInstPtr &inst, const DVRStrideDetector::Candidate &candidate,
    bool restart)
{
    if (restart) {
        // Preserve the superseded generation before restart() clears its
        // controller, taint, FLR, and loop-bound state.  The paper resets
        // these structures for the more-inner trigger, but the result of the
        // abandoned generation remains an observable analysis outcome.
        recordDVRDiscoveryGeneration("innermost_switch");
        // A repeated striding PC is a new, more-inner Discovery generation.
        // Do not let the previous generation's VTT, FLR, LCR, recorder, or
        // nested state leak into the new candidate.
        dvrDiscovery.restart(candidate, inst->seqNum);
        dvrNestedController.reset();
        dvrNestedContext.reset();
        dvrNestedDiscoveryMode.reset();
    } else {
        dvrDiscovery.arm(candidate, inst->seqNum);
    }
    dvrStrideDetector.beginDiscovery(candidate.pc);
    ++cpuStats.dvrDiscoveryStarts;

    if (dvrMode == "nested" &&
        dvrNestedController.startRoot(candidate.pc, inst->seqNum).event ==
            DVRNestedController::Event::Started) {
        ++cpuStats.dvrNestedRootStarts;
    }
    dvrTaintTracker.begin(inst);
    dvrCommittedFinalLoadPC = 0;
    dvrLoopBoundDetector.begin(candidate.pc);
    dvrInstructionRecorder.begin(inst);
    dvrDispatchTainted.clear();
    dvrDispatchDependentLoads.clear();
    dvrDispatchRecorded.clear();
    if (!dvrNestedDiscoveryMode.active())
        dvrNestedDiscoveryMode.reset();
    dvrCommittedNestedCandidate = {};
    dvrCurrentTriggerPC = candidate.pc;
    dvrCurrentTriggerAddress = candidate.address;
    dvrTraceVector("discovery_start", curTick(), candidate.pc,
                   candidate.address, restart ? 1 : 0);
    dvrInitiatingLoadValue = 0;
    captureDVRRegisterSnapshot(
        inst->threadNumber, inst, dvrDiscoveryStartRegs);
}

void
CPU::recordDVRDiscoveryGeneration(
    const char *reason,
    const DVRLoopBoundDetector::Inference *inference,
    Addr trigger_pc,
    int64_t stride)
{
    if (trigger_pc == 0)
        trigger_pc = dvrCurrentTriggerPC;
    if (trigger_pc == 0)
        return;
    DVRDiscoveryGenerationRecord record;
    record.id = ++dvrDiscoveryGeneration;
    record.initialTriggerPC = trigger_pc;
    record.finalLoadPC = dvrCommittedFinalLoadPC != 0 ?
        dvrCommittedFinalLoadPC : dvrTaintTracker.flr();
    record.loopBranchPC = dvrLoopBoundDetector.branchPC();
    record.loopTargetPC = dvrLoopBoundDetector.targetPC();
    record.stride = stride != 0 ? stride : dvrDiscovery.currentStride();
    record.boundSource0 = dvrLoopBoundDetector.source0();
    record.boundSource1 = dvrLoopBoundDetector.source1();
    record.comparison = dvrLoopBoundDetector.comparisonKind();
    record.hasBound = dvrLoopBoundDetector.hasBound();
    record.reason = reason;
    if (inference) {
        record.matched = inference->matched;
        record.lanes = inference->lanes;
    }
    dvrTraceLoopBound(curTick(), reason, record.initialTriggerPC,
                      record.finalLoadPC, record.loopBranchPC,
                      record.loopTargetPC, record.boundSource0,
                      record.boundSource1, record.comparison,
                      record.hasBound, inference);
    dvrDiscoveryHistory.push_back(record);
    dvrTraceVector("discovery_generation_record", curTick(),
                   record.initialTriggerPC, record.finalLoadPC,
                   record.lanes, static_cast<int>(record.id));
    DPRINTF(O3CPU,
            "DVR discovery generation id=%llu reason=%s trigger=%#x "
            "flr=%#x loop=%#x->%#x bound=%d matched=%d lanes=%u\n",
            static_cast<unsigned long long>(record.id), record.reason,
            record.initialTriggerPC, record.finalLoadPC,
            record.loopBranchPC, record.loopTargetPC, record.hasBound,
            record.matched, record.lanes);
}

void
CPU::observeDVRDispatch(const DynInstPtr &inst)
{
    static unsigned dvrTraceCandidates = 0;
    if (!enableDVR || inPRE || inst->isPRE())
        return;

    dvrDiscovery.observeDispatch(inst->pcState().instAddr(), inst->seqNum);
    // observeDispatch() marks stopSequence when the current trigger is seen
    // again.  From that point until commit, Discovery only waits for its
    // terminating trigger; younger speculative loads are outside the window.
    const bool discovery_window = dvrDiscovery.isDiscovering() &&
        !dvrDiscovery.stopPending();
    const bool discovery_completion_pending = dvrDiscovery.stopPending();

    if (inst->isLoad()) {
        std::optional<DVRStrideDetector::Candidate> candidate;
        // Close the RPT/innermost window at the terminating trigger.
        if (!discovery_completion_pending) {
            candidate = dvrStrideDetector.observeDispatch(
                inst->pcState().instAddr(), discovery_window,
                discovery_window ? dvrCurrentTriggerPC : 0,
                inst->seqNum);
        }
        if (candidate) {
            const bool pc_allowed =
                (dvrPCMin == 0 || candidate->pc >= dvrPCMin) &&
                (dvrPCMax == 0 || candidate->pc <= dvrPCMax);
            if (!pc_allowed)
                candidate.reset();
        }
        if (candidate) {
            ++dvrTraceCandidates;
            if (candidate->pc >= 0x129de && candidate->pc <= 0x12b22) {
                DPRINTF(O3CPU,
                    "DVR_TRACE candidate pc=%#x addr=%#x stride=%lld discovering=%d\n",
                    candidate->pc, candidate->address,
                    static_cast<long long>(candidate->stride),
                    dvrDiscovery.isDiscovering());
            }
            ++cpuStats.dvrStrideCandidates;
            if (dvrMode == "vr" && candidate->pc == 0x12a56) {
                DPRINTF(O3CPU,
                    "DVR_TRACE vr_candidate pc=%#x rob_full=%d queue=%u outstanding=%u\n",
                    candidate->pc, rob.isFull(inst->threadNumber),
                    dvrPrefetchQueue.size(), dvrHelperLoadEntries.size());
            }
            if (dvrMode == "vr" &&
                rob.isFull(inst->threadNumber)) {
                if (dvrPrefetchQueue.empty() && dvrHelperLoadEntries.empty())
                    launchDVRVectorRunahead(inst->threadNumber,
                        candidate->address, candidate->pc, candidate->stride);
            } else if (dvrMode == "offload") {
                // Figure 8 Offload starts the same vector-runahead work
                // proactively, without waiting for a full ROB or Discovery.
                if (dvrPrefetchQueue.empty() && dvrHelperLoadEntries.empty())
                    launchDVRVectorRunahead(inst->threadNumber,
                        candidate->address, candidate->pc, candidate->stride);
            } else if (candidate->repeatedDuringDiscovery &&
                       discovery_window &&
                       candidate->pc != dvrCurrentTriggerPC) {
                ++cpuStats.dvrDiscoveryInnermostSwitches;
                beginDVRDiscoveryAtDispatch(inst, *candidate, true);
            } else if (dvrMode == "vr") {
                // Classic VR only enters runahead after the ROB trigger;
                // do not accidentally arm the DVR Discovery path for every
                // stride observed while the ROB still has free entries.
            } else if (dvrDiscovery.isDiscovering() ||
                       dvrNestedDiscoveryMode.active()) {
                if (!dvrPendingNestedCandidate.valid ||
                    inst->seqNum < dvrPendingNestedCandidate.sequence) {
                    dvrPendingNestedCandidate = {
                        true, candidate->pc, inst->seqNum,
                        candidate->address, candidate->stride};
                }
            } else {
                beginDVRDiscoveryAtDispatch(inst, *candidate, false);
            }
        }
    }

    if (discovery_window &&
        inst->seqNum > dvrDiscovery.triggerSeq()) {
        const auto observation = dvrTaintTracker.observeSpeculative(
            inst, inst->seqNum);
        // Alternate-path replay needs a complete decoded suffix, including
        // non-tainted arithmetic that computes a path's live-out address.
        // The taint sets still control dependency classification; this set
        // controls recorder coverage only.
        dvrDispatchRecorded.insert(inst->seqNum);
        if (observation.taintedInstruction) {
            ++cpuStats.dvrTaintedInstructions;
            dvrDispatchTainted.insert(inst->seqNum);
            dvrDispatchRecorded.insert(inst->seqNum);
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
    if (dvrDependentLoadPCs.count(inst->pcState().instAddr()) != 0) {
        const Addr line = dvrPrefetchLine(address);
        ++cpuStats.dvrDependentDemandLoads;
        if (dvrDependentCompletedLines.erase(line) != 0)
            ++cpuStats.dvrDependentDemandCovered;
        if (dvrAlternateDependentLines.erase(line) != 0)
            ++cpuStats.dvrAlternatePathDemandCovered;
        else if (dvrDependentOutstandingLines.count(line) != 0)
            ++cpuStats.dvrDependentDemandLate;
    }
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
