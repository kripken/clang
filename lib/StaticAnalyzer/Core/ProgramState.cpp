//= ProgramState.cpp - Path-Sensitive "State" for tracking values --*- C++ -*--=
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file implements ProgramState and ProgramStateManager.
//
//===----------------------------------------------------------------------===//

#include "clang/Analysis/CFG.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/SubEngine.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/TaintManager.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace ento;

// Give the vtable for ConstraintManager somewhere to live.
// FIXME: Move this elsewhere.
ConstraintManager::~ConstraintManager() {}

namespace clang { namespace  ento {
/// Increments the number of times this state is referenced.

void ProgramStateRetain(const ProgramState *state) {
  ++const_cast<ProgramState*>(state)->refCount;
}

/// Decrement the number of times this state is referenced.
void ProgramStateRelease(const ProgramState *state) {
  assert(state->refCount > 0);
  ProgramState *s = const_cast<ProgramState*>(state);
  if (--s->refCount == 0) {
    ProgramStateManager &Mgr = s->getStateManager();
    Mgr.StateSet.RemoveNode(s);
    s->~ProgramState();    
    Mgr.freeStates.push_back(s);
  }
}
}}

ProgramState::ProgramState(ProgramStateManager *mgr, const Environment& env,
                 StoreRef st, GenericDataMap gdm)
  : stateMgr(mgr),
    Env(env),
    store(st.getStore()),
    GDM(gdm),
    refCount(0) {
  stateMgr->getStoreManager().incrementReferenceCount(store);
}

ProgramState::ProgramState(const ProgramState &RHS)
    : llvm::FoldingSetNode(),
      stateMgr(RHS.stateMgr),
      Env(RHS.Env),
      store(RHS.store),
      GDM(RHS.GDM),
      refCount(0) {
  stateMgr->getStoreManager().incrementReferenceCount(store);
}

ProgramState::~ProgramState() {
  if (store)
    stateMgr->getStoreManager().decrementReferenceCount(store);
}

ProgramStateManager::~ProgramStateManager() {
  for (GDMContextsTy::iterator I=GDMContexts.begin(), E=GDMContexts.end();
       I!=E; ++I)
    I->second.second(I->second.first);
}

ProgramStateRef 
ProgramStateManager::removeDeadBindings(ProgramStateRef state,
                                   const StackFrameContext *LCtx,
                                   SymbolReaper& SymReaper) {

  // This code essentially performs a "mark-and-sweep" of the VariableBindings.
  // The roots are any Block-level exprs and Decls that our liveness algorithm
  // tells us are live.  We then see what Decls they may reference, and keep
  // those around.  This code more than likely can be made faster, and the
  // frequency of which this method is called should be experimented with
  // for optimum performance.
  ProgramState NewState = *state;

  NewState.Env = EnvMgr.removeDeadBindings(NewState.Env, SymReaper, state);

  // Clean up the store.
  StoreRef newStore = StoreMgr->removeDeadBindings(NewState.getStore(), LCtx,
                                                   SymReaper);
  NewState.setStore(newStore);
  SymReaper.setReapedStore(newStore);
  
  return getPersistentState(NewState);
}

ProgramStateRef ProgramStateManager::MarshalState(ProgramStateRef state,
                                            const StackFrameContext *InitLoc) {
  // make up an empty state for now.
  ProgramState State(this,
                EnvMgr.getInitialEnvironment(),
                StoreMgr->getInitialStore(InitLoc),
                GDMFactory.getEmptyMap());

  return getPersistentState(State);
}

ProgramStateRef ProgramState::bindCompoundLiteral(const CompoundLiteralExpr *CL,
                                            const LocationContext *LC,
                                            SVal V) const {
  const StoreRef &newStore = 
    getStateManager().StoreMgr->BindCompoundLiteral(getStore(), CL, LC, V);
  return makeWithStore(newStore);
}

ProgramStateRef ProgramState::bindDecl(const VarRegion* VR, SVal IVal) const {
  const StoreRef &newStore =
    getStateManager().StoreMgr->BindDecl(getStore(), VR, IVal);
  return makeWithStore(newStore);
}

ProgramStateRef ProgramState::bindDeclWithNoInit(const VarRegion* VR) const {
  const StoreRef &newStore =
    getStateManager().StoreMgr->BindDeclWithNoInit(getStore(), VR);
  return makeWithStore(newStore);
}

