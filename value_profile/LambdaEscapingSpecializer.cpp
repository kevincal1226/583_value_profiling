#include "LambdaEscapingSpecializer.hpp"

#include <set>

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

using namespace llvm;

namespace lambdaopt {

// Backwards-scan to find store to the capture field in the same block
// (unused right now but kept for completeness / experimentation).
static StoreInst *findCaptureStoreForCall(CallInst *CI, StructType *ClosureTy, const LambdaCaptureKey &Key) {
    if (!CI || !ClosureTy)
        return nullptr;

    BasicBlock *BB = CI->getParent();
    if (!BB)
        return nullptr;

    if (CI->arg_empty())
        return nullptr;

    Value *ThisArg = CI->getArgOperand(0);
    Value *ThisBase = stripPointerCasts(ThisArg);

    for (auto It = BasicBlock::iterator(CI); It != BB->begin();) {
        --It;
        Instruction &I = *It;

        auto *SI = dyn_cast<StoreInst>(&I);
        if (!SI)
            continue;

        auto *GEP = dyn_cast<GetElementPtrInst>(stripPointerCasts(SI->getPointerOperand()));
        if (!GEP)
            continue;

        if (GEP->getSourceElementType() != ClosureTy)
            continue;

        if (GEP->getNumIndices() < 2)
            continue;

        auto IdxIt = GEP->idx_begin();
        auto *Idx0 = dyn_cast<ConstantInt>(IdxIt->get());
        auto *Idx1 = dyn_cast<ConstantInt>((++IdxIt)->get());
        if (!Idx0 || !Idx1)
            continue;

        if (Idx0->getSExtValue() != 0)
            continue;
        if (Idx1->getZExtValue() != Key.FieldIndex)
            continue;

        Value *GEPBase = stripPointerCasts(GEP->getPointerOperand());
        if (GEPBase != ThisBase)
            continue;

        return SI;
    }

    return nullptr;
}

// Depth-first recursive search for a store to Ptr across CFG (unused,
// but kept from original code).
static StoreInst *findStoreRecursive(Value *Ptr, BasicBlock *StartBB, Instruction *Before,
                                     std::set<BasicBlock *> &Visited) {
    if (!StartBB || Visited.count(StartBB))
        return nullptr;
    Visited.insert(StartBB);

    bool CheckBefore = (Before && Before->getParent() == StartBB);

    for (auto it = StartBB->rbegin(); it != StartBB->rend(); ++it) {
        Instruction &I = *it;
        if (CheckBefore && &I == Before)
            break;

        if (auto *SI = dyn_cast<StoreInst>(&I)) {
            if (stripPointerCasts(SI->getPointerOperand()) == stripPointerCasts(Ptr)) {
                return SI;
            }
        }
    }

    for (BasicBlock *Pred : predecessors(StartBB)) {
        if (StoreInst *S = findStoreRecursive(Ptr, Pred, nullptr, Visited))
            return S;
    }

    return nullptr;
}

// Find the *last* store to the given closure field in the block, before CI.
static StoreInst *findLastStoreToClosureFieldBefore(CallBase *CB, StructType *ClosureTy, const LambdaCaptureKey &Key) {
    if (!CB || !ClosureTy)
        return nullptr;

    BasicBlock *BB = CB->getParent();
    if (!BB)
        return nullptr;

    StoreInst *LastStore = nullptr;

    for (Instruction &I : *BB) {
        if (&I == CB)
            break;

        auto *SI = dyn_cast<StoreInst>(&I);
        if (!SI)
            continue;

        Value *Ptr = SI->getPointerOperand();
        auto *GEP = dyn_cast<GetElementPtrInst>(stripPointerCasts(Ptr));
        if (!GEP)
            continue;

        if (GEP->getSourceElementType() != ClosureTy)
            continue;

        if (GEP->getNumIndices() < 2)
            continue;

        auto idxIt = GEP->idx_begin();
        auto *Idx0 = dyn_cast<ConstantInt>(idxIt->get());
        auto *Idx1 = dyn_cast<ConstantInt>((++idxIt)->get());
        if ((Idx0 == nullptr) || (Idx1 == nullptr))
            continue;

        if (Idx0->getSExtValue() != 0)
            continue;
        if (Idx1->getZExtValue() != Key.FieldIndex)
            continue;

        LastStore = SI;
    }

    return LastStore;
}

// Clone an arbitrary function (e.g., the std::function helper / ctor / invoker)
// and specialize loads from the closure field to the hot constant.
//
// We name it based on the original function name, closure type, field index,
// and hot value so we can reuse the clone if we see multiple call sites.
static Function *cloneAndSpecializeForCapture(Function *OrigF, StructType *ClosureTy, const LambdaCaptureKey &Key,
                                              const CaptureProfile &Prof) {
    if (!OrigF || OrigF->isDeclaration())
        return nullptr;

    Module *M = OrigF->getParent();
    if (!M)
        return nullptr;

    // Deterministic name so we can reuse a clone if it already exists.
    std::string NewName = OrigF->getName().str();
    NewName += ".lambda_hot.";
    NewName += ClosureTy->getName().str();
    NewName += ".field";
    NewName += std::to_string(Key.FieldIndex);
    NewName += ".value";
    NewName += std::to_string(Prof.HotValue);

    if (Function *Existing = M->getFunction(NewName)) {
        return Existing;
    }

    FunctionType *FTy = OrigF->getFunctionType();
    Function *NewF = Function::Create(FTy, OrigF->getLinkage(), NewName, M);

    // Preserve calling convention and attributes.
    NewF->setCallingConv(OrigF->getCallingConv());
    NewF->copyAttributesFrom(OrigF);
    NewF->removeFnAttr(Attribute::NoInline);
    NewF->addFnAttr(Attribute::AlwaysInline);

    ValueToValueMapTy VMap;
    {
        auto A = OrigF->arg_begin();
        auto B = NewF->arg_begin();
        for (; A != OrigF->arg_end(); ++A, ++B) {
            B->setName(A->getName());
            VMap[&*A] = &*B;
        }
    }

    SmallVector<ReturnInst *, 8> Returns;
    CloneFunctionInto(NewF, OrigF, VMap, CloneFunctionChangeType::LocalChangesOnly, Returns);

    // Now specialize loads from closure.field[Key.FieldIndex] to Prof.HotValue.
    bool Specialized = specializeCaptureInClone(NewF, ClosureTy, Key, Prof);
    if (!Specialized) {

        errs() << "[lambda-opt] cloneAndSpecializeForCapture: no "
                  "matching loads in clone '"
               << NewF->getName() << "'; erasing.\n";
        NewF->eraseFromParent();
        return nullptr;
    }

    errs() << "[lambda-opt] Escaping: created hot specialization '" << NewF->getName() << "' from '" << OrigF->getName()
           << "' for lambda '" << Key.Name << "', field " << Key.FieldIndex << " = " << Prof.HotValue << "\n";

    return NewF;
}

// Ensure we have a hot clone of the lambda operator with the capture baked in.
static Function *getOrCreateHotLambdaOperator(Module &M, StructType *ClosureTy, const LambdaCaptureKey &Key,
                                              const CaptureProfile &Prof) {
    Function *OriginalOp = findLambdaOperatorFunc(M, ClosureTy);
    if (!OriginalOp)
        return nullptr;

    std::string HotName = OriginalOp->getName().str();
    HotName += ".hot.field";
    HotName += std::to_string(Key.FieldIndex);
    HotName += ".value";
    HotName += std::to_string(Prof.HotValue);

    if (Function *Existing = M.getFunction(HotName))
        return Existing;

    Function *HotClone = cloneLambdaOperator(OriginalOp, Key, Prof);
    if (!HotClone)
        return nullptr;

    bool Specialized = specializeCaptureInClone(HotClone, ClosureTy, Key, Prof);
    if (!Specialized) {
        errs() << "[lambda-opt] Escaping: hot lambda operator clone had no matching loads: " << HotClone->getName()
               << "\n";
        HotClone->eraseFromParent();
        return nullptr;
    }

    HotClone->removeFnAttr(Attribute::NoInline);
    HotClone->addFnAttr(Attribute::AlwaysInline);
    return HotClone;
}

static StructType *findFunctionImplStruct(Module &M, StructType *ClosureTy) {
    for (StructType *ST : M.getIdentifiedStructTypes()) {
        if (!ST || ST->isOpaque())
            continue;
        if (!ST->getName().contains("__function::__func"))
            continue;
        if (ST->getNumElements() >= 2 && ST->getElementType(1) == ClosureTy)
            return ST;
    }
    return nullptr;
}

static StructType *findStdFunctionStruct(Module &M) {
    for (StructType *ST : M.getIdentifiedStructTypes()) {
        if (!ST || ST->isOpaque())
            continue;
        if (ST->getName().contains("class.std::__1::function"))
            return ST;
    }
    return nullptr;
}

// Find the std::function __call thunk whose second field is the lambda closure.
static Function *findStdFunctionCallThunk(Module &M, StructType *ClosureTy) {
    StructType *FuncImplTy = findFunctionImplStruct(M, ClosureTy);
    for (Function &F : M) {
        if (F.isDeclaration())
            continue;

        if (!F.getName().contains("__function6__func"))
            continue;
        if (!F.getName().contains("clE")) // call operator entry in the vtable
            continue;

        if (F.arg_size() < 2)
            continue;

        if (FuncImplTy) {
            bool MentionsImpl = false;
            for (Instruction &I : instructions(F)) {
                if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
                    if (GEP->getSourceElementType() == FuncImplTy) {
                        MentionsImpl = true;
                        break;
                    }
                }
            }
            if (!MentionsImpl)
                continue;
        }

        return &F;
    }
    return nullptr;
}

