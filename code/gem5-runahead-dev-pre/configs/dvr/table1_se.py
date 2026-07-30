#!/usr/bin/env python3

"""gem5 SE configuration approximating DVR paper Table 1."""

import argparse

import m5
from m5.objects import *


class DVRIntAdd(FUDesc):
    opList = [OpDesc(opClass="IntAlu", opLat=1)]
    count = 4


class DVRIntMult(FUDesc):
    opList = [OpDesc(opClass="IntMult", opLat=3)]
    count = 1


class DVRIntDiv(FUDesc):
    opList = [OpDesc(opClass="IntDiv", opLat=18, pipelined=False)]
    count = 1


class DVRFPAdd(FUDesc):
    opList = [
        OpDesc(opClass="FloatAdd", opLat=3),
        OpDesc(opClass="FloatCmp", opLat=3),
        OpDesc(opClass="FloatCvt", opLat=3),
    ]
    count = 1


class DVRFPMult(FUDesc):
    opList = [
        OpDesc(opClass="FloatMult", opLat=5),
        OpDesc(opClass="FloatMultAcc", opLat=5),
        OpDesc(opClass="FloatMisc", opLat=5),
    ]
    count = 1


class DVRFPDiv(FUDesc):
    opList = [
        OpDesc(opClass="FloatDiv", opLat=6, pipelined=False),
        OpDesc(opClass="FloatSqrt", opLat=6, pipelined=False),
    ]
    count = 1


class DVRVecALU(FUDesc):
    opList = [
        OpDesc(opClass="SimdAlu"), OpDesc(opClass="SimdCmp"),
        OpDesc(opClass="SimdCvt"), OpDesc(opClass="SimdFloatAlu"),
        OpDesc(opClass="SimdFloatCmp"), OpDesc(opClass="SimdFloatCvt"),
        OpDesc(opClass="SimdReduceAlu"), OpDesc(opClass="SimdReduceCmp"),
        OpDesc(opClass="SimdFloatReduceCmp"),
    ]
    count = 3


class DVRVecShift(FUDesc):
    opList = [OpDesc(opClass="SimdShift"), OpDesc(opClass="SimdShiftAcc")]
    count = 2


class DVRVecAdd(FUDesc):
    opList = [
        OpDesc(opClass="SimdAdd"), OpDesc(opClass="SimdAddAcc"),
        OpDesc(opClass="SimdFloatAdd"), OpDesc(opClass="SimdReduceAdd"),
        OpDesc(opClass="SimdFloatReduceAdd"),
    ]
    count = 2


class DVRVecMult(FUDesc):
    opList = [
        OpDesc(opClass="SimdMult"), OpDesc(opClass="SimdMultAcc"),
        OpDesc(opClass="SimdFloatMult"),
        OpDesc(opClass="SimdFloatMultAcc"),
    ]
    count = 2


class DVRVecShuffle(FUDesc):
    opList = [
        OpDesc(opClass="SimdMisc"), OpDesc(opClass="SimdFloatMisc"),
        OpDesc(opClass="SimdDiv"), OpDesc(opClass="SimdSqrt"),
        OpDesc(opClass="SimdFloatDiv"), OpDesc(opClass="SimdFloatSqrt"),
    ]
    count = 2


class DVRFUPool(FUPool):
    FUList = [
        DVRIntAdd(), DVRIntMult(), DVRIntDiv(), DVRFPAdd(), DVRFPMult(),
        DVRFPDiv(), DVRVecALU(), DVRVecShift(), DVRVecAdd(), DVRVecMult(),
        DVRVecShuffle(), PredALU(), RdWrPort(count=4), IprPort(),
    ]


class Table1L1I(Cache):
    size = "32KiB"
    assoc = 4
    tag_latency = 2
    data_latency = 2
    response_latency = 0
    mshrs = 8
    tgts_per_mshr = 20
    is_read_only = True
    writeback_clean = True