ProgramStateRef ProgramState::bindLoc(Loc LV, SVal V) const {
  ProgramStateManager &Mgr = getStateManager();
  ProgramStateRef newState = makeWithStore(Mgr.StoreMgr->Bind(getStore(), 
                                                             LV, V));
  const MemRegion *MR = LV.getAsRegion();
  if (MR && Mgr.getOwningEngine())
    return Mgr.getOwningEngine()->processRegionChange(newState, MR);

  return newState;
}

ProgramStateRef ProgramState::bindDefault(SVal loc, SVal V) const {
  ProgramStateManager &Mgr = getStateManager();
  const MemRegion *R = cast<loc::MemRegionVal>(loc).getRegion();
  const StoreRef &newStore = Mgr.StoreMgr->BindDefault(getStore(), R, V);
  ProgramStateRef new_state = makeWithStore(newStore);
  return Mgr.getOwningEngine() ? 
           Mgr.getOwningEngine()->processRegionChange(new_state, R) : 
           new_state;
}

ProgramStateRef 
ProgramState::invalidateRegions(ArrayRef<const MemRegion *> Regions,
                                const Expr *E, unsigned Count,
                                const LocationContext *LCtx,
                                StoreManager::InvalidatedSymbols *IS,
                                const CallEvent *Call) const {
  if (!IS) {
    StoreManager::InvalidatedSymbols invalidated;
    return invalidateRegionsImpl(Regions, E, Count, LCtx,
                                 invalidated, Call);
  }
  return invalidateRegionsImpl(Regions, E, Count, LCtx, *IS, Call);
}

ProgramStateRef 
ProgramState::invalidateRegionsImpl(ArrayRef<const MemRegion *> Regions,
                                    const Expr *E, unsigned Count,
                                    const LocationContext *LCtx,
                                    StoreManager::InvalidatedSymbols &IS,
                                    const CallEvent *Call) const {
  ProgramStateManager &Mgr = getStateManager();
  SubEngine* Eng = Mgr.getOwningEngine();
 
  if (Eng && Eng->wantsRegionChangeUpdate(this)) {
    StoreManager::InvalidatedRegions Invalidated;
    const StoreRef &newStore
      = Mgr.StoreMgr->invalidateRegions(getStore(), Regions, E, Count, LCtx, IS,
                                        Call, &Invalidated);
    ProgramStateRef newState = makeWithStore(newStore);
    return Eng->processRegionChanges(newState, &IS, Regions, Invalidated, Call);
  }

  const StoreRef &newStore =
    Mgr.StoreMgr->invalidateRegions(getStore(), Regions, E, Count, LCtx, IS,
                                    Call, NULL);
  return makeWithStore(newStore);
}

ProgramStateRef ProgramState::unbindLoc(Loc LV) const {
  assert(!isa<loc::MemRegionVal>(LV) && "Use invalidateRegion instead.");

  Store OldStore = getStore();
  const StoreRef &newStore = getStateManager().StoreMgr->Remove(OldStore, LV);

  if (newStore.getStore() == OldStore)
    return this;

  return makeWithStore(newStore);
}

ProgramStateRef 
ProgramState::enterStackFrame(const LocationContext *callerCtx,
                              const StackFrameContext *calleeCtx) const {
  const StoreRef &new_store =
    getStateManager().StoreMgr->enterStackFrame(this, callerCtx, calleeCtx);
  return makeWithStore(new_store);
}

SVal ProgramState::getSValAsScalarOrLoc(const MemRegion *R) const {
  // We only want to do fetches from regions that we can actually bind
  // values.  For example, SymbolicRegions of type 'id<...>' cannot
  // have direct bindings (but their can be bindings on their subregions).
  if (!R->isBoundable())
    return UnknownVal();

  if (const TypedValueRegion *TR = dyn_cast<TypedValueRegion>(R)) {
    QualType T = TR->getValueType();
    if (Loc::isLocType(T) || T->isIntegerType())
      return getSVal(R);
  }

  return UnknownVal();
}