// The vtable constant that references the given __call thunk.
static GlobalVariable *findVTableForCallThunk(Module &M, Function *CallThunk) {
    if (!CallThunk)
        return nullptr;

    for (GlobalVariable &GV : M.globals()) {
        if (!GV.hasInitializer())
            continue;
        if (!GV.getName().contains("__function6__func"))
            continue;

        auto *InitStruct = dyn_cast<ConstantStruct>(GV.getInitializer());
        if (!InitStruct || InitStruct->getNumOperands() == 0)
            continue;

        auto *Arr = dyn_cast<ConstantArray>(InitStruct->getOperand(0));
        if (!Arr)
            continue;

        for (unsigned i = 0; i < Arr->getNumOperands(); ++i) {
            Constant *Op = Arr->getOperand(i);
            if (auto *CE = dyn_cast<ConstantExpr>(Op)) {
                if (CE->getOpcode() == Instruction::BitCast)
                    Op = CE->getOperand(0);
            }

            if (Op == CallThunk)
                return &GV;
        }
    }

    return nullptr;
}

static GlobalVariable *cloneVTableWithHotCall(GlobalVariable *OrigVT, Function *HotCall, const LambdaCaptureKey &Key,
                                              const CaptureProfile &Prof) {
    if (!OrigVT || !HotCall)
        return nullptr;

    std::string NewName = OrigVT->getName().str();
    NewName += ".lambda_hot.field";
    NewName += std::to_string(Key.FieldIndex);
    NewName += ".value";
    NewName += std::to_string(Prof.HotValue);

    if (GlobalVariable *Existing = OrigVT->getParent()->getNamedGlobal(NewName))
        return Existing;

    auto *InitStruct = dyn_cast<ConstantStruct>(OrigVT->getInitializer());
    if (!InitStruct || InitStruct->getNumOperands() == 0)
        return nullptr;

    auto *Arr = dyn_cast<ConstantArray>(InitStruct->getOperand(0));
    if (!Arr || Arr->getNumOperands() <= 8)
        return nullptr;

    SmallVector<Constant *, 11> Elts;
    Elts.reserve(Arr->getNumOperands());
    for (unsigned i = 0; i < Arr->getNumOperands(); ++i) {
        if (i == 8) {
            Constant *HotPtr = ConstantExpr::getBitCast(HotCall, Arr->getType()->getElementType());
            Elts.push_back(HotPtr);
        }
        else {
            Elts.push_back(Arr->getOperand(i));
        }
    }

    auto *NewArr = ConstantArray::get(Arr->getType(), Elts);
    auto *NewInit = ConstantStruct::get(cast<StructType>(InitStruct->getType()), NewArr);

    auto *NewGV = new GlobalVariable(*OrigVT->getParent(), NewInit->getType(), /*isConstant*/ true,
                                     OrigVT->getLinkage(), NewInit, NewName, /*InsertBefore*/ nullptr,
                                     OrigVT->getThreadLocalMode(), OrigVT->getAddressSpace());
    NewGV->copyAttributesFrom(OrigVT);
    NewGV->setInitializer(NewInit);
    return NewGV;
}

