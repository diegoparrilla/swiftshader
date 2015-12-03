//===- subzero/src/IceRegAlloc.h - Linear-scan reg. allocation --*- C++ -*-===//
//
//                        The Subzero Code Generator
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief Declares the LinearScan data structure used during linear-scan
/// register allocation.
///
/// This holds the various work queues for the linear-scan algorithm.
///
//===----------------------------------------------------------------------===//

#ifndef SUBZERO_SRC_ICEREGALLOC_H
#define SUBZERO_SRC_ICEREGALLOC_H

#include "IceDefs.h"
#include "IceOperand.h"
#include "IceTypes.h"

namespace Ice {

class LinearScan {
  LinearScan() = delete;
  LinearScan(const LinearScan &) = delete;
  LinearScan &operator=(const LinearScan &) = delete;

public:
  explicit LinearScan(Cfg *Func);
  void init(RegAllocKind Kind);
  void scan(const llvm::SmallBitVector &RegMask, bool Randomized);
  // Returns the number of times some variable has been assigned a register but
  // later evicted because of a higher-priority allocation.  The idea is that we
  // can implement "second-chance bin-packing" by rerunning register allocation
  // until there are no more evictions.
  SizeT getNumEvictions() const { return Evicted.size(); }
  bool hasEvictions() const { return !Evicted.empty(); }
  void dump(Cfg *Func) const;

  // TODO(stichnot): Statically choose the size based on the target being
  // compiled.
  static constexpr size_t REGS_SIZE = 32;

private:
  using OrderedRanges = CfgVector<Variable *>;
  using UnorderedRanges = CfgVector<Variable *>;
  using DefUseErrorList = llvm::SmallVector<SizeT, 10>;

  class IterationState {
    IterationState(const IterationState &) = delete;
    IterationState operator=(const IterationState &) = delete;

  public:
    IterationState() = default;
    Variable *Cur = nullptr;
    Variable *Prefer = nullptr;
    int32_t PreferReg = Variable::NoRegister;
    bool AllowOverlap = false;
    llvm::SmallBitVector RegMask;
    llvm::SmallBitVector Free;
    llvm::SmallBitVector PrecoloredUnhandledMask; // Note: only used for dumping
    llvm::SmallVector<RegWeight, REGS_SIZE> Weights;
  };

  bool livenessValidateIntervals(const DefUseErrorList &DefsWithoutUses,
                                 const DefUseErrorList &UsesBeforeDefs,
                                 const CfgVector<InstNumberT> &LRBegin,
                                 const CfgVector<InstNumberT> &LREnd) const;
  void initForGlobal();
  void initForInfOnly();
  void initForSecondChance();
  /// Move an item from the From set to the To set. From[Index] is pushed onto
  /// the end of To[], then the item is efficiently removed from From[] by
  /// effectively swapping it with the last item in From[] and then popping it
  /// from the back. As such, the caller is best off iterating over From[] in
  /// reverse order to avoid the need for special handling of the iterator.
  void moveItem(UnorderedRanges &From, SizeT Index, UnorderedRanges &To) {
    To.push_back(From[Index]);
    From[Index] = From.back();
    From.pop_back();
  }

  /// \name scan helper functions.
  /// @{
  /// Free up a register for infinite-weight Cur by spilling and reloading some
  /// register that isn't used during Cur's live range.
  void addSpillFill(IterationState &Iter);
  /// Check for active ranges that have expired or become inactive.
  void handleActiveRangeExpiredOrInactive(const Variable *Cur);
  /// Check for inactive ranges that have expired or reactivated.
  void handleInactiveRangeExpiredOrReactivated(const Variable *Cur);
  void findRegisterPreference(IterationState &Iter);
  void filterFreeWithInactiveRanges(IterationState &Iter);
  void filterFreeWithPrecoloredRanges(IterationState &Iter);
  void allocatePrecoloredRegister(Variable *Cur);
  void allocatePreferredRegister(IterationState &Iter);
  void allocateFreeRegister(IterationState &Iter);
  void handleNoFreeRegisters(IterationState &Iter);
  void assignFinalRegisters(const llvm::SmallBitVector &RegMaskFull,
                            const llvm::SmallBitVector &PreDefinedRegisters,
                            bool Randomized);
  /// @}

  void dumpLiveRangeTrace(const char *Label, const Variable *Item);

  Cfg *const Func;
  GlobalContext *const Ctx;
  TargetLowering *const Target;

  OrderedRanges Unhandled;
  /// UnhandledPrecolored is a subset of Unhandled, specially collected for
  /// faster processing.
  OrderedRanges UnhandledPrecolored;
  UnorderedRanges Active, Inactive, Handled;
  UnorderedRanges Evicted;
  CfgVector<InstNumberT> Kills;
  RegAllocKind Kind = RAK_Unknown;
  /// RegUses[I] is the number of live ranges (variables) that register I is
  /// currently assigned to. It can be greater than 1 as a result of
  /// AllowOverlap inference.
  llvm::SmallVector<int32_t, REGS_SIZE> RegUses;
  // TODO(jpp): for some architectures a SmallBitVector might not be big
  // enough. Evaluate what the performance impact on those architectures is.
  llvm::SmallVector<const llvm::SmallBitVector *, REGS_SIZE> RegAliases;
  bool FindPreference = false;
  bool FindOverlap = false;

  const bool Verbose;
};

} // end of namespace Ice

#endif // SUBZERO_SRC_ICEREGALLOC_H
