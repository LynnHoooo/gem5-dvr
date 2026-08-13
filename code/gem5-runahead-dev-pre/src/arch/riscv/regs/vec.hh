#ifndef __ARCH_RISCV_REGS_VEC_HH__
#define __ARCH_RISCV_REGS_VEC_HH__

#include <cstdint>

#include "arch/generic/vec_pred_reg.hh"
#include "arch/generic/vec_reg.hh"

namespace gem5
{
namespace RiscvISA
{

using VecElem = uint64_t;
constexpr unsigned NumVecRegs = 32;
constexpr unsigned NumVecElemPerVecReg = 4; // VLEN=256, SEW=64 view
constexpr unsigned NumVecPredRegs = 1;      // architectural v0
constexpr unsigned NumVecSpecialRegs = 0;

using VecRegContainer = gem5::VecRegContainer<32>;
using VecPredRegContainer = gem5::VecPredRegContainer<256, true>;

} // namespace RiscvISA
} // namespace gem5

#endif