static Constant *buildHotVTablePointer(GlobalVariable *VTableGV) {
    if (!VTableGV)
        return nullptr;
    auto *VTStructTy = dyn_cast<StructType>(VTableGV->getValueType());
    if (!VTStructTy || VTStructTy->getNumElements() == 0)
        return nullptr;
    auto *ArrTy = dyn_cast<ArrayType>(VTStructTy->getElementType(0));
    if (!ArrTy || ArrTy->getNumElements() <= 2)
        return nullptr;

    LLVMContext &Ctx = VTableGV->getContext();
    Constant *Zero = ConstantInt::get(Type::getInt32Ty(Ctx), 0);
    Constant *Two = ConstantInt::get(Type::getInt32Ty(Ctx), 2);
    SmallVector<Constant *, 3> Indices = {Zero, Zero, Two};
    return ConstantExpr::getInBoundsGetElementPtr(VTableGV->getValueType(), VTableGV, Indices);
}

// Build a hot __call thunk that forwards directly to the hot lambda operator.
static Function *buildHotFunctionCallThunk(Function *OrigCall, Function *HotLambdaOp, StructType *ClosureTy,
                                           const LambdaCaptureKey &Key, const CaptureProfile &Prof) {
    if (!OrigCall || !HotLambdaOp)
        return nullptr;

    Module *M = OrigCall->getParent();
    LLVMContext &Ctx = M->getContext();

    StructType *FuncImplTy = findFunctionImplStruct(*M, ClosureTy);
    if (!FuncImplTy || FuncImplTy->isOpaque() || FuncImplTy->getNumElements() < 2)
        return nullptr;

    std::string NewName = OrigCall->getName().str();
    NewName += ".lambda_hot.field";
    NewName += std::to_string(Key.FieldIndex);
    NewName += ".value";
    NewName += std::to_string(Prof.HotValue);

    if (Function *Existing = M->getFunction(NewName))
        return Existing;

    Function *Thunk = Function::Create(OrigCall->getFunctionType(), OrigCall->getLinkage(), NewName, M);
    Thunk->setCallingConv(OrigCall->getCallingConv());
    Thunk->copyAttributesFrom(OrigCall);
    Thunk->removeFnAttr(Attribute::NoInline);
    Thunk->addFnAttr(Attribute::AlwaysInline);

    BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", Thunk);
    IRBuilder<> B(Entry);

    auto ArgIt = Thunk->arg_begin();
    Argument *ThisArg = ArgIt++;

    // Extract the lambda object out of the __function::__func wrapper.
    Value *LambdaPtr = B.CreateStructGEP(FuncImplTy, ThisArg, 1, "lambda.obj");

    SmallVector<Value *, 4> Args;
    Args.push_back(LambdaPtr);

    unsigned OpArgIndex = 1;
    for (; ArgIt != Thunk->arg_end() && OpArgIndex < HotLambdaOp->arg_size(); ++ArgIt, ++OpArgIndex) {
        Argument *ArgPtr = &*ArgIt;
        Type *LoadTy = HotLambdaOp->getFunctionType()->getParamType(OpArgIndex);
        Value *Val = nullptr;
        if (LoadTy) {
            unsigned AS = 0;
            if (auto *PT = dyn_cast<PointerType>(ArgPtr->getType()))
                AS = PT->getAddressSpace();
            Value *Ptr = ArgPtr;
            if (!ArgPtr->getType()->isPointerTy()) {
                Ptr = B.CreateBitCast(ArgPtr, PointerType::get(Ctx, AS));
            }
            Val = B.CreateLoad(LoadTy, Ptr, "arg" + std::to_string(OpArgIndex));
        }
        Args.push_back(Val ? Val : ArgPtr);
    }

    Value *Res = B.CreateCall(HotLambdaOp, Args, "hot.call");
    B.CreateRet(Res);
    return Thunk;
}