SVal ProgramState::getSVal(Loc location, QualType T) const {
  SVal V = getRawSVal(cast<Loc>(location), T);

  // If 'V' is a symbolic value that is *perfectly* constrained to
  // be a constant value, use that value instead to lessen the burden
  // on later analysis stages (so we have less symbolic values to reason
  // about).
  if (!T.isNull()) {
    if (SymbolRef sym = V.getAsSymbol()) {
      if (const llvm::APSInt *Int = getSymVal(sym)) {
        // FIXME: Because we don't correctly model (yet) sign-extension
        // and truncation of symbolic values, we need to convert
        // the integer value to the correct signedness and bitwidth.
        //
        // This shows up in the following:
        //
        //   char foo();
        //   unsigned x = foo();
        //   if (x == 54)
        //     ...
        //
        //  The symbolic value stored to 'x' is actually the conjured
        //  symbol for the call to foo(); the type of that symbol is 'char',
        //  not unsigned.
        const llvm::APSInt &NewV = getBasicVals().Convert(T, *Int);
        
        if (isa<Loc>(V))
          return loc::ConcreteInt(NewV);
        else
          return nonloc::ConcreteInt(NewV);
      }
    }
  }
  
  return V;
}

ProgramStateRef ProgramState::BindExpr(const Stmt *S,
                                           const LocationContext *LCtx,
                                           SVal V, bool Invalidate) const{
  Environment NewEnv =
    getStateManager().EnvMgr.bindExpr(Env, EnvironmentEntry(S, LCtx), V,
                                      Invalidate);
  if (NewEnv == Env)
    return this;

  ProgramState NewSt = *this;
  NewSt.Env = NewEnv;
  return getStateManager().getPersistentState(NewSt);
}

ProgramStateRef 
ProgramState::bindExprAndLocation(const Stmt *S, const LocationContext *LCtx,
                                  SVal location,
                                  SVal V) const {
  Environment NewEnv =
    getStateManager().EnvMgr.bindExprAndLocation(Env,
                                                 EnvironmentEntry(S, LCtx),
                                                 location, V);

  if (NewEnv == Env)
    return this;
  
  ProgramState NewSt = *this;
  NewSt.Env = NewEnv;
  return getStateManager().getPersistentState(NewSt);
}

ProgramStateRef ProgramState::assumeInBound(DefinedOrUnknownSVal Idx,
                                      DefinedOrUnknownSVal UpperBound,
                                      bool Assumption,
                                      QualType indexTy) const {
  if (Idx.isUnknown() || UpperBound.isUnknown())
    return this;

  // Build an expression for 0 <= Idx < UpperBound.
  // This is the same as Idx + MIN < UpperBound + MIN, if overflow is allowed.
  // FIXME: This should probably be part of SValBuilder.
  ProgramStateManager &SM = getStateManager();
  SValBuilder &svalBuilder = SM.getSValBuilder();
  ASTContext &Ctx = svalBuilder.getContext();

  // Get the offset: the minimum value of the array index type.
  BasicValueFactory &BVF = svalBuilder.getBasicValueFactory();
  // FIXME: This should be using ValueManager::ArrayindexTy...somehow.
  if (indexTy.isNull())
    indexTy = Ctx.IntTy;
  nonloc::ConcreteInt Min(BVF.getMinValue(indexTy));

  // Adjust the index.
  SVal newIdx = svalBuilder.evalBinOpNN(this, BO_Add,
                                        cast<NonLoc>(Idx), Min, indexTy);
  if (newIdx.isUnknownOrUndef())
    return this;

  // Adjust the upper bound.
  SVal newBound =
    svalBuilder.evalBinOpNN(this, BO_Add, cast<NonLoc>(UpperBound),
                            Min, indexTy);

  if (newBound.isUnknownOrUndef())
    return this;

  // Build the actual comparison.
  SVal inBound = svalBuilder.evalBinOpNN(this, BO_LT,
                                cast<NonLoc>(newIdx), cast<NonLoc>(newBound),
                                Ctx.IntTy);
  if (inBound.isUnknownOrUndef())
    return this;

  // Finally, let the constraint manager take care of it.
  ConstraintManager &CM = SM.getConstraintManager();
  return CM.assume(this, cast<DefinedSVal>(inBound), Assumption);
}