class Table1L1D(Cache):
    size = "32KiB"
    assoc = 8
    tag_latency = 4
    data_latency = 4
    response_latency = 0
    mshrs = 24
    tgts_per_mshr = 20
    prefetcher = StridePrefetcher(table_entries="16", table_assoc=4, degree=4)


class Table1L2(Cache):
    size = "256KiB"
    assoc = 8
    tag_latency = 8
    data_latency = 8
    response_latency = 0
    mshrs = 32
    tgts_per_mshr = 12


class Table1L3(Cache):
    size = "8MiB"
    assoc = 16
    tag_latency = 30
    data_latency = 30
    response_latency = 0
    mshrs = 64
    tgts_per_mshr = 12


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmd", required=True)
    parser.add_argument("--options", default="")
    parser.add_argument("--maxinsts", type=int, default=0)
    parser.add_argument("--dvr", action="store_true")
    parser.add_argument("--dvr-helper-max-uops", type=int, default=200)
    parser.add_argument("--discovery-max-insts", type=int, default=512)
    return parser.parse_args()


def main():
    args = parse_args()
    system = System()
    system.clk_domain = SrcClockDomain(
        clock="1GHz", voltage_domain=VoltageDomain(voltage="1.0V")
    )
    system.mem_mode = "timing"
    system.mem_ranges = [AddrRange("512MiB")]
    system.cache_line_size = 64

    system.cpu = DerivO3CPU(
        clk_domain=SrcClockDomain(
            clock="4GHz", voltage_domain=system.clk_domain.voltage_domain
        ),
        fetchWidth=5, decodeWidth=5, renameWidth=5, dispatchWidth=5,
        issueWidth=5, wbWidth=5, commitWidth=5, squashWidth=5,
        numROBEntries=350, numIQEntries=128, LQEntries=128, SQEntries=72,
        numPhysIntRegs=256, numPhysFloatRegs=256, numPhysVecRegs=128,
        branchPred=TAGE_SC_L_8KB(), fuPool=DVRFUPool(),
        enableDVR=args.dvr, dvrDiscoveryMaxInsts=args.discovery_max_insts,
        dvrHelperMaxUops=args.dvr_helper_max_uops,
    )
    if args.maxinsts:
        system.cpu.max_insts_any_thread = args.maxinsts

    l1i = Table1L1I()
    l1d = Table1L1D()
    system.cpu.addPrivateSplitL1Caches(l1i, l1d)

    system.l2bus = L2XBar(frontend_latency=0, forward_latency=0,
                          response_latency=0)
    system.l2 = Table1L2()
    system.l3bus = L2XBar(frontend_latency=0, forward_latency=0,
                          response_latency=0)
    system.l3 = Table1L3()
    system.membus = SystemXBar(frontend_latency=0, forward_latency=0,
                               response_latency=0)

    system.cpu.connectAllPorts(system.l2bus.cpu_side_ports,
                               system.membus.cpu_side_ports,
                               system.membus.mem_side_ports)
    system.l2.cpu_side = system.l2bus.mem_side_ports
    system.l2.mem_side = system.l3bus.cpu_side_ports
    system.l3.cpu_side = system.l3bus.mem_side_ports
    system.l3.mem_side = system.membus.cpu_side_ports

    system.memory = SimpleMemory(
        range=system.mem_ranges[0], latency="50ns", bandwidth="51.2GB/s"
    )
    system.memory.port = system.membus.mem_side_ports
    system.system_port = system.membus.cpu_side_ports

    process = Process(pid=100)
    process.executable = args.cmd
    process.cmd = [args.cmd] + (args.options.split() if args.options else [])
    system.workload = SEWorkload.init_compatible(args.cmd)
    system.cpu.workload = process
    system.cpu.createThreads()
    system.cpu.createInterruptController()

    root = Root(full_system=False, system=system)
    m5.instantiate()
    event = m5.simulate()
    print("Exiting @ tick {} because {}".format(m5.curTick(), event.getCause()))


if __name__ == "__m5_main__":
    main()