// After construction, patch the std::function object's vptr to the hot vtable.
static void patchFunctionVTable(IRBuilder<> &B, Value *FunctionObjPtr, Constant *HotVPtrConst) {
    if (!FunctionObjPtr || !HotVPtrConst)
        return;

    Module *M = B.GetInsertBlock()->getModule();
    StructType *FuncStructTy = findStdFunctionStruct(*M);
    if (!FuncStructTy || FuncStructTy->isOpaque() || FuncStructTy->getNumElements() == 0)
        return;

    auto *ValueFuncTy = dyn_cast<StructType>(FuncStructTy->getElementType(0));
    if (!ValueFuncTy || ValueFuncTy->isOpaque() || ValueFuncTy->getNumElements() < 2)
        return;

    LLVMContext &Ctx = B.getContext();
    // Field 1 of __value_func is the pointer to the erased callable (__base).
    Value *ValueFuncPtr = B.CreateStructGEP(FuncStructTy, FunctionObjPtr, 0, "value.func");
    Value *BasePtrPtr = B.CreateStructGEP(ValueFuncTy, ValueFuncPtr, 1, "value.func.ptr");
    Value *BasePtr = B.CreateLoad(PointerType::get(Ctx, 0), BasePtrPtr, "callable.ptr");

    StructType *BaseTy = StructType::getTypeByName(Ctx, "class.std::__1::__function::__base");
    if (!BaseTy)
        BaseTy = StructType::getTypeByName(Ctx, "class.std::__1::__function::__baseIFddEEE");
    if (!BaseTy)
        return;

    Value *BaseObj = B.CreateBitCast(BasePtr, PointerType::get(Ctx, BasePtr->getType()->getPointerAddressSpace()));
    Value *VPtrSlot = B.CreateStructGEP(BaseTy, BaseObj, 0, "vptr.slot");
    B.CreateStore(HotVPtrConst, VPtrSlot);
}