ProgramStateRef ProgramStateManager::getInitialState(const LocationContext *InitLoc) {
  ProgramState State(this,
                EnvMgr.getInitialEnvironment(),
                StoreMgr->getInitialStore(InitLoc),
                GDMFactory.getEmptyMap());

  return getPersistentState(State);
}

ProgramStateRef ProgramStateManager::getPersistentStateWithGDM(
                                                     ProgramStateRef FromState,
                                                     ProgramStateRef GDMState) {
  ProgramState NewState(*FromState);
  NewState.GDM = GDMState->GDM;
  return getPersistentState(NewState);
}

ProgramStateRef ProgramStateManager::getPersistentState(ProgramState &State) {

  llvm::FoldingSetNodeID ID;
  State.Profile(ID);
  void *InsertPos;

  if (ProgramState *I = StateSet.FindNodeOrInsertPos(ID, InsertPos))
    return I;

  ProgramState *newState = 0;
  if (!freeStates.empty()) {
    newState = freeStates.back();
    freeStates.pop_back();    
  }
  else {
    newState = (ProgramState*) Alloc.Allocate<ProgramState>();
  }
  new (newState) ProgramState(State);
  StateSet.InsertNode(newState, InsertPos);
  return newState;
}

ProgramStateRef ProgramState::makeWithStore(const StoreRef &store) const {
  ProgramState NewSt(*this);
  NewSt.setStore(store);
  return getStateManager().getPersistentState(NewSt);
}

void ProgramState::setStore(const StoreRef &newStore) {
  Store newStoreStore = newStore.getStore();
  if (newStoreStore)
    stateMgr->getStoreManager().incrementReferenceCount(newStoreStore);
  if (store)
    stateMgr->getStoreManager().decrementReferenceCount(store);
  store = newStoreStore;
}

//===----------------------------------------------------------------------===//
//  State pretty-printing.
//===----------------------------------------------------------------------===//

void ProgramState::print(raw_ostream &Out,
                         const char *NL, const char *Sep) const {
  // Print the store.
  ProgramStateManager &Mgr = getStateManager();
  Mgr.getStoreManager().print(getStore(), Out, NL, Sep);

  // Print out the environment.
  Env.print(Out, NL, Sep);

  // Print out the constraints.
  Mgr.getConstraintManager().print(this, Out, NL, Sep);

  // Print checker-specific data.
  Mgr.getOwningEngine()->printState(Out, this, NL, Sep);
}

void ProgramState::printDOT(raw_ostream &Out) const {
  print(Out, "\\l", "\\|");
}

void ProgramState::dump() const {
  print(llvm::errs());
}

void ProgramState::printTaint(raw_ostream &Out,
                              const char *NL, const char *Sep) const {
  TaintMapImpl TM = get<TaintMap>();

  if (!TM.isEmpty())
    Out <<"Tainted Symbols:" << NL;

  for (TaintMapImpl::iterator I = TM.begin(), E = TM.end(); I != E; ++I) {
    Out << I->first << " : " << I->second << NL;
  }
}

void ProgramState::dumpTaint() const {
  printTaint(llvm::errs());
}

//===----------------------------------------------------------------------===//
// Generic Data Map.
//===----------------------------------------------------------------------===//

void *const* ProgramState::FindGDM(void *K) const {
  return GDM.lookup(K);
}

void*
ProgramStateManager::FindGDMContext(void *K,
                               void *(*CreateContext)(llvm::BumpPtrAllocator&),
                               void (*DeleteContext)(void*)) {

  std::pair<void*, void (*)(void*)>& p = GDMContexts[K];
  if (!p.first) {
    p.first = CreateContext(Alloc);
    p.second = DeleteContext;
  }

  return p.first;
}

ProgramStateRef ProgramStateManager::addGDM(ProgramStateRef St, void *Key, void *Data){
  ProgramState::GenericDataMap M1 = St->getGDM();
  ProgramState::GenericDataMap M2 = GDMFactory.add(M1, Key, Data);

  if (M1 == M2)
    return St;

  ProgramState NewSt = *St;
  NewSt.GDM = M2;
  return getPersistentState(NewSt);
}

