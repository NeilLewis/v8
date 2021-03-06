// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_WASM_BASELINE_LIFTOFF_ASSEMBLER_X64_H_
#define V8_WASM_BASELINE_LIFTOFF_ASSEMBLER_X64_H_

#include "src/wasm/baseline/liftoff-assembler.h"

#include "src/assembler.h"
#include "src/wasm/wasm-opcodes.h"

namespace v8 {
namespace internal {
namespace wasm {

namespace liftoff {

inline Operand GetStackSlot(uint32_t index) {
  // rbp-8 holds the stack marker, rbp-16 is the wasm context, first stack slot
  // is located at rbp-24.
  constexpr int32_t kStackSlotSize = 8;
  constexpr int32_t kFirstStackSlotOffset = -24;
  return Operand(rbp, kFirstStackSlotOffset - index * kStackSlotSize);
}

// TODO(clemensh): Make this a constexpr variable once Operand is constexpr.
inline Operand GetContextOperand() { return Operand(rbp, -16); }

}  // namespace liftoff

void LiftoffAssembler::ReserveStackSpace(uint32_t bytes) {
  subp(rsp, Immediate(bytes));
}

void LiftoffAssembler::LoadConstant(LiftoffRegister reg, WasmValue value) {
  switch (value.type()) {
    case kWasmI32:
      if (value.to_i32() == 0) {
        xorl(reg.gp(), reg.gp());
      } else {
        movl(reg.gp(), Immediate(value.to_i32()));
      }
      break;
    case kWasmF32:
      TurboAssembler::Move(reg.fp(), value.to_f32_boxed().get_bits());
      break;
    default:
      UNREACHABLE();
  }
}

void LiftoffAssembler::LoadFromContext(Register dst, uint32_t offset,
                                       int size) {
  DCHECK_LE(offset, kMaxInt);
  movp(dst, liftoff::GetContextOperand());
  DCHECK(size == 4 || size == 8);
  if (size == 4) {
    movl(dst, Operand(dst, offset));
  } else {
    movq(dst, Operand(dst, offset));
  }
}

void LiftoffAssembler::SpillContext(Register context) {
  movp(liftoff::GetContextOperand(), context);
}

void LiftoffAssembler::Load(LiftoffRegister dst, Register src_addr,
                            Register offset_reg, uint32_t offset_imm,
                            LoadType type, LiftoffRegList pinned) {
  Operand src_op = offset_reg == no_reg
                       ? Operand(src_addr, offset_imm)
                       : Operand(src_addr, offset_reg, times_1, offset_imm);
  if (offset_imm > kMaxInt) {
    // The immediate can not be encoded in the operand. Load it to a register
    // first.
    Register src = GetUnusedRegister(kGpReg, pinned).gp();
    movl(src, Immediate(offset_imm));
    if (offset_reg != no_reg) {
      emit_ptrsize_add(src, src, offset_reg);
    }
    src_op = Operand(src_addr, src, times_1, 0);
  }
  switch (type.value()) {
    case LoadType::kI32Load8U:
      movzxbl(dst.gp(), src_op);
      break;
    case LoadType::kI32Load8S:
      movsxbl(dst.gp(), src_op);
      break;
    case LoadType::kI32Load16U:
      movzxwl(dst.gp(), src_op);
      break;
    case LoadType::kI32Load16S:
      movsxwl(dst.gp(), src_op);
      break;
    case LoadType::kI32Load:
      movl(dst.gp(), src_op);
      break;
    case LoadType::kI64Load:
      movq(dst.gp(), src_op);
      break;
    default:
      UNREACHABLE();
  }
}

void LiftoffAssembler::Store(Register dst_addr, Register offset_reg,
                             uint32_t offset_imm, LiftoffRegister src,
                             StoreType type, LiftoffRegList pinned) {
  Operand dst_op = offset_reg == no_reg
                       ? Operand(dst_addr, offset_imm)
                       : Operand(dst_addr, offset_reg, times_1, offset_imm);
  if (offset_imm > kMaxInt) {
    // The immediate can not be encoded in the operand. Load it to a register
    // first.
    Register dst = GetUnusedRegister(kGpReg, pinned).gp();
    movl(dst, Immediate(offset_imm));
    if (offset_reg != no_reg) {
      emit_ptrsize_add(dst, dst, offset_reg);
    }
    dst_op = Operand(dst_addr, dst, times_1, 0);
  }
  switch (type.value()) {
    case StoreType::kI32Store8:
      movb(dst_op, src.gp());
      break;
    case StoreType::kI32Store16:
      movw(dst_op, src.gp());
      break;
    case StoreType::kI32Store:
      movl(dst_op, src.gp());
      break;
    case StoreType::kI64Store:
      movq(dst_op, src.gp());
      break;
    default:
      UNREACHABLE();
  }
}

void LiftoffAssembler::LoadCallerFrameSlot(LiftoffRegister dst,
                                           uint32_t caller_slot_idx) {
  constexpr int32_t kStackSlotSize = 8;
  Operand src(rbp, kStackSlotSize * (caller_slot_idx + 1));
  // TODO(clemensh): Handle different sizes here.
  if (dst.is_gp()) {
    movq(dst.gp(), src);
  } else {
    Movsd(dst.fp(), src);
  }
}

void LiftoffAssembler::MoveStackValue(uint32_t dst_index, uint32_t src_index) {
  DCHECK_NE(dst_index, src_index);
  if (cache_state_.has_unused_register(kGpReg)) {
    LiftoffRegister reg = GetUnusedRegister(kGpReg);
    Fill(reg, src_index);
    Spill(dst_index, reg);
  } else {
    pushq(liftoff::GetStackSlot(src_index));
    popq(liftoff::GetStackSlot(dst_index));
  }
}

void LiftoffAssembler::MoveToReturnRegister(LiftoffRegister reg) {
  // TODO(wasm): Extract the destination register from the CallDescriptor.
  // TODO(wasm): Add multi-return support.
  LiftoffRegister dst =
      reg.is_gp() ? LiftoffRegister(rax) : LiftoffRegister(xmm1);
  if (reg != dst) Move(dst, reg);
}

void LiftoffAssembler::Move(LiftoffRegister dst, LiftoffRegister src) {
  // The caller should check that the registers are not equal. For most
  // occurences, this is already guaranteed, so no need to check within this
  // method.
  DCHECK_NE(dst, src);
  DCHECK_EQ(dst.reg_class(), src.reg_class());
  // TODO(clemensh): Handle different sizes here.
  if (dst.is_gp()) {
    movq(dst.gp(), src.gp());
  } else {
    movsd(dst.fp(), src.fp());
  }
}

void LiftoffAssembler::Spill(uint32_t index, LiftoffRegister reg) {
  Operand dst = liftoff::GetStackSlot(index);
  // TODO(clemensh): Handle different sizes here.
  if (reg.is_gp()) {
    movq(dst, reg.gp());
  } else {
    Movsd(dst, reg.fp());
  }
}

void LiftoffAssembler::Spill(uint32_t index, WasmValue value) {
  Operand dst = liftoff::GetStackSlot(index);
  switch (value.type()) {
    case kWasmI32:
      movl(dst, Immediate(value.to_i32()));
      break;
    case kWasmF32:
      movl(dst, Immediate(value.to_f32_boxed().get_bits()));
      break;
    default:
      UNREACHABLE();
  }
}

void LiftoffAssembler::Fill(LiftoffRegister reg, uint32_t index) {
  Operand src = liftoff::GetStackSlot(index);
  // TODO(clemensh): Handle different sizes here.
  if (reg.is_gp()) {
    movq(reg.gp(), src);
  } else {
    Movsd(reg.fp(), src);
  }
}

void LiftoffAssembler::emit_i32_add(Register dst, Register lhs, Register rhs) {
  if (lhs != dst) {
    leal(dst, Operand(lhs, rhs, times_1, 0));
  } else {
    addl(dst, rhs);
  }
}

void LiftoffAssembler::emit_ptrsize_add(Register dst, Register lhs,
                                        Register rhs) {
  if (lhs != dst) {
    leap(dst, Operand(lhs, rhs, times_1, 0));
  } else {
    addp(dst, rhs);
  }
}

void LiftoffAssembler::emit_i32_sub(Register dst, Register lhs, Register rhs) {
  if (dst == rhs) {
    negl(dst);
    addl(dst, lhs);
  } else {
    if (dst != lhs) movl(dst, lhs);
    subl(dst, rhs);
  }
}

#define COMMUTATIVE_I32_BINOP(name, instruction)                     \
  void LiftoffAssembler::emit_i32_##name(Register dst, Register lhs, \
                                         Register rhs) {             \
    if (dst == rhs) {                                                \
      instruction##l(dst, lhs);                                      \
    } else {                                                         \
      if (dst != lhs) movl(dst, lhs);                                \
      instruction##l(dst, rhs);                                      \
    }                                                                \
  }

// clang-format off
COMMUTATIVE_I32_BINOP(mul, imul)
COMMUTATIVE_I32_BINOP(and, and)
COMMUTATIVE_I32_BINOP(or, or)
COMMUTATIVE_I32_BINOP(xor, xor)
// clang-format on

#undef COMMUTATIVE_I32_BINOP

void LiftoffAssembler::emit_f32_add(DoubleRegister dst, DoubleRegister lhs,
                                    DoubleRegister rhs) {
  if (CpuFeatures::IsSupported(AVX)) {
    CpuFeatureScope scope(this, AVX);
    vaddss(dst, lhs, rhs);
  } else if (dst == rhs) {
    addss(dst, lhs);
  } else {
    if (dst != lhs) movss(dst, lhs);
    addss(dst, rhs);
  }
}

void LiftoffAssembler::emit_f32_sub(DoubleRegister dst, DoubleRegister lhs,
                                    DoubleRegister rhs) {
  if (CpuFeatures::IsSupported(AVX)) {
    CpuFeatureScope scope(this, AVX);
    vsubss(dst, lhs, rhs);
  } else if (dst == rhs) {
    movss(kScratchDoubleReg, rhs);
    movss(dst, lhs);
    subss(dst, kScratchDoubleReg);
  } else {
    if (dst != lhs) movss(dst, lhs);
    subss(dst, rhs);
  }
}

void LiftoffAssembler::emit_f32_mul(DoubleRegister dst, DoubleRegister lhs,
                                    DoubleRegister rhs) {
  if (CpuFeatures::IsSupported(AVX)) {
    CpuFeatureScope scope(this, AVX);
    vmulss(dst, lhs, rhs);
  } else if (dst == rhs) {
    mulss(dst, lhs);
  } else {
    if (dst != lhs) movss(dst, lhs);
    mulss(dst, rhs);
  }
}

void LiftoffAssembler::emit_i32_test(Register reg) { testl(reg, reg); }

void LiftoffAssembler::emit_i32_compare(Register lhs, Register rhs) {
  cmpl(lhs, rhs);
}

void LiftoffAssembler::emit_jump(Label* label) { jmp(label); }

void LiftoffAssembler::emit_cond_jump(Condition cond, Label* label) {
  j(cond, label);
}

void LiftoffAssembler::CallTrapCallbackForTesting() {
  PrepareCallCFunction(0);
  CallCFunction(
      ExternalReference::wasm_call_trap_callback_for_testing(isolate()), 0);
}

void LiftoffAssembler::AssertUnreachable(BailoutReason reason) {
  TurboAssembler::AssertUnreachable(reason);
}

}  // namespace wasm
}  // namespace internal
}  // namespace v8

#endif  // V8_WASM_BASELINE_LIFTOFF_ASSEMBLER_X64_H_