// Build a lightweight wrapper that forces one argument to the hot constant and
// forwards to OrigCallee. This guarantees the hot path calls a distinct
// function even if we could not specialize loads inside OrigCallee.
static Function *buildHotArgWrapper(Function *OrigCallee, StructType *ClosureTy, const LambdaCaptureKey &Key,
                                    const CaptureProfile &Prof, int OverrideArgIndex, bool IsAggregateArg) {
    if (!OrigCallee)
        return nullptr;

    Module *M = OrigCallee->getParent();
    if (!M)
        return nullptr;

    std::string NewName = OrigCallee->getName().str();
    NewName += ".lambda_hot_wrapper.";
    NewName += ClosureTy->getName().str();
    NewName += ".field";
    NewName += std::to_string(Key.FieldIndex);
    NewName += ".value";
    NewName += std::to_string(Prof.HotValue);

    if (Function *Existing = M->getFunction(NewName))
        return Existing;

    FunctionType *FTy = OrigCallee->getFunctionType();
    Function *Wrapper = Function::Create(FTy, OrigCallee->getLinkage(), NewName, M);
    Wrapper->copyAttributesFrom(OrigCallee);
    Wrapper->removeFnAttr(Attribute::NoInline);
    Wrapper->addFnAttr(Attribute::AlwaysInline);

    BasicBlock *Entry = BasicBlock::Create(M->getContext(), "entry", Wrapper);
    IRBuilder<> B(Entry);

    SmallVector<Value *, 8> Args;
    Args.reserve(Wrapper->arg_size());

    for (unsigned i = 0; i < Wrapper->arg_size(); ++i) {
        Argument &Arg = *Wrapper->getArg(i);
        if (static_cast<int>(i) == OverrideArgIndex) {
            if (IsAggregateArg) {
                Value *Agg = &Arg;
                Type *AggTy = Agg->getType();
                Value *HotAgg = Agg;

                // For aggregates, attempt to insert the hot constant into the
                // requested field index if it is an array/struct of scalars.
                if (auto *ArrTy = dyn_cast<ArrayType>(AggTy)) {
                    Type *ElemTy = ArrTy->getElementType();
                    Value *ElemConst = nullptr;
                    if (auto *ElemInt = dyn_cast<IntegerType>(ElemTy))
                        ElemConst = ConstantInt::get(ElemInt, Prof.HotValue, /*isSigned=*/true);
                    else if (ElemTy->isFloatingPointTy())
                        ElemConst = ConstantFP::get(ElemTy, static_cast<double>(Prof.HotValue));

                    if (ElemConst && Key.FieldIndex < ArrTy->getNumElements()) {
                        HotAgg = B.CreateInsertValue(Agg, ElemConst, {Key.FieldIndex}, Agg->getName() + ".hot");
                    }
                }
                // If we can't meaningfully rewrite, just forward the original aggregate.
                Args.push_back(HotAgg);
            }
            else {
                Type *ArgTy = Arg.getType();
                Value *ConstArg = nullptr;
                if (auto *IntTy = dyn_cast<IntegerType>(ArgTy))
                    ConstArg = ConstantInt::get(IntTy, Prof.HotValue, /*isSigned=*/true);
                else if (ArgTy->isFloatingPointTy())
                    ConstArg = ConstantFP::get(ArgTy, static_cast<double>(Prof.HotValue));
                if (ConstArg)
                    Args.push_back(ConstArg);
                else
                    Args.push_back(&Arg);
            }
        }
        else {
            Args.push_back(&Arg);
        }
    }

    Value *Ret = nullptr;
    if (OrigCallee->getReturnType()->isVoidTy()) {
        B.CreateCall(OrigCallee, Args);
        B.CreateRetVoid();
    }
    else {
        Ret = B.CreateCall(OrigCallee, Args, "hot.call");
        B.CreateRet(Ret);
    }

    return Wrapper;
}