ProgramStateRef ProgramStateManager::removeGDM(ProgramStateRef state, void *Key) {
  ProgramState::GenericDataMap OldM = state->getGDM();
  ProgramState::GenericDataMap NewM = GDMFactory.remove(OldM, Key);

  if (NewM == OldM)
    return state;

  ProgramState NewState = *state;
  NewState.GDM = NewM;
  return getPersistentState(NewState);
}

void ScanReachableSymbols::anchor() { }

bool ScanReachableSymbols::scan(nonloc::CompoundVal val) {
  for (nonloc::CompoundVal::iterator I=val.begin(), E=val.end(); I!=E; ++I)
    if (!scan(*I))
      return false;

  return true;
}

bool ScanReachableSymbols::scan(const SymExpr *sym) {
  unsigned &isVisited = visited[sym];
  if (isVisited)
    return true;
  isVisited = 1;
  
  if (!visitor.VisitSymbol(sym))
    return false;
  
  // TODO: should be rewritten using SymExpr::symbol_iterator.
  switch (sym->getKind()) {
    case SymExpr::RegionValueKind:
    case SymExpr::ConjuredKind:
    case SymExpr::DerivedKind:
    case SymExpr::ExtentKind:
    case SymExpr::MetadataKind:
      break;
    case SymExpr::CastSymbolKind:
      return scan(cast<SymbolCast>(sym)->getOperand());
    case SymExpr::SymIntKind:
      return scan(cast<SymIntExpr>(sym)->getLHS());
    case SymExpr::IntSymKind:
      return scan(cast<IntSymExpr>(sym)->getRHS());
    case SymExpr::SymSymKind: {
      const SymSymExpr *x = cast<SymSymExpr>(sym);
      return scan(x->getLHS()) && scan(x->getRHS());
    }
  }
  return true;
}

bool ScanReachableSymbols::scan(SVal val) {
  if (loc::MemRegionVal *X = dyn_cast<loc::MemRegionVal>(&val))
    return scan(X->getRegion());

  if (nonloc::LocAsInteger *X = dyn_cast<nonloc::LocAsInteger>(&val))
    return scan(X->getLoc());

  if (SymbolRef Sym = val.getAsSymbol())
    return scan(Sym);

  if (const SymExpr *Sym = val.getAsSymbolicExpression())
    return scan(Sym);

  if (nonloc::CompoundVal *X = dyn_cast<nonloc::CompoundVal>(&val))
    return scan(*X);

  return true;
}

bool ScanReachableSymbols::scan(const MemRegion *R) {
  if (isa<MemSpaceRegion>(R))
    return true;
  
  unsigned &isVisited = visited[R];
  if (isVisited)
    return true;
  isVisited = 1;
  
  
  if (!visitor.VisitMemRegion(R))
    return false;

  // If this is a symbolic region, visit the symbol for the region.
  if (const SymbolicRegion *SR = dyn_cast<SymbolicRegion>(R))
    if (!visitor.VisitSymbol(SR->getSymbol()))
      return false;

  // If this is a subregion, also visit the parent regions.
  if (const SubRegion *SR = dyn_cast<SubRegion>(R))
    if (!scan(SR->getSuperRegion()))
      return false;

  // Regions captured by a block are also implicitly reachable.
  if (const BlockDataRegion *BDR = dyn_cast<BlockDataRegion>(R)) {
    BlockDataRegion::referenced_vars_iterator I = BDR->referenced_vars_begin(),
                                              E = BDR->referenced_vars_end();
    for ( ; I != E; ++I) {
      if (!scan(I.getCapturedRegion()))
        return false;
    }
  }

  // Now look at the binding to this region (if any).
  if (!scan(state->getSValAsScalarOrLoc(R)))
    return false;

  // Now look at the subregions.
  if (!SRM.get())
    SRM.reset(state->getStateManager().getStoreManager().
                                           getSubRegionMap(state->getStore()));

  return SRM->iterSubRegions(R, *this);
}

bool ProgramState::scanReachableSymbols(SVal val, SymbolVisitor& visitor) const {
  ScanReachableSymbols S(this, visitor);
  return S.scan(val);
}

bool ProgramState::scanReachableSymbols(const SVal *I, const SVal *E,
                                   SymbolVisitor &visitor) const {
  ScanReachableSymbols S(this, visitor);
  for ( ; I != E; ++I) {
    if (!S.scan(*I))
      return false;
  }
  return true;
}

