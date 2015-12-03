//===- subzero/src/IceTargetLoweringX86BaseImpl.h - x86 lowering -*- C++ -*-==//
//
//                        The Subzero Code Generator
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief Implements the TargetLoweringX86Base class, which consists almost
/// entirely of the lowering sequence for each high-level instruction.
///
//===----------------------------------------------------------------------===//

#ifndef SUBZERO_SRC_ICETARGETLOWERINGX86BASEIMPL_H
#define SUBZERO_SRC_ICETARGETLOWERINGX86BASEIMPL_H

#include "IceCfg.h"
#include "IceCfgNode.h"
#include "IceClFlags.h"
#include "IceDefs.h"
#include "IceELFObjectWriter.h"
#include "IceGlobalInits.h"
#include "IceInstVarIter.h"
#include "IceLiveness.h"
#include "IceOperand.h"
#include "IcePhiLoweringImpl.h"
#include "IceUtils.h"
#include "llvm/Support/MathExtras.h"

#include <stack>

namespace Ice {
namespace X86Internal {

/// A helper class to ease the settings of RandomizationPoolingPause to disable
/// constant blinding or pooling for some translation phases.
class BoolFlagSaver {
  BoolFlagSaver() = delete;
  BoolFlagSaver(const BoolFlagSaver &) = delete;
  BoolFlagSaver &operator=(const BoolFlagSaver &) = delete;

public:
  BoolFlagSaver(bool &F, bool NewValue) : OldValue(F), Flag(F) { F = NewValue; }
  ~BoolFlagSaver() { Flag = OldValue; }

private:
  const bool OldValue;
  bool &Flag;
};

template <class MachineTraits> class BoolFoldingEntry {
  BoolFoldingEntry(const BoolFoldingEntry &) = delete;

public:
  BoolFoldingEntry() = default;
  explicit BoolFoldingEntry(Inst *I);
  BoolFoldingEntry &operator=(const BoolFoldingEntry &) = default;
  /// Instr is the instruction producing the i1-type variable of interest.
  Inst *Instr = nullptr;
  /// IsComplex is the cached result of BoolFolding::hasComplexLowering(Instr).
  bool IsComplex = false;
  /// IsLiveOut is initialized conservatively to true, and is set to false when
  /// we encounter an instruction that ends Var's live range. We disable the
  /// folding optimization when Var is live beyond this basic block. Note that
  /// if liveness analysis is not performed (e.g. in Om1 mode), IsLiveOut will
  /// always be true and the folding optimization will never be performed.
  bool IsLiveOut = true;
  // NumUses counts the number of times Var is used as a source operand in the
  // basic block. If IsComplex is true and there is more than one use of Var,
  // then the folding optimization is disabled for Var.
  uint32_t NumUses = 0;
};

template <class MachineTraits> class BoolFolding {
public:
  enum BoolFoldingProducerKind {
    PK_None,
    // TODO(jpp): PK_Icmp32 is no longer meaningful. Rename to PK_IcmpNative.
    PK_Icmp32,
    PK_Icmp64,
    PK_Fcmp,
    PK_Trunc,
    PK_Arith // A flag-setting arithmetic instruction.
  };

  /// Currently the actual enum values are not used (other than CK_None), but we
  /// go ahead and produce them anyway for symmetry with the
  /// BoolFoldingProducerKind.
  enum BoolFoldingConsumerKind { CK_None, CK_Br, CK_Select, CK_Sext, CK_Zext };

private:
  BoolFolding(const BoolFolding &) = delete;
  BoolFolding &operator=(const BoolFolding &) = delete;

public:
  BoolFolding() = default;
  static BoolFoldingProducerKind getProducerKind(const Inst *Instr);
  static BoolFoldingConsumerKind getConsumerKind(const Inst *Instr);
  static bool hasComplexLowering(const Inst *Instr);
  void init(CfgNode *Node);
  const Inst *getProducerFor(const Operand *Opnd) const;
  void dump(const Cfg *Func) const;

private:
  /// Returns true if Producers contains a valid entry for the given VarNum.
  bool containsValid(SizeT VarNum) const {
    auto Element = Producers.find(VarNum);
    return Element != Producers.end() && Element->second.Instr != nullptr;
  }
  void setInvalid(SizeT VarNum) { Producers[VarNum].Instr = nullptr; }
  /// Producers maps Variable::Number to a BoolFoldingEntry.
  std::unordered_map<SizeT, BoolFoldingEntry<MachineTraits>> Producers;
};

template <class MachineTraits>
BoolFoldingEntry<MachineTraits>::BoolFoldingEntry(Inst *I)
    : Instr(I), IsComplex(BoolFolding<MachineTraits>::hasComplexLowering(I)) {}

template <class MachineTraits>
typename BoolFolding<MachineTraits>::BoolFoldingProducerKind
BoolFolding<MachineTraits>::getProducerKind(const Inst *Instr) {
  if (llvm::isa<InstIcmp>(Instr)) {
    if (MachineTraits::Is64Bit || Instr->getSrc(0)->getType() != IceType_i64)
      return PK_Icmp32;
    return PK_Icmp64;
  }
  if (llvm::isa<InstFcmp>(Instr))
    return PK_Fcmp;
  if (auto *Arith = llvm::dyn_cast<InstArithmetic>(Instr)) {
    if (MachineTraits::Is64Bit || Arith->getSrc(0)->getType() != IceType_i64) {
      switch (Arith->getOp()) {
      default:
        return PK_None;
      case InstArithmetic::And:
      case InstArithmetic::Or:
        return PK_Arith;
      }
    }
  }
  return PK_None; // TODO(stichnot): remove this

  if (auto *Cast = llvm::dyn_cast<InstCast>(Instr)) {
    switch (Cast->getCastKind()) {
    default:
      return PK_None;
    case InstCast::Trunc:
      return PK_Trunc;
    }
  }
  return PK_None;
}

template <class MachineTraits>
typename BoolFolding<MachineTraits>::BoolFoldingConsumerKind
BoolFolding<MachineTraits>::getConsumerKind(const Inst *Instr) {
  if (llvm::isa<InstBr>(Instr))
    return CK_Br;
  if (llvm::isa<InstSelect>(Instr))
    return CK_Select;
  return CK_None; // TODO(stichnot): remove this

  if (auto *Cast = llvm::dyn_cast<InstCast>(Instr)) {
    switch (Cast->getCastKind()) {
    default:
      return CK_None;
    case InstCast::Sext:
      return CK_Sext;
    case InstCast::Zext:
      return CK_Zext;
    }
  }
  return CK_None;
}

/// Returns true if the producing instruction has a "complex" lowering sequence.
/// This generally means that its lowering sequence requires more than one
/// conditional branch, namely 64-bit integer compares and some floating-point
/// compares. When this is true, and there is more than one consumer, we prefer
/// to disable the folding optimization because it minimizes branches.
template <class MachineTraits>
bool BoolFolding<MachineTraits>::hasComplexLowering(const Inst *Instr) {
  switch (getProducerKind(Instr)) {
  default:
    return false;
  case PK_Icmp64:
    return true;
  case PK_Fcmp:
    return MachineTraits::TableFcmp[llvm::cast<InstFcmp>(Instr)->getCondition()]
               .C2 != MachineTraits::Cond::Br_None;
  }
}

template <class MachineTraits>
void BoolFolding<MachineTraits>::init(CfgNode *Node) {
  Producers.clear();
  for (Inst &Instr : Node->getInsts()) {
    // Check whether Instr is a valid producer.
    Variable *Var = Instr.getDest();
    if (!Instr.isDeleted() // only consider non-deleted instructions
        && Var             // only instructions with an actual dest var
        && Var->getType() == IceType_i1          // only bool-type dest vars
        && getProducerKind(&Instr) != PK_None) { // white-listed instructions
      Producers[Var->getIndex()] = BoolFoldingEntry<MachineTraits>(&Instr);
    }
    // Check each src variable against the map.
    FOREACH_VAR_IN_INST(Var, Instr) {
      SizeT VarNum = Var->getIndex();
      if (containsValid(VarNum)) {
        if (IndexOfVarOperandInInst(Var) !=
                0 // All valid consumers use Var as the first source operand
            ||
            getConsumerKind(&Instr) == CK_None // must be white-listed
            ||
            (getConsumerKind(&Instr) != CK_Br && // Icmp64 only folds in branch
             getProducerKind(Producers[VarNum].Instr) != PK_Icmp32) ||
            (Producers[VarNum].IsComplex && // complex can't be multi-use
             Producers[VarNum].NumUses > 0)) {
          setInvalid(VarNum);
          continue;
        }
        ++Producers[VarNum].NumUses;
        if (Instr.isLastUse(Var)) {
          Producers[VarNum].IsLiveOut = false;
        }
      }
    }
  }
  for (auto &I : Producers) {
    // Ignore entries previously marked invalid.
    if (I.second.Instr == nullptr)
      continue;
    // Disable the producer if its dest may be live beyond this block.
    if (I.second.IsLiveOut) {
      setInvalid(I.first);
      continue;
    }
    // Mark as "dead" rather than outright deleting. This is so that other
    // peephole style optimizations during or before lowering have access to
    // this instruction in undeleted form. See for example
    // tryOptimizedCmpxchgCmpBr().
    I.second.Instr->setDead();
  }
}

template <class MachineTraits>
const Inst *
BoolFolding<MachineTraits>::getProducerFor(const Operand *Opnd) const {
  auto *Var = llvm::dyn_cast<const Variable>(Opnd);
  if (Var == nullptr)
    return nullptr;
  SizeT VarNum = Var->getIndex();
  auto Element = Producers.find(VarNum);
  if (Element == Producers.end())
    return nullptr;
  return Element->second.Instr;
}

template <class MachineTraits>
void BoolFolding<MachineTraits>::dump(const Cfg *Func) const {
  if (!BuildDefs::dump() || !Func->isVerbose(IceV_Folding))
    return;
  OstreamLocker L(Func->getContext());
  Ostream &Str = Func->getContext()->getStrDump();
  for (auto &I : Producers) {
    if (I.second.Instr == nullptr)
      continue;
    Str << "Found foldable producer:\n  ";
    I.second.Instr->dump(Func);
    Str << "\n";
  }
}

template <class Machine>
void TargetX86Base<Machine>::initNodeForLowering(CfgNode *Node) {
  FoldingInfo.init(Node);
  FoldingInfo.dump(Func);
}

template <class Machine>
TargetX86Base<Machine>::TargetX86Base(Cfg *Func)
    : TargetLowering(Func) {
  static_assert(
      (Traits::InstructionSet::End - Traits::InstructionSet::Begin) ==
          (TargetInstructionSet::X86InstructionSet_End -
           TargetInstructionSet::X86InstructionSet_Begin),
      "Traits::InstructionSet range different from TargetInstructionSet");
  if (Func->getContext()->getFlags().getTargetInstructionSet() !=
      TargetInstructionSet::BaseInstructionSet) {
    InstructionSet = static_cast<typename Traits::InstructionSet>(
        (Func->getContext()->getFlags().getTargetInstructionSet() -
         TargetInstructionSet::X86InstructionSet_Begin) +
        Traits::InstructionSet::Begin);
  }
}

template <class Machine> void TargetX86Base<Machine>::staticInit() {
  Traits::initRegisterSet(&TypeToRegisterSet, &RegisterAliases, &ScratchRegs);
}

template <class Machine> void TargetX86Base<Machine>::translateO2() {
  TimerMarker T(TimerStack::TT_O2, Func);

  genTargetHelperCalls();
  Func->dump("After target helper call insertion");

  // Merge Alloca instructions, and lay out the stack.
  static constexpr bool SortAndCombineAllocas = true;
  Func->processAllocas(SortAndCombineAllocas);
  Func->dump("After Alloca processing");

  if (!Ctx->getFlags().getPhiEdgeSplit()) {
    // Lower Phi instructions.
    Func->placePhiLoads();
    if (Func->hasError())
      return;
    Func->placePhiStores();
    if (Func->hasError())
      return;
    Func->deletePhis();
    if (Func->hasError())
      return;
    Func->dump("After Phi lowering");
  }

  // Run this early so it can be used to focus optimizations on potentially hot
  // code.
  // TODO(stichnot,ascull): currently only used for regalloc not
  // expensive high level optimizations which could be focused on potentially
  // hot code.
  Func->computeLoopNestDepth();
  Func->dump("After loop nest depth analysis");

  // Address mode optimization.
  Func->getVMetadata()->init(VMK_SingleDefs);
  Func->doAddressOpt();

  // Find read-modify-write opportunities. Do this after address mode
  // optimization so that doAddressOpt() doesn't need to be applied to RMW
  // instructions as well.
  findRMW();
  Func->dump("After RMW transform");

  // Argument lowering
  Func->doArgLowering();

  // Target lowering. This requires liveness analysis for some parts of the
  // lowering decisions, such as compare/branch fusing. If non-lightweight
  // liveness analysis is used, the instructions need to be renumbered first
  // TODO: This renumbering should only be necessary if we're actually
  // calculating live intervals, which we only do for register allocation.
  Func->renumberInstructions();
  if (Func->hasError())
    return;

  // TODO: It should be sufficient to use the fastest liveness calculation,
  // i.e. livenessLightweight(). However, for some reason that slows down the
  // rest of the translation. Investigate.
  Func->liveness(Liveness_Basic);
  if (Func->hasError())
    return;
  Func->dump("After x86 address mode opt");

  // Disable constant blinding or pooling for load optimization.
  {
    BoolFlagSaver B(RandomizationPoolingPaused, true);
    doLoadOpt();
  }
  Func->genCode();
  if (Func->hasError())
    return;
  Func->dump("After x86 codegen");

  // Register allocation. This requires instruction renumbering and full
  // liveness analysis. Loops must be identified before liveness so variable
  // use weights are correct.
  Func->renumberInstructions();
  if (Func->hasError())
    return;
  Func->liveness(Liveness_Intervals);
  if (Func->hasError())
    return;
  // Validate the live range computations. The expensive validation call is
  // deliberately only made when assertions are enabled.
  assert(Func->validateLiveness());
  // The post-codegen dump is done here, after liveness analysis and associated
  // cleanup, to make the dump cleaner and more useful.
  Func->dump("After initial x8632 codegen");
  Func->getVMetadata()->init(VMK_All);
  regAlloc(RAK_Global);
  if (Func->hasError())
    return;
  Func->dump("After linear scan regalloc");

  if (Ctx->getFlags().getPhiEdgeSplit()) {
    Func->advancedPhiLowering();
    Func->dump("After advanced Phi lowering");
  }

  // Stack frame mapping.
  Func->genFrame();
  if (Func->hasError())
    return;
  Func->dump("After stack frame mapping");

  Func->contractEmptyNodes();
  Func->reorderNodes();

  // Shuffle basic block order if -reorder-basic-blocks is enabled.
  Func->shuffleNodes();

  // Branch optimization.  This needs to be done just before code emission. In
  // particular, no transformations that insert or reorder CfgNodes should be
  // done after branch optimization. We go ahead and do it before nop insertion
  // to reduce the amount of work needed for searching for opportunities.
  Func->doBranchOpt();
  Func->dump("After branch optimization");

  // Nop insertion if -nop-insertion is enabled.
  Func->doNopInsertion();

  // Mark nodes that require sandbox alignment
  if (Ctx->getFlags().getUseSandboxing())
    Func->markNodesForSandboxing();
}

template <class Machine> void TargetX86Base<Machine>::translateOm1() {
  TimerMarker T(TimerStack::TT_Om1, Func);

  genTargetHelperCalls();

  // Do not merge Alloca instructions, and lay out the stack.
  static constexpr bool SortAndCombineAllocas = false;
  Func->processAllocas(SortAndCombineAllocas);
  Func->dump("After Alloca processing");

  Func->placePhiLoads();
  if (Func->hasError())
    return;
  Func->placePhiStores();
  if (Func->hasError())
    return;
  Func->deletePhis();
  if (Func->hasError())
    return;
  Func->dump("After Phi lowering");

  Func->doArgLowering();
  Func->genCode();
  if (Func->hasError())
    return;
  Func->dump("After initial x8632 codegen");

  regAlloc(RAK_InfOnly);
  if (Func->hasError())
    return;
  Func->dump("After regalloc of infinite-weight variables");

  Func->genFrame();
  if (Func->hasError())
    return;
  Func->dump("After stack frame mapping");

  // Shuffle basic block order if -reorder-basic-blocks is enabled.
  Func->shuffleNodes();

  // Nop insertion if -nop-insertion is enabled.
  Func->doNopInsertion();

  // Mark nodes that require sandbox alignment
  if (Ctx->getFlags().getUseSandboxing())
    Func->markNodesForSandboxing();
}

inline bool canRMW(const InstArithmetic *Arith) {
  Type Ty = Arith->getDest()->getType();
  // X86 vector instructions write to a register and have no RMW option.
  if (isVectorType(Ty))
    return false;
  bool isI64 = Ty == IceType_i64;

  switch (Arith->getOp()) {
  // Not handled for lack of simple lowering:
  //   shift on i64
  //   mul, udiv, urem, sdiv, srem, frem
  // Not handled for lack of RMW instructions:
  //   fadd, fsub, fmul, fdiv (also vector types)
  default:
    return false;
  case InstArithmetic::Add:
  case InstArithmetic::Sub:
  case InstArithmetic::And:
  case InstArithmetic::Or:
  case InstArithmetic::Xor:
    return true;
  case InstArithmetic::Shl:
  case InstArithmetic::Lshr:
  case InstArithmetic::Ashr:
    return false; // TODO(stichnot): implement
    return !isI64;
  }
}

template <class Machine>
bool isSameMemAddressOperand(const Operand *A, const Operand *B) {
  if (A == B)
    return true;
  if (auto *MemA = llvm::dyn_cast<
          typename TargetX86Base<Machine>::Traits::X86OperandMem>(A)) {
    if (auto *MemB = llvm::dyn_cast<
            typename TargetX86Base<Machine>::Traits::X86OperandMem>(B)) {
      return MemA->getBase() == MemB->getBase() &&
             MemA->getOffset() == MemB->getOffset() &&
             MemA->getIndex() == MemB->getIndex() &&
             MemA->getShift() == MemB->getShift() &&
             MemA->getSegmentRegister() == MemB->getSegmentRegister();
    }
  }
  return false;
}

template <class Machine> void TargetX86Base<Machine>::findRMW() {
  Func->dump("Before RMW");
  if (Func->isVerbose(IceV_RMW))
    Func->getContext()->lockStr();
  for (CfgNode *Node : Func->getNodes()) {
    // Walk through the instructions, considering each sequence of 3
    // instructions, and look for the particular RMW pattern. Note that this
    // search can be "broken" (false negatives) if there are intervening
    // deleted instructions, or intervening instructions that could be safely
    // moved out of the way to reveal an RMW pattern.
    auto E = Node->getInsts().end();
    auto I1 = E, I2 = E, I3 = Node->getInsts().begin();
    for (; I3 != E; I1 = I2, I2 = I3, ++I3) {
      // Make I3 skip over deleted instructions.
      while (I3 != E && I3->isDeleted())
        ++I3;
      if (I1 == E || I2 == E || I3 == E)
        continue;
      assert(!I1->isDeleted());
      assert(!I2->isDeleted());
      assert(!I3->isDeleted());
      auto *Load = llvm::dyn_cast<InstLoad>(I1);
      auto *Arith = llvm::dyn_cast<InstArithmetic>(I2);
      auto *Store = llvm::dyn_cast<InstStore>(I3);
      if (!Load || !Arith || !Store)
        continue;
      // Look for:
      //   a = Load addr
      //   b = <op> a, other
      //   Store b, addr
      // Change to:
      //   a = Load addr
      //   b = <op> a, other
      //   x = FakeDef
      //   RMW <op>, addr, other, x
      //   b = Store b, addr, x
      // Note that inferTwoAddress() makes sure setDestRedefined() gets called
      // on the updated Store instruction, to avoid liveness problems later.
      //
      // With this transformation, the Store instruction acquires a Dest
      // variable and is now subject to dead code elimination if there are no
      // more uses of "b".  Variable "x" is a beacon for determining whether the
      // Store instruction gets dead-code eliminated.  If the Store instruction
      // is eliminated, then it must be the case that the RMW instruction ends
      // x's live range, and therefore the RMW instruction will be retained and
      // later lowered.  On the other hand, if the RMW instruction does not end
      // x's live range, then the Store instruction must still be present, and
      // therefore the RMW instruction is ignored during lowering because it is
      // redundant with the Store instruction.
      //
      // Note that if "a" has further uses, the RMW transformation may still
      // trigger, resulting in two loads and one store, which is worse than the
      // original one load and one store.  However, this is probably rare, and
      // caching probably keeps it just as fast.
      if (!isSameMemAddressOperand<Machine>(Load->getSourceAddress(),
                                            Store->getAddr()))
        continue;
      Operand *ArithSrcFromLoad = Arith->getSrc(0);
      Operand *ArithSrcOther = Arith->getSrc(1);
      if (ArithSrcFromLoad != Load->getDest()) {
        if (!Arith->isCommutative() || ArithSrcOther != Load->getDest())
          continue;
        std::swap(ArithSrcFromLoad, ArithSrcOther);
      }
      if (Arith->getDest() != Store->getData())
        continue;
      if (!canRMW(Arith))
        continue;
      if (Func->isVerbose(IceV_RMW)) {
        Ostream &Str = Func->getContext()->getStrDump();
        Str << "Found RMW in " << Func->getFunctionName() << ":\n  ";
        Load->dump(Func);
        Str << "\n  ";
        Arith->dump(Func);
        Str << "\n  ";
        Store->dump(Func);
        Str << "\n";
      }
      Variable *Beacon = Func->makeVariable(IceType_i32);
      Beacon->setMustNotHaveReg();
      Store->setRmwBeacon(Beacon);
      InstFakeDef *BeaconDef = InstFakeDef::create(Func, Beacon);
      Node->getInsts().insert(I3, BeaconDef);
      auto *RMW = Traits::Insts::FakeRMW::create(
          Func, ArithSrcOther, Store->getAddr(), Beacon, Arith->getOp());
      Node->getInsts().insert(I3, RMW);
    }
  }
  if (Func->isVerbose(IceV_RMW))
    Func->getContext()->unlockStr();
}

// Converts a ConstantInteger32 operand into its constant value, or
// MemoryOrderInvalid if the operand is not a ConstantInteger32.
inline uint64_t getConstantMemoryOrder(Operand *Opnd) {
  if (auto *Integer = llvm::dyn_cast<ConstantInteger32>(Opnd))
    return Integer->getValue();
  return Intrinsics::MemoryOrderInvalid;
}

/// Determines whether the dest of a Load instruction can be folded into one of
/// the src operands of a 2-operand instruction. This is true as long as the
/// load dest matches exactly one of the binary instruction's src operands.
/// Replaces Src0 or Src1 with LoadSrc if the answer is true.
inline bool canFoldLoadIntoBinaryInst(Operand *LoadSrc, Variable *LoadDest,
                                      Operand *&Src0, Operand *&Src1) {
  if (Src0 == LoadDest && Src1 != LoadDest) {
    Src0 = LoadSrc;
    return true;
  }
  if (Src0 != LoadDest && Src1 == LoadDest) {
    Src1 = LoadSrc;
    return true;
  }
  return false;
}

template <class Machine> void TargetX86Base<Machine>::doLoadOpt() {
  for (CfgNode *Node : Func->getNodes()) {
    Context.init(Node);
    while (!Context.atEnd()) {
      Variable *LoadDest = nullptr;
      Operand *LoadSrc = nullptr;
      Inst *CurInst = Context.getCur();
      Inst *Next = Context.getNextInst();
      // Determine whether the current instruction is a Load instruction or
      // equivalent.
      if (auto *Load = llvm::dyn_cast<InstLoad>(CurInst)) {
        // An InstLoad always qualifies.
        LoadDest = Load->getDest();
        constexpr bool DoLegalize = false;
        LoadSrc = formMemoryOperand(Load->getSourceAddress(),
                                    LoadDest->getType(), DoLegalize);
      } else if (auto *Intrin = llvm::dyn_cast<InstIntrinsicCall>(CurInst)) {
        // An AtomicLoad intrinsic qualifies as long as it has a valid memory
        // ordering, and can be implemented in a single instruction (i.e., not
        // i64 on x86-32).
        Intrinsics::IntrinsicID ID = Intrin->getIntrinsicInfo().ID;
        if (ID == Intrinsics::AtomicLoad &&
            (Traits::Is64Bit || Intrin->getDest()->getType() != IceType_i64) &&
            Intrinsics::isMemoryOrderValid(
                ID, getConstantMemoryOrder(Intrin->getArg(1)))) {
          LoadDest = Intrin->getDest();
          constexpr bool DoLegalize = false;
          LoadSrc = formMemoryOperand(Intrin->getArg(0), LoadDest->getType(),
                                      DoLegalize);
        }
      }
      // A Load instruction can be folded into the following instruction only
      // if the following instruction ends the Load's Dest variable's live
      // range.
      if (LoadDest && Next && Next->isLastUse(LoadDest)) {
        assert(LoadSrc);
        Inst *NewInst = nullptr;
        if (auto *Arith = llvm::dyn_cast<InstArithmetic>(Next)) {
          Operand *Src0 = Arith->getSrc(0);
          Operand *Src1 = Arith->getSrc(1);
          if (canFoldLoadIntoBinaryInst(LoadSrc, LoadDest, Src0, Src1)) {
            NewInst = InstArithmetic::create(Func, Arith->getOp(),
                                             Arith->getDest(), Src0, Src1);
          }
        } else if (auto *Icmp = llvm::dyn_cast<InstIcmp>(Next)) {
          Operand *Src0 = Icmp->getSrc(0);
          Operand *Src1 = Icmp->getSrc(1);
          if (canFoldLoadIntoBinaryInst(LoadSrc, LoadDest, Src0, Src1)) {
            NewInst = InstIcmp::create(Func, Icmp->getCondition(),
                                       Icmp->getDest(), Src0, Src1);
          }
        } else if (auto *Fcmp = llvm::dyn_cast<InstFcmp>(Next)) {
          Operand *Src0 = Fcmp->getSrc(0);
          Operand *Src1 = Fcmp->getSrc(1);
          if (canFoldLoadIntoBinaryInst(LoadSrc, LoadDest, Src0, Src1)) {
            NewInst = InstFcmp::create(Func, Fcmp->getCondition(),
                                       Fcmp->getDest(), Src0, Src1);
          }
        } else if (auto *Select = llvm::dyn_cast<InstSelect>(Next)) {
          Operand *Src0 = Select->getTrueOperand();
          Operand *Src1 = Select->getFalseOperand();
          if (canFoldLoadIntoBinaryInst(LoadSrc, LoadDest, Src0, Src1)) {
            NewInst = InstSelect::create(Func, Select->getDest(),
                                         Select->getCondition(), Src0, Src1);
          }
        } else if (auto *Cast = llvm::dyn_cast<InstCast>(Next)) {
          // The load dest can always be folded into a Cast instruction.
          Variable *Src0 = llvm::dyn_cast<Variable>(Cast->getSrc(0));
          if (Src0 == LoadDest) {
            NewInst = InstCast::create(Func, Cast->getCastKind(),
                                       Cast->getDest(), LoadSrc);
          }
        }
        if (NewInst) {
          CurInst->setDeleted();
          Next->setDeleted();
          Context.insert(NewInst);
          // Update NewInst->LiveRangesEnded so that target lowering may
          // benefit. Also update NewInst->HasSideEffects.
          NewInst->spliceLivenessInfo(Next, CurInst);
        }
      }
      Context.advanceCur();
      Context.advanceNext();
    }
  }
  Func->dump("After load optimization");
}

template <class Machine>
bool TargetX86Base<Machine>::doBranchOpt(Inst *I, const CfgNode *NextNode) {
  if (auto *Br = llvm::dyn_cast<typename Traits::Insts::Br>(I)) {
    return Br->optimizeBranch(NextNode);
  }
  return false;
}

template <class Machine>
Variable *TargetX86Base<Machine>::getPhysicalRegister(SizeT RegNum, Type Ty) {
  // Special case: never allow partial reads/writes to/from %rBP and %rSP.
  if (RegNum == Traits::RegisterSet::Reg_esp ||
      RegNum == Traits::RegisterSet::Reg_ebp)
    Ty = Traits::WordType;
  if (Ty == IceType_void)
    Ty = IceType_i32;
  if (PhysicalRegisters[Ty].empty())
    PhysicalRegisters[Ty].resize(Traits::RegisterSet::Reg_NUM);
  assert(RegNum < PhysicalRegisters[Ty].size());
  Variable *Reg = PhysicalRegisters[Ty][RegNum];
  if (Reg == nullptr) {
    Reg = Func->makeVariable(Ty);
    Reg->setRegNum(RegNum);
    PhysicalRegisters[Ty][RegNum] = Reg;
    // Specially mark a named physical register as an "argument" so that it is
    // considered live upon function entry.  Otherwise it's possible to get
    // liveness validation errors for saving callee-save registers.
    Func->addImplicitArg(Reg);
    // Don't bother tracking the live range of a named physical register.
    Reg->setIgnoreLiveness();
  }
  return Reg;
}

template <class Machine>
IceString TargetX86Base<Machine>::getRegName(SizeT RegNum, Type) const {
  return Traits::getRegName(RegNum);
}

template <class Machine>
void TargetX86Base<Machine>::emitVariable(const Variable *Var) const {
  if (!BuildDefs::dump())
    return;
  Ostream &Str = Ctx->getStrEmit();
  if (Var->hasReg()) {
    Str << "%" << getRegName(Var->getRegNum(), Var->getType());
    return;
  }
  if (Var->mustHaveReg()) {
    llvm_unreachable("Infinite-weight Variable has no register assigned");
  }
  const int32_t Offset = Var->getStackOffset();
  int32_t BaseRegNum = Var->getBaseRegNum();
  if (BaseRegNum == Variable::NoRegister)
    BaseRegNum = getFrameOrStackReg();
  // Print in the form "Offset(%reg)", taking care that:
  //   - Offset is never printed when it is 0

  const bool DecorateAsm = Func->getContext()->getFlags().getDecorateAsm();
  // Only print Offset when it is nonzero, regardless of DecorateAsm.
  if (Offset) {
    if (DecorateAsm) {
      Str << Var->getSymbolicStackOffset(Func);
    } else {
      Str << Offset;
    }
  }
  const Type FrameSPTy = Traits::WordType;
  Str << "(%" << getRegName(BaseRegNum, FrameSPTy) << ")";
}

template <class Machine>
typename TargetX86Base<Machine>::Traits::Address
TargetX86Base<Machine>::stackVarToAsmOperand(const Variable *Var) const {
  if (Var->hasReg())
    llvm_unreachable("Stack Variable has a register assigned");
  if (Var->mustHaveReg()) {
    llvm_unreachable("Infinite-weight Variable has no register assigned");
  }
  int32_t Offset = Var->getStackOffset();
  int32_t BaseRegNum = Var->getBaseRegNum();
  if (Var->getBaseRegNum() == Variable::NoRegister)
    BaseRegNum = getFrameOrStackReg();
  return typename Traits::Address(Traits::getEncodedGPR(BaseRegNum), Offset,
                                  AssemblerFixup::NoFixup);
}

/// Helper function for addProlog().
///
/// This assumes Arg is an argument passed on the stack. This sets the frame
/// offset for Arg and updates InArgsSizeBytes according to Arg's width. For an
/// I64 arg that has been split into Lo and Hi components, it calls itself
/// recursively on the components, taking care to handle Lo first because of the
/// little-endian architecture. Lastly, this function generates an instruction
/// to copy Arg into its assigned register if applicable.
template <class Machine>
void TargetX86Base<Machine>::finishArgumentLowering(Variable *Arg,
                                                    Variable *FramePtr,
                                                    size_t BasicFrameOffset,
                                                    size_t StackAdjBytes,
                                                    size_t &InArgsSizeBytes) {
  if (!Traits::Is64Bit) {
    if (auto *Arg64On32 = llvm::dyn_cast<Variable64On32>(Arg)) {
      Variable *Lo = Arg64On32->getLo();
      Variable *Hi = Arg64On32->getHi();
      finishArgumentLowering(Lo, FramePtr, BasicFrameOffset, StackAdjBytes,
                             InArgsSizeBytes);
      finishArgumentLowering(Hi, FramePtr, BasicFrameOffset, StackAdjBytes,
                             InArgsSizeBytes);
      return;
    }
  }
  Type Ty = Arg->getType();
  if (isVectorType(Ty)) {
    InArgsSizeBytes = Traits::applyStackAlignment(InArgsSizeBytes);
  }
  Arg->setStackOffset(BasicFrameOffset + InArgsSizeBytes);
  InArgsSizeBytes += typeWidthInBytesOnStack(Ty);
  if (Arg->hasReg()) {
    assert(Ty != IceType_i64 || Traits::Is64Bit);
    typename Traits::X86OperandMem *Mem = Traits::X86OperandMem::create(
        Func, Ty, FramePtr,
        Ctx->getConstantInt32(Arg->getStackOffset() + StackAdjBytes));
    if (isVectorType(Arg->getType())) {
      _movp(Arg, Mem);
    } else {
      _mov(Arg, Mem);
    }
    // This argument-copying instruction uses an explicit Traits::X86OperandMem
    // operand instead of a Variable, so its fill-from-stack operation has to
    // be tracked separately for statistics.
    Ctx->statsUpdateFills();
  }
}

template <class Machine> Type TargetX86Base<Machine>::stackSlotType() {
  return Traits::WordType;
}

template <class Machine>
template <typename T>
typename std::enable_if<!T::Is64Bit, Operand>::type *
TargetX86Base<Machine>::loOperand(Operand *Operand) {
  assert(Operand->getType() == IceType_i64 ||
         Operand->getType() == IceType_f64);
  if (Operand->getType() != IceType_i64 && Operand->getType() != IceType_f64)
    return Operand;
  if (auto *Var64On32 = llvm::dyn_cast<Variable64On32>(Operand))
    return Var64On32->getLo();
  if (auto *Const = llvm::dyn_cast<ConstantInteger64>(Operand)) {
    auto *ConstInt = llvm::dyn_cast<ConstantInteger32>(
        Ctx->getConstantInt32(static_cast<int32_t>(Const->getValue())));
    // Check if we need to blind/pool the constant.
    return legalize(ConstInt);
  }
  if (auto *Mem = llvm::dyn_cast<typename Traits::X86OperandMem>(Operand)) {
    auto *MemOperand = Traits::X86OperandMem::create(
        Func, IceType_i32, Mem->getBase(), Mem->getOffset(), Mem->getIndex(),
        Mem->getShift(), Mem->getSegmentRegister());
    // Test if we should randomize or pool the offset, if so randomize it or
    // pool it then create mem operand with the blinded/pooled constant.
    // Otherwise, return the mem operand as ordinary mem operand.
    return legalize(MemOperand);
  }
  llvm_unreachable("Unsupported operand type");
  return nullptr;
}

template <class Machine>
template <typename T>
typename std::enable_if<!T::Is64Bit, Operand>::type *
TargetX86Base<Machine>::hiOperand(Operand *Operand) {
  assert(Operand->getType() == IceType_i64 ||
         Operand->getType() == IceType_f64);
  if (Operand->getType() != IceType_i64 && Operand->getType() != IceType_f64)
    return Operand;
  if (auto *Var64On32 = llvm::dyn_cast<Variable64On32>(Operand))
    return Var64On32->getHi();
  if (auto *Const = llvm::dyn_cast<ConstantInteger64>(Operand)) {
    auto *ConstInt = llvm::dyn_cast<ConstantInteger32>(
        Ctx->getConstantInt32(static_cast<int32_t>(Const->getValue() >> 32)));
    // Check if we need to blind/pool the constant.
    return legalize(ConstInt);
  }
  if (auto *Mem = llvm::dyn_cast<typename Traits::X86OperandMem>(Operand)) {
    Constant *Offset = Mem->getOffset();
    if (Offset == nullptr) {
      Offset = Ctx->getConstantInt32(4);
    } else if (auto *IntOffset = llvm::dyn_cast<ConstantInteger32>(Offset)) {
      Offset = Ctx->getConstantInt32(4 + IntOffset->getValue());
    } else if (auto *SymOffset = llvm::dyn_cast<ConstantRelocatable>(Offset)) {
      assert(!Utils::WouldOverflowAdd(SymOffset->getOffset(), 4));
      Offset =
          Ctx->getConstantSym(4 + SymOffset->getOffset(), SymOffset->getName(),
                              SymOffset->getSuppressMangling());
    }
    auto *MemOperand = Traits::X86OperandMem::create(
        Func, IceType_i32, Mem->getBase(), Offset, Mem->getIndex(),
        Mem->getShift(), Mem->getSegmentRegister());
    // Test if the Offset is an eligible i32 constants for randomization and
    // pooling. Blind/pool it if it is. Otherwise return as oridinary mem
    // operand.
    return legalize(MemOperand);
  }
  llvm_unreachable("Unsupported operand type");
  return nullptr;
}

template <class Machine>
llvm::SmallBitVector
TargetX86Base<Machine>::getRegisterSet(RegSetMask Include,
                                       RegSetMask Exclude) const {
  return Traits::getRegisterSet(Include, Exclude);
}

template <class Machine>
void TargetX86Base<Machine>::lowerAlloca(const InstAlloca *Inst) {
  // Conservatively require the stack to be aligned. Some stack adjustment
  // operations implemented below assume that the stack is aligned before the
  // alloca. All the alloca code ensures that the stack alignment is preserved
  // after the alloca. The stack alignment restriction can be relaxed in some
  // cases.
  NeedsStackAlignment = true;

  // For default align=0, set it to the real value 1, to avoid any
  // bit-manipulation problems below.
  const uint32_t AlignmentParam = std::max(1u, Inst->getAlignInBytes());

  // LLVM enforces power of 2 alignment.
  assert(llvm::isPowerOf2_32(AlignmentParam));
  assert(llvm::isPowerOf2_32(Traits::X86_STACK_ALIGNMENT_BYTES));

  const uint32_t Alignment =
      std::max(AlignmentParam, Traits::X86_STACK_ALIGNMENT_BYTES);
  const bool OverAligned = Alignment > Traits::X86_STACK_ALIGNMENT_BYTES;
  const bool OptM1 = Ctx->getFlags().getOptLevel() == Opt_m1;
  const bool AllocaWithKnownOffset = Inst->getKnownFrameOffset();
  const bool UseFramePointer =
      hasFramePointer() || OverAligned || !AllocaWithKnownOffset || OptM1;

  if (UseFramePointer)
    setHasFramePointer();

  Variable *esp = getPhysicalRegister(Traits::RegisterSet::Reg_esp);
  if (OverAligned) {
    _and(esp, Ctx->getConstantInt32(-Alignment));
  }

  Variable *Dest = Inst->getDest();
  Operand *TotalSize = legalize(Inst->getSizeInBytes());

  if (const auto *ConstantTotalSize =
          llvm::dyn_cast<ConstantInteger32>(TotalSize)) {
    const uint32_t Value =
        Utils::applyAlignment(ConstantTotalSize->getValue(), Alignment);
    if (!UseFramePointer) {
      // If we don't need a Frame Pointer, this alloca has a known offset to the
      // stack pointer. We don't need adjust the stack pointer, nor assign any
      // value to Dest, as Dest is rematerializable.
      assert(Dest->isRematerializable());
      FixedAllocaSizeBytes += Value;
      Context.insert(InstFakeDef::create(Func, Dest));
    } else {
      _sub(esp, Ctx->getConstantInt32(Value));
    }
  } else {
    // Non-constant sizes need to be adjusted to the next highest multiple of
    // the required alignment at runtime.
    Variable *T = makeReg(IceType_i32);
    _mov(T, TotalSize);
    _add(T, Ctx->getConstantInt32(Alignment - 1));
    _and(T, Ctx->getConstantInt32(-Alignment));
    _sub(esp, T);
  }
  // Add enough to the returned address to account for the out args area.
  uint32_t OutArgsSize = maxOutArgsSizeBytes();
  if (OutArgsSize > 0) {
    Variable *T = makeReg(IceType_i32);
    typename Traits::X86OperandMem *CalculateOperand =
        Traits::X86OperandMem::create(
            Func, IceType_i32, esp,
            Ctx->getConstantInt(IceType_i32, OutArgsSize));
    _lea(T, CalculateOperand);
    _mov(Dest, T);
  } else {
    _mov(Dest, esp);
  }
}

/// Strength-reduce scalar integer multiplication by a constant (for i32 or
/// narrower) for certain constants. The lea instruction can be used to multiply
/// by 3, 5, or 9, and the lsh instruction can be used to multiply by powers of
/// 2. These can be combined such that e.g. multiplying by 100 can be done as 2
/// lea-based multiplies by 5, combined with left-shifting by 2.
template <class Machine>
bool TargetX86Base<Machine>::optimizeScalarMul(Variable *Dest, Operand *Src0,
                                               int32_t Src1) {
  // Disable this optimization for Om1 and O0, just to keep things simple
  // there.
  if (Ctx->getFlags().getOptLevel() < Opt_1)
    return false;
  Type Ty = Dest->getType();
  Variable *T = nullptr;
  if (Src1 == -1) {
    _mov(T, Src0);
    _neg(T);
    _mov(Dest, T);
    return true;
  }
  if (Src1 == 0) {
    _mov(Dest, Ctx->getConstantZero(Ty));
    return true;
  }
  if (Src1 == 1) {
    _mov(T, Src0);
    _mov(Dest, T);
    return true;
  }
  // Don't bother with the edge case where Src1 == MININT.
  if (Src1 == -Src1)
    return false;
  const bool Src1IsNegative = Src1 < 0;
  if (Src1IsNegative)
    Src1 = -Src1;
  uint32_t Count9 = 0;
  uint32_t Count5 = 0;
  uint32_t Count3 = 0;
  uint32_t Count2 = 0;
  uint32_t CountOps = 0;
  while (Src1 > 1) {
    if (Src1 % 9 == 0) {
      ++CountOps;
      ++Count9;
      Src1 /= 9;
    } else if (Src1 % 5 == 0) {
      ++CountOps;
      ++Count5;
      Src1 /= 5;
    } else if (Src1 % 3 == 0) {
      ++CountOps;
      ++Count3;
      Src1 /= 3;
    } else if (Src1 % 2 == 0) {
      if (Count2 == 0)
        ++CountOps;
      ++Count2;
      Src1 /= 2;
    } else {
      return false;
    }
  }
  // Lea optimization only works for i16 and i32 types, not i8.
  if (Ty != IceType_i16 && Ty != IceType_i32 && (Count3 || Count5 || Count9))
    return false;
  // Limit the number of lea/shl operations for a single multiply, to a
  // somewhat arbitrary choice of 3.
  constexpr uint32_t MaxOpsForOptimizedMul = 3;
  if (CountOps > MaxOpsForOptimizedMul)
    return false;
  _mov(T, Src0);
  Constant *Zero = Ctx->getConstantZero(IceType_i32);
  for (uint32_t i = 0; i < Count9; ++i) {
    constexpr uint16_t Shift = 3; // log2(9-1)
    _lea(T,
         Traits::X86OperandMem::create(Func, IceType_void, T, Zero, T, Shift));
  }
  for (uint32_t i = 0; i < Count5; ++i) {
    constexpr uint16_t Shift = 2; // log2(5-1)
    _lea(T,
         Traits::X86OperandMem::create(Func, IceType_void, T, Zero, T, Shift));
  }
  for (uint32_t i = 0; i < Count3; ++i) {
    constexpr uint16_t Shift = 1; // log2(3-1)
    _lea(T,
         Traits::X86OperandMem::create(Func, IceType_void, T, Zero, T, Shift));
  }
  if (Count2) {
    _shl(T, Ctx->getConstantInt(Ty, Count2));
  }
  if (Src1IsNegative)
    _neg(T);
  _mov(Dest, T);
  return true;
}

template <class Machine>
void TargetX86Base<Machine>::lowerShift64(InstArithmetic::OpKind Op,
                                          Operand *Src0Lo, Operand *Src0Hi,
                                          Operand *Src1Lo, Variable *DestLo,
                                          Variable *DestHi) {
  // TODO: Refactor the similarities between Shl, Lshr, and Ashr.
  Variable *T_1 = nullptr, *T_2 = nullptr, *T_3 = nullptr;
  Constant *Zero = Ctx->getConstantZero(IceType_i32);
  Constant *SignExtend = Ctx->getConstantInt32(0x1f);
  if (auto *ConstantShiftAmount = llvm::dyn_cast<ConstantInteger32>(Src1Lo)) {
    uint32_t ShiftAmount = ConstantShiftAmount->getValue();
    if (ShiftAmount > 32) {
      Constant *ReducedShift = Ctx->getConstantInt32(ShiftAmount - 32);
      switch (Op) {
      default:
        assert(0 && "non-shift op");
        break;
      case InstArithmetic::Shl: {
        // a=b<<c ==>
        //   t2 = b.lo
        //   t2 = shl t2, ShiftAmount-32
        //   t3 = t2
        //   t2 = 0
        _mov(T_2, Src0Lo);
        _shl(T_2, ReducedShift);
        _mov(DestHi, T_2);
        _mov(DestLo, Zero);
      } break;
      case InstArithmetic::Lshr: {
        // a=b>>c (unsigned) ==>
        //   t2 = b.hi
        //   t2 = shr t2, ShiftAmount-32
        //   a.lo = t2
        //   a.hi = 0
        _mov(T_2, Src0Hi);
        _shr(T_2, ReducedShift);
        _mov(DestLo, T_2);
        _mov(DestHi, Zero);
      } break;
      case InstArithmetic::Ashr: {
        // a=b>>c (signed) ==>
        //   t3 = b.hi
        //   t3 = sar t3, 0x1f
        //   t2 = b.hi
        //   t2 = shrd t2, t3, ShiftAmount-32
        //   a.lo = t2
        //   a.hi = t3
        _mov(T_3, Src0Hi);
        _sar(T_3, SignExtend);
        _mov(T_2, Src0Hi);
        _shrd(T_2, T_3, ReducedShift);
        _mov(DestLo, T_2);
        _mov(DestHi, T_3);
      } break;
      }
    } else if (ShiftAmount == 32) {
      switch (Op) {
      default:
        assert(0 && "non-shift op");
        break;
      case InstArithmetic::Shl: {
        // a=b<<c ==>
        //   t2 = b.lo
        //   a.hi = t2
        //   a.lo = 0
        _mov(T_2, Src0Lo);
        _mov(DestHi, T_2);
        _mov(DestLo, Zero);
      } break;
      case InstArithmetic::Lshr: {
        // a=b>>c (unsigned) ==>
        //   t2 = b.hi
        //   a.lo = t2
        //   a.hi = 0
        _mov(T_2, Src0Hi);
        _mov(DestLo, T_2);
        _mov(DestHi, Zero);
      } break;
      case InstArithmetic::Ashr: {
        // a=b>>c (signed) ==>
        //   t2 = b.hi
        //   a.lo = t2
        //   t3 = b.hi
        //   t3 = sar t3, 0x1f
        //   a.hi = t3
        _mov(T_2, Src0Hi);
        _mov(DestLo, T_2);
        _mov(T_3, Src0Hi);
        _sar(T_3, SignExtend);
        _mov(DestHi, T_3);
      } break;
      }
    } else {
      // COMMON PREFIX OF: a=b SHIFT_OP c ==>
      //   t2 = b.lo
      //   t3 = b.hi
      _mov(T_2, Src0Lo);
      _mov(T_3, Src0Hi);
      switch (Op) {
      default:
        assert(0 && "non-shift op");
        break;
      case InstArithmetic::Shl: {
        // a=b<<c ==>
        //   t3 = shld t3, t2, ShiftAmount
        //   t2 = shl t2, ShiftAmount
        _shld(T_3, T_2, ConstantShiftAmount);
        _shl(T_2, ConstantShiftAmount);
      } break;
      case InstArithmetic::Lshr: {
        // a=b>>c (unsigned) ==>
        //   t2 = shrd t2, t3, ShiftAmount
        //   t3 = shr t3, ShiftAmount
        _shrd(T_2, T_3, ConstantShiftAmount);
        _shr(T_3, ConstantShiftAmount);
      } break;
      case InstArithmetic::Ashr: {
        // a=b>>c (signed) ==>
        //   t2 = shrd t2, t3, ShiftAmount
        //   t3 = sar t3, ShiftAmount
        _shrd(T_2, T_3, ConstantShiftAmount);
        _sar(T_3, ConstantShiftAmount);
      } break;
      }
      // COMMON SUFFIX OF: a=b SHIFT_OP c ==>
      //   a.lo = t2
      //   a.hi = t3
      _mov(DestLo, T_2);
      _mov(DestHi, T_3);
    }
  } else {
    // NON-CONSTANT CASES.
    Constant *BitTest = Ctx->getConstantInt32(0x20);
    typename Traits::Insts::Label *Label =
        Traits::Insts::Label::create(Func, this);
    // COMMON PREFIX OF: a=b SHIFT_OP c ==>
    //   t1:ecx = c.lo & 0xff
    //   t2 = b.lo
    //   t3 = b.hi
    T_1 = copyToReg8(Src1Lo, Traits::RegisterSet::Reg_cl);
    _mov(T_2, Src0Lo);
    _mov(T_3, Src0Hi);
    switch (Op) {
    default:
      assert(0 && "non-shift op");
      break;
    case InstArithmetic::Shl: {
      // a=b<<c ==>
      //   t3 = shld t3, t2, t1
      //   t2 = shl t2, t1
      //   test t1, 0x20
      //   je L1
      //   use(t3)
      //   t3 = t2
      //   t2 = 0
      _shld(T_3, T_2, T_1);
      _shl(T_2, T_1);
      _test(T_1, BitTest);
      _br(Traits::Cond::Br_e, Label);
      // T_2 and T_3 are being assigned again because of the intra-block control
      // flow, so we need the _mov_redefined variant to avoid liveness problems.
      _mov_redefined(T_3, T_2);
      _mov_redefined(T_2, Zero);
    } break;
    case InstArithmetic::Lshr: {
      // a=b>>c (unsigned) ==>
      //   t2 = shrd t2, t3, t1
      //   t3 = shr t3, t1
      //   test t1, 0x20
      //   je L1
      //   use(t2)
      //   t2 = t3
      //   t3 = 0
      _shrd(T_2, T_3, T_1);
      _shr(T_3, T_1);
      _test(T_1, BitTest);
      _br(Traits::Cond::Br_e, Label);
      // T_2 and T_3 are being assigned again because of the intra-block control
      // flow, so we need the _mov_redefined variant to avoid liveness problems.
      _mov_redefined(T_2, T_3);
      _mov_redefined(T_3, Zero);
    } break;
    case InstArithmetic::Ashr: {
      // a=b>>c (signed) ==>
      //   t2 = shrd t2, t3, t1
      //   t3 = sar t3, t1
      //   test t1, 0x20
      //   je L1
      //   use(t2)
      //   t2 = t3
      //   t3 = sar t3, 0x1f
      Constant *SignExtend = Ctx->getConstantInt32(0x1f);
      _shrd(T_2, T_3, T_1);
      _sar(T_3, T_1);
      _test(T_1, BitTest);
      _br(Traits::Cond::Br_e, Label);
      // T_2 and T_3 are being assigned again because of the intra-block control
      // flow, so T_2 needs the _mov_redefined variant to avoid liveness
      // problems. T_3 doesn't need special treatment because it is reassigned
      // via _sar instead of _mov.
      _mov_redefined(T_2, T_3);
      _sar(T_3, SignExtend);
    } break;
    }
    // COMMON SUFFIX OF: a=b SHIFT_OP c ==>
    // L1:
    //   a.lo = t2
    //   a.hi = t3
    Context.insert(Label);
    _mov(DestLo, T_2);
    _mov(DestHi, T_3);
  }
}

template <class Machine>
void TargetX86Base<Machine>::lowerArithmetic(const InstArithmetic *Inst) {
  Variable *Dest = Inst->getDest();
  if (Dest->isRematerializable()) {
    Context.insert(InstFakeDef::create(Func, Dest));
    return;
  }
  Type Ty = Dest->getType();
  Operand *Src0 = legalize(Inst->getSrc(0));
  Operand *Src1 = legalize(Inst->getSrc(1));
  if (Inst->isCommutative()) {
    uint32_t SwapCount = 0;
    if (!llvm::isa<Variable>(Src0) && llvm::isa<Variable>(Src1)) {
      std::swap(Src0, Src1);
      ++SwapCount;
    }
    if (llvm::isa<Constant>(Src0) && !llvm::isa<Constant>(Src1)) {
      std::swap(Src0, Src1);
      ++SwapCount;
    }
    // Improve two-address code patterns by avoiding a copy to the dest
    // register when one of the source operands ends its lifetime here.
    if (!Inst->isLastUse(Src0) && Inst->isLastUse(Src1)) {
      std::swap(Src0, Src1);
      ++SwapCount;
    }
    assert(SwapCount <= 1);
    (void)SwapCount;
  }
  if (!Traits::Is64Bit && Ty == IceType_i64) {
    // These x86-32 helper-call-involved instructions are lowered in this
    // separate switch. This is because loOperand() and hiOperand() may insert
    // redundant instructions for constant blinding and pooling. Such redundant
    // instructions will fail liveness analysis under -Om1 setting. And,
    // actually these arguments do not need to be processed with loOperand()
    // and hiOperand() to be used.
    switch (Inst->getOp()) {
    case InstArithmetic::Udiv:
    case InstArithmetic::Sdiv:
    case InstArithmetic::Urem:
    case InstArithmetic::Srem:
      llvm::report_fatal_error("Helper call was expected");
      return;
    default:
      break;
    }

    Variable *DestLo = llvm::cast<Variable>(loOperand(Dest));
    Variable *DestHi = llvm::cast<Variable>(hiOperand(Dest));
    Operand *Src0Lo = loOperand(Src0);
    Operand *Src0Hi = hiOperand(Src0);
    Operand *Src1Lo = loOperand(Src1);
    Operand *Src1Hi = hiOperand(Src1);
    Variable *T_Lo = nullptr, *T_Hi = nullptr;
    switch (Inst->getOp()) {
    case InstArithmetic::_num:
      llvm_unreachable("Unknown arithmetic operator");
      break;
    case InstArithmetic::Add:
      _mov(T_Lo, Src0Lo);
      _add(T_Lo, Src1Lo);
      _mov(DestLo, T_Lo);
      _mov(T_Hi, Src0Hi);
      _adc(T_Hi, Src1Hi);
      _mov(DestHi, T_Hi);
      break;
    case InstArithmetic::And:
      _mov(T_Lo, Src0Lo);
      _and(T_Lo, Src1Lo);
      _mov(DestLo, T_Lo);
      _mov(T_Hi, Src0Hi);
      _and(T_Hi, Src1Hi);
      _mov(DestHi, T_Hi);
      break;
    case InstArithmetic::Or:
      _mov(T_Lo, Src0Lo);
      _or(T_Lo, Src1Lo);
      _mov(DestLo, T_Lo);
      _mov(T_Hi, Src0Hi);
      _or(T_Hi, Src1Hi);
      _mov(DestHi, T_Hi);
      break;
    case InstArithmetic::Xor:
      _mov(T_Lo, Src0Lo);
      _xor(T_Lo, Src1Lo);
      _mov(DestLo, T_Lo);
      _mov(T_Hi, Src0Hi);
      _xor(T_Hi, Src1Hi);
      _mov(DestHi, T_Hi);
      break;
    case InstArithmetic::Sub:
      _mov(T_Lo, Src0Lo);
      _sub(T_Lo, Src1Lo);
      _mov(DestLo, T_Lo);
      _mov(T_Hi, Src0Hi);
      _sbb(T_Hi, Src1Hi);
      _mov(DestHi, T_Hi);
      break;
    case InstArithmetic::Mul: {
      Variable *T_1 = nullptr, *T_2 = nullptr, *T_3 = nullptr;
      Variable *T_4Lo = makeReg(IceType_i32, Traits::RegisterSet::Reg_eax);
      Variable *T_4Hi = makeReg(IceType_i32, Traits::RegisterSet::Reg_edx);
      // gcc does the following:
      // a=b*c ==>
      //   t1 = b.hi; t1 *=(imul) c.lo
      //   t2 = c.hi; t2 *=(imul) b.lo
      //   t3:eax = b.lo
      //   t4.hi:edx,t4.lo:eax = t3:eax *(mul) c.lo
      //   a.lo = t4.lo
      //   t4.hi += t1
      //   t4.hi += t2
      //   a.hi = t4.hi
      // The mul instruction cannot take an immediate operand.
      Src1Lo = legalize(Src1Lo, Legal_Reg | Legal_Mem);
      _mov(T_1, Src0Hi);
      _imul(T_1, Src1Lo);
      _mov(T_2, Src1Hi);
      _imul(T_2, Src0Lo);
      _mov(T_3, Src0Lo, Traits::RegisterSet::Reg_eax);
      _mul(T_4Lo, T_3, Src1Lo);
      // The mul instruction produces two dest variables, edx:eax. We create a
      // fake definition of edx to account for this.
      Context.insert(InstFakeDef::create(Func, T_4Hi, T_4Lo));
      _mov(DestLo, T_4Lo);
      _add(T_4Hi, T_1);
      _add(T_4Hi, T_2);
      _mov(DestHi, T_4Hi);
    } break;
    case InstArithmetic::Shl:
    case InstArithmetic::Lshr:
    case InstArithmetic::Ashr:
      lowerShift64(Inst->getOp(), Src0Lo, Src0Hi, Src1Lo, DestLo, DestHi);
      break;
    case InstArithmetic::Fadd:
    case InstArithmetic::Fsub:
    case InstArithmetic::Fmul:
    case InstArithmetic::Fdiv:
    case InstArithmetic::Frem:
      llvm_unreachable("FP instruction with i64 type");
      break;
    case InstArithmetic::Udiv:
    case InstArithmetic::Sdiv:
    case InstArithmetic::Urem:
    case InstArithmetic::Srem:
      llvm_unreachable("Call-helper-involved instruction for i64 type \
                       should have already been handled before");
      break;
    }
    return;
  }
  if (isVectorType(Ty)) {
    // TODO: Trap on integer divide and integer modulo by zero. See:
    // https://code.google.com/p/nativeclient/issues/detail?id=3899
    if (llvm::isa<typename Traits::X86OperandMem>(Src1))
      Src1 = legalizeToReg(Src1);
    switch (Inst->getOp()) {
    case InstArithmetic::_num:
      llvm_unreachable("Unknown arithmetic operator");
      break;
    case InstArithmetic::Add: {
      Variable *T = makeReg(Ty);
      _movp(T, Src0);
      _padd(T, Src1);
      _movp(Dest, T);
    } break;
    case InstArithmetic::And: {
      Variable *T = makeReg(Ty);
      _movp(T, Src0);
      _pand(T, Src1);
      _movp(Dest, T);
    } break;
    case InstArithmetic::Or: {
      Variable *T = makeReg(Ty);
      _movp(T, Src0);
      _por(T, Src1);
      _movp(Dest, T);
    } break;
    case InstArithmetic::Xor: {
      Variable *T = makeReg(Ty);
      _movp(T, Src0);
      _pxor(T, Src1);
      _movp(Dest, T);
    } break;
    case InstArithmetic::Sub: {
      Variable *T = makeReg(Ty);
      _movp(T, Src0);
      _psub(T, Src1);
      _movp(Dest, T);
    } break;
    case InstArithmetic::Mul: {
      bool TypesAreValidForPmull = Ty == IceType_v4i32 || Ty == IceType_v8i16;
      bool InstructionSetIsValidForPmull =
          Ty == IceType_v8i16 || InstructionSet >= Traits::SSE4_1;
      if (TypesAreValidForPmull && InstructionSetIsValidForPmull) {
        Variable *T = makeReg(Ty);
        _movp(T, Src0);
        _pmull(T, Src0 == Src1 ? T : Src1);
        _movp(Dest, T);
      } else if (Ty == IceType_v4i32) {
        // Lowering sequence:
        // Note: The mask arguments have index 0 on the left.
        //
        // movups  T1, Src0
        // pshufd  T2, Src0, {1,0,3,0}
        // pshufd  T3, Src1, {1,0,3,0}
        // # T1 = {Src0[0] * Src1[0], Src0[2] * Src1[2]}
        // pmuludq T1, Src1
        // # T2 = {Src0[1] * Src1[1], Src0[3] * Src1[3]}
        // pmuludq T2, T3
        // # T1 = {lo(T1[0]), lo(T1[2]), lo(T2[0]), lo(T2[2])}
        // shufps  T1, T2, {0,2,0,2}
        // pshufd  T4, T1, {0,2,1,3}
        // movups  Dest, T4

        // Mask that directs pshufd to create a vector with entries
        // Src[1, 0, 3, 0]
        constexpr unsigned Constant1030 = 0x31;
        Constant *Mask1030 = Ctx->getConstantInt32(Constant1030);
        // Mask that directs shufps to create a vector with entries
        // Dest[0, 2], Src[0, 2]
        constexpr unsigned Mask0202 = 0x88;
        // Mask that directs pshufd to create a vector with entries
        // Src[0, 2, 1, 3]
        constexpr unsigned Mask0213 = 0xd8;
        Variable *T1 = makeReg(IceType_v4i32);
        Variable *T2 = makeReg(IceType_v4i32);
        Variable *T3 = makeReg(IceType_v4i32);
        Variable *T4 = makeReg(IceType_v4i32);
        _movp(T1, Src0);
        _pshufd(T2, Src0, Mask1030);
        _pshufd(T3, Src1, Mask1030);
        _pmuludq(T1, Src1);
        _pmuludq(T2, T3);
        _shufps(T1, T2, Ctx->getConstantInt32(Mask0202));
        _pshufd(T4, T1, Ctx->getConstantInt32(Mask0213));
        _movp(Dest, T4);
      } else if (Ty == IceType_v16i8) {
        llvm::report_fatal_error("Scalarized operation was expected");
      } else {
        llvm::report_fatal_error("Invalid vector multiply type");
      }
    } break;
    case InstArithmetic::Shl:
    case InstArithmetic::Lshr:
    case InstArithmetic::Ashr:
    case InstArithmetic::Udiv:
    case InstArithmetic::Urem:
    case InstArithmetic::Sdiv:
    case InstArithmetic::Srem:
      llvm::report_fatal_error("Scalarized operation was expected");
      break;
    case InstArithmetic::Fadd: {
      Variable *T = makeReg(Ty);
      _movp(T, Src0);
      _addps(T, Src1);
      _movp(Dest, T);
    } break;
    case InstArithmetic::Fsub: {
      Variable *T = makeReg(Ty);
      _movp(T, Src0);
      _subps(T, Src1);
      _movp(Dest, T);
    } break;
    case InstArithmetic::Fmul: {
      Variable *T = makeReg(Ty);
      _movp(T, Src0);
      _mulps(T, Src0 == Src1 ? T : Src1);
      _movp(Dest, T);
    } break;
    case InstArithmetic::Fdiv: {
      Variable *T = makeReg(Ty);
      _movp(T, Src0);
      _divps(T, Src1);
      _movp(Dest, T);
    } break;
    case InstArithmetic::Frem:
      llvm::report_fatal_error("Scalarized operation was expected");
      break;
    }
    return;
  }
  Variable *T_edx = nullptr;
  Variable *T = nullptr;
  switch (Inst->getOp()) {
  case InstArithmetic::_num:
    llvm_unreachable("Unknown arithmetic operator");
    break;
  case InstArithmetic::Add:
    _mov(T, Src0);
    _add(T, Src1);
    _mov(Dest, T);
    break;
  case InstArithmetic::And:
    _mov(T, Src0);
    _and(T, Src1);
    _mov(Dest, T);
    break;
  case InstArithmetic::Or:
    _mov(T, Src0);
    _or(T, Src1);
    _mov(Dest, T);
    break;
  case InstArithmetic::Xor:
    _mov(T, Src0);
    _xor(T, Src1);
    _mov(Dest, T);
    break;
  case InstArithmetic::Sub:
    _mov(T, Src0);
    _sub(T, Src1);
    _mov(Dest, T);
    break;
  case InstArithmetic::Mul:
    if (auto *C = llvm::dyn_cast<ConstantInteger32>(Src1)) {
      if (optimizeScalarMul(Dest, Src0, C->getValue()))
        return;
    }
    // The 8-bit version of imul only allows the form "imul r/m8" where T must
    // be in al.
    if (isByteSizedArithType(Ty)) {
      _mov(T, Src0, Traits::RegisterSet::Reg_al);
      Src1 = legalize(Src1, Legal_Reg | Legal_Mem);
      _imul(T, Src0 == Src1 ? T : Src1);
      _mov(Dest, T);
    } else if (auto *ImmConst = llvm::dyn_cast<ConstantInteger32>(Src1)) {
      T = makeReg(Ty);
      _imul_imm(T, Src0, ImmConst);
      _mov(Dest, T);
    } else {
      _mov(T, Src0);
      _imul(T, Src0 == Src1 ? T : Src1);
      _mov(Dest, T);
    }
    break;
  case InstArithmetic::Shl:
    _mov(T, Src0);
    if (!llvm::isa<ConstantInteger32>(Src1))
      Src1 = copyToReg8(Src1, Traits::RegisterSet::Reg_cl);
    _shl(T, Src1);
    _mov(Dest, T);
    break;
  case InstArithmetic::Lshr:
    _mov(T, Src0);
    if (!llvm::isa<ConstantInteger32>(Src1))
      Src1 = copyToReg8(Src1, Traits::RegisterSet::Reg_cl);
    _shr(T, Src1);
    _mov(Dest, T);
    break;
  case InstArithmetic::Ashr:
    _mov(T, Src0);
    if (!llvm::isa<ConstantInteger32>(Src1))
      Src1 = copyToReg8(Src1, Traits::RegisterSet::Reg_cl);
    _sar(T, Src1);
    _mov(Dest, T);
    break;
  case InstArithmetic::Udiv: {
    // div and idiv are the few arithmetic operators that do not allow
    // immediates as the operand.
    Src1 = legalize(Src1, Legal_Reg | Legal_Mem);
    uint32_t Eax = Traits::RegisterSet::Reg_eax;
    uint32_t Edx = Traits::RegisterSet::Reg_edx;
    switch (Ty) {
    default:
      llvm_unreachable("Bad type for udiv");
    // fallthrough
    case IceType_i32:
      break;
    case IceType_i16:
      Eax = Traits::RegisterSet::Reg_ax;
      Edx = Traits::RegisterSet::Reg_dx;
      break;
    case IceType_i8:
      Eax = Traits::RegisterSet::Reg_al;
      Edx = Traits::RegisterSet::Reg_ah;
      break;
    }
    _mov(T, Src0, Eax);
    _mov(T_edx, Ctx->getConstantZero(Ty), Edx);
    _div(T, Src1, T_edx);
    _mov(Dest, T);
  } break;
  case InstArithmetic::Sdiv:
    // TODO(stichnot): Enable this after doing better performance and cross
    // testing.
    if (false && Ctx->getFlags().getOptLevel() >= Opt_1) {
      // Optimize division by constant power of 2, but not for Om1 or O0, just
      // to keep things simple there.
      if (auto *C = llvm::dyn_cast<ConstantInteger32>(Src1)) {
        int32_t Divisor = C->getValue();
        uint32_t UDivisor = static_cast<uint32_t>(Divisor);
        if (Divisor > 0 && llvm::isPowerOf2_32(UDivisor)) {
          uint32_t LogDiv = llvm::Log2_32(UDivisor);
          // LLVM does the following for dest=src/(1<<log):
          //   t=src
          //   sar t,typewidth-1 // -1 if src is negative, 0 if not
          //   shr t,typewidth-log
          //   add t,src
          //   sar t,log
          //   dest=t
          uint32_t TypeWidth = Traits::X86_CHAR_BIT * typeWidthInBytes(Ty);
          _mov(T, Src0);
          // If for some reason we are dividing by 1, just treat it like an
          // assignment.
          if (LogDiv > 0) {
            // The initial sar is unnecessary when dividing by 2.
            if (LogDiv > 1)
              _sar(T, Ctx->getConstantInt(Ty, TypeWidth - 1));
            _shr(T, Ctx->getConstantInt(Ty, TypeWidth - LogDiv));
            _add(T, Src0);
            _sar(T, Ctx->getConstantInt(Ty, LogDiv));
          }
          _mov(Dest, T);
          return;
        }
      }
    }
    Src1 = legalize(Src1, Legal_Reg | Legal_Mem);
    switch (Ty) {
    default:
      llvm_unreachable("Bad type for sdiv");
    // fallthrough
    case IceType_i32:
      T_edx = makeReg(Ty, Traits::RegisterSet::Reg_edx);
      _mov(T, Src0, Traits::RegisterSet::Reg_eax);
      break;
    case IceType_i16:
      T_edx = makeReg(Ty, Traits::RegisterSet::Reg_dx);
      _mov(T, Src0, Traits::RegisterSet::Reg_ax);
      break;
    case IceType_i8:
      T_edx = makeReg(IceType_i16, Traits::RegisterSet::Reg_ax);
      _mov(T, Src0, Traits::RegisterSet::Reg_al);
      break;
    }
    _cbwdq(T_edx, T);
    _idiv(T, Src1, T_edx);
    _mov(Dest, T);
    break;
  case InstArithmetic::Urem: {
    Src1 = legalize(Src1, Legal_Reg | Legal_Mem);
    uint32_t Eax = Traits::RegisterSet::Reg_eax;
    uint32_t Edx = Traits::RegisterSet::Reg_edx;
    switch (Ty) {
    default:
      llvm_unreachable("Bad type for urem");
    // fallthrough
    case IceType_i32:
      break;
    case IceType_i16:
      Eax = Traits::RegisterSet::Reg_ax;
      Edx = Traits::RegisterSet::Reg_dx;
      break;
    case IceType_i8:
      Eax = Traits::RegisterSet::Reg_al;
      Edx = Traits::RegisterSet::Reg_ah;
      break;
    }
    T_edx = makeReg(Ty, Edx);
    _mov(T_edx, Ctx->getConstantZero(Ty));
    _mov(T, Src0, Eax);
    _div(T_edx, Src1, T);
    _mov(Dest, T_edx);
  } break;
  case InstArithmetic::Srem: {
    // TODO(stichnot): Enable this after doing better performance and cross
    // testing.
    if (false && Ctx->getFlags().getOptLevel() >= Opt_1) {
      // Optimize mod by constant power of 2, but not for Om1 or O0, just to
      // keep things simple there.
      if (auto *C = llvm::dyn_cast<ConstantInteger32>(Src1)) {
        int32_t Divisor = C->getValue();
        uint32_t UDivisor = static_cast<uint32_t>(Divisor);
        if (Divisor > 0 && llvm::isPowerOf2_32(UDivisor)) {
          uint32_t LogDiv = llvm::Log2_32(UDivisor);
          // LLVM does the following for dest=src%(1<<log):
          //   t=src
          //   sar t,typewidth-1 // -1 if src is negative, 0 if not
          //   shr t,typewidth-log
          //   add t,src
          //   and t, -(1<<log)
          //   sub t,src
          //   neg t
          //   dest=t
          uint32_t TypeWidth = Traits::X86_CHAR_BIT * typeWidthInBytes(Ty);
          // If for some reason we are dividing by 1, just assign 0.
          if (LogDiv == 0) {
            _mov(Dest, Ctx->getConstantZero(Ty));
            return;
          }
          _mov(T, Src0);
          // The initial sar is unnecessary when dividing by 2.
          if (LogDiv > 1)
            _sar(T, Ctx->getConstantInt(Ty, TypeWidth - 1));
          _shr(T, Ctx->getConstantInt(Ty, TypeWidth - LogDiv));
          _add(T, Src0);
          _and(T, Ctx->getConstantInt(Ty, -(1 << LogDiv)));
          _sub(T, Src0);
          _neg(T);
          _mov(Dest, T);
          return;
        }
      }
    }
    Src1 = legalize(Src1, Legal_Reg | Legal_Mem);
    uint32_t Eax = Traits::RegisterSet::Reg_eax;
    uint32_t Edx = Traits::RegisterSet::Reg_edx;
    switch (Ty) {
    default:
      llvm_unreachable("Bad type for srem");
    // fallthrough
    case IceType_i32:
      break;
    case IceType_i16:
      Eax = Traits::RegisterSet::Reg_ax;
      Edx = Traits::RegisterSet::Reg_dx;
      break;
    case IceType_i8:
      Eax = Traits::RegisterSet::Reg_al;
      Edx = Traits::RegisterSet::Reg_ah;
      break;
    }
    T_edx = makeReg(Ty, Edx);
    _mov(T, Src0, Eax);
    _cbwdq(T_edx, T);
    _idiv(T_edx, Src1, T);
    _mov(Dest, T_edx);
  } break;
  case InstArithmetic::Fadd:
    _mov(T, Src0);
    _addss(T, Src1);
    _mov(Dest, T);
    break;
  case InstArithmetic::Fsub:
    _mov(T, Src0);
    _subss(T, Src1);
    _mov(Dest, T);
    break;
  case InstArithmetic::Fmul:
    _mov(T, Src0);
    _mulss(T, Src0 == Src1 ? T : Src1);
    _mov(Dest, T);
    break;
  case InstArithmetic::Fdiv:
    _mov(T, Src0);
    _divss(T, Src1);
    _mov(Dest, T);
    break;
  case InstArithmetic::Frem:
    llvm::report_fatal_error("Helper call was expected");
    break;
  }
}

template <class Machine>
void TargetX86Base<Machine>::lowerAssign(const InstAssign *Inst) {
  Variable *Dest = Inst->getDest();
  if (Dest->isRematerializable()) {
    Context.insert(InstFakeDef::create(Func, Dest));
    return;
  }
  Operand *Src0 = Inst->getSrc(0);
  assert(Dest->getType() == Src0->getType());
  if (!Traits::Is64Bit && Dest->getType() == IceType_i64) {
    Src0 = legalize(Src0);
    Operand *Src0Lo = loOperand(Src0);
    Operand *Src0Hi = hiOperand(Src0);
    Variable *DestLo = llvm::cast<Variable>(loOperand(Dest));
    Variable *DestHi = llvm::cast<Variable>(hiOperand(Dest));
    Variable *T_Lo = nullptr, *T_Hi = nullptr;
    _mov(T_Lo, Src0Lo);
    _mov(DestLo, T_Lo);
    _mov(T_Hi, Src0Hi);
    _mov(DestHi, T_Hi);
  } else {
    Operand *Src0Legal;
    if (Dest->hasReg()) {
      // If Dest already has a physical register, then only basic legalization
      // is needed, as the source operand can be a register, immediate, or
      // memory.
      Src0Legal = legalize(Src0, Legal_Reg, Dest->getRegNum());
    } else {
      // If Dest could be a stack operand, then RI must be a physical register
      // or a scalar integer immediate.
      Src0Legal = legalize(Src0, Legal_Reg | Legal_Imm);
    }
    if (isVectorType(Dest->getType()))
      _movp(Dest, Src0Legal);
    else
      _mov(Dest, Src0Legal);
  }
}

template <class Machine>
void TargetX86Base<Machine>::lowerBr(const InstBr *Inst) {
  if (Inst->isUnconditional()) {
    _br(Inst->getTargetUnconditional());
    return;
  }
  Operand *Cond = Inst->getCondition();

  // Handle folding opportunities.
  if (const class Inst *Producer = FoldingInfo.getProducerFor(Cond)) {
    assert(Producer->isDeleted());
    switch (BoolFolding::getProducerKind(Producer)) {
    default:
      break;
    case BoolFolding::PK_Icmp32:
    case BoolFolding::PK_Icmp64: {
      lowerIcmpAndBr(llvm::dyn_cast<InstIcmp>(Producer), Inst);
      return;
    }
    case BoolFolding::PK_Fcmp: {
      lowerFcmpAndBr(llvm::dyn_cast<InstFcmp>(Producer), Inst);
      return;
    }
    case BoolFolding::PK_Arith: {
      lowerArithAndBr(llvm::dyn_cast<InstArithmetic>(Producer), Inst);
      return;
    }
    }
  }
  Operand *Src0 = legalize(Cond, Legal_Reg | Legal_Mem);
  Constant *Zero = Ctx->getConstantZero(IceType_i32);
  _cmp(Src0, Zero);
  _br(Traits::Cond::Br_ne, Inst->getTargetTrue(), Inst->getTargetFalse());
}

template <class Machine>
void TargetX86Base<Machine>::lowerCast(const InstCast *Inst) {
  // a = cast(b) ==> t=cast(b); a=t; (link t->b, link a->t, no overlap)
  InstCast::OpKind CastKind = Inst->getCastKind();
  Variable *Dest = Inst->getDest();
  Type DestTy = Dest->getType();
  switch (CastKind) {
  default:
    Func->setError("Cast type not supported");
    return;
  case InstCast::Sext: {
    // Src0RM is the source operand legalized to physical register or memory,
    // but not immediate, since the relevant x86 native instructions don't
    // allow an immediate operand. If the operand is an immediate, we could
    // consider computing the strength-reduced result at translation time, but
    // we're unlikely to see something like that in the bitcode that the
    // optimizer wouldn't have already taken care of.
    Operand *Src0RM = legalize(Inst->getSrc(0), Legal_Reg | Legal_Mem);
    if (isVectorType(DestTy)) {
      if (DestTy == IceType_v16i8) {
        // onemask = materialize(1,1,...); dst = (src & onemask) > 0
        Variable *OneMask = makeVectorOfOnes(DestTy);
        Variable *T = makeReg(DestTy);
        _movp(T, Src0RM);
        _pand(T, OneMask);
        Variable *Zeros = makeVectorOfZeros(DestTy);
        _pcmpgt(T, Zeros);
        _movp(Dest, T);
      } else {
        /// width = width(elty) - 1; dest = (src << width) >> width
        SizeT ShiftAmount =
            Traits::X86_CHAR_BIT * typeWidthInBytes(typeElementType(DestTy)) -
            1;
        Constant *ShiftConstant = Ctx->getConstantInt8(ShiftAmount);
        Variable *T = makeReg(DestTy);
        _movp(T, Src0RM);
        _psll(T, ShiftConstant);
        _psra(T, ShiftConstant);
        _movp(Dest, T);
      }
    } else if (!Traits::Is64Bit && DestTy == IceType_i64) {
      // t1=movsx src; t2=t1; t2=sar t2, 31; dst.lo=t1; dst.hi=t2
      Constant *Shift = Ctx->getConstantInt32(31);
      Variable *DestLo = llvm::cast<Variable>(loOperand(Dest));
      Variable *DestHi = llvm::cast<Variable>(hiOperand(Dest));
      Variable *T_Lo = makeReg(DestLo->getType());
      if (Src0RM->getType() == IceType_i32) {
        _mov(T_Lo, Src0RM);
      } else if (Src0RM->getType() == IceType_i1) {
        _movzx(T_Lo, Src0RM);
        _shl(T_Lo, Shift);
        _sar(T_Lo, Shift);
      } else {
        _movsx(T_Lo, Src0RM);
      }
      _mov(DestLo, T_Lo);
      Variable *T_Hi = nullptr;
      _mov(T_Hi, T_Lo);
      if (Src0RM->getType() != IceType_i1)
        // For i1, the sar instruction is already done above.
        _sar(T_Hi, Shift);
      _mov(DestHi, T_Hi);
    } else if (Src0RM->getType() == IceType_i1) {
      // t1 = src
      // shl t1, dst_bitwidth - 1
      // sar t1, dst_bitwidth - 1
      // dst = t1
      size_t DestBits = Traits::X86_CHAR_BIT * typeWidthInBytes(DestTy);
      Constant *ShiftAmount = Ctx->getConstantInt32(DestBits - 1);
      Variable *T = makeReg(DestTy);
      if (typeWidthInBytes(DestTy) <= typeWidthInBytes(Src0RM->getType())) {
        _mov(T, Src0RM);
      } else {
        // Widen the source using movsx or movzx. (It doesn't matter which one,
        // since the following shl/sar overwrite the bits.)
        _movzx(T, Src0RM);
      }
      _shl(T, ShiftAmount);
      _sar(T, ShiftAmount);
      _mov(Dest, T);
    } else {
      // t1 = movsx src; dst = t1
      Variable *T = makeReg(DestTy);
      _movsx(T, Src0RM);
      _mov(Dest, T);
    }
    break;
  }
  case InstCast::Zext: {
    Operand *Src0RM = legalize(Inst->getSrc(0), Legal_Reg | Legal_Mem);
    if (isVectorType(DestTy)) {
      // onemask = materialize(1,1,...); dest = onemask & src
      Variable *OneMask = makeVectorOfOnes(DestTy);
      Variable *T = makeReg(DestTy);
      _movp(T, Src0RM);
      _pand(T, OneMask);
      _movp(Dest, T);
    } else if (!Traits::Is64Bit && DestTy == IceType_i64) {
      // t1=movzx src; dst.lo=t1; dst.hi=0
      Constant *Zero = Ctx->getConstantZero(IceType_i32);
      Variable *DestLo = llvm::cast<Variable>(loOperand(Dest));
      Variable *DestHi = llvm::cast<Variable>(hiOperand(Dest));
      Variable *Tmp = makeReg(DestLo->getType());
      if (Src0RM->getType() == IceType_i32) {
        _mov(Tmp, Src0RM);
      } else {
        _movzx(Tmp, Src0RM);
      }
      _mov(DestLo, Tmp);
      _mov(DestHi, Zero);
    } else if (Src0RM->getType() == IceType_i1) {
      // t = Src0RM; Dest = t
      Variable *T = nullptr;
      if (DestTy == IceType_i8) {
        _mov(T, Src0RM);
      } else {
        assert(DestTy != IceType_i1);
        assert(Traits::Is64Bit || DestTy != IceType_i64);
        // Use 32-bit for both 16-bit and 32-bit, since 32-bit ops are shorter.
        // In x86-64 we need to widen T to 64-bits to ensure that T -- if
        // written to the stack (i.e., in -Om1) will be fully zero-extended.
        T = makeReg(DestTy == IceType_i64 ? IceType_i64 : IceType_i32);
        _movzx(T, Src0RM);
      }
      _mov(Dest, T);
    } else {
      // t1 = movzx src; dst = t1
      Variable *T = makeReg(DestTy);
      _movzx(T, Src0RM);
      _mov(Dest, T);
    }
    break;
  }
  case InstCast::Trunc: {
    if (isVectorType(DestTy)) {
      // onemask = materialize(1,1,...); dst = src & onemask
      Operand *Src0RM = legalize(Inst->getSrc(0), Legal_Reg | Legal_Mem);
      Type Src0Ty = Src0RM->getType();
      Variable *OneMask = makeVectorOfOnes(Src0Ty);
      Variable *T = makeReg(DestTy);
      _movp(T, Src0RM);
      _pand(T, OneMask);
      _movp(Dest, T);
    } else if (DestTy == IceType_i1 || DestTy == IceType_i8) {
      // Make sure we truncate from and into valid registers.
      Operand *Src0 = legalizeUndef(Inst->getSrc(0));
      if (!Traits::Is64Bit && Src0->getType() == IceType_i64)
        Src0 = loOperand(Src0);
      Operand *Src0RM = legalize(Src0, Legal_Reg | Legal_Mem);
      Variable *T = copyToReg8(Src0RM);
      if (DestTy == IceType_i1)
        _and(T, Ctx->getConstantInt1(1));
      _mov(Dest, T);
    } else {
      Operand *Src0 = legalizeUndef(Inst->getSrc(0));
      if (!Traits::Is64Bit && Src0->getType() == IceType_i64)
        Src0 = loOperand(Src0);
      Operand *Src0RM = legalize(Src0, Legal_Reg | Legal_Mem);
      // t1 = trunc Src0RM; Dest = t1
      Variable *T = makeReg(DestTy);
      _mov(T, Src0RM);
      _mov(Dest, T);
    }
    break;
  }
  case InstCast::Fptrunc:
  case InstCast::Fpext: {
    Operand *Src0RM = legalize(Inst->getSrc(0), Legal_Reg | Legal_Mem);
    // t1 = cvt Src0RM; Dest = t1
    Variable *T = makeReg(DestTy);
    _cvt(T, Src0RM, Traits::Insts::Cvt::Float2float);
    _mov(Dest, T);
    break;
  }
  case InstCast::Fptosi:
    if (isVectorType(DestTy)) {
      assert(DestTy == IceType_v4i32 &&
             Inst->getSrc(0)->getType() == IceType_v4f32);
      Operand *Src0RM = legalize(Inst->getSrc(0), Legal_Reg | Legal_Mem);
      if (llvm::isa<typename Traits::X86OperandMem>(Src0RM))
        Src0RM = legalizeToReg(Src0RM);
      Variable *T = makeReg(DestTy);
      _cvt(T, Src0RM, Traits::Insts::Cvt::Tps2dq);
      _movp(Dest, T);
    } else if (!Traits::Is64Bit && DestTy == IceType_i64) {
      llvm::report_fatal_error("Helper call was expected");
    } else {
      Operand *Src0RM = legalize(Inst->getSrc(0), Legal_Reg | Legal_Mem);
      // t1.i32 = cvt Src0RM; t2.dest_type = t1; Dest = t2.dest_type
      Variable *T_1 = nullptr;
      if (Traits::Is64Bit && DestTy == IceType_i64) {
        T_1 = makeReg(IceType_i64);
      } else {
        assert(DestTy != IceType_i64);
        T_1 = makeReg(IceType_i32);
      }
      // cvt() requires its integer argument to be a GPR.
      Variable *T_2 = makeReg(DestTy);
      if (isByteSizedType(DestTy)) {
        assert(T_1->getType() == IceType_i32);
        T_1->setRegClass(RCX86_Is32To8);
        T_2->setRegClass(RCX86_IsTrunc8Rcvr);
      }
      _cvt(T_1, Src0RM, Traits::Insts::Cvt::Tss2si);
      _mov(T_2, T_1); // T_1 and T_2 may have different integer types
      if (DestTy == IceType_i1)
        _and(T_2, Ctx->getConstantInt1(1));
      _mov(Dest, T_2);
    }
    break;
  case InstCast::Fptoui:
    if (isVectorType(DestTy)) {
      llvm::report_fatal_error("Helper call was expected");
    } else if (DestTy == IceType_i64 ||
               (!Traits::Is64Bit && DestTy == IceType_i32)) {
      llvm::report_fatal_error("Helper call was expected");
    } else {
      Operand *Src0RM = legalize(Inst->getSrc(0), Legal_Reg | Legal_Mem);
      // t1.i32 = cvt Src0RM; t2.dest_type = t1; Dest = t2.dest_type
      assert(DestTy != IceType_i64);
      Variable *T_1 = nullptr;
      if (Traits::Is64Bit && DestTy == IceType_i32) {
        T_1 = makeReg(IceType_i64);
      } else {
        assert(DestTy != IceType_i32);
        T_1 = makeReg(IceType_i32);
      }
      Variable *T_2 = makeReg(DestTy);
      if (isByteSizedType(DestTy)) {
        assert(T_1->getType() == IceType_i32);
        T_1->setRegClass(RCX86_Is32To8);
        T_2->setRegClass(RCX86_IsTrunc8Rcvr);
      }
      _cvt(T_1, Src0RM, Traits::Insts::Cvt::Tss2si);
      _mov(T_2, T_1); // T_1 and T_2 may have different integer types
      if (DestTy == IceType_i1)
        _and(T_2, Ctx->getConstantInt1(1));
      _mov(Dest, T_2);
    }
    break;
  case InstCast::Sitofp:
    if (isVectorType(DestTy)) {
      assert(DestTy == IceType_v4f32 &&
             Inst->getSrc(0)->getType() == IceType_v4i32);
      Operand *Src0RM = legalize(Inst->getSrc(0), Legal_Reg | Legal_Mem);
      if (llvm::isa<typename Traits::X86OperandMem>(Src0RM))
        Src0RM = legalizeToReg(Src0RM);
      Variable *T = makeReg(DestTy);
      _cvt(T, Src0RM, Traits::Insts::Cvt::Dq2ps);
      _movp(Dest, T);
    } else if (!Traits::Is64Bit && Inst->getSrc(0)->getType() == IceType_i64) {
      llvm::report_fatal_error("Helper call was expected");
    } else {
      Operand *Src0RM = legalize(Inst->getSrc(0), Legal_Reg | Legal_Mem);
      // Sign-extend the operand.
      // t1.i32 = movsx Src0RM; t2 = Cvt t1.i32; Dest = t2
      Variable *T_1 = nullptr;
      if (Traits::Is64Bit && Src0RM->getType() == IceType_i64) {
        T_1 = makeReg(IceType_i64);
      } else {
        assert(Src0RM->getType() != IceType_i64);
        T_1 = makeReg(IceType_i32);
      }
      Variable *T_2 = makeReg(DestTy);
      if (Src0RM->getType() == T_1->getType())
        _mov(T_1, Src0RM);
      else
        _movsx(T_1, Src0RM);
      _cvt(T_2, T_1, Traits::Insts::Cvt::Si2ss);
      _mov(Dest, T_2);
    }
    break;
  case InstCast::Uitofp: {
    Operand *Src0 = Inst->getSrc(0);
    if (isVectorType(Src0->getType())) {
      llvm::report_fatal_error("Helper call was expected");
    } else if (Src0->getType() == IceType_i64 ||
               (!Traits::Is64Bit && Src0->getType() == IceType_i32)) {
      llvm::report_fatal_error("Helper call was expected");
    } else {
      Operand *Src0RM = legalize(Src0, Legal_Reg | Legal_Mem);
      // Zero-extend the operand.
      // t1.i32 = movzx Src0RM; t2 = Cvt t1.i32; Dest = t2
      Variable *T_1 = nullptr;
      if (Traits::Is64Bit && Src0RM->getType() == IceType_i32) {
        T_1 = makeReg(IceType_i64);
      } else {
        assert(Src0RM->getType() != IceType_i64);
        assert(Traits::Is64Bit || Src0RM->getType() != IceType_i32);
        T_1 = makeReg(IceType_i32);
      }
      Variable *T_2 = makeReg(DestTy);
      if (Src0RM->getType() == T_1->getType())
        _mov(T_1, Src0RM);
      else
        _movzx(T_1, Src0RM);
      _cvt(T_2, T_1, Traits::Insts::Cvt::Si2ss);
      _mov(Dest, T_2);
    }
    break;
  }
  case InstCast::Bitcast: {
    Operand *Src0 = Inst->getSrc(0);
    if (DestTy == Src0->getType()) {
      InstAssign *Assign = InstAssign::create(Func, Dest, Src0);
      lowerAssign(Assign);
      return;
    }
    switch (DestTy) {
    default:
      llvm_unreachable("Unexpected Bitcast dest type");
    case IceType_i8: {
      llvm::report_fatal_error("Helper call was expected");
    } break;
    case IceType_i16: {
      llvm::report_fatal_error("Helper call was expected");
    } break;
    case IceType_i32:
    case IceType_f32: {
      Operand *Src0RM = legalize(Src0, Legal_Reg | Legal_Mem);
      Type SrcType = Src0RM->getType();
      assert((DestTy == IceType_i32 && SrcType == IceType_f32) ||
             (DestTy == IceType_f32 && SrcType == IceType_i32));
      // a.i32 = bitcast b.f32 ==>
      //   t.f32 = b.f32
      //   s.f32 = spill t.f32
      //   a.i32 = s.f32
      Variable *T = nullptr;
      // TODO: Should be able to force a spill setup by calling legalize() with
      // Legal_Mem and not Legal_Reg or Legal_Imm.
      typename Traits::SpillVariable *SpillVar =
          Func->makeVariable<typename Traits::SpillVariable>(SrcType);
      SpillVar->setLinkedTo(Dest);
      Variable *Spill = SpillVar;
      Spill->setMustNotHaveReg();
      _mov(T, Src0RM);
      _mov(Spill, T);
      _mov(Dest, Spill);
    } break;
    case IceType_i64: {
      assert(Src0->getType() == IceType_f64);
      if (Traits::Is64Bit) {
        // Movd requires its fp argument (in this case, the bitcast source) to
        // be an xmm register.
        Variable *Src0R = legalizeToReg(Src0);
        Variable *T = makeReg(IceType_i64);
        _movd(T, Src0R);
        _mov(Dest, T);
      } else {
        Operand *Src0RM = legalize(Src0, Legal_Reg | Legal_Mem);
        // a.i64 = bitcast b.f64 ==>
        //   s.f64 = spill b.f64
        //   t_lo.i32 = lo(s.f64)
        //   a_lo.i32 = t_lo.i32
        //   t_hi.i32 = hi(s.f64)
        //   a_hi.i32 = t_hi.i32
        Operand *SpillLo, *SpillHi;
        if (auto *Src0Var = llvm::dyn_cast<Variable>(Src0RM)) {
          typename Traits::SpillVariable *SpillVar =
              Func->makeVariable<typename Traits::SpillVariable>(IceType_f64);
          SpillVar->setLinkedTo(Src0Var);
          Variable *Spill = SpillVar;
          Spill->setMustNotHaveReg();
          _movq(Spill, Src0RM);
          SpillLo = Traits::VariableSplit::create(Func, Spill,
                                                  Traits::VariableSplit::Low);
          SpillHi = Traits::VariableSplit::create(Func, Spill,
                                                  Traits::VariableSplit::High);
        } else {
          SpillLo = loOperand(Src0RM);
          SpillHi = hiOperand(Src0RM);
        }

        Variable *DestLo = llvm::cast<Variable>(loOperand(Dest));
        Variable *DestHi = llvm::cast<Variable>(hiOperand(Dest));
        Variable *T_Lo = makeReg(IceType_i32);
        Variable *T_Hi = makeReg(IceType_i32);

        _mov(T_Lo, SpillLo);
        _mov(DestLo, T_Lo);
        _mov(T_Hi, SpillHi);
        _mov(DestHi, T_Hi);
      }
    } break;
    case IceType_f64: {
      assert(Src0->getType() == IceType_i64);
      if (Traits::Is64Bit) {
        Operand *Src0RM = legalize(Src0, Legal_Reg | Legal_Mem);
        Variable *T = makeReg(IceType_f64);
        // Movd requires its fp argument (in this case, the bitcast
        // destination) to be an xmm register.
        _movd(T, Src0RM);
        _mov(Dest, T);
      } else {
        Src0 = legalize(Src0);
        if (llvm::isa<typename Traits::X86OperandMem>(Src0)) {
          Variable *T = Func->makeVariable(DestTy);
          _movq(T, Src0);
          _movq(Dest, T);
          break;
        }
        // a.f64 = bitcast b.i64 ==>
        //   t_lo.i32 = b_lo.i32
        //   FakeDef(s.f64)
        //   lo(s.f64) = t_lo.i32
        //   t_hi.i32 = b_hi.i32
        //   hi(s.f64) = t_hi.i32
        //   a.f64 = s.f64
        typename Traits::SpillVariable *SpillVar =
            Func->makeVariable<typename Traits::SpillVariable>(IceType_f64);
        SpillVar->setLinkedTo(Dest);
        Variable *Spill = SpillVar;
        Spill->setMustNotHaveReg();

        Variable *T_Lo = nullptr, *T_Hi = nullptr;
        typename Traits::VariableSplit *SpillLo = Traits::VariableSplit::create(
            Func, Spill, Traits::VariableSplit::Low);
        typename Traits::VariableSplit *SpillHi = Traits::VariableSplit::create(
            Func, Spill, Traits::VariableSplit::High);
        _mov(T_Lo, loOperand(Src0));
        // Technically, the Spill is defined after the _store happens, but
        // SpillLo is considered a "use" of Spill so define Spill before it is
        // used.
        Context.insert(InstFakeDef::create(Func, Spill));
        _store(T_Lo, SpillLo);
        _mov(T_Hi, hiOperand(Src0));
        _store(T_Hi, SpillHi);
        _movq(Dest, Spill);
      }
    } break;
    case IceType_v8i1: {
      llvm::report_fatal_error("Helper call was expected");
    } break;
    case IceType_v16i1: {
      llvm::report_fatal_error("Helper call was expected");
    } break;
    case IceType_v8i16:
    case IceType_v16i8:
    case IceType_v4i32:
    case IceType_v4f32: {
      _movp(Dest, legalizeToReg(Src0));
    } break;
    }
    break;
  }
  }
}

template <class Machine>
void TargetX86Base<Machine>::lowerExtractElement(
    const InstExtractElement *Inst) {
  Operand *SourceVectNotLegalized = Inst->getSrc(0);
  ConstantInteger32 *ElementIndex =
      llvm::dyn_cast<ConstantInteger32>(Inst->getSrc(1));
  // Only constant indices are allowed in PNaCl IR.
  assert(ElementIndex);

  unsigned Index = ElementIndex->getValue();
  Type Ty = SourceVectNotLegalized->getType();
  Type ElementTy = typeElementType(Ty);
  Type InVectorElementTy = Traits::getInVectorElementType(Ty);

  // TODO(wala): Determine the best lowering sequences for each type.
  bool CanUsePextr = Ty == IceType_v8i16 || Ty == IceType_v8i1 ||
                     (InstructionSet >= Traits::SSE4_1 && Ty != IceType_v4f32);
  Variable *ExtractedElementR =
      makeReg(CanUsePextr ? IceType_i32 : InVectorElementTy);
  if (CanUsePextr) {
    // Use pextrb, pextrw, or pextrd.  The "b" and "w" versions clear the upper
    // bits of the destination register, so we represent this by always
    // extracting into an i32 register.  The _mov into Dest below will do
    // truncation as necessary.
    Constant *Mask = Ctx->getConstantInt32(Index);
    Variable *SourceVectR = legalizeToReg(SourceVectNotLegalized);
    _pextr(ExtractedElementR, SourceVectR, Mask);
  } else if (Ty == IceType_v4i32 || Ty == IceType_v4f32 || Ty == IceType_v4i1) {
    // Use pshufd and movd/movss.
    Variable *T = nullptr;
    if (Index) {
      // The shuffle only needs to occur if the element to be extracted is not
      // at the lowest index.
      Constant *Mask = Ctx->getConstantInt32(Index);
      T = makeReg(Ty);
      _pshufd(T, legalize(SourceVectNotLegalized, Legal_Reg | Legal_Mem), Mask);
    } else {
      T = legalizeToReg(SourceVectNotLegalized);
    }

    if (InVectorElementTy == IceType_i32) {
      _movd(ExtractedElementR, T);
    } else { // Ty == IceType_f32
      // TODO(wala): _movss is only used here because _mov does not allow a
      // vector source and a scalar destination.  _mov should be able to be
      // used here.
      // _movss is a binary instruction, so the FakeDef is needed to keep the
      // live range analysis consistent.
      Context.insert(InstFakeDef::create(Func, ExtractedElementR));
      _movss(ExtractedElementR, T);
    }
  } else {
    assert(Ty == IceType_v16i8 || Ty == IceType_v16i1);
    // Spill the value to a stack slot and do the extraction in memory.
    //
    // TODO(wala): use legalize(SourceVectNotLegalized, Legal_Mem) when support
    // for legalizing to mem is implemented.
    Variable *Slot = Func->makeVariable(Ty);
    Slot->setMustNotHaveReg();
    _movp(Slot, legalizeToReg(SourceVectNotLegalized));

    // Compute the location of the element in memory.
    unsigned Offset = Index * typeWidthInBytes(InVectorElementTy);
    typename Traits::X86OperandMem *Loc =
        getMemoryOperandForStackSlot(InVectorElementTy, Slot, Offset);
    _mov(ExtractedElementR, Loc);
  }

  if (ElementTy == IceType_i1) {
    // Truncate extracted integers to i1s if necessary.
    Variable *T = makeReg(IceType_i1);
    InstCast *Cast =
        InstCast::create(Func, InstCast::Trunc, T, ExtractedElementR);
    lowerCast(Cast);
    ExtractedElementR = T;
  }

  // Copy the element to the destination.
  Variable *Dest = Inst->getDest();
  _mov(Dest, ExtractedElementR);
}

template <class Machine>
void TargetX86Base<Machine>::lowerFcmp(const InstFcmp *Inst) {
  constexpr InstBr *Br = nullptr;
  lowerFcmpAndBr(Inst, Br);
}

template <class Machine>
void TargetX86Base<Machine>::lowerFcmpAndBr(const InstFcmp *Inst,
                                            const InstBr *Br) {
  Operand *Src0 = Inst->getSrc(0);
  Operand *Src1 = Inst->getSrc(1);
  Variable *Dest = Inst->getDest();

  if (isVectorType(Dest->getType())) {
    if (Br)
      llvm::report_fatal_error("vector compare/branch cannot be folded");
    InstFcmp::FCond Condition = Inst->getCondition();
    size_t Index = static_cast<size_t>(Condition);
    assert(Index < Traits::TableFcmpSize);

    if (Traits::TableFcmp[Index].SwapVectorOperands)
      std::swap(Src0, Src1);

    Variable *T = nullptr;

    if (Condition == InstFcmp::True) {
      // makeVectorOfOnes() requires an integer vector type.
      T = makeVectorOfMinusOnes(IceType_v4i32);
    } else if (Condition == InstFcmp::False) {
      T = makeVectorOfZeros(Dest->getType());
    } else {
      Operand *Src0RM = legalize(Src0, Legal_Reg | Legal_Mem);
      Operand *Src1RM = legalize(Src1, Legal_Reg | Legal_Mem);
      if (llvm::isa<typename Traits::X86OperandMem>(Src1RM))
        Src1RM = legalizeToReg(Src1RM);

      switch (Condition) {
      default: {
        typename Traits::Cond::CmppsCond Predicate =
            Traits::TableFcmp[Index].Predicate;
        assert(Predicate != Traits::Cond::Cmpps_Invalid);
        T = makeReg(Src0RM->getType());
        _movp(T, Src0RM);
        _cmpps(T, Src1RM, Predicate);
      } break;
      case InstFcmp::One: {
        // Check both unequal and ordered.
        T = makeReg(Src0RM->getType());
        Variable *T2 = makeReg(Src0RM->getType());
        _movp(T, Src0RM);
        _cmpps(T, Src1RM, Traits::Cond::Cmpps_neq);
        _movp(T2, Src0RM);
        _cmpps(T2, Src1RM, Traits::Cond::Cmpps_ord);
        _pand(T, T2);
      } break;
      case InstFcmp::Ueq: {
        // Check both equal or unordered.
        T = makeReg(Src0RM->getType());
        Variable *T2 = makeReg(Src0RM->getType());
        _movp(T, Src0RM);
        _cmpps(T, Src1RM, Traits::Cond::Cmpps_eq);
        _movp(T2, Src0RM);
        _cmpps(T2, Src1RM, Traits::Cond::Cmpps_unord);
        _por(T, T2);
      } break;
      }
    }

    _movp(Dest, T);
    eliminateNextVectorSextInstruction(Dest);
    return;
  }

  // Lowering a = fcmp cond, b, c
  //   ucomiss b, c       /* only if C1 != Br_None */
  //                      /* but swap b,c order if SwapOperands==true */
  //   mov a, <default>
  //   j<C1> label        /* only if C1 != Br_None */
  //   j<C2> label        /* only if C2 != Br_None */
  //   FakeUse(a)         /* only if C1 != Br_None */
  //   mov a, !<default>  /* only if C1 != Br_None */
  //   label:             /* only if C1 != Br_None */
  //
  // setcc lowering when C1 != Br_None && C2 == Br_None:
  //   ucomiss b, c       /* but swap b,c order if SwapOperands==true */
  //   setcc a, C1
  InstFcmp::FCond Condition = Inst->getCondition();
  size_t Index = static_cast<size_t>(Condition);
  assert(Index < Traits::TableFcmpSize);
  if (Traits::TableFcmp[Index].SwapScalarOperands)
    std::swap(Src0, Src1);
  bool HasC1 = (Traits::TableFcmp[Index].C1 != Traits::Cond::Br_None);
  bool HasC2 = (Traits::TableFcmp[Index].C2 != Traits::Cond::Br_None);
  if (HasC1) {
    Src0 = legalize(Src0);
    Operand *Src1RM = legalize(Src1, Legal_Reg | Legal_Mem);
    Variable *T = nullptr;
    _mov(T, Src0);
    _ucomiss(T, Src1RM);
    if (!HasC2) {
      assert(Traits::TableFcmp[Index].Default);
      setccOrBr(Traits::TableFcmp[Index].C1, Dest, Br);
      return;
    }
  }
  int32_t IntDefault = Traits::TableFcmp[Index].Default;
  if (Br == nullptr) {
    Constant *Default = Ctx->getConstantInt(Dest->getType(), IntDefault);
    _mov(Dest, Default);
    if (HasC1) {
      typename Traits::Insts::Label *Label =
          Traits::Insts::Label::create(Func, this);
      _br(Traits::TableFcmp[Index].C1, Label);
      if (HasC2) {
        _br(Traits::TableFcmp[Index].C2, Label);
      }
      Constant *NonDefault = Ctx->getConstantInt(Dest->getType(), !IntDefault);
      _mov_redefined(Dest, NonDefault);
      Context.insert(Label);
    }
  } else {
    CfgNode *TrueSucc = Br->getTargetTrue();
    CfgNode *FalseSucc = Br->getTargetFalse();
    if (IntDefault != 0)
      std::swap(TrueSucc, FalseSucc);
    if (HasC1) {
      _br(Traits::TableFcmp[Index].C1, FalseSucc);
      if (HasC2) {
        _br(Traits::TableFcmp[Index].C2, FalseSucc);
      }
      _br(TrueSucc);
      return;
    }
    _br(FalseSucc);
  }
}

inline bool isZero(const Operand *Opnd) {
  if (auto *C64 = llvm::dyn_cast<ConstantInteger64>(Opnd))
    return C64->getValue() == 0;
  if (auto *C32 = llvm::dyn_cast<ConstantInteger32>(Opnd))
    return C32->getValue() == 0;
  return false;
}

template <class Machine>
void TargetX86Base<Machine>::lowerIcmp(const InstIcmp *Inst) {
  constexpr InstBr *Br = nullptr;
  lowerIcmpAndBr(Inst, Br);
}

template <class Machine>
void TargetX86Base<Machine>::lowerIcmpAndBr(const InstIcmp *Icmp,
                                            const InstBr *Br) {
  Operand *Src0 = legalize(Icmp->getSrc(0));
  Operand *Src1 = legalize(Icmp->getSrc(1));
  Variable *Dest = Icmp->getDest();

  if (isVectorType(Dest->getType())) {
    if (Br)
      llvm::report_fatal_error("vector compare/branch cannot be folded");
    Type Ty = Src0->getType();
    // Promote i1 vectors to 128 bit integer vector types.
    if (typeElementType(Ty) == IceType_i1) {
      Type NewTy = IceType_NUM;
      switch (Ty) {
      default:
        llvm_unreachable("unexpected type");
        break;
      case IceType_v4i1:
        NewTy = IceType_v4i32;
        break;
      case IceType_v8i1:
        NewTy = IceType_v8i16;
        break;
      case IceType_v16i1:
        NewTy = IceType_v16i8;
        break;
      }
      Variable *NewSrc0 = Func->makeVariable(NewTy);
      Variable *NewSrc1 = Func->makeVariable(NewTy);
      lowerCast(InstCast::create(Func, InstCast::Sext, NewSrc0, Src0));
      lowerCast(InstCast::create(Func, InstCast::Sext, NewSrc1, Src1));
      Src0 = NewSrc0;
      Src1 = NewSrc1;
      Ty = NewTy;
    }

    InstIcmp::ICond Condition = Icmp->getCondition();

    Operand *Src0RM = legalize(Src0, Legal_Reg | Legal_Mem);
    Operand *Src1RM = legalize(Src1, Legal_Reg | Legal_Mem);

    // SSE2 only has signed comparison operations. Transform unsigned inputs in
    // a manner that allows for the use of signed comparison operations by
    // flipping the high order bits.
    if (Condition == InstIcmp::Ugt || Condition == InstIcmp::Uge ||
        Condition == InstIcmp::Ult || Condition == InstIcmp::Ule) {
      Variable *T0 = makeReg(Ty);
      Variable *T1 = makeReg(Ty);
      Variable *HighOrderBits = makeVectorOfHighOrderBits(Ty);
      _movp(T0, Src0RM);
      _pxor(T0, HighOrderBits);
      _movp(T1, Src1RM);
      _pxor(T1, HighOrderBits);
      Src0RM = T0;
      Src1RM = T1;
    }

    Variable *T = makeReg(Ty);
    switch (Condition) {
    default:
      llvm_unreachable("unexpected condition");
      break;
    case InstIcmp::Eq: {
      if (llvm::isa<typename Traits::X86OperandMem>(Src1RM))
        Src1RM = legalizeToReg(Src1RM);
      _movp(T, Src0RM);
      _pcmpeq(T, Src1RM);
    } break;
    case InstIcmp::Ne: {
      if (llvm::isa<typename Traits::X86OperandMem>(Src1RM))
        Src1RM = legalizeToReg(Src1RM);
      _movp(T, Src0RM);
      _pcmpeq(T, Src1RM);
      Variable *MinusOne = makeVectorOfMinusOnes(Ty);
      _pxor(T, MinusOne);
    } break;
    case InstIcmp::Ugt:
    case InstIcmp::Sgt: {
      if (llvm::isa<typename Traits::X86OperandMem>(Src1RM))
        Src1RM = legalizeToReg(Src1RM);
      _movp(T, Src0RM);
      _pcmpgt(T, Src1RM);
    } break;
    case InstIcmp::Uge:
    case InstIcmp::Sge: {
      // !(Src1RM > Src0RM)
      if (llvm::isa<typename Traits::X86OperandMem>(Src0RM))
        Src0RM = legalizeToReg(Src0RM);
      _movp(T, Src1RM);
      _pcmpgt(T, Src0RM);
      Variable *MinusOne = makeVectorOfMinusOnes(Ty);
      _pxor(T, MinusOne);
    } break;
    case InstIcmp::Ult:
    case InstIcmp::Slt: {
      if (llvm::isa<typename Traits::X86OperandMem>(Src0RM))
        Src0RM = legalizeToReg(Src0RM);
      _movp(T, Src1RM);
      _pcmpgt(T, Src0RM);
    } break;
    case InstIcmp::Ule:
    case InstIcmp::Sle: {
      // !(Src0RM > Src1RM)
      if (llvm::isa<typename Traits::X86OperandMem>(Src1RM))
        Src1RM = legalizeToReg(Src1RM);
      _movp(T, Src0RM);
      _pcmpgt(T, Src1RM);
      Variable *MinusOne = makeVectorOfMinusOnes(Ty);
      _pxor(T, MinusOne);
    } break;
    }

    _movp(Dest, T);
    eliminateNextVectorSextInstruction(Dest);
    return;
  }

  if (!Traits::Is64Bit && Src0->getType() == IceType_i64) {
    lowerIcmp64(Icmp, Br);
    return;
  }

  // cmp b, c
  if (isZero(Src1)) {
    switch (Icmp->getCondition()) {
    default:
      break;
    case InstIcmp::Uge:
      movOrBr(true, Dest, Br);
      return;
    case InstIcmp::Ult:
      movOrBr(false, Dest, Br);
      return;
    }
  }
  Operand *Src0RM = legalizeSrc0ForCmp(Src0, Src1);
  _cmp(Src0RM, Src1);
  setccOrBr(Traits::getIcmp32Mapping(Icmp->getCondition()), Dest, Br);
}

template <typename Machine>
template <typename T>
typename std::enable_if<!T::Is64Bit, void>::type
TargetX86Base<Machine>::lowerIcmp64(const InstIcmp *Icmp, const InstBr *Br) {
  // a=icmp cond, b, c ==> cmp b,c; a=1; br cond,L1; FakeUse(a); a=0; L1:
  Operand *Src0 = legalize(Icmp->getSrc(0));
  Operand *Src1 = legalize(Icmp->getSrc(1));
  Variable *Dest = Icmp->getDest();
  InstIcmp::ICond Condition = Icmp->getCondition();
  size_t Index = static_cast<size_t>(Condition);
  assert(Index < Traits::TableIcmp64Size);
  Operand *Src0LoRM = nullptr;
  Operand *Src0HiRM = nullptr;
  // Legalize the portions of Src0 that are going to be needed.
  if (isZero(Src1)) {
    switch (Condition) {
    default:
      llvm_unreachable("unexpected condition");
      break;
    // These two are not optimized, so we fall through to the general case,
    // which needs the upper and lower halves legalized.
    case InstIcmp::Sgt:
    case InstIcmp::Sle:
    // These four compare after performing an "or" of the high and low half, so
    // they need the upper and lower halves legalized.
    case InstIcmp::Eq:
    case InstIcmp::Ule:
    case InstIcmp::Ne:
    case InstIcmp::Ugt:
      Src0LoRM = legalize(loOperand(Src0), Legal_Reg | Legal_Mem);
    // These two test only the high half's sign bit, so they need only
    // the upper half legalized.
    case InstIcmp::Sge:
    case InstIcmp::Slt:
      Src0HiRM = legalize(hiOperand(Src0), Legal_Reg | Legal_Mem);
      break;

    // These two move constants and hence need no legalization.
    case InstIcmp::Uge:
    case InstIcmp::Ult:
      break;
    }
  } else {
    Src0LoRM = legalize(loOperand(Src0), Legal_Reg | Legal_Mem);
    Src0HiRM = legalize(hiOperand(Src0), Legal_Reg | Legal_Mem);
  }
  // Optimize comparisons with zero.
  if (isZero(Src1)) {
    Constant *SignMask = Ctx->getConstantInt32(0x80000000);
    Variable *Temp = nullptr;
    switch (Condition) {
    default:
      llvm_unreachable("unexpected condition");
      break;
    case InstIcmp::Eq:
    case InstIcmp::Ule:
      // Mov Src0HiRM first, because it was legalized most recently, and will
      // sometimes avoid a move before the OR.
      _mov(Temp, Src0HiRM);
      _or(Temp, Src0LoRM);
      Context.insert(InstFakeUse::create(Func, Temp));
      setccOrBr(Traits::Cond::Br_e, Dest, Br);
      return;
    case InstIcmp::Ne:
    case InstIcmp::Ugt:
      // Mov Src0HiRM first, because it was legalized most recently, and will
      // sometimes avoid a move before the OR.
      _mov(Temp, Src0HiRM);
      _or(Temp, Src0LoRM);
      Context.insert(InstFakeUse::create(Func, Temp));
      setccOrBr(Traits::Cond::Br_ne, Dest, Br);
      return;
    case InstIcmp::Uge:
      movOrBr(true, Dest, Br);
      return;
    case InstIcmp::Ult:
      movOrBr(false, Dest, Br);
      return;
    case InstIcmp::Sgt:
      break;
    case InstIcmp::Sge:
      _test(Src0HiRM, SignMask);
      setccOrBr(Traits::Cond::Br_e, Dest, Br);
      return;
    case InstIcmp::Slt:
      _test(Src0HiRM, SignMask);
      setccOrBr(Traits::Cond::Br_ne, Dest, Br);
      return;
    case InstIcmp::Sle:
      break;
    }
  }
  // Handle general compares.
  Operand *Src1LoRI = legalize(loOperand(Src1), Legal_Reg | Legal_Imm);
  Operand *Src1HiRI = legalize(hiOperand(Src1), Legal_Reg | Legal_Imm);
  if (Br == nullptr) {
    Constant *Zero = Ctx->getConstantInt(Dest->getType(), 0);
    Constant *One = Ctx->getConstantInt(Dest->getType(), 1);
    typename Traits::Insts::Label *LabelFalse =
        Traits::Insts::Label::create(Func, this);
    typename Traits::Insts::Label *LabelTrue =
        Traits::Insts::Label::create(Func, this);
    _mov(Dest, One);
    _cmp(Src0HiRM, Src1HiRI);
    if (Traits::TableIcmp64[Index].C1 != Traits::Cond::Br_None)
      _br(Traits::TableIcmp64[Index].C1, LabelTrue);
    if (Traits::TableIcmp64[Index].C2 != Traits::Cond::Br_None)
      _br(Traits::TableIcmp64[Index].C2, LabelFalse);
    _cmp(Src0LoRM, Src1LoRI);
    _br(Traits::TableIcmp64[Index].C3, LabelTrue);
    Context.insert(LabelFalse);
    _mov_redefined(Dest, Zero);
    Context.insert(LabelTrue);
  } else {
    _cmp(Src0HiRM, Src1HiRI);
    if (Traits::TableIcmp64[Index].C1 != Traits::Cond::Br_None)
      _br(Traits::TableIcmp64[Index].C1, Br->getTargetTrue());
    if (Traits::TableIcmp64[Index].C2 != Traits::Cond::Br_None)
      _br(Traits::TableIcmp64[Index].C2, Br->getTargetFalse());
    _cmp(Src0LoRM, Src1LoRI);
    _br(Traits::TableIcmp64[Index].C3, Br->getTargetTrue(),
        Br->getTargetFalse());
  }
}

template <class Machine>
void TargetX86Base<Machine>::setccOrBr(typename Traits::Cond::BrCond Condition,
                                       Variable *Dest, const InstBr *Br) {
  if (Br == nullptr) {
    _setcc(Dest, Condition);
  } else {
    _br(Condition, Br->getTargetTrue(), Br->getTargetFalse());
  }
}

template <class Machine>
void TargetX86Base<Machine>::movOrBr(bool IcmpResult, Variable *Dest,
                                     const InstBr *Br) {
  if (Br == nullptr) {
    _mov(Dest, Ctx->getConstantInt(Dest->getType(), (IcmpResult ? 1 : 0)));
  } else {
    // TODO(sehr,stichnot): This could be done with a single unconditional
    // branch instruction, but subzero doesn't know how to handle the resulting
    // control flow graph changes now.  Make it do so to eliminate mov and cmp.
    _mov(Dest, Ctx->getConstantInt(Dest->getType(), (IcmpResult ? 1 : 0)));
    _cmp(Dest, Ctx->getConstantInt(Dest->getType(), 0));
    _br(Traits::Cond::Br_ne, Br->getTargetTrue(), Br->getTargetFalse());
  }
}

template <class Machine>
void TargetX86Base<Machine>::lowerArithAndBr(const InstArithmetic *Arith,
                                             const InstBr *Br) {
  Variable *T = nullptr;
  Operand *Src0 = legalize(Arith->getSrc(0));
  Operand *Src1 = legalize(Arith->getSrc(1));
  Variable *Dest = Arith->getDest();
  switch (Arith->getOp()) {
  default:
    llvm_unreachable("arithmetic operator not AND or OR");
    break;
  case InstArithmetic::And:
    _mov(T, Src0);
    // Test cannot have an address in the second position.  Since T is
    // guaranteed to be a register and Src1 could be a memory load, ensure
    // that the second argument is a register.
    if (llvm::isa<Constant>(Src1))
      _test(T, Src1);
    else
      _test(Src1, T);
    break;
  case InstArithmetic::Or:
    _mov(T, Src0);
    _or(T, Src1);
    break;
  }
  Context.insert(InstFakeUse::create(Func, T));
  Context.insert(InstFakeDef::create(Func, Dest));
  _br(Traits::Cond::Br_ne, Br->getTargetTrue(), Br->getTargetFalse());
}

template <class Machine>
void TargetX86Base<Machine>::lowerInsertElement(const InstInsertElement *Inst) {
  Operand *SourceVectNotLegalized = Inst->getSrc(0);
  Operand *ElementToInsertNotLegalized = Inst->getSrc(1);
  ConstantInteger32 *ElementIndex =
      llvm::dyn_cast<ConstantInteger32>(Inst->getSrc(2));
  // Only constant indices are allowed in PNaCl IR.
  assert(ElementIndex);
  unsigned Index = ElementIndex->getValue();
  assert(Index < typeNumElements(SourceVectNotLegalized->getType()));

  Type Ty = SourceVectNotLegalized->getType();
  Type ElementTy = typeElementType(Ty);
  Type InVectorElementTy = Traits::getInVectorElementType(Ty);

  if (ElementTy == IceType_i1) {
    // Expand the element to the appropriate size for it to be inserted in the
    // vector.
    Variable *Expanded = Func->makeVariable(InVectorElementTy);
    InstCast *Cast = InstCast::create(Func, InstCast::Zext, Expanded,
                                      ElementToInsertNotLegalized);
    lowerCast(Cast);
    ElementToInsertNotLegalized = Expanded;
  }

  if (Ty == IceType_v8i16 || Ty == IceType_v8i1 ||
      InstructionSet >= Traits::SSE4_1) {
    // Use insertps, pinsrb, pinsrw, or pinsrd.
    Operand *ElementRM =
        legalize(ElementToInsertNotLegalized, Legal_Reg | Legal_Mem);
    Operand *SourceVectRM =
        legalize(SourceVectNotLegalized, Legal_Reg | Legal_Mem);
    Variable *T = makeReg(Ty);
    _movp(T, SourceVectRM);
    if (Ty == IceType_v4f32) {
      _insertps(T, ElementRM, Ctx->getConstantInt32(Index << 4));
    } else {
      // For the pinsrb and pinsrw instructions, when the source operand is a
      // register, it must be a full r32 register like eax, and not ax/al/ah.
      // For filetype=asm, InstX86Pinsr<Machine>::emit() compensates for the use
      // of r16 and r8 by converting them through getBaseReg(), while emitIAS()
      // validates that the original and base register encodings are the same.
      if (ElementRM->getType() == IceType_i8 &&
          llvm::isa<Variable>(ElementRM)) {
        // Don't use ah/bh/ch/dh for pinsrb.
        ElementRM = copyToReg8(ElementRM);
      }
      _pinsr(T, ElementRM, Ctx->getConstantInt32(Index));
    }
    _movp(Inst->getDest(), T);
  } else if (Ty == IceType_v4i32 || Ty == IceType_v4f32 || Ty == IceType_v4i1) {
    // Use shufps or movss.
    Variable *ElementR = nullptr;
    Operand *SourceVectRM =
        legalize(SourceVectNotLegalized, Legal_Reg | Legal_Mem);

    if (InVectorElementTy == IceType_f32) {
      // ElementR will be in an XMM register since it is floating point.
      ElementR = legalizeToReg(ElementToInsertNotLegalized);
    } else {
      // Copy an integer to an XMM register.
      Operand *T = legalize(ElementToInsertNotLegalized, Legal_Reg | Legal_Mem);
      ElementR = makeReg(Ty);
      _movd(ElementR, T);
    }

    if (Index == 0) {
      Variable *T = makeReg(Ty);
      _movp(T, SourceVectRM);
      _movss(T, ElementR);
      _movp(Inst->getDest(), T);
      return;
    }

    // shufps treats the source and destination operands as vectors of four
    // doublewords. The destination's two high doublewords are selected from
    // the source operand and the two low doublewords are selected from the
    // (original value of) the destination operand. An insertelement operation
    // can be effected with a sequence of two shufps operations with
    // appropriate masks. In all cases below, Element[0] is being inserted into
    // SourceVectOperand. Indices are ordered from left to right.
    //
    // insertelement into index 1 (result is stored in ElementR):
    //   ElementR := ElementR[0, 0] SourceVectRM[0, 0]
    //   ElementR := ElementR[3, 0] SourceVectRM[2, 3]
    //
    // insertelement into index 2 (result is stored in T):
    //   T := SourceVectRM
    //   ElementR := ElementR[0, 0] T[0, 3]
    //   T := T[0, 1] ElementR[0, 3]
    //
    // insertelement into index 3 (result is stored in T):
    //   T := SourceVectRM
    //   ElementR := ElementR[0, 0] T[0, 2]
    //   T := T[0, 1] ElementR[3, 0]
    const unsigned char Mask1[3] = {0, 192, 128};
    const unsigned char Mask2[3] = {227, 196, 52};

    Constant *Mask1Constant = Ctx->getConstantInt32(Mask1[Index - 1]);
    Constant *Mask2Constant = Ctx->getConstantInt32(Mask2[Index - 1]);

    if (Index == 1) {
      _shufps(ElementR, SourceVectRM, Mask1Constant);
      _shufps(ElementR, SourceVectRM, Mask2Constant);
      _movp(Inst->getDest(), ElementR);
    } else {
      Variable *T = makeReg(Ty);
      _movp(T, SourceVectRM);
      _shufps(ElementR, T, Mask1Constant);
      _shufps(T, ElementR, Mask2Constant);
      _movp(Inst->getDest(), T);
    }
  } else {
    assert(Ty == IceType_v16i8 || Ty == IceType_v16i1);
    // Spill the value to a stack slot and perform the insertion in memory.
    //
    // TODO(wala): use legalize(SourceVectNotLegalized, Legal_Mem) when support
    // for legalizing to mem is implemented.
    Variable *Slot = Func->makeVariable(Ty);
    Slot->setMustNotHaveReg();
    _movp(Slot, legalizeToReg(SourceVectNotLegalized));

    // Compute the location of the position to insert in memory.
    unsigned Offset = Index * typeWidthInBytes(InVectorElementTy);
    typename Traits::X86OperandMem *Loc =
        getMemoryOperandForStackSlot(InVectorElementTy, Slot, Offset);
    _store(legalizeToReg(ElementToInsertNotLegalized), Loc);

    Variable *T = makeReg(Ty);
    _movp(T, Slot);
    _movp(Inst->getDest(), T);
  }
}

template <class Machine>
void TargetX86Base<Machine>::lowerIntrinsicCall(
    const InstIntrinsicCall *Instr) {
  switch (Intrinsics::IntrinsicID ID = Instr->getIntrinsicInfo().ID) {
  case Intrinsics::AtomicCmpxchg: {
    if (!Intrinsics::isMemoryOrderValid(
            ID, getConstantMemoryOrder(Instr->getArg(3)),
            getConstantMemoryOrder(Instr->getArg(4)))) {
      Func->setError("Unexpected memory ordering for AtomicCmpxchg");
      return;
    }
    Variable *DestPrev = Instr->getDest();
    Operand *PtrToMem = legalize(Instr->getArg(0));
    Operand *Expected = legalize(Instr->getArg(1));
    Operand *Desired = legalize(Instr->getArg(2));
    if (tryOptimizedCmpxchgCmpBr(DestPrev, PtrToMem, Expected, Desired))
      return;
    lowerAtomicCmpxchg(DestPrev, PtrToMem, Expected, Desired);
    return;
  }
  case Intrinsics::AtomicFence:
    if (!Intrinsics::isMemoryOrderValid(
            ID, getConstantMemoryOrder(Instr->getArg(0)))) {
      Func->setError("Unexpected memory ordering for AtomicFence");
      return;
    }
    _mfence();
    return;
  case Intrinsics::AtomicFenceAll:
    // NOTE: FenceAll should prevent and load/store from being moved across the
    // fence (both atomic and non-atomic). The InstX8632Mfence instruction is
    // currently marked coarsely as "HasSideEffects".
    _mfence();
    return;
  case Intrinsics::AtomicIsLockFree: {
    // X86 is always lock free for 8/16/32/64 bit accesses.
    // TODO(jvoung): Since the result is constant when given a constant byte
    // size, this opens up DCE opportunities.
    Operand *ByteSize = Instr->getArg(0);
    Variable *Dest = Instr->getDest();
    if (ConstantInteger32 *CI = llvm::dyn_cast<ConstantInteger32>(ByteSize)) {
      Constant *Result;
      switch (CI->getValue()) {
      default:
        // Some x86-64 processors support the cmpxchg16b instruction, which can
        // make 16-byte operations lock free (when used with the LOCK prefix).
        // However, that's not supported in 32-bit mode, so just return 0 even
        // for large sizes.
        Result = Ctx->getConstantZero(IceType_i32);
        break;
      case 1:
      case 2:
      case 4:
      case 8:
        Result = Ctx->getConstantInt32(1);
        break;
      }
      _mov(Dest, Result);
      return;
    }
    // The PNaCl ABI requires the byte size to be a compile-time constant.
    Func->setError("AtomicIsLockFree byte size should be compile-time const");
    return;
  }
  case Intrinsics::AtomicLoad: {
    // We require the memory address to be naturally aligned. Given that is the
    // case, then normal loads are atomic.
    if (!Intrinsics::isMemoryOrderValid(
            ID, getConstantMemoryOrder(Instr->getArg(1)))) {
      Func->setError("Unexpected memory ordering for AtomicLoad");
      return;
    }
    Variable *Dest = Instr->getDest();
    if (!Traits::Is64Bit) {
      if (auto *Dest64On32 = llvm::dyn_cast<Variable64On32>(Dest)) {
        // Follow what GCC does and use a movq instead of what lowerLoad()
        // normally does (split the load into two). Thus, this skips
        // load/arithmetic op folding. Load/arithmetic folding can't happen
        // anyway, since this is x86-32 and integer arithmetic only happens on
        // 32-bit quantities.
        Variable *T = makeReg(IceType_f64);
        typename Traits::X86OperandMem *Addr =
            formMemoryOperand(Instr->getArg(0), IceType_f64);
        _movq(T, Addr);
        // Then cast the bits back out of the XMM register to the i64 Dest.
        InstCast *Cast = InstCast::create(Func, InstCast::Bitcast, Dest, T);
        lowerCast(Cast);
        // Make sure that the atomic load isn't elided when unused.
        Context.insert(InstFakeUse::create(Func, Dest64On32->getLo()));
        Context.insert(InstFakeUse::create(Func, Dest64On32->getHi()));
        return;
      }
    }
    InstLoad *Load = InstLoad::create(Func, Dest, Instr->getArg(0));
    lowerLoad(Load);
    // Make sure the atomic load isn't elided when unused, by adding a FakeUse.
    // Since lowerLoad may fuse the load w/ an arithmetic instruction, insert
    // the FakeUse on the last-inserted instruction's dest.
    Context.insert(
        InstFakeUse::create(Func, Context.getLastInserted()->getDest()));
    return;
  }
  case Intrinsics::AtomicRMW:
    if (!Intrinsics::isMemoryOrderValid(
            ID, getConstantMemoryOrder(Instr->getArg(3)))) {
      Func->setError("Unexpected memory ordering for AtomicRMW");
      return;
    }
    lowerAtomicRMW(
        Instr->getDest(),
        static_cast<uint32_t>(
            llvm::cast<ConstantInteger32>(Instr->getArg(0))->getValue()),
        Instr->getArg(1), Instr->getArg(2));
    return;
  case Intrinsics::AtomicStore: {
    if (!Intrinsics::isMemoryOrderValid(
            ID, getConstantMemoryOrder(Instr->getArg(2)))) {
      Func->setError("Unexpected memory ordering for AtomicStore");
      return;
    }
    // We require the memory address to be naturally aligned. Given that is the
    // case, then normal stores are atomic. Add a fence after the store to make
    // it visible.
    Operand *Value = Instr->getArg(0);
    Operand *Ptr = Instr->getArg(1);
    if (!Traits::Is64Bit && Value->getType() == IceType_i64) {
      // Use a movq instead of what lowerStore() normally does (split the store
      // into two), following what GCC does. Cast the bits from int -> to an
      // xmm register first.
      Variable *T = makeReg(IceType_f64);
      InstCast *Cast = InstCast::create(Func, InstCast::Bitcast, T, Value);
      lowerCast(Cast);
      // Then store XMM w/ a movq.
      typename Traits::X86OperandMem *Addr =
          formMemoryOperand(Ptr, IceType_f64);
      _storeq(T, Addr);
      _mfence();
      return;
    }
    InstStore *Store = InstStore::create(Func, Value, Ptr);
    lowerStore(Store);
    _mfence();
    return;
  }
  case Intrinsics::Bswap: {
    Variable *Dest = Instr->getDest();
    Operand *Val = Instr->getArg(0);
    // In 32-bit mode, bswap only works on 32-bit arguments, and the argument
    // must be a register. Use rotate left for 16-bit bswap.
    if (!Traits::Is64Bit && Val->getType() == IceType_i64) {
      Val = legalizeUndef(Val);
      Variable *T_Lo = legalizeToReg(loOperand(Val));
      Variable *T_Hi = legalizeToReg(hiOperand(Val));
      Variable *DestLo = llvm::cast<Variable>(loOperand(Dest));
      Variable *DestHi = llvm::cast<Variable>(hiOperand(Dest));
      _bswap(T_Lo);
      _bswap(T_Hi);
      _mov(DestLo, T_Hi);
      _mov(DestHi, T_Lo);
    } else if ((Traits::Is64Bit && Val->getType() == IceType_i64) ||
               Val->getType() == IceType_i32) {
      Variable *T = legalizeToReg(Val);
      _bswap(T);
      _mov(Dest, T);
    } else {
      assert(Val->getType() == IceType_i16);
      Constant *Eight = Ctx->getConstantInt16(8);
      Variable *T = nullptr;
      Val = legalize(Val);
      _mov(T, Val);
      _rol(T, Eight);
      _mov(Dest, T);
    }
    return;
  }
  case Intrinsics::Ctpop: {
    Variable *Dest = Instr->getDest();
    Variable *T = nullptr;
    Operand *Val = Instr->getArg(0);
    Type ValTy = Val->getType();
    assert(ValTy == IceType_i32 || ValTy == IceType_i64);

    if (!Traits::Is64Bit) {
      T = Dest;
    } else {
      T = makeReg(IceType_i64);
      if (ValTy == IceType_i32) {
        // in x86-64, __popcountsi2 is not defined, so we cheat a bit by
        // converting it to a 64-bit value, and using ctpop_i64. _movzx should
        // ensure we will not have any bits set on Val's upper 32 bits.
        Variable *V = makeReg(IceType_i64);
        _movzx(V, Val);
        Val = V;
      }
      ValTy = IceType_i64;
    }

    InstCall *Call = makeHelperCall(
        ValTy == IceType_i32 ? H_call_ctpop_i32 : H_call_ctpop_i64, T, 1);
    Call->addArg(Val);
    lowerCall(Call);
    // The popcount helpers always return 32-bit values, while the intrinsic's
    // signature matches the native POPCNT instruction and fills a 64-bit reg
    // (in 64-bit mode). Thus, clear the upper bits of the dest just in case
    // the user doesn't do that in the IR. If the user does that in the IR,
    // then this zero'ing instruction is dead and gets optimized out.
    if (!Traits::Is64Bit) {
      assert(T == Dest);
      if (Val->getType() == IceType_i64) {
        Variable *DestHi = llvm::cast<Variable>(hiOperand(Dest));
        Constant *Zero = Ctx->getConstantZero(IceType_i32);
        _mov(DestHi, Zero);
      }
    } else {
      assert(Val->getType() == IceType_i64);
      // T is 64 bit. It needs to be copied to dest. We need to:
      //
      // T_1.32 = trunc T.64 to i32
      // T_2.64 = zext T_1.32 to i64
      // Dest.<<right_size>> = T_2.<<right_size>>
      //
      // which ensures the upper 32 bits will always be cleared. Just doing a
      //
      // mov Dest.32 = trunc T.32 to i32
      //
      // is dangerous because there's a chance the compiler will optimize this
      // copy out. To use _movzx we need two new registers (one 32-, and
      // another 64-bit wide.)
      Variable *T_1 = makeReg(IceType_i32);
      _mov(T_1, T);
      Variable *T_2 = makeReg(IceType_i64);
      _movzx(T_2, T_1);
      _mov(Dest, T_2);
    }
    return;
  }
  case Intrinsics::Ctlz: {
    // The "is zero undef" parameter is ignored and we always return a
    // well-defined value.
    Operand *Val = legalize(Instr->getArg(0));
    Operand *FirstVal;
    Operand *SecondVal = nullptr;
    if (!Traits::Is64Bit && Val->getType() == IceType_i64) {
      FirstVal = loOperand(Val);
      SecondVal = hiOperand(Val);
    } else {
      FirstVal = Val;
    }
    constexpr bool IsCttz = false;
    lowerCountZeros(IsCttz, Val->getType(), Instr->getDest(), FirstVal,
                    SecondVal);
    return;
  }
  case Intrinsics::Cttz: {
    // The "is zero undef" parameter is ignored and we always return a
    // well-defined value.
    Operand *Val = legalize(Instr->getArg(0));
    Operand *FirstVal;
    Operand *SecondVal = nullptr;
    if (!Traits::Is64Bit && Val->getType() == IceType_i64) {
      FirstVal = hiOperand(Val);
      SecondVal = loOperand(Val);
    } else {
      FirstVal = Val;
    }
    constexpr bool IsCttz = true;
    lowerCountZeros(IsCttz, Val->getType(), Instr->getDest(), FirstVal,
                    SecondVal);
    return;
  }
  case Intrinsics::Fabs: {
    Operand *Src = legalize(Instr->getArg(0));
    Type Ty = Src->getType();
    Variable *Dest = Instr->getDest();
    Variable *T = makeVectorOfFabsMask(Ty);
    // The pand instruction operates on an m128 memory operand, so if Src is an
    // f32 or f64, we need to make sure it's in a register.
    if (isVectorType(Ty)) {
      if (llvm::isa<typename Traits::X86OperandMem>(Src))
        Src = legalizeToReg(Src);
    } else {
      Src = legalizeToReg(Src);
    }
    _pand(T, Src);
    if (isVectorType(Ty))
      _movp(Dest, T);
    else
      _mov(Dest, T);
    return;
  }
  case Intrinsics::Longjmp: {
    InstCall *Call = makeHelperCall(H_call_longjmp, nullptr, 2);
    Call->addArg(Instr->getArg(0));
    Call->addArg(Instr->getArg(1));
    lowerCall(Call);
    return;
  }
  case Intrinsics::Memcpy: {
    lowerMemcpy(Instr->getArg(0), Instr->getArg(1), Instr->getArg(2));
    return;
  }
  case Intrinsics::Memmove: {
    lowerMemmove(Instr->getArg(0), Instr->getArg(1), Instr->getArg(2));
    return;
  }
  case Intrinsics::Memset: {
    lowerMemset(Instr->getArg(0), Instr->getArg(1), Instr->getArg(2));
    return;
  }
  case Intrinsics::NaClReadTP: {
    if (Ctx->getFlags().getUseSandboxing()) {
      Operand *Src = dispatchToConcrete(&Machine::createNaClReadTPSrcOperand);
      Variable *Dest = Instr->getDest();
      Variable *T = nullptr;
      _mov(T, Src);
      _mov(Dest, T);
    } else {
      InstCall *Call = makeHelperCall(H_call_read_tp, Instr->getDest(), 0);
      lowerCall(Call);
    }
    return;
  }
  case Intrinsics::Setjmp: {
    InstCall *Call = makeHelperCall(H_call_setjmp, Instr->getDest(), 1);
    Call->addArg(Instr->getArg(0));
    lowerCall(Call);
    return;
  }
  case Intrinsics::Sqrt: {
    Operand *Src = legalize(Instr->getArg(0));
    Variable *Dest = Instr->getDest();
    Variable *T = makeReg(Dest->getType());
    _sqrtss(T, Src);
    _mov(Dest, T);
    return;
  }
  case Intrinsics::Stacksave: {
    Variable *esp =
        Func->getTarget()->getPhysicalRegister(Traits::RegisterSet::Reg_esp);
    Variable *Dest = Instr->getDest();
    _mov(Dest, esp);
    return;
  }
  case Intrinsics::Stackrestore: {
    Variable *esp =
        Func->getTarget()->getPhysicalRegister(Traits::RegisterSet::Reg_esp);
    _mov_redefined(esp, Instr->getArg(0));
    return;
  }
  case Intrinsics::Trap:
    _ud2();
    return;
  case Intrinsics::UnknownIntrinsic:
    Func->setError("Should not be lowering UnknownIntrinsic");
    return;
  }
  return;
}

template <class Machine>
void TargetX86Base<Machine>::lowerAtomicCmpxchg(Variable *DestPrev,
                                                Operand *Ptr, Operand *Expected,
                                                Operand *Desired) {
  Type Ty = Expected->getType();
  if (!Traits::Is64Bit && Ty == IceType_i64) {
    // Reserve the pre-colored registers first, before adding any more
    // infinite-weight variables from formMemoryOperand's legalization.
    Variable *T_edx = makeReg(IceType_i32, Traits::RegisterSet::Reg_edx);
    Variable *T_eax = makeReg(IceType_i32, Traits::RegisterSet::Reg_eax);
    Variable *T_ecx = makeReg(IceType_i32, Traits::RegisterSet::Reg_ecx);
    Variable *T_ebx = makeReg(IceType_i32, Traits::RegisterSet::Reg_ebx);
    _mov(T_eax, loOperand(Expected));
    _mov(T_edx, hiOperand(Expected));
    _mov(T_ebx, loOperand(Desired));
    _mov(T_ecx, hiOperand(Desired));
    typename Traits::X86OperandMem *Addr = formMemoryOperand(Ptr, Ty);
    constexpr bool Locked = true;
    _cmpxchg8b(Addr, T_edx, T_eax, T_ecx, T_ebx, Locked);
    Variable *DestLo = llvm::cast<Variable>(loOperand(DestPrev));
    Variable *DestHi = llvm::cast<Variable>(hiOperand(DestPrev));
    _mov(DestLo, T_eax);
    _mov(DestHi, T_edx);
    return;
  }
  int32_t Eax;
  switch (Ty) {
  default:
    llvm_unreachable("Bad type for cmpxchg");
  // fallthrough
  case IceType_i32:
    Eax = Traits::RegisterSet::Reg_eax;
    break;
  case IceType_i16:
    Eax = Traits::RegisterSet::Reg_ax;
    break;
  case IceType_i8:
    Eax = Traits::RegisterSet::Reg_al;
    break;
  }
  Variable *T_eax = makeReg(Ty, Eax);
  _mov(T_eax, Expected);
  typename Traits::X86OperandMem *Addr = formMemoryOperand(Ptr, Ty);
  Variable *DesiredReg = legalizeToReg(Desired);
  constexpr bool Locked = true;
  _cmpxchg(Addr, T_eax, DesiredReg, Locked);
  _mov(DestPrev, T_eax);
}

template <class Machine>
bool TargetX86Base<Machine>::tryOptimizedCmpxchgCmpBr(Variable *Dest,
                                                      Operand *PtrToMem,
                                                      Operand *Expected,
                                                      Operand *Desired) {
  if (Ctx->getFlags().getOptLevel() == Opt_m1)
    return false;
  // Peek ahead a few instructions and see how Dest is used.
  // It's very common to have:
  //
  // %x = call i32 @llvm.nacl.atomic.cmpxchg.i32(i32* ptr, i32 %expected, ...)
  // [%y_phi = ...] // list of phi stores
  // %p = icmp eq i32 %x, %expected
  // br i1 %p, label %l1, label %l2
  //
  // which we can optimize into:
  //
  // %x = <cmpxchg code>
  // [%y_phi = ...] // list of phi stores
  // br eq, %l1, %l2
  InstList::iterator I = Context.getCur();
  // I is currently the InstIntrinsicCall. Peek past that.
  // This assumes that the atomic cmpxchg has not been lowered yet,
  // so that the instructions seen in the scan from "Cur" is simple.
  assert(llvm::isa<InstIntrinsicCall>(*I));
  Inst *NextInst = Context.getNextInst(I);
  if (!NextInst)
    return false;
  // There might be phi assignments right before the compare+branch, since this
  // could be a backward branch for a loop. This placement of assignments is
  // determined by placePhiStores().
  std::vector<InstAssign *> PhiAssigns;
  while (InstAssign *PhiAssign = llvm::dyn_cast<InstAssign>(NextInst)) {
    if (PhiAssign->getDest() == Dest)
      return false;
    PhiAssigns.push_back(PhiAssign);
    NextInst = Context.getNextInst(I);
    if (!NextInst)
      return false;
  }
  if (InstIcmp *NextCmp = llvm::dyn_cast<InstIcmp>(NextInst)) {
    if (!(NextCmp->getCondition() == InstIcmp::Eq &&
          ((NextCmp->getSrc(0) == Dest && NextCmp->getSrc(1) == Expected) ||
           (NextCmp->getSrc(1) == Dest && NextCmp->getSrc(0) == Expected)))) {
      return false;
    }
    NextInst = Context.getNextInst(I);
    if (!NextInst)
      return false;
    if (InstBr *NextBr = llvm::dyn_cast<InstBr>(NextInst)) {
      if (!NextBr->isUnconditional() &&
          NextCmp->getDest() == NextBr->getCondition() &&
          NextBr->isLastUse(NextCmp->getDest())) {
        lowerAtomicCmpxchg(Dest, PtrToMem, Expected, Desired);
        for (size_t i = 0; i < PhiAssigns.size(); ++i) {
          // Lower the phi assignments now, before the branch (same placement
          // as before).
          InstAssign *PhiAssign = PhiAssigns[i];
          PhiAssign->setDeleted();
          lowerAssign(PhiAssign);
          Context.advanceNext();
        }
        _br(Traits::Cond::Br_e, NextBr->getTargetTrue(),
            NextBr->getTargetFalse());
        // Skip over the old compare and branch, by deleting them.
        NextCmp->setDeleted();
        NextBr->setDeleted();
        Context.advanceNext();
        Context.advanceNext();
        return true;
      }
    }
  }
  return false;
}

template <class Machine>
void TargetX86Base<Machine>::lowerAtomicRMW(Variable *Dest, uint32_t Operation,
                                            Operand *Ptr, Operand *Val) {
  bool NeedsCmpxchg = false;
  LowerBinOp Op_Lo = nullptr;
  LowerBinOp Op_Hi = nullptr;
  switch (Operation) {
  default:
    Func->setError("Unknown AtomicRMW operation");
    return;
  case Intrinsics::AtomicAdd: {
    if (!Traits::Is64Bit && Dest->getType() == IceType_i64) {
      // All the fall-through paths must set this to true, but use this
      // for asserting.
      NeedsCmpxchg = true;
      Op_Lo = &TargetX86Base<Machine>::_add;
      Op_Hi = &TargetX86Base<Machine>::_adc;
      break;
    }
    typename Traits::X86OperandMem *Addr =
        formMemoryOperand(Ptr, Dest->getType());
    constexpr bool Locked = true;
    Variable *T = nullptr;
    _mov(T, Val);
    _xadd(Addr, T, Locked);
    _mov(Dest, T);
    return;
  }
  case Intrinsics::AtomicSub: {
    if (!Traits::Is64Bit && Dest->getType() == IceType_i64) {
      NeedsCmpxchg = true;
      Op_Lo = &TargetX86Base<Machine>::_sub;
      Op_Hi = &TargetX86Base<Machine>::_sbb;
      break;
    }
    typename Traits::X86OperandMem *Addr =
        formMemoryOperand(Ptr, Dest->getType());
    constexpr bool Locked = true;
    Variable *T = nullptr;
    _mov(T, Val);
    _neg(T);
    _xadd(Addr, T, Locked);
    _mov(Dest, T);
    return;
  }
  case Intrinsics::AtomicOr:
    // TODO(jvoung): If Dest is null or dead, then some of these
    // operations do not need an "exchange", but just a locked op.
    // That appears to be "worth" it for sub, or, and, and xor.
    // xadd is probably fine vs lock add for add, and xchg is fine
    // vs an atomic store.
    NeedsCmpxchg = true;
    Op_Lo = &TargetX86Base<Machine>::_or;
    Op_Hi = &TargetX86Base<Machine>::_or;
    break;
  case Intrinsics::AtomicAnd:
    NeedsCmpxchg = true;
    Op_Lo = &TargetX86Base<Machine>::_and;
    Op_Hi = &TargetX86Base<Machine>::_and;
    break;
  case Intrinsics::AtomicXor:
    NeedsCmpxchg = true;
    Op_Lo = &TargetX86Base<Machine>::_xor;
    Op_Hi = &TargetX86Base<Machine>::_xor;
    break;
  case Intrinsics::AtomicExchange:
    if (!Traits::Is64Bit && Dest->getType() == IceType_i64) {
      NeedsCmpxchg = true;
      // NeedsCmpxchg, but no real Op_Lo/Op_Hi need to be done. The values
      // just need to be moved to the ecx and ebx registers.
      Op_Lo = nullptr;
      Op_Hi = nullptr;
      break;
    }
    typename Traits::X86OperandMem *Addr =
        formMemoryOperand(Ptr, Dest->getType());
    Variable *T = nullptr;
    _mov(T, Val);
    _xchg(Addr, T);
    _mov(Dest, T);
    return;
  }
  // Otherwise, we need a cmpxchg loop.
  (void)NeedsCmpxchg;
  assert(NeedsCmpxchg);
  expandAtomicRMWAsCmpxchg(Op_Lo, Op_Hi, Dest, Ptr, Val);
}

template <class Machine>
void TargetX86Base<Machine>::expandAtomicRMWAsCmpxchg(LowerBinOp Op_Lo,
                                                      LowerBinOp Op_Hi,
                                                      Variable *Dest,
                                                      Operand *Ptr,
                                                      Operand *Val) {
  // Expand a more complex RMW operation as a cmpxchg loop:
  // For 64-bit:
  //   mov     eax, [ptr]
  //   mov     edx, [ptr + 4]
  // .LABEL:
  //   mov     ebx, eax
  //   <Op_Lo> ebx, <desired_adj_lo>
  //   mov     ecx, edx
  //   <Op_Hi> ecx, <desired_adj_hi>
  //   lock cmpxchg8b [ptr]
  //   jne     .LABEL
  //   mov     <dest_lo>, eax
  //   mov     <dest_lo>, edx
  //
  // For 32-bit:
  //   mov     eax, [ptr]
  // .LABEL:
  //   mov     <reg>, eax
  //   op      <reg>, [desired_adj]
  //   lock cmpxchg [ptr], <reg>
  //   jne     .LABEL
  //   mov     <dest>, eax
  //
  // If Op_{Lo,Hi} are nullptr, then just copy the value.
  Val = legalize(Val);
  Type Ty = Val->getType();
  if (!Traits::Is64Bit && Ty == IceType_i64) {
    Variable *T_edx = makeReg(IceType_i32, Traits::RegisterSet::Reg_edx);
    Variable *T_eax = makeReg(IceType_i32, Traits::RegisterSet::Reg_eax);
    typename Traits::X86OperandMem *Addr = formMemoryOperand(Ptr, Ty);
    _mov(T_eax, loOperand(Addr));
    _mov(T_edx, hiOperand(Addr));
    Variable *T_ecx = makeReg(IceType_i32, Traits::RegisterSet::Reg_ecx);
    Variable *T_ebx = makeReg(IceType_i32, Traits::RegisterSet::Reg_ebx);
    typename Traits::Insts::Label *Label =
        Traits::Insts::Label::create(Func, this);
    const bool IsXchg8b = Op_Lo == nullptr && Op_Hi == nullptr;
    if (!IsXchg8b) {
      Context.insert(Label);
      _mov(T_ebx, T_eax);
      (this->*Op_Lo)(T_ebx, loOperand(Val));
      _mov(T_ecx, T_edx);
      (this->*Op_Hi)(T_ecx, hiOperand(Val));
    } else {
      // This is for xchg, which doesn't need an actual Op_Lo/Op_Hi.
      // It just needs the Val loaded into ebx and ecx.
      // That can also be done before the loop.
      _mov(T_ebx, loOperand(Val));
      _mov(T_ecx, hiOperand(Val));
      Context.insert(Label);
    }
    constexpr bool Locked = true;
    _cmpxchg8b(Addr, T_edx, T_eax, T_ecx, T_ebx, Locked);
    _br(Traits::Cond::Br_ne, Label);
    if (!IsXchg8b) {
      // If Val is a variable, model the extended live range of Val through
      // the end of the loop, since it will be re-used by the loop.
      if (Variable *ValVar = llvm::dyn_cast<Variable>(Val)) {
        Variable *ValLo = llvm::cast<Variable>(loOperand(ValVar));
        Variable *ValHi = llvm::cast<Variable>(hiOperand(ValVar));
        Context.insert(InstFakeUse::create(Func, ValLo));
        Context.insert(InstFakeUse::create(Func, ValHi));
      }
    } else {
      // For xchg, the loop is slightly smaller and ebx/ecx are used.
      Context.insert(InstFakeUse::create(Func, T_ebx));
      Context.insert(InstFakeUse::create(Func, T_ecx));
    }
    // The address base (if any) is also reused in the loop.
    if (Variable *Base = Addr->getBase())
      Context.insert(InstFakeUse::create(Func, Base));
    Variable *DestLo = llvm::cast<Variable>(loOperand(Dest));
    Variable *DestHi = llvm::cast<Variable>(hiOperand(Dest));
    _mov(DestLo, T_eax);
    _mov(DestHi, T_edx);
    return;
  }
  typename Traits::X86OperandMem *Addr = formMemoryOperand(Ptr, Ty);
  int32_t Eax;
  switch (Ty) {
  default:
    llvm_unreachable("Bad type for atomicRMW");
  // fallthrough
  case IceType_i32:
    Eax = Traits::RegisterSet::Reg_eax;
    break;
  case IceType_i16:
    Eax = Traits::RegisterSet::Reg_ax;
    break;
  case IceType_i8:
    Eax = Traits::RegisterSet::Reg_al;
    break;
  }
  Variable *T_eax = makeReg(Ty, Eax);
  _mov(T_eax, Addr);
  typename Traits::Insts::Label *Label =
      Traits::Insts::Label::create(Func, this);
  Context.insert(Label);
  // We want to pick a different register for T than Eax, so don't use
  // _mov(T == nullptr, T_eax).
  Variable *T = makeReg(Ty);
  _mov(T, T_eax);
  (this->*Op_Lo)(T, Val);
  constexpr bool Locked = true;
  _cmpxchg(Addr, T_eax, T, Locked);
  _br(Traits::Cond::Br_ne, Label);
  // If Val is a variable, model the extended live range of Val through
  // the end of the loop, since it will be re-used by the loop.
  if (Variable *ValVar = llvm::dyn_cast<Variable>(Val)) {
    Context.insert(InstFakeUse::create(Func, ValVar));
  }
  // The address base (if any) is also reused in the loop.
  if (Variable *Base = Addr->getBase())
    Context.insert(InstFakeUse::create(Func, Base));
  _mov(Dest, T_eax);
}

/// Lowers count {trailing, leading} zeros intrinsic.
///
/// We could do constant folding here, but that should have
/// been done by the front-end/middle-end optimizations.
template <class Machine>
void TargetX86Base<Machine>::lowerCountZeros(bool Cttz, Type Ty, Variable *Dest,
                                             Operand *FirstVal,
                                             Operand *SecondVal) {
  // TODO(jvoung): Determine if the user CPU supports LZCNT (BMI).
  // Then the instructions will handle the Val == 0 case much more simply
  // and won't require conversion from bit position to number of zeros.
  //
  // Otherwise:
  //   bsr IF_NOT_ZERO, Val
  //   mov T_DEST, 63
  //   cmovne T_DEST, IF_NOT_ZERO
  //   xor T_DEST, 31
  //   mov DEST, T_DEST
  //
  // NOTE: T_DEST must be a register because cmov requires its dest to be a
  // register. Also, bsf and bsr require their dest to be a register.
  //
  // The xor DEST, 31 converts a bit position to # of leading zeroes.
  // E.g., for 000... 00001100, bsr will say that the most significant bit
  // set is at position 3, while the number of leading zeros is 28. Xor is
  // like (31 - N) for N <= 31, and converts 63 to 32 (for the all-zeros case).
  //
  // Similar for 64-bit, but start w/ speculating that the upper 32 bits
  // are all zero, and compute the result for that case (checking the lower
  // 32 bits). Then actually compute the result for the upper bits and
  // cmov in the result from the lower computation if the earlier speculation
  // was correct.
  //
  // Cttz, is similar, but uses bsf instead, and doesn't require the xor
  // bit position conversion, and the speculation is reversed.
  assert(Ty == IceType_i32 || Ty == IceType_i64);
  Variable *T = makeReg(IceType_i32);
  Operand *FirstValRM = legalize(FirstVal, Legal_Mem | Legal_Reg);
  if (Cttz) {
    _bsf(T, FirstValRM);
  } else {
    _bsr(T, FirstValRM);
  }
  Variable *T_Dest = makeReg(IceType_i32);
  Constant *ThirtyTwo = Ctx->getConstantInt32(32);
  Constant *ThirtyOne = Ctx->getConstantInt32(31);
  if (Cttz) {
    _mov(T_Dest, ThirtyTwo);
  } else {
    Constant *SixtyThree = Ctx->getConstantInt32(63);
    _mov(T_Dest, SixtyThree);
  }
  _cmov(T_Dest, T, Traits::Cond::Br_ne);
  if (!Cttz) {
    _xor(T_Dest, ThirtyOne);
  }
  if (Traits::Is64Bit || Ty == IceType_i32) {
    _mov(Dest, T_Dest);
    return;
  }
  _add(T_Dest, ThirtyTwo);
  Variable *DestLo = llvm::cast<Variable>(loOperand(Dest));
  Variable *DestHi = llvm::cast<Variable>(hiOperand(Dest));
  // Will be using "test" on this, so we need a registerized variable.
  Variable *SecondVar = legalizeToReg(SecondVal);
  Variable *T_Dest2 = makeReg(IceType_i32);
  if (Cttz) {
    _bsf(T_Dest2, SecondVar);
  } else {
    _bsr(T_Dest2, SecondVar);
    _xor(T_Dest2, ThirtyOne);
  }
  _test(SecondVar, SecondVar);
  _cmov(T_Dest2, T_Dest, Traits::Cond::Br_e);
  _mov(DestLo, T_Dest2);
  _mov(DestHi, Ctx->getConstantZero(IceType_i32));
}

template <class Machine>
void TargetX86Base<Machine>::typedLoad(Type Ty, Variable *Dest, Variable *Base,
                                       Constant *Offset) {
  auto *Mem = Traits::X86OperandMem::create(Func, Ty, Base, Offset);

  if (isVectorType(Ty))
    _movp(Dest, Mem);
  else if (Ty == IceType_f64)
    _movq(Dest, Mem);
  else
    _mov(Dest, Mem);
}

template <class Machine>
void TargetX86Base<Machine>::typedStore(Type Ty, Variable *Value,
                                        Variable *Base, Constant *Offset) {
  auto *Mem = Traits::X86OperandMem::create(Func, Ty, Base, Offset);

  if (isVectorType(Ty))
    _storep(Value, Mem);
  else if (Ty == IceType_f64)
    _storeq(Value, Mem);
  else
    _store(Value, Mem);
}

template <class Machine>
void TargetX86Base<Machine>::copyMemory(Type Ty, Variable *Dest, Variable *Src,
                                        int32_t OffsetAmt) {
  Constant *Offset = OffsetAmt ? Ctx->getConstantInt32(OffsetAmt) : nullptr;
  // TODO(ascull): this or add nullptr test to _movp, _movq
  Variable *Data = makeReg(Ty);

  typedLoad(Ty, Data, Src, Offset);
  typedStore(Ty, Data, Dest, Offset);
}

template <class Machine>
void TargetX86Base<Machine>::lowerMemcpy(Operand *Dest, Operand *Src,
                                         Operand *Count) {
  // There is a load and store for each chunk in the unroll
  constexpr uint32_t BytesPerStorep = 16;

  // Check if the operands are constants
  const auto *CountConst = llvm::dyn_cast<const ConstantInteger32>(Count);
  const bool IsCountConst = CountConst != nullptr;
  const uint32_t CountValue = IsCountConst ? CountConst->getValue() : 0;

  if (shouldOptimizeMemIntrins() && IsCountConst &&
      CountValue <= BytesPerStorep * Traits::MEMCPY_UNROLL_LIMIT) {
    // Unlikely, but nothing to do if it does happen
    if (CountValue == 0)
      return;

    Variable *SrcBase = legalizeToReg(Src);
    Variable *DestBase = legalizeToReg(Dest);

    // Find the largest type that can be used and use it as much as possible in
    // reverse order. Then handle any remainder with overlapping copies. Since
    // the remainder will be at the end, there will be reduced pressure on the
    // memory unit as the accesses to the same memory are far apart.
    Type Ty = largestTypeInSize(CountValue);
    uint32_t TyWidth = typeWidthInBytes(Ty);

    uint32_t RemainingBytes = CountValue;
    int32_t Offset = (CountValue & ~(TyWidth - 1)) - TyWidth;
    while (RemainingBytes >= TyWidth) {
      copyMemory(Ty, DestBase, SrcBase, Offset);
      RemainingBytes -= TyWidth;
      Offset -= TyWidth;
    }

    if (RemainingBytes == 0)
      return;

    // Lower the remaining bytes. Adjust to larger types in order to make use
    // of overlaps in the copies.
    Type LeftOverTy = firstTypeThatFitsSize(RemainingBytes);
    Offset = CountValue - typeWidthInBytes(LeftOverTy);
    copyMemory(LeftOverTy, DestBase, SrcBase, Offset);
    return;
  }

  // Fall back on a function call
  InstCall *Call = makeHelperCall(H_call_memcpy, nullptr, 3);
  Call->addArg(Dest);
  Call->addArg(Src);
  Call->addArg(Count);
  lowerCall(Call);
}

template <class Machine>
void TargetX86Base<Machine>::lowerMemmove(Operand *Dest, Operand *Src,
                                          Operand *Count) {
  // There is a load and store for each chunk in the unroll
  constexpr uint32_t BytesPerStorep = 16;

  // Check if the operands are constants
  const auto *CountConst = llvm::dyn_cast<const ConstantInteger32>(Count);
  const bool IsCountConst = CountConst != nullptr;
  const uint32_t CountValue = IsCountConst ? CountConst->getValue() : 0;

  if (shouldOptimizeMemIntrins() && IsCountConst &&
      CountValue <= BytesPerStorep * Traits::MEMMOVE_UNROLL_LIMIT) {
    // Unlikely, but nothing to do if it does happen
    if (CountValue == 0)
      return;

    Variable *SrcBase = legalizeToReg(Src);
    Variable *DestBase = legalizeToReg(Dest);

    std::tuple<Type, Constant *, Variable *>
        Moves[Traits::MEMMOVE_UNROLL_LIMIT];
    Constant *Offset;
    Variable *Reg;

    // Copy the data into registers as the source and destination could overlap
    // so make sure not to clobber the memory. This also means overlapping
    // moves can be used as we are taking a safe snapshot of the memory.
    Type Ty = largestTypeInSize(CountValue);
    uint32_t TyWidth = typeWidthInBytes(Ty);

    uint32_t RemainingBytes = CountValue;
    int32_t OffsetAmt = (CountValue & ~(TyWidth - 1)) - TyWidth;
    size_t N = 0;
    while (RemainingBytes >= TyWidth) {
      assert(N <= Traits::MEMMOVE_UNROLL_LIMIT);
      Offset = Ctx->getConstantInt32(OffsetAmt);
      Reg = makeReg(Ty);
      typedLoad(Ty, Reg, SrcBase, Offset);
      RemainingBytes -= TyWidth;
      OffsetAmt -= TyWidth;
      Moves[N++] = std::make_tuple(Ty, Offset, Reg);
    }

    if (RemainingBytes != 0) {
      // Lower the remaining bytes. Adjust to larger types in order to make use
      // of overlaps in the copies.
      assert(N <= Traits::MEMMOVE_UNROLL_LIMIT);
      Ty = firstTypeThatFitsSize(RemainingBytes);
      Offset = Ctx->getConstantInt32(CountValue - typeWidthInBytes(Ty));
      Reg = makeReg(Ty);
      typedLoad(Ty, Reg, SrcBase, Offset);
      Moves[N++] = std::make_tuple(Ty, Offset, Reg);
    }

    // Copy the data out into the destination memory
    for (size_t i = 0; i < N; ++i) {
      std::tie(Ty, Offset, Reg) = Moves[i];
      typedStore(Ty, Reg, DestBase, Offset);
    }

    return;
  }

  // Fall back on a function call
  InstCall *Call = makeHelperCall(H_call_memmove, nullptr, 3);
  Call->addArg(Dest);
  Call->addArg(Src);
  Call->addArg(Count);
  lowerCall(Call);
}

template <class Machine>
void TargetX86Base<Machine>::lowerMemset(Operand *Dest, Operand *Val,
                                         Operand *Count) {
  constexpr uint32_t BytesPerStorep = 16;
  constexpr uint32_t BytesPerStoreq = 8;
  constexpr uint32_t BytesPerStorei32 = 4;
  assert(Val->getType() == IceType_i8);

  // Check if the operands are constants
  const auto *CountConst = llvm::dyn_cast<const ConstantInteger32>(Count);
  const auto *ValConst = llvm::dyn_cast<const ConstantInteger32>(Val);
  const bool IsCountConst = CountConst != nullptr;
  const bool IsValConst = ValConst != nullptr;
  const uint32_t CountValue = IsCountConst ? CountConst->getValue() : 0;
  const uint32_t ValValue = IsValConst ? ValConst->getValue() : 0;

  // Unlikely, but nothing to do if it does happen
  if (IsCountConst && CountValue == 0)
    return;

  // TODO(ascull): if the count is constant but val is not it would be possible
  // to inline by spreading the value across 4 bytes and accessing subregs e.g.
  // eax, ax and al.
  if (shouldOptimizeMemIntrins() && IsCountConst && IsValConst) {
    Variable *Base = nullptr;
    Variable *VecReg = nullptr;
    const uint32_t SpreadValue =
        (ValValue << 24) | (ValValue << 16) | (ValValue << 8) | ValValue;

    auto lowerSet = [this, &Base, SpreadValue, &VecReg](Type Ty,
                                                        uint32_t OffsetAmt) {
      assert(Base != nullptr);
      Constant *Offset = OffsetAmt ? Ctx->getConstantInt32(OffsetAmt) : nullptr;

      // TODO(ascull): is 64-bit better with vector or scalar movq?
      auto *Mem = Traits::X86OperandMem::create(Func, Ty, Base, Offset);
      if (isVectorType(Ty)) {
        assert(VecReg != nullptr);
        _storep(VecReg, Mem);
      } else if (Ty == IceType_f64) {
        assert(VecReg != nullptr);
        _storeq(VecReg, Mem);
      } else {
        _store(Ctx->getConstantInt(Ty, SpreadValue), Mem);
      }
    };

    // Find the largest type that can be used and use it as much as possible in
    // reverse order. Then handle any remainder with overlapping copies. Since
    // the remainder will be at the end, there will be reduces pressure on the
    // memory unit as the access to the same memory are far apart.
    Type Ty;
    if (ValValue == 0 && CountValue >= BytesPerStoreq &&
        CountValue <= BytesPerStorep * Traits::MEMCPY_UNROLL_LIMIT) {
      // When the value is zero it can be loaded into a vector register cheaply
      // using the xor trick.
      Base = legalizeToReg(Dest);
      VecReg = makeVectorOfZeros(IceType_v16i8);
      Ty = largestTypeInSize(CountValue);
    } else if (CountValue <= BytesPerStorei32 * Traits::MEMCPY_UNROLL_LIMIT) {
      // When the value is non-zero or the count is small we can't use vector
      // instructions so are limited to 32-bit stores.
      Base = legalizeToReg(Dest);
      constexpr uint32_t MaxSize = 4;
      Ty = largestTypeInSize(CountValue, MaxSize);
    }

    if (Base) {
      uint32_t TyWidth = typeWidthInBytes(Ty);

      uint32_t RemainingBytes = CountValue;
      uint32_t Offset = (CountValue & ~(TyWidth - 1)) - TyWidth;
      while (RemainingBytes >= TyWidth) {
        lowerSet(Ty, Offset);
        RemainingBytes -= TyWidth;
        Offset -= TyWidth;
      }

      if (RemainingBytes == 0)
        return;

      // Lower the remaining bytes. Adjust to larger types in order to make use
      // of overlaps in the copies.
      Type LeftOverTy = firstTypeThatFitsSize(RemainingBytes);
      Offset = CountValue - typeWidthInBytes(LeftOverTy);
      lowerSet(LeftOverTy, Offset);
      return;
    }
  }

  // Fall back on calling the memset function. The value operand needs to be
  // extended to a stack slot size because the PNaCl ABI requires arguments to
  // be at least 32 bits wide.
  Operand *ValExt;
  if (IsValConst) {
    ValExt = Ctx->getConstantInt(stackSlotType(), ValValue);
  } else {
    Variable *ValExtVar = Func->makeVariable(stackSlotType());
    lowerCast(InstCast::create(Func, InstCast::Zext, ValExtVar, Val));
    ValExt = ValExtVar;
  }
  InstCall *Call = makeHelperCall(H_call_memset, nullptr, 3);
  Call->addArg(Dest);
  Call->addArg(ValExt);
  Call->addArg(Count);
  lowerCall(Call);
}

template <class Machine>
void TargetX86Base<Machine>::lowerIndirectJump(Variable *Target) {
  const bool NeedSandboxing = Ctx->getFlags().getUseSandboxing();
  if (NeedSandboxing) {
    _bundle_lock();
    const SizeT BundleSize =
        1 << Func->getAssembler<>()->getBundleAlignLog2Bytes();
    _and(Target, Ctx->getConstantInt32(~(BundleSize - 1)));
  }
  _jmp(Target);
  if (NeedSandboxing)
    _bundle_unlock();
}

inline bool isAdd(const Inst *Inst) {
  if (auto *Arith = llvm::dyn_cast_or_null<const InstArithmetic>(Inst)) {
    return (Arith->getOp() == InstArithmetic::Add);
  }
  return false;
}

inline void dumpAddressOpt(const Cfg *Func,
                           const ConstantRelocatable *Relocatable,
                           int32_t Offset, const Variable *Base,
                           const Variable *Index, uint16_t Shift,
                           const Inst *Reason) {
  if (!BuildDefs::dump())
    return;
  if (!Func->isVerbose(IceV_AddrOpt))
    return;
  OstreamLocker L(Func->getContext());
  Ostream &Str = Func->getContext()->getStrDump();
  Str << "Instruction: ";
  Reason->dumpDecorated(Func);
  Str << "  results in Base=";
  if (Base)
    Base->dump(Func);
  else
    Str << "<null>";
  Str << ", Index=";
  if (Index)
    Index->dump(Func);
  else
    Str << "<null>";
  Str << ", Shift=" << Shift << ", Offset=" << Offset
      << ", Relocatable=" << Relocatable << "\n";
}

inline bool matchAssign(const VariablesMetadata *VMetadata, Variable *&Var,
                        ConstantRelocatable *&Relocatable, int32_t &Offset,
                        const Inst *&Reason) {
  // Var originates from Var=SrcVar ==> set Var:=SrcVar
  if (Var == nullptr)
    return false;
  if (const Inst *VarAssign = VMetadata->getSingleDefinition(Var)) {
    assert(!VMetadata->isMultiDef(Var));
    if (llvm::isa<InstAssign>(VarAssign)) {
      Operand *SrcOp = VarAssign->getSrc(0);
      assert(SrcOp);
      if (auto *SrcVar = llvm::dyn_cast<Variable>(SrcOp)) {
        if (!VMetadata->isMultiDef(SrcVar) &&
            // TODO: ensure SrcVar stays single-BB
            true) {
          Var = SrcVar;
          Reason = VarAssign;
          return true;
        }
      } else if (auto *Const = llvm::dyn_cast<ConstantInteger32>(SrcOp)) {
        int32_t MoreOffset = Const->getValue();
        if (Utils::WouldOverflowAdd(Offset, MoreOffset))
          return false;
        Var = nullptr;
        Offset += MoreOffset;
        Reason = VarAssign;
        return true;
      } else if (auto *AddReloc = llvm::dyn_cast<ConstantRelocatable>(SrcOp)) {
        if (Relocatable == nullptr) {
          Var = nullptr;
          Relocatable = AddReloc;
          Reason = VarAssign;
          return true;
        }
      }
    }
  }
  return false;
}

inline bool matchCombinedBaseIndex(const VariablesMetadata *VMetadata,
                                   Variable *&Base, Variable *&Index,
                                   uint16_t &Shift, const Inst *&Reason) {
  // Index==nullptr && Base is Base=Var1+Var2 ==>
  //   set Base=Var1, Index=Var2, Shift=0
  if (Base == nullptr)
    return false;
  if (Index != nullptr)
    return false;
  auto *BaseInst = VMetadata->getSingleDefinition(Base);
  if (BaseInst == nullptr)
    return false;
  assert(!VMetadata->isMultiDef(Base));
  if (BaseInst->getSrcSize() < 2)
    return false;
  if (auto *Var1 = llvm::dyn_cast<Variable>(BaseInst->getSrc(0))) {
    if (VMetadata->isMultiDef(Var1))
      return false;
    if (auto *Var2 = llvm::dyn_cast<Variable>(BaseInst->getSrc(1))) {
      if (VMetadata->isMultiDef(Var2))
        return false;
      if (isAdd(BaseInst) &&
          // TODO: ensure Var1 and Var2 stay single-BB
          true) {
        Base = Var1;
        Index = Var2;
        Shift = 0; // should already have been 0
        Reason = BaseInst;
        return true;
      }
    }
  }
  return false;
}

inline bool matchShiftedIndex(const VariablesMetadata *VMetadata,
                              Variable *&Index, uint16_t &Shift,
                              const Inst *&Reason) {
  // Index is Index=Var*Const && log2(Const)+Shift<=3 ==>
  //   Index=Var, Shift+=log2(Const)
  if (Index == nullptr)
    return false;
  auto *IndexInst = VMetadata->getSingleDefinition(Index);
  if (IndexInst == nullptr)
    return false;
  assert(!VMetadata->isMultiDef(Index));
  if (IndexInst->getSrcSize() < 2)
    return false;
  if (auto *ArithInst = llvm::dyn_cast<InstArithmetic>(IndexInst)) {
    if (auto *Var = llvm::dyn_cast<Variable>(ArithInst->getSrc(0))) {
      if (auto *Const =
              llvm::dyn_cast<ConstantInteger32>(ArithInst->getSrc(1))) {
        if (VMetadata->isMultiDef(Var) || Const->getType() != IceType_i32)
          return false;
        switch (ArithInst->getOp()) {
        default:
          return false;
        case InstArithmetic::Mul: {
          uint32_t Mult = Const->getValue();
          uint32_t LogMult;
          switch (Mult) {
          case 1:
            LogMult = 0;
            break;
          case 2:
            LogMult = 1;
            break;
          case 4:
            LogMult = 2;
            break;
          case 8:
            LogMult = 3;
            break;
          default:
            return false;
          }
          if (Shift + LogMult <= 3) {
            Index = Var;
            Shift += LogMult;
            Reason = IndexInst;
            return true;
          }
        }
        case InstArithmetic::Shl: {
          uint32_t ShiftAmount = Const->getValue();
          switch (ShiftAmount) {
          case 0:
          case 1:
          case 2:
          case 3:
            break;
          default:
            return false;
          }
          if (Shift + ShiftAmount <= 3) {
            Index = Var;
            Shift += ShiftAmount;
            Reason = IndexInst;
            return true;
          }
        }
        }
      }
    }
  }
  return false;
}

inline bool matchOffsetBase(const VariablesMetadata *VMetadata, Variable *&Base,
                            ConstantRelocatable *&Relocatable, int32_t &Offset,
                            const Inst *&Reason) {
  // Base is Base=Var+Const || Base is Base=Const+Var ==>
  //   set Base=Var, Offset+=Const
  // Base is Base=Var-Const ==>
  //   set Base=Var, Offset-=Const
  if (Base == nullptr) {
    return false;
  }
  const Inst *BaseInst = VMetadata->getSingleDefinition(Base);
  if (BaseInst == nullptr) {
    return false;
  }
  assert(!VMetadata->isMultiDef(Base));
  if (auto *ArithInst = llvm::dyn_cast<const InstArithmetic>(BaseInst)) {
    if (ArithInst->getOp() != InstArithmetic::Add &&
        ArithInst->getOp() != InstArithmetic::Sub)
      return false;
    bool IsAdd = ArithInst->getOp() == InstArithmetic::Add;
    Operand *Src0 = ArithInst->getSrc(0);
    Operand *Src1 = ArithInst->getSrc(1);
    auto *Var0 = llvm::dyn_cast<Variable>(Src0);
    auto *Var1 = llvm::dyn_cast<Variable>(Src1);
    auto *Const0 = llvm::dyn_cast<ConstantInteger32>(Src0);
    auto *Const1 = llvm::dyn_cast<ConstantInteger32>(Src1);
    auto *Reloc0 = llvm::dyn_cast<ConstantRelocatable>(Src0);
    auto *Reloc1 = llvm::dyn_cast<ConstantRelocatable>(Src1);
    Variable *NewBase = nullptr;
    int32_t NewOffset = Offset;
    ConstantRelocatable *NewRelocatable = Relocatable;
    if (Var0 && Var1)
      // TODO(sehr): merge base/index splitting into here.
      return false;
    if (!IsAdd && Var1)
      return false;
    if (Var0)
      NewBase = Var0;
    else if (Var1)
      NewBase = Var1;
    // Don't know how to add/subtract two relocatables.
    if ((Relocatable && (Reloc0 || Reloc1)) || (Reloc0 && Reloc1))
      return false;
    // Don't know how to subtract a relocatable.
    if (!IsAdd && Reloc1)
      return false;
    // Incorporate ConstantRelocatables.
    if (Reloc0)
      NewRelocatable = Reloc0;
    else if (Reloc1)
      NewRelocatable = Reloc1;
    // Compute the updated constant offset.
    if (Const0) {
      int32_t MoreOffset = IsAdd ? Const0->getValue() : -Const0->getValue();
      if (Utils::WouldOverflowAdd(NewOffset, MoreOffset))
        return false;
      NewOffset += MoreOffset;
    }
    if (Const1) {
      int32_t MoreOffset = IsAdd ? Const1->getValue() : -Const1->getValue();
      if (Utils::WouldOverflowAdd(NewOffset, MoreOffset))
        return false;
      NewOffset += MoreOffset;
    }
    // Update the computed address parameters once we are sure optimization
    // is valid.
    Base = NewBase;
    Offset = NewOffset;
    Relocatable = NewRelocatable;
    Reason = BaseInst;
    return true;
  }
  return false;
}

// Builds information for a canonical address expresion:
//   <Relocatable + Offset>(Base, Index, Shift)
// On entry:
//   Relocatable == null,
//   Offset == 0,
//   Base is a Variable,
//   Index == nullptr,
//   Shift == 0
inline bool computeAddressOpt(Cfg *Func, const Inst *Instr,
                              ConstantRelocatable *&Relocatable,
                              int32_t &Offset, Variable *&Base,
                              Variable *&Index, uint16_t &Shift) {
  bool AddressWasOptimized = false;
  Func->resetCurrentNode();
  if (Func->isVerbose(IceV_AddrOpt)) {
    OstreamLocker L(Func->getContext());
    Ostream &Str = Func->getContext()->getStrDump();
    Str << "\nStarting computeAddressOpt for instruction:\n  ";
    Instr->dumpDecorated(Func);
  }
  if (Base == nullptr)
    return AddressWasOptimized;
  // If the Base has more than one use or is live across multiple blocks, then
  // don't go further. Alternatively (?), never consider a transformation that
  // would change a variable that is currently *not* live across basic block
  // boundaries into one that *is*.
  if (Func->getVMetadata()->isMultiBlock(Base) /* || Base->getUseCount() > 1*/)
    return AddressWasOptimized;

  const bool MockBounds = Func->getContext()->getFlags().getMockBoundsCheck();
  const VariablesMetadata *VMetadata = Func->getVMetadata();
  const Inst *Reason = nullptr;
  do {
    if (Reason) {
      dumpAddressOpt(Func, Relocatable, Offset, Base, Index, Shift, Reason);
      AddressWasOptimized = true;
      Reason = nullptr;
    }
    // Update Base and Index to follow through assignments to definitions.
    if (matchAssign(VMetadata, Base, Relocatable, Offset, Reason)) {
      // Assignments of Base from a Relocatable or ConstantInt32 can result
      // in Base becoming nullptr.  To avoid code duplication in this loop we
      // prefer that Base be non-nullptr if possible.
      if ((Base == nullptr) && (Index != nullptr) && Shift == 0)
        std::swap(Base, Index);
      continue;
    }
    if (matchAssign(VMetadata, Index, Relocatable, Offset, Reason))
      continue;

    if (!MockBounds) {
      // Transition from:
      //   <Relocatable + Offset>(Base) to
      //   <Relocatable + Offset>(Base, Index)
      if (matchCombinedBaseIndex(VMetadata, Base, Index, Shift, Reason))
        continue;
      // Recognize multiply/shift and update Shift amount.
      // Index becomes Index=Var<<Const && Const+Shift<=3 ==>
      //   Index=Var, Shift+=Const
      // Index becomes Index=Const*Var && log2(Const)+Shift<=3 ==>
      //   Index=Var, Shift+=log2(Const)
      if (matchShiftedIndex(VMetadata, Index, Shift, Reason))
        continue;
      // If Shift is zero, the choice of Base and Index was purely arbitrary.
      // Recognize multiply/shift and set Shift amount.
      // Shift==0 && Base is Base=Var*Const && log2(Const)+Shift<=3 ==>
      //   swap(Index,Base)
      // Similar for Base=Const*Var and Base=Var<<Const
      if (Shift == 0 && matchShiftedIndex(VMetadata, Base, Shift, Reason)) {
        std::swap(Base, Index);
        continue;
      }
    }
    // Update Offset to reflect additions/subtractions with constants and
    // relocatables.
    // TODO: consider overflow issues with respect to Offset.
    if (matchOffsetBase(VMetadata, Base, Relocatable, Offset, Reason))
      continue;
    if (Shift == 0 &&
        matchOffsetBase(VMetadata, Index, Relocatable, Offset, Reason))
      continue;
    // TODO(sehr, stichnot): Handle updates of Index with Shift != 0.
    // Index is Index=Var+Const ==>
    //   set Index=Var, Offset+=(Const<<Shift)
    // Index is Index=Const+Var ==>
    //   set Index=Var, Offset+=(Const<<Shift)
    // Index is Index=Var-Const ==>
    //   set Index=Var, Offset-=(Const<<Shift)
    break;
  } while (Reason);
  return AddressWasOptimized;
}

/// Add a mock bounds check on the memory address before using it as a load or
/// store operand.  The basic idea is that given a memory operand [reg], we
/// would first add bounds-check code something like:
///
///   cmp reg, <lb>
///   jl out_of_line_error
///   cmp reg, <ub>
///   jg out_of_line_error
///
/// In reality, the specific code will depend on how <lb> and <ub> are
/// represented, e.g. an immediate, a global, or a function argument.
///
/// As such, we need to enforce that the memory operand does not have the form
/// [reg1+reg2], because then there is no simple cmp instruction that would
/// suffice.  However, we consider [reg+offset] to be OK because the offset is
/// usually small, and so <ub> could have a safety buffer built in and then we
/// could instead branch to a custom out_of_line_error that does the precise
/// check and jumps back if it turns out OK.
///
/// For the purpose of mocking the bounds check, we'll do something like this:
///
///   cmp reg, 0
///   je label
///   cmp reg, 1
///   je label
///   label:
///
/// Also note that we don't need to add a bounds check to a dereference of a
/// simple global variable address.
template <class Machine>
void TargetX86Base<Machine>::doMockBoundsCheck(Operand *Opnd) {
  if (!Ctx->getFlags().getMockBoundsCheck())
    return;
  if (auto *Mem = llvm::dyn_cast<typename Traits::X86OperandMem>(Opnd)) {
    if (Mem->getIndex()) {
      llvm::report_fatal_error("doMockBoundsCheck: Opnd contains index reg");
    }
    Opnd = Mem->getBase();
  }
  // At this point Opnd could be nullptr, or Variable, or Constant, or perhaps
  // something else.  We only care if it is Variable.
  auto *Var = llvm::dyn_cast_or_null<Variable>(Opnd);
  if (Var == nullptr)
    return;
  // We use lowerStore() to copy out-args onto the stack.  This creates a memory
  // operand with the stack pointer as the base register.  Don't do bounds
  // checks on that.
  if (Var->getRegNum() == Traits::RegisterSet::Reg_esp)
    return;

  typename Traits::Insts::Label *Label =
      Traits::Insts::Label::create(Func, this);
  _cmp(Opnd, Ctx->getConstantZero(IceType_i32));
  _br(Traits::Cond::Br_e, Label);
  _cmp(Opnd, Ctx->getConstantInt32(1));
  _br(Traits::Cond::Br_e, Label);
  Context.insert(Label);
}

template <class Machine>
void TargetX86Base<Machine>::lowerLoad(const InstLoad *Load) {
  // A Load instruction can be treated the same as an Assign instruction, after
  // the source operand is transformed into an Traits::X86OperandMem operand.
  // Note that the address mode optimization already creates an
  // Traits::X86OperandMem operand, so it doesn't need another level of
  // transformation.
  Variable *DestLoad = Load->getDest();
  Type Ty = DestLoad->getType();
  Operand *Src0 = formMemoryOperand(Load->getSourceAddress(), Ty);
  doMockBoundsCheck(Src0);
  InstAssign *Assign = InstAssign::create(Func, DestLoad, Src0);
  lowerAssign(Assign);
}

template <class Machine> void TargetX86Base<Machine>::doAddressOptLoad() {
  Inst *Inst = Context.getCur();
  Variable *Dest = Inst->getDest();
  Operand *Addr = Inst->getSrc(0);
  Variable *Index = nullptr;
  ConstantRelocatable *Relocatable = nullptr;
  uint16_t Shift = 0;
  int32_t Offset = 0;
  // Vanilla ICE load instructions should not use the segment registers, and
  // computeAddressOpt only works at the level of Variables and Constants, not
  // other Traits::X86OperandMem, so there should be no mention of segment
  // registers there either.
  const typename Traits::X86OperandMem::SegmentRegisters SegmentReg =
      Traits::X86OperandMem::DefaultSegment;
  auto *Base = llvm::dyn_cast<Variable>(Addr);
  if (computeAddressOpt(Func, Inst, Relocatable, Offset, Base, Index, Shift)) {
    Inst->setDeleted();
    Constant *OffsetOp = nullptr;
    if (Relocatable == nullptr) {
      OffsetOp = Ctx->getConstantInt32(Offset);
    } else {
      OffsetOp = Ctx->getConstantSym(Relocatable->getOffset() + Offset,
                                     Relocatable->getName(),
                                     Relocatable->getSuppressMangling());
    }
    Addr = Traits::X86OperandMem::create(Func, Dest->getType(), Base, OffsetOp,
                                         Index, Shift, SegmentReg);
    Context.insert(InstLoad::create(Func, Dest, Addr));
  }
}

template <class Machine>
void TargetX86Base<Machine>::randomlyInsertNop(float Probability,
                                               RandomNumberGenerator &RNG) {
  RandomNumberGeneratorWrapper RNGW(RNG);
  if (RNGW.getTrueWithProbability(Probability)) {
    _nop(RNGW(Traits::X86_NUM_NOP_VARIANTS));
  }
}

template <class Machine>
void TargetX86Base<Machine>::lowerPhi(const InstPhi * /*Inst*/) {
  Func->setError("Phi found in regular instruction list");
}

template <class Machine>
void TargetX86Base<Machine>::lowerSelect(const InstSelect *Inst) {
  Variable *Dest = Inst->getDest();
  Type DestTy = Dest->getType();
  Operand *SrcT = Inst->getTrueOperand();
  Operand *SrcF = Inst->getFalseOperand();
  Operand *Condition = Inst->getCondition();

  if (isVectorType(DestTy)) {
    Type SrcTy = SrcT->getType();
    Variable *T = makeReg(SrcTy);
    Operand *SrcTRM = legalize(SrcT, Legal_Reg | Legal_Mem);
    Operand *SrcFRM = legalize(SrcF, Legal_Reg | Legal_Mem);
    if (InstructionSet >= Traits::SSE4_1) {
      // TODO(wala): If the condition operand is a constant, use blendps or
      // pblendw.
      //
      // Use blendvps or pblendvb to implement select.
      if (SrcTy == IceType_v4i1 || SrcTy == IceType_v4i32 ||
          SrcTy == IceType_v4f32) {
        Operand *ConditionRM = legalize(Condition, Legal_Reg | Legal_Mem);
        Variable *xmm0 = makeReg(IceType_v4i32, Traits::RegisterSet::Reg_xmm0);
        _movp(xmm0, ConditionRM);
        _psll(xmm0, Ctx->getConstantInt8(31));
        _movp(T, SrcFRM);
        _blendvps(T, SrcTRM, xmm0);
        _movp(Dest, T);
      } else {
        assert(typeNumElements(SrcTy) == 8 || typeNumElements(SrcTy) == 16);
        Type SignExtTy = Condition->getType() == IceType_v8i1 ? IceType_v8i16
                                                              : IceType_v16i8;
        Variable *xmm0 = makeReg(SignExtTy, Traits::RegisterSet::Reg_xmm0);
        lowerCast(InstCast::create(Func, InstCast::Sext, xmm0, Condition));
        _movp(T, SrcFRM);
        _pblendvb(T, SrcTRM, xmm0);
        _movp(Dest, T);
      }
      return;
    }
    // Lower select without Traits::SSE4.1:
    // a=d?b:c ==>
    //   if elementtype(d) != i1:
    //      d=sext(d);
    //   a=(b&d)|(c&~d);
    Variable *T2 = makeReg(SrcTy);
    // Sign extend the condition operand if applicable.
    if (SrcTy == IceType_v4f32) {
      // The sext operation takes only integer arguments.
      Variable *T3 = Func->makeVariable(IceType_v4i32);
      lowerCast(InstCast::create(Func, InstCast::Sext, T3, Condition));
      _movp(T, T3);
    } else if (typeElementType(SrcTy) != IceType_i1) {
      lowerCast(InstCast::create(Func, InstCast::Sext, T, Condition));
    } else {
      Operand *ConditionRM = legalize(Condition, Legal_Reg | Legal_Mem);
      _movp(T, ConditionRM);
    }
    _movp(T2, T);
    _pand(T, SrcTRM);
    _pandn(T2, SrcFRM);
    _por(T, T2);
    _movp(Dest, T);

    return;
  }

  typename Traits::Cond::BrCond Cond = Traits::Cond::Br_ne;
  Operand *CmpOpnd0 = nullptr;
  Operand *CmpOpnd1 = nullptr;
  // Handle folding opportunities.
  if (const class Inst *Producer = FoldingInfo.getProducerFor(Condition)) {
    assert(Producer->isDeleted());
    switch (BoolFolding::getProducerKind(Producer)) {
    default:
      break;
    case BoolFolding::PK_Icmp32: {
      auto *Cmp = llvm::dyn_cast<InstIcmp>(Producer);
      Cond = Traits::getIcmp32Mapping(Cmp->getCondition());
      CmpOpnd1 = legalize(Producer->getSrc(1));
      CmpOpnd0 = legalizeSrc0ForCmp(Producer->getSrc(0), CmpOpnd1);
    } break;
    }
  }
  if (CmpOpnd0 == nullptr) {
    CmpOpnd0 = legalize(Condition, Legal_Reg | Legal_Mem);
    CmpOpnd1 = Ctx->getConstantZero(IceType_i32);
  }
  assert(CmpOpnd0);
  assert(CmpOpnd1);

  _cmp(CmpOpnd0, CmpOpnd1);
  if (typeWidthInBytes(DestTy) == 1 || isFloatingType(DestTy)) {
    // The cmov instruction doesn't allow 8-bit or FP operands, so we need
    // explicit control flow.
    // d=cmp e,f; a=d?b:c ==> cmp e,f; a=b; jne L1; a=c; L1:
    typename Traits::Insts::Label *Label =
        Traits::Insts::Label::create(Func, this);
    SrcT = legalize(SrcT, Legal_Reg | Legal_Imm);
    _mov(Dest, SrcT);
    _br(Cond, Label);
    SrcF = legalize(SrcF, Legal_Reg | Legal_Imm);
    _mov_redefined(Dest, SrcF);
    Context.insert(Label);
    return;
  }
  // mov t, SrcF; cmov_cond t, SrcT; mov dest, t
  // But if SrcT is immediate, we might be able to do better, as the cmov
  // instruction doesn't allow an immediate operand:
  // mov t, SrcT; cmov_!cond t, SrcF; mov dest, t
  if (llvm::isa<Constant>(SrcT) && !llvm::isa<Constant>(SrcF)) {
    std::swap(SrcT, SrcF);
    Cond = InstX86Base<Machine>::getOppositeCondition(Cond);
  }
  if (!Traits::Is64Bit && DestTy == IceType_i64) {
    SrcT = legalizeUndef(SrcT);
    SrcF = legalizeUndef(SrcF);
    // Set the low portion.
    Variable *DestLo = llvm::cast<Variable>(loOperand(Dest));
    Variable *TLo = nullptr;
    Operand *SrcFLo = legalize(loOperand(SrcF));
    _mov(TLo, SrcFLo);
    Operand *SrcTLo = legalize(loOperand(SrcT), Legal_Reg | Legal_Mem);
    _cmov(TLo, SrcTLo, Cond);
    _mov(DestLo, TLo);
    // Set the high portion.
    Variable *DestHi = llvm::cast<Variable>(hiOperand(Dest));
    Variable *THi = nullptr;
    Operand *SrcFHi = legalize(hiOperand(SrcF));
    _mov(THi, SrcFHi);
    Operand *SrcTHi = legalize(hiOperand(SrcT), Legal_Reg | Legal_Mem);
    _cmov(THi, SrcTHi, Cond);
    _mov(DestHi, THi);
    return;
  }

  assert(DestTy == IceType_i16 || DestTy == IceType_i32 ||
         (Traits::Is64Bit && DestTy == IceType_i64));
  Variable *T = nullptr;
  SrcF = legalize(SrcF);
  _mov(T, SrcF);
  SrcT = legalize(SrcT, Legal_Reg | Legal_Mem);
  _cmov(T, SrcT, Cond);
  _mov(Dest, T);
}

template <class Machine>
void TargetX86Base<Machine>::lowerStore(const InstStore *Inst) {
  Operand *Value = Inst->getData();
  Operand *Addr = Inst->getAddr();
  typename Traits::X86OperandMem *NewAddr =
      formMemoryOperand(Addr, Value->getType());
  doMockBoundsCheck(NewAddr);
  Type Ty = NewAddr->getType();

  if (!Traits::Is64Bit && Ty == IceType_i64) {
    Value = legalizeUndef(Value);
    Operand *ValueHi = legalize(hiOperand(Value), Legal_Reg | Legal_Imm);
    Operand *ValueLo = legalize(loOperand(Value), Legal_Reg | Legal_Imm);
    _store(ValueHi,
           llvm::cast<typename Traits::X86OperandMem>(hiOperand(NewAddr)));
    _store(ValueLo,
           llvm::cast<typename Traits::X86OperandMem>(loOperand(NewAddr)));
  } else if (isVectorType(Ty)) {
    _storep(legalizeToReg(Value), NewAddr);
  } else {
    Value = legalize(Value, Legal_Reg | Legal_Imm);
    _store(Value, NewAddr);
  }
}

template <class Machine> void TargetX86Base<Machine>::doAddressOptStore() {
  InstStore *Inst = llvm::cast<InstStore>(Context.getCur());
  Operand *Data = Inst->getData();
  Operand *Addr = Inst->getAddr();
  Variable *Index = nullptr;
  ConstantRelocatable *Relocatable = nullptr;
  uint16_t Shift = 0;
  int32_t Offset = 0;
  auto *Base = llvm::dyn_cast<Variable>(Addr);
  // Vanilla ICE store instructions should not use the segment registers, and
  // computeAddressOpt only works at the level of Variables and Constants, not
  // other Traits::X86OperandMem, so there should be no mention of segment
  // registers there either.
  const typename Traits::X86OperandMem::SegmentRegisters SegmentReg =
      Traits::X86OperandMem::DefaultSegment;
  if (computeAddressOpt(Func, Inst, Relocatable, Offset, Base, Index, Shift)) {
    Inst->setDeleted();
    Constant *OffsetOp = nullptr;
    if (Relocatable == nullptr) {
      OffsetOp = Ctx->getConstantInt32(Offset);
    } else {
      OffsetOp = Ctx->getConstantSym(Relocatable->getOffset() + Offset,
                                     Relocatable->getName(),
                                     Relocatable->getSuppressMangling());
    }
    Addr = Traits::X86OperandMem::create(Func, Data->getType(), Base, OffsetOp,
                                         Index, Shift, SegmentReg);
    InstStore *NewStore = InstStore::create(Func, Data, Addr);
    if (Inst->getDest())
      NewStore->setRmwBeacon(Inst->getRmwBeacon());
    Context.insert(NewStore);
  }
}

template <class Machine>
Operand *TargetX86Base<Machine>::lowerCmpRange(Operand *Comparison,
                                               uint64_t Min, uint64_t Max) {
  // TODO(ascull): 64-bit should not reach here but only because it is not
  // implemented yet. This should be able to handle the 64-bit case.
  assert(Traits::Is64Bit || Comparison->getType() != IceType_i64);
  // Subtracting 0 is a nop so don't do it
  if (Min != 0) {
    // Avoid clobbering the comparison by copying it
    Variable *T = nullptr;
    _mov(T, Comparison);
    _sub(T, Ctx->getConstantInt32(Min));
    Comparison = T;
  }

  _cmp(Comparison, Ctx->getConstantInt32(Max - Min));

  return Comparison;
}

template <class Machine>
void TargetX86Base<Machine>::lowerCaseCluster(const CaseCluster &Case,
                                              Operand *Comparison, bool DoneCmp,
                                              CfgNode *DefaultTarget) {
  switch (Case.getKind()) {
  case CaseCluster::JumpTable: {
    typename Traits::Insts::Label *SkipJumpTable;

    Operand *RangeIndex =
        lowerCmpRange(Comparison, Case.getLow(), Case.getHigh());
    if (DefaultTarget == nullptr) {
      // Skip over jump table logic if comparison not in range and no default
      SkipJumpTable = Traits::Insts::Label::create(Func, this);
      _br(Traits::Cond::Br_a, SkipJumpTable);
    } else {
      _br(Traits::Cond::Br_a, DefaultTarget);
    }

    InstJumpTable *JumpTable = Case.getJumpTable();
    Context.insert(JumpTable);

    // Make sure the index is a register of the same width as the base
    Variable *Index;
    if (RangeIndex->getType() != getPointerType()) {
      Index = makeReg(getPointerType());
      _movzx(Index, RangeIndex);
    } else {
      Index = legalizeToReg(RangeIndex);
    }

    constexpr RelocOffsetT RelocOffset = 0;
    constexpr bool SuppressMangling = true;
    IceString MangledName = Ctx->mangleName(Func->getFunctionName());
    Constant *Base = Ctx->getConstantSym(
        RelocOffset, InstJumpTable::makeName(MangledName, JumpTable->getId()),
        SuppressMangling);
    Constant *Offset = nullptr;
    uint16_t Shift = typeWidthInBytesLog2(getPointerType());
    // TODO(ascull): remove need for legalize by allowing null base in memop
    auto *TargetInMemory = Traits::X86OperandMem::create(
        Func, getPointerType(), legalizeToReg(Base), Offset, Index, Shift);
    Variable *Target = nullptr;
    _mov(Target, TargetInMemory);
    lowerIndirectJump(Target);

    if (DefaultTarget == nullptr)
      Context.insert(SkipJumpTable);
    return;
  }
  case CaseCluster::Range: {
    if (Case.isUnitRange()) {
      // Single item
      if (!DoneCmp) {
        Constant *Value = Ctx->getConstantInt32(Case.getLow());
        _cmp(Comparison, Value);
      }
      _br(Traits::Cond::Br_e, Case.getTarget());
    } else if (DoneCmp && Case.isPairRange()) {
      // Range of two items with first item aleady compared against
      _br(Traits::Cond::Br_e, Case.getTarget());
      Constant *Value = Ctx->getConstantInt32(Case.getHigh());
      _cmp(Comparison, Value);
      _br(Traits::Cond::Br_e, Case.getTarget());
    } else {
      // Range
      lowerCmpRange(Comparison, Case.getLow(), Case.getHigh());
      _br(Traits::Cond::Br_be, Case.getTarget());
    }
    if (DefaultTarget != nullptr)
      _br(DefaultTarget);
    return;
  }
  }
}

template <class Machine>
void TargetX86Base<Machine>::lowerSwitch(const InstSwitch *Inst) {
  // Group cases together and navigate through them with a binary search
  CaseClusterArray CaseClusters = CaseCluster::clusterizeSwitch(Func, Inst);
  Operand *Src0 = Inst->getComparison();
  CfgNode *DefaultTarget = Inst->getLabelDefault();

  assert(CaseClusters.size() != 0); // Should always be at least one

  if (!Traits::Is64Bit && Src0->getType() == IceType_i64) {
    Src0 = legalize(Src0); // get Base/Index into physical registers
    Operand *Src0Lo = loOperand(Src0);
    Operand *Src0Hi = hiOperand(Src0);
    if (CaseClusters.back().getHigh() > UINT32_MAX) {
      // TODO(ascull): handle 64-bit case properly (currently naive version)
      // This might be handled by a higher level lowering of switches.
      SizeT NumCases = Inst->getNumCases();
      if (NumCases >= 2) {
        Src0Lo = legalizeToReg(Src0Lo);
        Src0Hi = legalizeToReg(Src0Hi);
      } else {
        Src0Lo = legalize(Src0Lo, Legal_Reg | Legal_Mem);
        Src0Hi = legalize(Src0Hi, Legal_Reg | Legal_Mem);
      }
      for (SizeT I = 0; I < NumCases; ++I) {
        Constant *ValueLo = Ctx->getConstantInt32(Inst->getValue(I));
        Constant *ValueHi = Ctx->getConstantInt32(Inst->getValue(I) >> 32);
        typename Traits::Insts::Label *Label =
            Traits::Insts::Label::create(Func, this);
        _cmp(Src0Lo, ValueLo);
        _br(Traits::Cond::Br_ne, Label);
        _cmp(Src0Hi, ValueHi);
        _br(Traits::Cond::Br_e, Inst->getLabel(I));
        Context.insert(Label);
      }
      _br(Inst->getLabelDefault());
      return;
    } else {
      // All the values are 32-bit so just check the operand is too and then
      // fall through to the 32-bit implementation. This is a common case.
      Src0Hi = legalize(Src0Hi, Legal_Reg | Legal_Mem);
      Constant *Zero = Ctx->getConstantInt32(0);
      _cmp(Src0Hi, Zero);
      _br(Traits::Cond::Br_ne, DefaultTarget);
      Src0 = Src0Lo;
    }
  }

  // 32-bit lowering

  if (CaseClusters.size() == 1) {
    // Jump straight to default if needed. Currently a common case as jump
    // tables occur on their own.
    constexpr bool DoneCmp = false;
    lowerCaseCluster(CaseClusters.front(), Src0, DoneCmp, DefaultTarget);
    return;
  }

  // Going to be using multiple times so get it in a register early
  Variable *Comparison = legalizeToReg(Src0);

  // A span is over the clusters
  struct SearchSpan {
    SearchSpan(SizeT Begin, SizeT Size, typename Traits::Insts::Label *Label)
        : Begin(Begin), Size(Size), Label(Label) {}

    SizeT Begin;
    SizeT Size;
    typename Traits::Insts::Label *Label;
  };
  // The stack will only grow to the height of the tree so 12 should be plenty
  std::stack<SearchSpan, llvm::SmallVector<SearchSpan, 12>> SearchSpanStack;
  SearchSpanStack.emplace(0, CaseClusters.size(), nullptr);
  bool DoneCmp = false;

  while (!SearchSpanStack.empty()) {
    SearchSpan Span = SearchSpanStack.top();
    SearchSpanStack.pop();

    if (Span.Label != nullptr)
      Context.insert(Span.Label);

    switch (Span.Size) {
    case 0:
      llvm::report_fatal_error("Invalid SearchSpan size");
      break;

    case 1:
      lowerCaseCluster(CaseClusters[Span.Begin], Comparison, DoneCmp,
                       SearchSpanStack.empty() ? nullptr : DefaultTarget);
      DoneCmp = false;
      break;

    case 2: {
      const CaseCluster *CaseA = &CaseClusters[Span.Begin];
      const CaseCluster *CaseB = &CaseClusters[Span.Begin + 1];

      // Placing a range last may allow register clobbering during the range
      // test. That means there is no need to clone the register. If it is a
      // unit range the comparison may have already been done in the binary
      // search (DoneCmp) and so it should be placed first. If this is a range
      // of two items and the comparison with the low value has already been
      // done, comparing with the other element is cheaper than a range test.
      // If the low end of the range is zero then there is no subtraction and
      // nothing to be gained.
      if (!CaseA->isUnitRange() &&
          !(CaseA->getLow() == 0 || (DoneCmp && CaseA->isPairRange()))) {
        std::swap(CaseA, CaseB);
        DoneCmp = false;
      }

      lowerCaseCluster(*CaseA, Comparison, DoneCmp);
      DoneCmp = false;
      lowerCaseCluster(*CaseB, Comparison, DoneCmp,
                       SearchSpanStack.empty() ? nullptr : DefaultTarget);
    } break;

    default:
      // Pick the middle item and branch b or ae
      SizeT PivotIndex = Span.Begin + (Span.Size / 2);
      const CaseCluster &Pivot = CaseClusters[PivotIndex];
      Constant *Value = Ctx->getConstantInt32(Pivot.getLow());
      typename Traits::Insts::Label *Label =
          Traits::Insts::Label::create(Func, this);
      _cmp(Comparison, Value);
      // TODO(ascull): does it alway have to be far?
      _br(Traits::Cond::Br_b, Label, Traits::Insts::Br::Far);
      // Lower the left and (pivot+right) sides, falling through to the right
      SearchSpanStack.emplace(Span.Begin, Span.Size / 2, Label);
      SearchSpanStack.emplace(PivotIndex, Span.Size - (Span.Size / 2), nullptr);
      DoneCmp = true;
      break;
    }
  }

  _br(DefaultTarget);
}

template <class Machine>
void TargetX86Base<Machine>::scalarizeArithmetic(InstArithmetic::OpKind Kind,
                                                 Variable *Dest, Operand *Src0,
                                                 Operand *Src1) {
  assert(isVectorType(Dest->getType()));
  Type Ty = Dest->getType();
  Type ElementTy = typeElementType(Ty);
  SizeT NumElements = typeNumElements(Ty);

  Operand *T = Ctx->getConstantUndef(Ty);
  for (SizeT I = 0; I < NumElements; ++I) {
    Constant *Index = Ctx->getConstantInt32(I);

    // Extract the next two inputs.
    Variable *Op0 = Func->makeVariable(ElementTy);
    Context.insert(InstExtractElement::create(Func, Op0, Src0, Index));
    Variable *Op1 = Func->makeVariable(ElementTy);
    Context.insert(InstExtractElement::create(Func, Op1, Src1, Index));

    // Perform the arithmetic as a scalar operation.
    Variable *Res = Func->makeVariable(ElementTy);
    auto *Arith = InstArithmetic::create(Func, Kind, Res, Op0, Op1);
    Context.insert(Arith);
    // We might have created an operation that needed a helper call.
    genTargetHelperCallFor(Arith);

    // Insert the result into position.
    Variable *DestT = Func->makeVariable(Ty);
    Context.insert(InstInsertElement::create(Func, DestT, T, Res, Index));
    T = DestT;
  }

  Context.insert(InstAssign::create(Func, Dest, T));
}

/// The following pattern occurs often in lowered C and C++ code:
///
///   %cmp     = fcmp/icmp pred <n x ty> %src0, %src1
///   %cmp.ext = sext <n x i1> %cmp to <n x ty>
///
/// We can eliminate the sext operation by copying the result of pcmpeqd,
/// pcmpgtd, or cmpps (which produce sign extended results) to the result of the
/// sext operation.
template <class Machine>
void TargetX86Base<Machine>::eliminateNextVectorSextInstruction(
    Variable *SignExtendedResult) {
  if (InstCast *NextCast =
          llvm::dyn_cast_or_null<InstCast>(Context.getNextInst())) {
    if (NextCast->getCastKind() == InstCast::Sext &&
        NextCast->getSrc(0) == SignExtendedResult) {
      NextCast->setDeleted();
      _movp(NextCast->getDest(), legalizeToReg(SignExtendedResult));
      // Skip over the instruction.
      Context.advanceNext();
    }
  }
}

template <class Machine>
void TargetX86Base<Machine>::lowerUnreachable(
    const InstUnreachable * /*Inst*/) {
  _ud2();
  // Add a fake use of esp to make sure esp adjustments after the unreachable
  // do not get dead-code eliminated.
  keepEspLiveAtExit();
}

template <class Machine>
void TargetX86Base<Machine>::lowerRMW(
    const typename Traits::Insts::FakeRMW *RMW) {
  // If the beacon variable's live range does not end in this instruction, then
  // it must end in the modified Store instruction that follows. This means
  // that the original Store instruction is still there, either because the
  // value being stored is used beyond the Store instruction, or because dead
  // code elimination did not happen. In either case, we cancel RMW lowering
  // (and the caller deletes the RMW instruction).
  if (!RMW->isLastUse(RMW->getBeacon()))
    return;
  Operand *Src = RMW->getData();
  Type Ty = Src->getType();
  typename Traits::X86OperandMem *Addr = formMemoryOperand(RMW->getAddr(), Ty);
  doMockBoundsCheck(Addr);
  if (!Traits::Is64Bit && Ty == IceType_i64) {
    Src = legalizeUndef(Src);
    Operand *SrcLo = legalize(loOperand(Src), Legal_Reg | Legal_Imm);
    Operand *SrcHi = legalize(hiOperand(Src), Legal_Reg | Legal_Imm);
    typename Traits::X86OperandMem *AddrLo =
        llvm::cast<typename Traits::X86OperandMem>(loOperand(Addr));
    typename Traits::X86OperandMem *AddrHi =
        llvm::cast<typename Traits::X86OperandMem>(hiOperand(Addr));
    switch (RMW->getOp()) {
    default:
      // TODO(stichnot): Implement other arithmetic operators.
      break;
    case InstArithmetic::Add:
      _add_rmw(AddrLo, SrcLo);
      _adc_rmw(AddrHi, SrcHi);
      return;
    case InstArithmetic::Sub:
      _sub_rmw(AddrLo, SrcLo);
      _sbb_rmw(AddrHi, SrcHi);
      return;
    case InstArithmetic::And:
      _and_rmw(AddrLo, SrcLo);
      _and_rmw(AddrHi, SrcHi);
      return;
    case InstArithmetic::Or:
      _or_rmw(AddrLo, SrcLo);
      _or_rmw(AddrHi, SrcHi);
      return;
    case InstArithmetic::Xor:
      _xor_rmw(AddrLo, SrcLo);
      _xor_rmw(AddrHi, SrcHi);
      return;
    }
  } else {
    // x86-32: i8, i16, i32
    // x86-64: i8, i16, i32, i64
    switch (RMW->getOp()) {
    default:
      // TODO(stichnot): Implement other arithmetic operators.
      break;
    case InstArithmetic::Add:
      Src = legalize(Src, Legal_Reg | Legal_Imm);
      _add_rmw(Addr, Src);
      return;
    case InstArithmetic::Sub:
      Src = legalize(Src, Legal_Reg | Legal_Imm);
      _sub_rmw(Addr, Src);
      return;
    case InstArithmetic::And:
      Src = legalize(Src, Legal_Reg | Legal_Imm);
      _and_rmw(Addr, Src);
      return;
    case InstArithmetic::Or:
      Src = legalize(Src, Legal_Reg | Legal_Imm);
      _or_rmw(Addr, Src);
      return;
    case InstArithmetic::Xor:
      Src = legalize(Src, Legal_Reg | Legal_Imm);
      _xor_rmw(Addr, Src);
      return;
    }
  }
  llvm::report_fatal_error("Couldn't lower RMW instruction");
}

template <class Machine>
void TargetX86Base<Machine>::lowerOther(const Inst *Instr) {
  if (const auto *RMW =
          llvm::dyn_cast<typename Traits::Insts::FakeRMW>(Instr)) {
    lowerRMW(RMW);
  } else {
    TargetLowering::lowerOther(Instr);
  }
}

/// Turn an i64 Phi instruction into a pair of i32 Phi instructions, to preserve
/// integrity of liveness analysis. Undef values are also turned into zeroes,
/// since loOperand() and hiOperand() don't expect Undef input.
template <class Machine> void TargetX86Base<Machine>::prelowerPhis() {
  if (Traits::Is64Bit) {
    // On x86-64 we don't need to prelower phis -- the architecture can handle
    // 64-bit integer natively.
    return;
  }

  // Pause constant blinding or pooling, blinding or pooling will be done later
  // during phi lowering assignments
  BoolFlagSaver B(RandomizationPoolingPaused, true);
  PhiLowering::prelowerPhis32Bit<TargetX86Base<Machine>>(
      this, Context.getNode(), Func);
}

template <class Machine>
void TargetX86Base<Machine>::genTargetHelperCallFor(Inst *Instr) {
  uint32_t StackArgumentsSize = 0;
  if (auto *Arith = llvm::dyn_cast<InstArithmetic>(Instr)) {
    const char *HelperName = nullptr;
    Variable *Dest = Arith->getDest();
    Type DestTy = Dest->getType();
    if (!Traits::Is64Bit && DestTy == IceType_i64) {
      switch (Arith->getOp()) {
      default:
        return;
      case InstArithmetic::Udiv:
        HelperName = H_udiv_i64;
        break;
      case InstArithmetic::Sdiv:
        HelperName = H_sdiv_i64;
        break;
      case InstArithmetic::Urem:
        HelperName = H_urem_i64;
        break;
      case InstArithmetic::Srem:
        HelperName = H_srem_i64;
        break;
      }
    } else if (isVectorType(DestTy)) {
      Variable *Dest = Arith->getDest();
      Operand *Src0 = Arith->getSrc(0);
      Operand *Src1 = Arith->getSrc(1);
      switch (Arith->getOp()) {
      default:
        return;
      case InstArithmetic::Mul:
        if (DestTy == IceType_v16i8) {
          scalarizeArithmetic(Arith->getOp(), Dest, Src0, Src1);
          Arith->setDeleted();
        }
        return;
      case InstArithmetic::Shl:
      case InstArithmetic::Lshr:
      case InstArithmetic::Ashr:
      case InstArithmetic::Udiv:
      case InstArithmetic::Urem:
      case InstArithmetic::Sdiv:
      case InstArithmetic::Srem:
      case InstArithmetic::Frem:
        scalarizeArithmetic(Arith->getOp(), Dest, Src0, Src1);
        Arith->setDeleted();
        return;
      }
    } else {
      switch (Arith->getOp()) {
      default:
        return;
      case InstArithmetic::Frem:
        if (isFloat32Asserting32Or64(DestTy))
          HelperName = H_frem_f32;
        else
          HelperName = H_frem_f64;
      }
    }
    constexpr SizeT MaxSrcs = 2;
    InstCall *Call = makeHelperCall(HelperName, Dest, MaxSrcs);
    Call->addArg(Arith->getSrc(0));
    Call->addArg(Arith->getSrc(1));
    StackArgumentsSize = getCallStackArgumentsSizeBytes(Call);
    Context.insert(Call);
    Arith->setDeleted();
  } else if (auto *Cast = llvm::dyn_cast<InstCast>(Instr)) {
    InstCast::OpKind CastKind = Cast->getCastKind();
    Operand *Src0 = Cast->getSrc(0);
    const Type SrcType = Src0->getType();
    Variable *Dest = Cast->getDest();
    const Type DestTy = Dest->getType();
    const char *HelperName = nullptr;
    switch (CastKind) {
    default:
      return;
    case InstCast::Fptosi:
      if (!Traits::Is64Bit && DestTy == IceType_i64) {
        HelperName = isFloat32Asserting32Or64(SrcType) ? H_fptosi_f32_i64
                                                       : H_fptosi_f64_i64;
      } else {
        return;
      }
      break;
    case InstCast::Fptoui:
      if (isVectorType(DestTy)) {
        assert(DestTy == IceType_v4i32 && SrcType == IceType_v4f32);
        HelperName = H_fptoui_4xi32_f32;
      } else if (DestTy == IceType_i64 ||
                 (!Traits::Is64Bit && DestTy == IceType_i32)) {
        if (Traits::Is64Bit) {
          HelperName = isFloat32Asserting32Or64(SrcType) ? H_fptoui_f32_i64
                                                         : H_fptoui_f64_i64;
        } else if (isInt32Asserting32Or64(DestTy)) {
          HelperName = isFloat32Asserting32Or64(SrcType) ? H_fptoui_f32_i32
                                                         : H_fptoui_f64_i32;
        } else {
          HelperName = isFloat32Asserting32Or64(SrcType) ? H_fptoui_f32_i64
                                                         : H_fptoui_f64_i64;
        }
      } else {
        return;
      }
      break;
    case InstCast::Sitofp:
      if (!Traits::Is64Bit && SrcType == IceType_i64) {
        HelperName = isFloat32Asserting32Or64(DestTy) ? H_sitofp_i64_f32
                                                      : H_sitofp_i64_f64;
      } else {
        return;
      }
      break;
    case InstCast::Uitofp:
      if (isVectorType(SrcType)) {
        assert(DestTy == IceType_v4f32 && SrcType == IceType_v4i32);
        HelperName = H_uitofp_4xi32_4xf32;
      } else if (SrcType == IceType_i64 ||
                 (!Traits::Is64Bit && SrcType == IceType_i32)) {
        if (isInt32Asserting32Or64(SrcType)) {
          HelperName = isFloat32Asserting32Or64(DestTy) ? H_uitofp_i32_f32
                                                        : H_uitofp_i32_f64;
        } else {
          HelperName = isFloat32Asserting32Or64(DestTy) ? H_uitofp_i64_f32
                                                        : H_uitofp_i64_f64;
        }
      } else {
        return;
      }
      break;
    case InstCast::Bitcast: {
      if (DestTy == Src0->getType())
        return;
      switch (DestTy) {
      default:
        return;
      case IceType_i8:
        assert(Src0->getType() == IceType_v8i1);
        HelperName = H_bitcast_8xi1_i8;
        break;
      case IceType_i16:
        assert(Src0->getType() == IceType_v16i1);
        HelperName = H_bitcast_16xi1_i16;
        break;
      case IceType_v8i1: {
        assert(Src0->getType() == IceType_i8);
        HelperName = H_bitcast_i8_8xi1;
        Variable *Src0AsI32 = Func->makeVariable(stackSlotType());
        // Arguments to functions are required to be at least 32 bits wide.
        Context.insert(InstCast::create(Func, InstCast::Zext, Src0AsI32, Src0));
        Src0 = Src0AsI32;
      } break;
      case IceType_v16i1: {
        assert(Src0->getType() == IceType_i16);
        HelperName = H_bitcast_i16_16xi1;
        Variable *Src0AsI32 = Func->makeVariable(stackSlotType());
        // Arguments to functions are required to be at least 32 bits wide.
        Context.insert(InstCast::create(Func, InstCast::Zext, Src0AsI32, Src0));
        Src0 = Src0AsI32;
      } break;
      }
    } break;
    }
    constexpr SizeT MaxSrcs = 1;
    InstCall *Call = makeHelperCall(HelperName, Dest, MaxSrcs);
    Call->addArg(Src0);
    StackArgumentsSize = getCallStackArgumentsSizeBytes(Call);
    Context.insert(Call);
    Cast->setDeleted();
  } else if (auto *Intrinsic = llvm::dyn_cast<InstIntrinsicCall>(Instr)) {
    std::vector<Type> ArgTypes;
    Type ReturnType = IceType_void;
    switch (Intrinsics::IntrinsicID ID = Intrinsic->getIntrinsicInfo().ID) {
    default:
      return;
    case Intrinsics::Ctpop: {
      Operand *Val = Intrinsic->getArg(0);
      Type ValTy = Val->getType();
      if (ValTy == IceType_i64)
        ArgTypes = {IceType_i64};
      else
        ArgTypes = {IceType_i32};
      ReturnType = IceType_i32;
    } break;
    case Intrinsics::Longjmp:
      ArgTypes = {IceType_i32, IceType_i32};
      ReturnType = IceType_void;
      break;
    case Intrinsics::Memcpy:
      ArgTypes = {IceType_i32, IceType_i32, IceType_i32};
      ReturnType = IceType_void;
      break;
    case Intrinsics::Memmove:
      ArgTypes = {IceType_i32, IceType_i32, IceType_i32};
      ReturnType = IceType_void;
      break;
    case Intrinsics::Memset:
      ArgTypes = {IceType_i32, IceType_i32, IceType_i32};
      ReturnType = IceType_void;
      break;
    case Intrinsics::NaClReadTP:
      ReturnType = IceType_i32;
      break;
    case Intrinsics::Setjmp:
      ArgTypes = {IceType_i32};
      ReturnType = IceType_i32;
      break;
    }
    StackArgumentsSize = getCallStackArgumentsSizeBytes(ArgTypes, ReturnType);
  } else if (auto *Call = llvm::dyn_cast<InstCall>(Instr)) {
    StackArgumentsSize = getCallStackArgumentsSizeBytes(Call);
  } else if (auto *Ret = llvm::dyn_cast<InstRet>(Instr)) {
    if (!Ret->hasRetValue())
      return;
    Operand *RetValue = Ret->getRetValue();
    Type ReturnType = RetValue->getType();
    if (!isScalarFloatingType(ReturnType))
      return;
    StackArgumentsSize = typeWidthInBytes(ReturnType);
  } else {
    return;
  }
  StackArgumentsSize = Traits::applyStackAlignment(StackArgumentsSize);
  updateMaxOutArgsSizeBytes(StackArgumentsSize);
}

template <class Machine>
uint32_t TargetX86Base<Machine>::getCallStackArgumentsSizeBytes(
    const std::vector<Type> &ArgTypes, Type ReturnType) {
  uint32_t OutArgumentsSizeBytes = 0;
  uint32_t XmmArgCount = 0;
  uint32_t GprArgCount = 0;
  for (Type Ty : ArgTypes) {
    // The PNaCl ABI requires the width of arguments to be at least 32 bits.
    assert(typeWidthInBytes(Ty) >= 4);
    if (isVectorType(Ty) && XmmArgCount < Traits::X86_MAX_XMM_ARGS) {
      ++XmmArgCount;
    } else if (isScalarIntegerType(Ty) &&
               GprArgCount < Traits::X86_MAX_GPR_ARGS) {
      // The 64 bit ABI allows some integers to be passed in GPRs.
      ++GprArgCount;
    } else {
      if (isVectorType(Ty)) {
        OutArgumentsSizeBytes =
            Traits::applyStackAlignment(OutArgumentsSizeBytes);
      }
      OutArgumentsSizeBytes += typeWidthInBytesOnStack(Ty);
    }
  }
  if (Traits::Is64Bit)
    return OutArgumentsSizeBytes;
  // The 32 bit ABI requires floating point values to be returned on the x87 FP
  // stack. Ensure there is enough space for the fstp/movs for floating returns.
  if (isScalarFloatingType(ReturnType)) {
    OutArgumentsSizeBytes =
        std::max(OutArgumentsSizeBytes,
                 static_cast<uint32_t>(typeWidthInBytesOnStack(ReturnType)));
  }
  return OutArgumentsSizeBytes;
}

template <class Machine>
uint32_t
TargetX86Base<Machine>::getCallStackArgumentsSizeBytes(const InstCall *Instr) {
  // Build a vector of the arguments' types.
  std::vector<Type> ArgTypes;
  for (SizeT i = 0, NumArgs = Instr->getNumArgs(); i < NumArgs; ++i) {
    Operand *Arg = Instr->getArg(i);
    ArgTypes.emplace_back(Arg->getType());
  }
  // Compute the return type (if any);
  Type ReturnType = IceType_void;
  Variable *Dest = Instr->getDest();
  if (Dest != nullptr)
    ReturnType = Dest->getType();
  return getCallStackArgumentsSizeBytes(ArgTypes, ReturnType);
}

template <class Machine>
Variable *TargetX86Base<Machine>::makeZeroedRegister(Type Ty, int32_t RegNum) {
  Variable *Reg = makeReg(Ty, RegNum);
  switch (Ty) {
  case IceType_i1:
  case IceType_i8:
  case IceType_i16:
  case IceType_i32:
  case IceType_i64:
    // Conservatively do "mov reg, 0" to avoid modifying FLAGS.
    _mov(Reg, Ctx->getConstantZero(Ty));
    break;
  case IceType_f32:
  case IceType_f64:
    Context.insert(InstFakeDef::create(Func, Reg));
    // TODO(stichnot): Use xorps/xorpd instead of pxor.
    _pxor(Reg, Reg);
    break;
  default:
    // All vector types use the same pxor instruction.
    assert(isVectorType(Ty));
    Context.insert(InstFakeDef::create(Func, Reg));
    _pxor(Reg, Reg);
    break;
  }
  return Reg;
}

// There is no support for loading or emitting vector constants, so the vector
// values returned from makeVectorOfZeros, makeVectorOfOnes, etc. are
// initialized with register operations.
//
// TODO(wala): Add limited support for vector constants so that complex
// initialization in registers is unnecessary.

template <class Machine>
Variable *TargetX86Base<Machine>::makeVectorOfZeros(Type Ty, int32_t RegNum) {
  return makeZeroedRegister(Ty, RegNum);
}

template <class Machine>
Variable *TargetX86Base<Machine>::makeVectorOfMinusOnes(Type Ty,
                                                        int32_t RegNum) {
  Variable *MinusOnes = makeReg(Ty, RegNum);
  // Insert a FakeDef so the live range of MinusOnes is not overestimated.
  Context.insert(InstFakeDef::create(Func, MinusOnes));
  _pcmpeq(MinusOnes, MinusOnes);
  return MinusOnes;
}

template <class Machine>
Variable *TargetX86Base<Machine>::makeVectorOfOnes(Type Ty, int32_t RegNum) {
  Variable *Dest = makeVectorOfZeros(Ty, RegNum);
  Variable *MinusOne = makeVectorOfMinusOnes(Ty);
  _psub(Dest, MinusOne);
  return Dest;
}

template <class Machine>
Variable *TargetX86Base<Machine>::makeVectorOfHighOrderBits(Type Ty,
                                                            int32_t RegNum) {
  assert(Ty == IceType_v4i32 || Ty == IceType_v4f32 || Ty == IceType_v8i16 ||
         Ty == IceType_v16i8);
  if (Ty == IceType_v4f32 || Ty == IceType_v4i32 || Ty == IceType_v8i16) {
    Variable *Reg = makeVectorOfOnes(Ty, RegNum);
    SizeT Shift =
        typeWidthInBytes(typeElementType(Ty)) * Traits::X86_CHAR_BIT - 1;
    _psll(Reg, Ctx->getConstantInt8(Shift));
    return Reg;
  } else {
    // SSE has no left shift operation for vectors of 8 bit integers.
    constexpr uint32_t HIGH_ORDER_BITS_MASK = 0x80808080;
    Constant *ConstantMask = Ctx->getConstantInt32(HIGH_ORDER_BITS_MASK);
    Variable *Reg = makeReg(Ty, RegNum);
    _movd(Reg, legalize(ConstantMask, Legal_Reg | Legal_Mem));
    _pshufd(Reg, Reg, Ctx->getConstantZero(IceType_i8));
    return Reg;
  }
}

/// Construct a mask in a register that can be and'ed with a floating-point
/// value to mask off its sign bit. The value will be <4 x 0x7fffffff> for f32
/// and v4f32, and <2 x 0x7fffffffffffffff> for f64. Construct it as vector of
/// ones logically right shifted one bit.
// TODO(stichnot): Fix the wala
// TODO: above, to represent vector constants in memory.
template <class Machine>
Variable *TargetX86Base<Machine>::makeVectorOfFabsMask(Type Ty,
                                                       int32_t RegNum) {
  Variable *Reg = makeVectorOfMinusOnes(Ty, RegNum);
  _psrl(Reg, Ctx->getConstantInt8(1));
  return Reg;
}

template <class Machine>
typename TargetX86Base<Machine>::Traits::X86OperandMem *
TargetX86Base<Machine>::getMemoryOperandForStackSlot(Type Ty, Variable *Slot,
                                                     uint32_t Offset) {
  // Ensure that Loc is a stack slot.
  assert(Slot->mustNotHaveReg());
  assert(Slot->getRegNum() == Variable::NoRegister);
  // Compute the location of Loc in memory.
  // TODO(wala,stichnot): lea should not
  // be required. The address of the stack slot is known at compile time
  // (although not until after addProlog()).
  constexpr Type PointerType = IceType_i32;
  Variable *Loc = makeReg(PointerType);
  _lea(Loc, Slot);
  Constant *ConstantOffset = Ctx->getConstantInt32(Offset);
  return Traits::X86OperandMem::create(Func, Ty, Loc, ConstantOffset);
}

/// Lowering helper to copy a scalar integer source operand into some 8-bit GPR.
/// Src is assumed to already be legalized.  If the source operand is known to
/// be a memory or immediate operand, a simple mov will suffice.  But if the
/// source operand can be a physical register, then it must first be copied into
/// a physical register that is truncable to 8-bit, then truncated into a
/// physical register that can receive a truncation, and finally copied into the
/// result 8-bit register (which in general can be any 8-bit register).  For
/// example, moving %ebp into %ah may be accomplished as:
///   movl %ebp, %edx
///   mov_trunc %edx, %dl  // this redundant assignment is ultimately elided
///   movb %dl, %ah
/// On the other hand, moving a memory or immediate operand into ah:
///   movb 4(%ebp), %ah
///   movb $my_imm, %ah
///
/// Note #1.  On a 64-bit target, the "movb 4(%ebp), %ah" is likely not
/// encodable, so RegNum=Reg_ah should NOT be given as an argument.  Instead,
/// use RegNum=NoRegister and then let the caller do a separate copy into
/// Reg_ah.
///
/// Note #2.  ConstantRelocatable operands are also put through this process
/// (not truncated directly) because our ELF emitter does R_386_32 relocations
/// but not R_386_8 relocations.
///
/// Note #3.  If Src is a Variable, the result will be an infinite-weight i8
/// Variable with the RCX86_IsTrunc8Rcvr register class.  As such, this helper
/// is a convenient way to prevent ah/bh/ch/dh from being an (invalid) argument
/// to the pinsrb instruction.
template <class Machine>
Variable *TargetX86Base<Machine>::copyToReg8(Operand *Src, int32_t RegNum) {
  Type Ty = Src->getType();
  assert(isScalarIntegerType(Ty));
  assert(Ty != IceType_i1);
  Variable *Reg = makeReg(IceType_i8, RegNum);
  Reg->setRegClass(RCX86_IsTrunc8Rcvr);
  if (llvm::isa<Variable>(Src) || llvm::isa<ConstantRelocatable>(Src)) {
    Variable *SrcTruncable = makeReg(Ty);
    switch (Ty) {
    case IceType_i64:
      SrcTruncable->setRegClass(RCX86_Is64To8);
      break;
    case IceType_i32:
      SrcTruncable->setRegClass(RCX86_Is32To8);
      break;
    case IceType_i16:
      SrcTruncable->setRegClass(RCX86_Is16To8);
      break;
    default:
      // i8 - just use default register class
      break;
    }
    Variable *SrcRcvr = makeReg(IceType_i8);
    SrcRcvr->setRegClass(RCX86_IsTrunc8Rcvr);
    _mov(SrcTruncable, Src);
    _mov(SrcRcvr, SrcTruncable);
    Src = SrcRcvr;
  }
  _mov(Reg, Src);
  return Reg;
}

/// Helper for legalize() to emit the right code to lower an operand to a
/// register of the appropriate type.
template <class Machine>
Variable *TargetX86Base<Machine>::copyToReg(Operand *Src, int32_t RegNum) {
  Type Ty = Src->getType();
  Variable *Reg = makeReg(Ty, RegNum);
  if (isVectorType(Ty)) {
    _movp(Reg, Src);
  } else {
    _mov(Reg, Src);
  }
  return Reg;
}

template <class Machine>
Operand *TargetX86Base<Machine>::legalize(Operand *From, LegalMask Allowed,
                                          int32_t RegNum) {
  Type Ty = From->getType();
  // Assert that a physical register is allowed. To date, all calls to
  // legalize() allow a physical register. If a physical register needs to be
  // explicitly disallowed, then new code will need to be written to force a
  // spill.
  assert(Allowed & Legal_Reg);
  // If we're asking for a specific physical register, make sure we're not
  // allowing any other operand kinds. (This could be future work, e.g. allow
  // the shl shift amount to be either an immediate or in ecx.)
  assert(RegNum == Variable::NoRegister || Allowed == Legal_Reg);

  // Substitute with an available infinite-weight variable if possible.  Only do
  // this when we are not asking for a specific register, and when the
  // substitution is not locked to a specific register, and when the types
  // match, in order to capture the vast majority of opportunities and avoid
  // corner cases in the lowering.
  if (RegNum == Variable::NoRegister) {
    if (Variable *Subst = getContext().availabilityGet(From)) {
      // At this point we know there is a potential substitution available.
      if (Subst->mustHaveReg() && !Subst->hasReg()) {
        // At this point we know the substitution will have a register.
        if (From->getType() == Subst->getType()) {
          // At this point we know the substitution's register is compatible.
          return Subst;
        }
      }
    }
  }

  if (auto *Mem = llvm::dyn_cast<typename Traits::X86OperandMem>(From)) {
    // Before doing anything with a Mem operand, we need to ensure that the
    // Base and Index components are in physical registers.
    Variable *Base = Mem->getBase();
    Variable *Index = Mem->getIndex();
    Variable *RegBase = nullptr;
    Variable *RegIndex = nullptr;
    if (Base) {
      RegBase = llvm::cast<Variable>(
          legalize(Base, Legal_Reg | Legal_Rematerializable));
    }
    if (Index) {
      RegIndex = llvm::cast<Variable>(
          legalize(Index, Legal_Reg | Legal_Rematerializable));
    }
    if (Base != RegBase || Index != RegIndex) {
      Mem = Traits::X86OperandMem::create(Func, Ty, RegBase, Mem->getOffset(),
                                          RegIndex, Mem->getShift(),
                                          Mem->getSegmentRegister());
    }

    // For all Memory Operands, we do randomization/pooling here
    From = randomizeOrPoolImmediate(Mem);

    if (!(Allowed & Legal_Mem)) {
      From = copyToReg(From, RegNum);
    }
    return From;
  }
  if (auto *Const = llvm::dyn_cast<Constant>(From)) {
    if (llvm::isa<ConstantUndef>(Const)) {
      From = legalizeUndef(Const, RegNum);
      if (isVectorType(Ty))
        return From;
      Const = llvm::cast<Constant>(From);
    }
    // There should be no constants of vector type (other than undef).
    assert(!isVectorType(Ty));

    // If the operand is a 64 bit constant integer we need to legalize it to a
    // register in x86-64.
    if (Traits::Is64Bit) {
      if (llvm::isa<ConstantInteger64>(Const)) {
        Variable *V = copyToReg(Const, RegNum);
        return V;
      }
    }

    // If the operand is an 32 bit constant integer, we should check whether we
    // need to randomize it or pool it.
    if (ConstantInteger32 *C = llvm::dyn_cast<ConstantInteger32>(Const)) {
      Operand *NewConst = randomizeOrPoolImmediate(C, RegNum);
      if (NewConst != Const) {
        return NewConst;
      }
    }

    // Convert a scalar floating point constant into an explicit memory
    // operand.
    if (isScalarFloatingType(Ty)) {
      if (auto *ConstFloat = llvm::dyn_cast<ConstantFloat>(Const)) {
        if (Utils::isPositiveZero(ConstFloat->getValue()))
          return makeZeroedRegister(Ty, RegNum);
      } else if (auto *ConstDouble = llvm::dyn_cast<ConstantDouble>(Const)) {
        if (Utils::isPositiveZero(ConstDouble->getValue()))
          return makeZeroedRegister(Ty, RegNum);
      }
      Variable *Base = nullptr;
      std::string Buffer;
      llvm::raw_string_ostream StrBuf(Buffer);
      llvm::cast<Constant>(From)->emitPoolLabel(StrBuf, Ctx);
      llvm::cast<Constant>(From)->setShouldBePooled(true);
      Constant *Offset = Ctx->getConstantSym(0, StrBuf.str(), true);
      From = Traits::X86OperandMem::create(Func, Ty, Base, Offset);
    }
    bool NeedsReg = false;
    if (!(Allowed & Legal_Imm) && !isScalarFloatingType(Ty))
      // Immediate specifically not allowed
      NeedsReg = true;
    if (!(Allowed & Legal_Mem) && isScalarFloatingType(Ty))
      // On x86, FP constants are lowered to mem operands.
      NeedsReg = true;
    if (NeedsReg) {
      From = copyToReg(From, RegNum);
    }
    return From;
  }
  if (auto *Var = llvm::dyn_cast<Variable>(From)) {
    // Check if the variable is guaranteed a physical register. This can happen
    // either when the variable is pre-colored or when it is assigned infinite
    // weight.
    bool MustHaveRegister = (Var->hasReg() || Var->mustHaveReg());
    bool MustRematerialize =
        (Var->isRematerializable() && !(Allowed & Legal_Rematerializable));
    // We need a new physical register for the operand if:
    // - Mem is not allowed and Var isn't guaranteed a physical register, or
    // - RegNum is required and Var->getRegNum() doesn't match, or
    // - Var is a rematerializable variable and rematerializable pass-through is
    //   not allowed (in which case we need an lea instruction).
    if (MustRematerialize) {
      assert(Ty == IceType_i32);
      Variable *NewVar = makeReg(Ty, RegNum);
      // Since Var is rematerializable, the offset will be added when the lea is
      // emitted.
      constexpr Constant *NoOffset = nullptr;
      auto *Mem = Traits::X86OperandMem::create(Func, Ty, Var, NoOffset);
      _lea(NewVar, Mem);
      From = NewVar;
    } else if ((!(Allowed & Legal_Mem) && !MustHaveRegister) ||
               (RegNum != Variable::NoRegister && RegNum != Var->getRegNum()) ||
               MustRematerialize) {
      From = copyToReg(From, RegNum);
    }
    return From;
  }
  llvm_unreachable("Unhandled operand kind in legalize()");
  return From;
}

/// Provide a trivial wrapper to legalize() for this common usage.
template <class Machine>
Variable *TargetX86Base<Machine>::legalizeToReg(Operand *From, int32_t RegNum) {
  return llvm::cast<Variable>(legalize(From, Legal_Reg, RegNum));
}

/// Legalize undef values to concrete values.
template <class Machine>
Operand *TargetX86Base<Machine>::legalizeUndef(Operand *From, int32_t RegNum) {
  Type Ty = From->getType();
  if (llvm::isa<ConstantUndef>(From)) {
    // Lower undefs to zero.  Another option is to lower undefs to an
    // uninitialized register; however, using an uninitialized register results
    // in less predictable code.
    //
    // If in the future the implementation is changed to lower undef values to
    // uninitialized registers, a FakeDef will be needed:
    //     Context.insert(InstFakeDef::create(Func, Reg));
    // This is in order to ensure that the live range of Reg is not
    // overestimated.  If the constant being lowered is a 64 bit value, then
    // the result should be split and the lo and hi components will need to go
    // in uninitialized registers.
    if (isVectorType(Ty))
      return makeVectorOfZeros(Ty, RegNum);
    return Ctx->getConstantZero(Ty);
  }
  return From;
}

/// For the cmp instruction, if Src1 is an immediate, or known to be a physical
/// register, we can allow Src0 to be a memory operand. Otherwise, Src0 must be
/// copied into a physical register. (Actually, either Src0 or Src1 can be
/// chosen for the physical register, but unfortunately we have to commit to one
/// or the other before register allocation.)
template <class Machine>
Operand *TargetX86Base<Machine>::legalizeSrc0ForCmp(Operand *Src0,
                                                    Operand *Src1) {
  bool IsSrc1ImmOrReg = false;
  if (llvm::isa<Constant>(Src1)) {
    IsSrc1ImmOrReg = true;
  } else if (auto *Var = llvm::dyn_cast<Variable>(Src1)) {
    if (Var->hasReg())
      IsSrc1ImmOrReg = true;
  }
  return legalize(Src0, IsSrc1ImmOrReg ? (Legal_Reg | Legal_Mem) : Legal_Reg);
}

template <class Machine>
typename TargetX86Base<Machine>::Traits::X86OperandMem *
TargetX86Base<Machine>::formMemoryOperand(Operand *Opnd, Type Ty,
                                          bool DoLegalize) {
  auto *Mem = llvm::dyn_cast<typename Traits::X86OperandMem>(Opnd);
  // It may be the case that address mode optimization already creates an
  // Traits::X86OperandMem, so in that case it wouldn't need another level of
  // transformation.
  if (!Mem) {
    Variable *Base = llvm::dyn_cast<Variable>(Opnd);
    Constant *Offset = llvm::dyn_cast<Constant>(Opnd);
    assert(Base || Offset);
    if (Offset) {
      // During memory operand building, we do not blind or pool the constant
      // offset, we will work on the whole memory operand later as one entity
      // later, this save one instruction. By turning blinding and pooling off,
      // we guarantee legalize(Offset) will return a Constant*.
      {
        BoolFlagSaver B(RandomizationPoolingPaused, true);

        Offset = llvm::cast<Constant>(legalize(Offset));
      }

      assert(llvm::isa<ConstantInteger32>(Offset) ||
             llvm::isa<ConstantRelocatable>(Offset));
    }
    Mem = Traits::X86OperandMem::create(Func, Ty, Base, Offset);
  }
  // Do legalization, which contains randomization/pooling or do
  // randomization/pooling.
  return llvm::cast<typename Traits::X86OperandMem>(
      DoLegalize ? legalize(Mem) : randomizeOrPoolImmediate(Mem));
}

template <class Machine>
Variable *TargetX86Base<Machine>::makeReg(Type Type, int32_t RegNum) {
  // There aren't any 64-bit integer registers for x86-32.
  assert(Traits::Is64Bit || Type != IceType_i64);
  Variable *Reg = Func->makeVariable(Type);
  if (RegNum == Variable::NoRegister)
    Reg->setMustHaveReg();
  else
    Reg->setRegNum(RegNum);
  return Reg;
}

template <class Machine>
const Type TargetX86Base<Machine>::TypeForSize[] = {
    IceType_i8, IceType_i16, IceType_i32,
    (Traits::Is64Bit ? IceType_i64 : IceType_f64), IceType_v16i8};
template <class Machine>
Type TargetX86Base<Machine>::largestTypeInSize(uint32_t Size,
                                               uint32_t MaxSize) {
  assert(Size != 0);
  uint32_t TyIndex = llvm::findLastSet(Size, llvm::ZB_Undefined);
  uint32_t MaxIndex = MaxSize == NoSizeLimit
                          ? llvm::array_lengthof(TypeForSize) - 1
                          : llvm::findLastSet(MaxSize, llvm::ZB_Undefined);
  return TypeForSize[std::min(TyIndex, MaxIndex)];
}

template <class Machine>
Type TargetX86Base<Machine>::firstTypeThatFitsSize(uint32_t Size,
                                                   uint32_t MaxSize) {
  assert(Size != 0);
  uint32_t TyIndex = llvm::findLastSet(Size, llvm::ZB_Undefined);
  if (!llvm::isPowerOf2_32(Size))
    ++TyIndex;
  uint32_t MaxIndex = MaxSize == NoSizeLimit
                          ? llvm::array_lengthof(TypeForSize) - 1
                          : llvm::findLastSet(MaxSize, llvm::ZB_Undefined);
  return TypeForSize[std::min(TyIndex, MaxIndex)];
}

template <class Machine> void TargetX86Base<Machine>::postLower() {
  if (Ctx->getFlags().getOptLevel() == Opt_m1)
    return;
  markRedefinitions();
  Context.availabilityUpdate();
}

template <class Machine>
void TargetX86Base<Machine>::makeRandomRegisterPermutation(
    llvm::SmallVectorImpl<int32_t> &Permutation,
    const llvm::SmallBitVector &ExcludeRegisters, uint64_t Salt) const {
  Traits::makeRandomRegisterPermutation(Ctx, Func, Permutation,
                                        ExcludeRegisters, Salt);
}

template <class Machine>
void TargetX86Base<Machine>::emit(const ConstantInteger32 *C) const {
  if (!BuildDefs::dump())
    return;
  Ostream &Str = Ctx->getStrEmit();
  Str << getConstantPrefix() << C->getValue();
}

template <class Machine>
void TargetX86Base<Machine>::emit(const ConstantInteger64 *C) const {
  if (!Traits::Is64Bit) {
    llvm::report_fatal_error("Not expecting to emit 64-bit integers");
  } else {
    if (!BuildDefs::dump())
      return;
    Ostream &Str = Ctx->getStrEmit();
    Str << getConstantPrefix() << C->getValue();
  }
}

template <class Machine>
void TargetX86Base<Machine>::emit(const ConstantFloat *C) const {
  if (!BuildDefs::dump())
    return;
  Ostream &Str = Ctx->getStrEmit();
  C->emitPoolLabel(Str, Ctx);
}

template <class Machine>
void TargetX86Base<Machine>::emit(const ConstantDouble *C) const {
  if (!BuildDefs::dump())
    return;
  Ostream &Str = Ctx->getStrEmit();
  C->emitPoolLabel(Str, Ctx);
}

template <class Machine>
void TargetX86Base<Machine>::emit(const ConstantUndef *) const {
  llvm::report_fatal_error("undef value encountered by emitter.");
}

/// Randomize or pool an Immediate.
template <class Machine>
Operand *TargetX86Base<Machine>::randomizeOrPoolImmediate(Constant *Immediate,
                                                          int32_t RegNum) {
  assert(llvm::isa<ConstantInteger32>(Immediate) ||
         llvm::isa<ConstantRelocatable>(Immediate));
  if (Ctx->getFlags().getRandomizeAndPoolImmediatesOption() == RPI_None ||
      RandomizationPoolingPaused == true) {
    // Immediates randomization/pooling off or paused
    return Immediate;
  }
  if (Immediate->shouldBeRandomizedOrPooled(Ctx)) {
    Ctx->statsUpdateRPImms();
    if (Ctx->getFlags().getRandomizeAndPoolImmediatesOption() ==
        RPI_Randomize) {
      // blind the constant
      // FROM:
      //  imm
      // TO:
      //  insert: mov imm+cookie, Reg
      //  insert: lea -cookie[Reg], Reg
      //  => Reg
      // If we have already assigned a phy register, we must come from
      // advancedPhiLowering()=>lowerAssign(). In this case we should reuse the
      // assigned register as this assignment is that start of its use-def
      // chain. So we add RegNum argument here. Note we use 'lea' instruction
      // instead of 'xor' to avoid affecting the flags.
      Variable *Reg = makeReg(IceType_i32, RegNum);
      ConstantInteger32 *Integer = llvm::cast<ConstantInteger32>(Immediate);
      uint32_t Value = Integer->getValue();
      uint32_t Cookie = Func->getConstantBlindingCookie();
      _mov(Reg, Ctx->getConstantInt(IceType_i32, Cookie + Value));
      Constant *Offset = Ctx->getConstantInt(IceType_i32, 0 - Cookie);
      _lea(Reg, Traits::X86OperandMem::create(Func, IceType_i32, Reg, Offset,
                                              nullptr, 0));
      if (Immediate->getType() != IceType_i32) {
        Variable *TruncReg = makeReg(Immediate->getType(), RegNum);
        _mov(TruncReg, Reg);
        return TruncReg;
      }
      return Reg;
    }
    if (Ctx->getFlags().getRandomizeAndPoolImmediatesOption() == RPI_Pool) {
      // pool the constant
      // FROM:
      //  imm
      // TO:
      //  insert: mov $label, Reg
      //  => Reg
      assert(Ctx->getFlags().getRandomizeAndPoolImmediatesOption() == RPI_Pool);
      Immediate->setShouldBePooled(true);
      // if we have already assigned a phy register, we must come from
      // advancedPhiLowering()=>lowerAssign(). In this case we should reuse the
      // assigned register as this assignment is that start of its use-def
      // chain. So we add RegNum argument here.
      Variable *Reg = makeReg(Immediate->getType(), RegNum);
      IceString Label;
      llvm::raw_string_ostream Label_stream(Label);
      Immediate->emitPoolLabel(Label_stream, Ctx);
      constexpr RelocOffsetT Offset = 0;
      constexpr bool SuppressMangling = true;
      Constant *Symbol =
          Ctx->getConstantSym(Offset, Label_stream.str(), SuppressMangling);
      typename Traits::X86OperandMem *MemOperand =
          Traits::X86OperandMem::create(Func, Immediate->getType(), nullptr,
                                        Symbol);
      _mov(Reg, MemOperand);
      return Reg;
    }
    assert("Unsupported -randomize-pool-immediates option" && false);
  }
  // the constant Immediate is not eligible for blinding/pooling
  return Immediate;
}

template <class Machine>
typename TargetX86Base<Machine>::Traits::X86OperandMem *
TargetX86Base<Machine>::randomizeOrPoolImmediate(
    typename Traits::X86OperandMem *MemOperand, int32_t RegNum) {
  assert(MemOperand);
  if (Ctx->getFlags().getRandomizeAndPoolImmediatesOption() == RPI_None ||
      RandomizationPoolingPaused == true) {
    // immediates randomization/pooling is turned off
    return MemOperand;
  }

  // If this memory operand is already a randomized one, we do not randomize it
  // again.
  if (MemOperand->getRandomized())
    return MemOperand;

  if (Constant *C = llvm::dyn_cast_or_null<Constant>(MemOperand->getOffset())) {
    if (C->shouldBeRandomizedOrPooled(Ctx)) {
      // The offset of this mem operand should be blinded or pooled
      Ctx->statsUpdateRPImms();
      if (Ctx->getFlags().getRandomizeAndPoolImmediatesOption() ==
          RPI_Randomize) {
        // blind the constant offset
        // FROM:
        //  offset[base, index, shift]
        // TO:
        //  insert: lea offset+cookie[base], RegTemp
        //  => -cookie[RegTemp, index, shift]
        uint32_t Value =
            llvm::dyn_cast<ConstantInteger32>(MemOperand->getOffset())
                ->getValue();
        uint32_t Cookie = Func->getConstantBlindingCookie();
        Constant *Mask1 = Ctx->getConstantInt(
            MemOperand->getOffset()->getType(), Cookie + Value);
        Constant *Mask2 =
            Ctx->getConstantInt(MemOperand->getOffset()->getType(), 0 - Cookie);

        typename Traits::X86OperandMem *TempMemOperand =
            Traits::X86OperandMem::create(Func, MemOperand->getType(),
                                          MemOperand->getBase(), Mask1);
        // If we have already assigned a physical register, we must come from
        // advancedPhiLowering()=>lowerAssign(). In this case we should reuse
        // the assigned register as this assignment is that start of its
        // use-def chain. So we add RegNum argument here.
        Variable *RegTemp = makeReg(MemOperand->getOffset()->getType(), RegNum);
        _lea(RegTemp, TempMemOperand);

        typename Traits::X86OperandMem *NewMemOperand =
            Traits::X86OperandMem::create(Func, MemOperand->getType(), RegTemp,
                                          Mask2, MemOperand->getIndex(),
                                          MemOperand->getShift(),
                                          MemOperand->getSegmentRegister());

        // Label this memory operand as randomized, so we won't randomize it
        // again in case we call legalize() multiple times on this memory
        // operand.
        NewMemOperand->setRandomized(true);
        return NewMemOperand;
      }
      if (Ctx->getFlags().getRandomizeAndPoolImmediatesOption() == RPI_Pool) {
        // pool the constant offset
        // FROM:
        //  offset[base, index, shift]
        // TO:
        //  insert: mov $label, RegTemp
        //  insert: lea [base, RegTemp], RegTemp
        //  =>[RegTemp, index, shift]
        assert(Ctx->getFlags().getRandomizeAndPoolImmediatesOption() ==
               RPI_Pool);
        // Memory operand should never exist as source operands in phi lowering
        // assignments, so there is no need to reuse any registers here. For
        // phi lowering, we should not ask for new physical registers in
        // general. However, if we do meet Memory Operand during phi lowering,
        // we should not blind or pool the immediates for now.
        if (RegNum != Variable::NoRegister)
          return MemOperand;
        Variable *RegTemp = makeReg(IceType_i32);
        IceString Label;
        llvm::raw_string_ostream Label_stream(Label);
        MemOperand->getOffset()->emitPoolLabel(Label_stream, Ctx);
        MemOperand->getOffset()->setShouldBePooled(true);
        constexpr RelocOffsetT SymOffset = 0;
        constexpr bool SuppressMangling = true;
        Constant *Symbol = Ctx->getConstantSym(SymOffset, Label_stream.str(),
                                               SuppressMangling);
        typename Traits::X86OperandMem *SymbolOperand =
            Traits::X86OperandMem::create(
                Func, MemOperand->getOffset()->getType(), nullptr, Symbol);
        _mov(RegTemp, SymbolOperand);
        // If we have a base variable here, we should add the lea instruction
        // to add the value of the base variable to RegTemp. If there is no
        // base variable, we won't need this lea instruction.
        if (MemOperand->getBase()) {
          typename Traits::X86OperandMem *CalculateOperand =
              Traits::X86OperandMem::create(
                  Func, MemOperand->getType(), MemOperand->getBase(), nullptr,
                  RegTemp, 0, MemOperand->getSegmentRegister());
          _lea(RegTemp, CalculateOperand);
        }
        typename Traits::X86OperandMem *NewMemOperand =
            Traits::X86OperandMem::create(Func, MemOperand->getType(), RegTemp,
                                          nullptr, MemOperand->getIndex(),
                                          MemOperand->getShift(),
                                          MemOperand->getSegmentRegister());
        return NewMemOperand;
      }
      assert("Unsupported -randomize-pool-immediates option" && false);
    }
  }
  // the offset is not eligible for blinding or pooling, return the original
  // mem operand
  return MemOperand;
}

} // end of namespace X86Internal
} // end of namespace Ice

#endif // SUBZERO_SRC_ICETARGETLOWERINGX86BASEIMPL_H