// Rewrite a std::function ctor call so we *branch once* on the capture value:
//
//   if (x == HotValue)
//       std::function(..., HotValue);
//   else
//       std::function(..., x);
//
// After this, the returned std::function is either hot or cold, but not both.
// Rewrite a std::function ctor call so we *branch once* on the capture value:
//
//   if (x == HotValue)
//       std::function(..., HotValue);
//   else
//       std::function(..., x);
//
// Supports two patterns:
//  1) Scalar integer capture passed as zext(load(field))
//  2) Closure passed by-value as an aggregate ([N x i64] load from the closure)
static bool rewriteStdFunctionConstructionWithBranch(CallBase *CB, StructType *ClosureTy, const LambdaCaptureKey &Key,
                                                     const CaptureProfile &Prof) {
    if (!CB || !ClosureTy)
        return false;

    Module *M = CB->getFunction()->getParent();

    // 0) We need the original callee and a hot specialization of it.
    Function *OrigCallee = CB->getCalledFunction();
    if (!OrigCallee)
        return false;
    if (OrigCallee->getName().contains(".lambda_hot."))
        return false;
    if (BasicBlock *BB = CB->getParent()) {
        StringRef BBName = BB->getName();
        if (BBName.contains("lambda.sf.hot") || BBName.contains("lambda.sf.cold"))
            return false; // don't rewrite the calls we just synthesized
    }

    Function *HotCallee = cloneAndSpecializeForCapture(OrigCallee, ClosureTy, Key, Prof);

    // Prepare hot lambda/operator plumbing so escaping std::function objects
    // dispatch to the specialized body on the hot path.
    Function *HotLambdaOp = getOrCreateHotLambdaOperator(*M, ClosureTy, Key, Prof);
    Function *StdFuncCallThunk = findStdFunctionCallThunk(*M, ClosureTy);
    GlobalVariable *StdFuncVTable = findVTableForCallThunk(*M, StdFuncCallThunk);
    Function *HotStdFuncCallThunk =
        buildHotFunctionCallThunk(StdFuncCallThunk, HotLambdaOp, ClosureTy, Key, Prof);
    GlobalVariable *HotVTable = cloneVTableWithHotCall(StdFuncVTable, HotStdFuncCallThunk, Key, Prof);
    Constant *HotVPtrConst = buildHotVTablePointer(HotVTable);
    // ---- Pattern A: scalar capture as zext(load(field)) ----
    int ScalarCaptureArgIndex = -1;
    ZExtInst *ZExt = nullptr;
    LoadInst *ScalarLoad = nullptr;
    GetElementPtrInst *ScalarLdGEP = nullptr;

    // ---- Pattern B: aggregate closure passed by-value ----
    int AggArgIndex = -1;
    LoadInst *AggLoad = nullptr; // e.g. [2 x i64] load
    Value *AggPtr = nullptr;

    for (unsigned i = 0; i < CB->arg_size(); ++i) {
        Value *Arg = CB->getArgOperand(i);

        // Pattern A: zext(load(gep(closure.field[fieldIndex])))
        if (auto *Z = dyn_cast<ZExtInst>(Arg)) {
            auto *L = dyn_cast<LoadInst>(Z->getOperand(0));
            if (!L)
                continue;

            Value *Ptr = L->getPointerOperand();
            auto *G = dyn_cast<GetElementPtrInst>(stripPointerCasts(Ptr));
            if (!G)
                continue;

            if (G->getSourceElementType() != ClosureTy)
                continue;
            if (G->getNumIndices() < 2)
                continue;

            auto idxIt = G->idx_begin();
            auto *Idx0 = dyn_cast<ConstantInt>(idxIt->get());
            auto *Idx1 = dyn_cast<ConstantInt>((++idxIt)->get());
            if (!Idx0 || !Idx1)
                continue;

            if (Idx0->getSExtValue() != 0)
                continue;
            if (Idx1->getZExtValue() != Key.FieldIndex)
                continue;

            ScalarCaptureArgIndex = static_cast<int>(i);
            ZExt = Z;
            ScalarLoad = L;
            ScalarLdGEP = G;
            (void)ScalarLdGEP;
            continue;
        }

        // Pattern B: whole closure loaded as [N x i64]
        if (auto *L = dyn_cast<LoadInst>(Arg)) {
            if (L->getType()->isArrayTy()) {
                // Quick sanity: only used by this call
                if (!L->hasOneUse() || L->user_back() != CB)
                    continue;

                // Optional: we could require the pointer to originate from
                // a GEP of ClosureTy, but in practice this pattern is
                // generated from %class.anon allocas.
                AggArgIndex = static_cast<int>(i);
                AggLoad = L;
                AggPtr = L->getPointerOperand();
                continue;
            }
        }
    }

    // If we matched neither pattern, this ctor is not in a form we handle.
    if (ScalarCaptureArgIndex < 0 && AggArgIndex < 0)
        return false;

    // If we could not build a specialized callee, synthesize a tiny wrapper
    // that forces the hot value into the ctor argument so the hot path calls a
    // distinct function body.
    if (!HotCallee) {
        int OverrideIdx = (ScalarCaptureArgIndex >= 0) ? ScalarCaptureArgIndex : AggArgIndex;
        bool IsAgg = (ScalarCaptureArgIndex < 0);
        HotCallee = buildHotArgWrapper(OrigCallee, ClosureTy, Key, Prof, OverrideIdx, IsAgg);
        if (!HotCallee)
            return false;
    }

    // 2) Find the store to that closure field earlier in the block: this gives us "x".
    StoreInst *Store = findLastStoreToClosureFieldBefore(CB, ClosureTy, Key);
    if (!Store) {
        llvm::errs() << "[lambda-opt] Escaping: could not find store to "
                        "closure field before std::function ctor; skipping.\n";
        return false;
    }

    Value *CaptureVal = Store->getValueOperand(); // this is "x"
    auto *CaptureTy = dyn_cast<IntegerType>(CaptureVal->getType());
    if (!CaptureTy) {
        llvm::errs() << "[lambda-opt] Escaping: non-integer capture value; "
                        "skipping.\n";
        return false;
    }

    ConstantInt *HotCapture = ConstantInt::get(CaptureTy, Prof.HotValue, /*isSigned=*/true);

    BasicBlock *OrigBB = CB->getParent();
    Function *ParentF = OrigBB->getParent();
    LLVMContext &Ctx = ParentF->getContext();

    // We need to handle both call and invoke sites.
    bool IsInvoke = isa<InvokeInst>(CB);

    BasicBlock *MergeBB = nullptr;
    BasicBlock *UnwindDest = nullptr;

    if (IsInvoke) {
        auto *II = cast<InvokeInst>(CB);
        UnwindDest = II->getUnwindDest();
        MergeBB = BasicBlock::Create(Ctx, "lambda.sf.merge", ParentF, II->getNormalDest());
    }
    else {
        MergeBB = OrigBB->splitBasicBlock(CB->getIterator(), "lambda.sf.merge");
    }

    // 3) Split block at the ctor call
    Instruction *OldTerm = OrigBB->getTerminator();
    IRBuilder<> CmpBuilder(OldTerm);

    // Branch on "x" (the value stored to the closure)
    Value *IsHot = CmpBuilder.CreateICmpEQ(CaptureVal, HotCapture, "lambda.sf.is_hot");

    // Create hot and cold blocks
    auto *HotBB = BasicBlock::Create(Ctx, "lambda.sf.hot", ParentF, MergeBB);
    auto *ColdBB = BasicBlock::Create(Ctx, "lambda.sf.cold", ParentF, MergeBB);

    // 4) Build arg lists for ctor (baseline: original args)
    SmallVector<Value *, 8> ColdArgs;
    ColdArgs.reserve(CB->arg_size());
    for (unsigned i = 0; i < CB->arg_size(); ++i)
        ColdArgs.push_back(CB->getArgOperand(i));

    SmallVector<Value *, 8> HotArgs = ColdArgs;

    // Pattern A (scalar): override ctor arg with hot constant (zext’d)
    if (ScalarCaptureArgIndex >= 0) {
        auto *CtorArgTy = dyn_cast<IntegerType>(ZExt->getType());
        if (!CtorArgTy) {
            llvm::errs() << "[lambda-opt] Escaping: unexpected non-int ctor "
                            "arg type in scalar pattern; skipping.\n";
            return false;
        }

        ConstantInt *HotCtorArg = ConstantInt::get(CtorArgTy, Prof.HotValue, /*isSigned=*/true);
        HotArgs[ScalarCaptureArgIndex] = HotCtorArg;
    }

    // NOTE: for Pattern B (aggregate), we will *not* directly reuse AggLoad.
    // Instead, we re-materialize loads in each branch after we possibly store
    // the hot capture into the closure. We will patch the args below.

    // 5) Cold path: original semantics (but we may rebuild aggregate arg)
    // Replace the original terminator with our guard once we've consumed CB.
    bool DetachedInvoke = false;
    if (IsInvoke) {
        CB->removeFromParent(); // detach so we can insert a branch
        DetachedInvoke = true;
    }
    else {
        OldTerm->eraseFromParent();
    }
    IRBuilder<> BrBuilder(OrigBB);
    BrBuilder.CreateCondBr(IsHot, HotBB, ColdBB);

    Value *ColdCall = nullptr;
    {
        IRBuilder<> B(ColdBB);

        // If we had an aggregate arg, rebuild the load *here*,
        // so we don't depend on the original AggLoad in OrigBB.
        if (AggArgIndex >= 0 && AggLoad && AggPtr) {
            Value *NewAggLoad = B.CreateLoad(AggLoad->getType(), AggPtr, AggLoad->getName() + ".sf.cold");
            ColdArgs[AggArgIndex] = NewAggLoad;
        }

        if (IsInvoke) {
            auto *II = cast<InvokeInst>(CB);
            ColdCall = B.CreateInvoke(OrigCallee, MergeBB, UnwindDest, ColdArgs, CB->getName() + ".cold");
        }
        else {
            ColdCall = B.CreateCall(OrigCallee, ColdArgs, CB->getName() + ".cold");
            B.CreateBr(MergeBB);
        }
    }

    // 6) Hot path:
    //    - overwrite closure field with HotValue (so any remaining loads see it)
    //    - call the *specialized* callee
    //    - retarget the vtable so std::function calls the hot operator()
    Value *HotCall = nullptr;
    BasicBlock *HotResultBB = HotBB;
    BasicBlock *HotInvokeCont = nullptr;
    {
        IRBuilder<> B(HotBB);

        // Overwrite the capture field with the hot value:
        Value *CapturePtr = Store->getPointerOperand();
        B.CreateStore(HotCapture, CapturePtr);

        // If aggregate arg, re-load it *after* the store, so it sees the hot
        // capture bit pattern in the closure.
        if (AggArgIndex >= 0 && AggLoad && AggPtr) {
            Value *NewAggLoad = B.CreateLoad(AggLoad->getType(), AggPtr, AggLoad->getName() + ".sf.hot");

            // Rewrite the specific capture slot to the constant so the callee
            // receives the baked-in value even if it copies the aggregate.
            if (auto *ArrTy = dyn_cast<ArrayType>(AggLoad->getType())) {
                Type *ElemTy = ArrTy->getElementType();
                Value *ElemConst = nullptr;
                if (auto *ElemInt = dyn_cast<IntegerType>(ElemTy)) {
                    ElemConst = ConstantInt::get(ElemInt, Prof.HotValue, /*isSigned=*/true);
                }
                else if (ElemTy->isFloatingPointTy()) {
                    ElemConst = ConstantFP::get(ElemTy, static_cast<double>(Prof.HotValue));
                }
                if (ElemConst) {
                    // Index sequence: [0, FieldIndex] for a simple array payload.
                    Value *HotAgg =
                        B.CreateInsertValue(NewAggLoad, ElemConst, {static_cast<unsigned>(Key.FieldIndex)},
                                            NewAggLoad->getName() + ".with_hot");
                    HotArgs[AggArgIndex] = HotAgg;
                }
                else {
                    HotArgs[AggArgIndex] = NewAggLoad;
                }
            }
            else {
                HotArgs[AggArgIndex] = NewAggLoad;
            }
        }

        if (IsInvoke) {
            auto *II = cast<InvokeInst>(CB);
            HotInvokeCont = BasicBlock::Create(Ctx, "lambda.sf.hot.cont", ParentF, MergeBB);
            HotCall = B.CreateInvoke(HotCallee, HotInvokeCont, UnwindDest, HotArgs, CB->getName() + ".hot");
            HotResultBB = HotInvokeCont;
        }
        else {
            HotCall = B.CreateCall(HotCallee, HotArgs, CB->getName() + ".hot");
        }
    }

    // If we have a hot vtable, install it before merging.
    if (HotVPtrConst) {
        IRBuilder<> PatchB(IsInvoke ? HotInvokeCont : HotBB);
        if (IsInvoke && HotInvokeCont) {
            // Insert after the invoke in the continuation block.
            auto IP = HotInvokeCont->getFirstInsertionPt();
            if (IP != HotInvokeCont->end())
                PatchB.SetInsertPoint(&*IP);
            else
                PatchB.SetInsertPoint(HotInvokeCont);
        }
        patchFunctionVTable(PatchB, CB->getArgOperand(0), HotVPtrConst);
        if (IsInvoke && HotInvokeCont) {
            PatchB.CreateBr(MergeBB);
        }
        else {
            PatchB.CreateBr(MergeBB);
        }
    }
    else if (IsInvoke && HotInvokeCont) {
        IRBuilder<> BranchB(HotInvokeCont);
        if (!HotInvokeCont->getTerminator())
            BranchB.CreateBr(MergeBB);
    }
    else if (!IsInvoke) {
        // For a normal call with no patching, ensure control still flows to the merge block.
        IRBuilder<> BranchB(HotBB);
        if (!HotBB->getTerminator())
            BranchB.CreateBr(MergeBB);
    }

    // 7) Merge ctor result if it is used (often it is not)
    if (!CB->getType()->isVoidTy()) {
        IRBuilder<> MB(MergeBB);
        auto InsertIt = MergeBB->getFirstNonPHIOrDbgOrAlloca();
        if (InsertIt != MergeBB->end())
            MB.SetInsertPoint(&*InsertIt);

        PHINode *PHI = MB.CreatePHI(CB->getType(), 2, CB->getName() + ".sf.sel");
        PHI->addIncoming(HotCall, HotResultBB);
        PHI->addIncoming(ColdCall, ColdBB);
        if (!CB->use_empty())
            CB->replaceAllUsesWith(PHI);
    }

    // For invoke, redirect the merge block to the original normal dest and
    // update incoming edges.
    if (IsInvoke) {
        auto *II = cast<InvokeInst>(CB);
        BasicBlock *NormalDest = II->getNormalDest();
        BranchInst::Create(NormalDest, MergeBB);

        // Any PHI nodes that expected OrigBB now expect MergeBB.
        for (PHINode &PN : NormalDest->phis()) {
            PN.replaceIncomingBlockWith(OrigBB, MergeBB);
        }
    }

    // 8) Clean up: original call + (optionally) original AggLoad
    if (AggLoad && AggLoad->use_empty())
        AggLoad->eraseFromParent();

    if (DetachedInvoke) {
        CB->dropAllReferences();
        delete CB;
    }
    else {
        CB->eraseFromParent();
    }

    llvm::errs() << "[lambda-opt] Escaping: branched std::function "
                    "construction for closure '"
                 << ClosureTy->getName() << "', field " << Key.FieldIndex << " on value == " << Prof.HotValue
                 << " using specialized callee '" << HotCallee->getName() << "'\n";

    return true;
}

bool EscapingLambdaSpecializer::run(Module &M) {
    bool Changed = false;

    for (auto &[Key, Prof] : Profiles) {
        // Skip noisy stdlib helper structs that show up in the profile.
        if (Key.Name.find("class.anon") == std::string::npos)
            continue;

        StructType *CT = findClosureType(M, Key);
        if (CT == nullptr) {
            continue;
        }

        for (Function &F : M) {
            for (BasicBlock &BB : F) {
                SmallVector<CallBase *, 8> Calls;
                for (Instruction &I : BB)
                    if (auto *CB = dyn_cast<CallBase>(&I))
                        Calls.push_back(CB);

                for (CallBase *CB : Calls) {
                    if (rewriteStdFunctionConstructionWithBranch(CB, CT, Key, Prof)) {
                        Changed = true;
                    }
                }
            }
        }
    }

    return Changed;
}

} // namespace lambdaopt