bool ProgramState::scanReachableSymbols(const MemRegion * const *I,
                                   const MemRegion * const *E,
                                   SymbolVisitor &visitor) const {
  ScanReachableSymbols S(this, visitor);
  for ( ; I != E; ++I) {
    if (!S.scan(*I))
      return false;
  }
  return true;
}

ProgramStateRef ProgramState::addTaint(const Stmt *S,
                                           const LocationContext *LCtx,
                                           TaintTagType Kind) const {
  if (const Expr *E = dyn_cast_or_null<Expr>(S))
    S = E->IgnoreParens();

  SymbolRef Sym = getSVal(S, LCtx).getAsSymbol();
  if (Sym)
    return addTaint(Sym, Kind);

  const MemRegion *R = getSVal(S, LCtx).getAsRegion();
  addTaint(R, Kind);

  // Cannot add taint, so just return the state.
  return this;
}

ProgramStateRef ProgramState::addTaint(const MemRegion *R,
                                           TaintTagType Kind) const {
  if (const SymbolicRegion *SR = dyn_cast_or_null<SymbolicRegion>(R))
    return addTaint(SR->getSymbol(), Kind);
  return this;
}

ProgramStateRef ProgramState::addTaint(SymbolRef Sym,
                                           TaintTagType Kind) const {
  // If this is a symbol cast, remove the cast before adding the taint. Taint
  // is cast agnostic.
  while (const SymbolCast *SC = dyn_cast<SymbolCast>(Sym))
    Sym = SC->getOperand();

  ProgramStateRef NewState = set<TaintMap>(Sym, Kind);
  assert(NewState);
  return NewState;
}

bool ProgramState::isTainted(const Stmt *S, const LocationContext *LCtx,
                             TaintTagType Kind) const {
  if (const Expr *E = dyn_cast_or_null<Expr>(S))
    S = E->IgnoreParens();

  SVal val = getSVal(S, LCtx);
  return isTainted(val, Kind);
}

bool ProgramState::isTainted(SVal V, TaintTagType Kind) const {
  if (const SymExpr *Sym = V.getAsSymExpr())
    return isTainted(Sym, Kind);
  if (const MemRegion *Reg = V.getAsRegion())
    return isTainted(Reg, Kind);
  return false;
}

bool ProgramState::isTainted(const MemRegion *Reg, TaintTagType K) const {
  if (!Reg)
    return false;

  // Element region (array element) is tainted if either the base or the offset
  // are tainted.
  if (const ElementRegion *ER = dyn_cast<ElementRegion>(Reg))
    return isTainted(ER->getSuperRegion(), K) || isTainted(ER->getIndex(), K);

  if (const SymbolicRegion *SR = dyn_cast<SymbolicRegion>(Reg))
    return isTainted(SR->getSymbol(), K);

  if (const SubRegion *ER = dyn_cast<SubRegion>(Reg))
    return isTainted(ER->getSuperRegion(), K);

  return false;
}

bool ProgramState::isTainted(SymbolRef Sym, TaintTagType Kind) const {
  if (!Sym)
    return false;
  
  // Traverse all the symbols this symbol depends on to see if any are tainted.
  bool Tainted = false;
  for (SymExpr::symbol_iterator SI = Sym->symbol_begin(), SE =Sym->symbol_end();
       SI != SE; ++SI) {
    assert(isa<SymbolData>(*SI));
    const TaintTagType *Tag = get<TaintMap>(*SI);
    Tainted = (Tag && *Tag == Kind);

    // If this is a SymbolDerived with a tainted parent, it's also tainted.
    if (const SymbolDerived *SD = dyn_cast<SymbolDerived>(*SI))
      Tainted = Tainted || isTainted(SD->getParentSymbol(), Kind);

    // If memory region is tainted, data is also tainted.
    if (const SymbolRegionValue *SRV = dyn_cast<SymbolRegionValue>(*SI))
      Tainted = Tainted || isTainted(SRV->getRegion(), Kind);

    // If If this is a SymbolCast from a tainted value, it's also tainted.
    if (const SymbolCast *SC = dyn_cast<SymbolCast>(*SI))
      Tainted = Tainted || isTainted(SC->getOperand(), Kind);

    if (Tainted)
      return true;
  }
  
  return Tainted;
}
