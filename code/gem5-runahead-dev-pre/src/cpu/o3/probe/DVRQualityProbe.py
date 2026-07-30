# DVR prefetch quality listener for a single selected L1 data cache.
#
# `manager` must name exactly one L1D (for example system.cpu.dcache).  It is
# intentionally not left at Parent.any: binding the same listener to L2/L3 as
# well would double-count every lookup and fill, and the reported accuracy,
# coverage and pollution would silently stop meaning what they claim.

from m5.objects.Probe import *
from m5.params import *


class DVRQualityProbe(ProbeListenerObject):
    type = "DVRQualityProbe"
    cxx_class = "gem5::o3::DVRQualityProbe"
    cxx_header = "cpu/o3/probe/dvr_quality_probe.hh"

    # Shadow-cache geometry.  Must match the measured L1D, otherwise coverage
    # is computed against a counterfactual cache of the wrong size.
    sets = Param.Unsigned(64, "Sets in the demand-only shadow cache")
    assoc = Param.Unsigned(8, "Ways in the demand-only shadow cache")
    line_bytes = Param.Unsigned(64, "Block size of the measured L1D in bytes")
