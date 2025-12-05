// Pass that *detects* escaping lambdas and reports the operator() we think
// belongs to each profiled closure, then rewrites std::function construction
// to install a hot specialization when the capture matches the profiled value.

#include "llvm/ADT/DenseSet.h"
#include <string>

#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "LambdaCommon.hpp"
#include "LambdaEscapeOptimizer.hpp"

using namespace llvm;

namespace lambdaopt {

// ----------------- helpers -----------------

static std::string demangleName(StringRef Name) { return llvm::demangle(Name.str()); }

llvm::StructType *getLambdaStructFromArg(llvm::Value *Arg) {
    if (auto *ZExt = dyn_cast<ZExtInst>(Arg))
        Arg = ZExt->getOperand(0);

    auto *LI = dyn_cast<LoadInst>(Arg);
    if (!LI)
        return nullptr;
    Value *Ptr = LI->getPointerOperand();

    auto *GEP = dyn_cast<GetElementPtrInst>(Ptr);
    if (!GEP)
        return nullptr;

    Type *BaseTy = GEP->getSourceElementType();
    if (auto *ST = dyn_cast<StructType>(BaseTy)) {
        errs() << "[getLambdaStructFromArg] BaseTy = " << ST->getName() << "\n";
        return ST;
    }

    return nullptr;
}

Function *FindLambdaBody(Module &M, StructType *ClosureTy) {
    for (Function &F : M) {
        if (F.isDeclaration())
            continue;

        // Lambdas' operator() manglings usually contain "clE"
        if (!F.getName().contains("clE"))
            continue;

        bool UsesClosureGEP = false;

        for (BasicBlock &BB : F) {
            for (Instruction &I : BB) {
                if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
                    if (GEP->getSourceElementType() == ClosureTy) {
                        UsesClosureGEP = true;
                        break;
                    }
                }
            }
            if (UsesClosureGEP)
                break;
        }

        if (!UsesClosureGEP)
            continue;

        // Optionally, require a pointer first param just as a sanity check
        FunctionType *FTy = F.getFunctionType();
        if (FTy->getNumParams() == 0 || !FTy->getParamType(0)->isPointerTy())
            continue;

        return &F; // This is your lambda body
    }

    return nullptr;
}

static Function *cloneAndSpecializeLambdaBody(Function *LambdaBody, StructType *ClosureTy, int HotValue) {
    Module *M = LambdaBody->getParent();
    LLVMContext &Ctx = M->getContext();

    // Clone the function
    ValueToValueMapTy VMap;
    Function *Clone = CloneFunction(LambdaBody, VMap);
    Clone->setName(LambdaBody->getName() + ".hot");
    Clone->setLinkage(GlobalValue::InternalLinkage);

    ConstantInt *HotConst = ConstantInt::get(Type::getInt32Ty(Ctx), HotValue);

    SmallVector<Instruction *, 8> ToErase;

    // Look for loads of closure->field0 and replace with constant
    for (BasicBlock &BB : *Clone) {
        for (Instruction &I : BB) {
            auto *LI = dyn_cast<LoadInst>(&I);
            if (!LI)
                continue;

            Value *Ptr = LI->getPointerOperand();
            auto *GEP = dyn_cast<GetElementPtrInst>(Ptr);
            if (!GEP)
                continue;

            // Must be a GEP off the closure struct
            if (GEP->getSourceElementType() != ClosureTy)
                continue;

            // Expect indices: 0, 0 (field 0)
            if (GEP->getNumIndices() != 2)
                continue;

            auto It = GEP->idx_begin();
            auto *Idx0 = dyn_cast<ConstantInt>(&*It);
            auto *Idx1 = dyn_cast<ConstantInt>(&*(++It));
            if (!Idx0 || !Idx1)
                continue;
            if (!Idx0->isZero() || !Idx1->isZero())
                continue;

            // This is the capture load we want to specialize.
            LI->replaceAllUsesWith(HotConst);
            ToErase.push_back(LI);
            if (GEP->use_empty())
                ToErase.push_back(GEP);
        }
    }

    for (Instruction *I : ToErase)
        I->eraseFromParent();

    return Clone;
}

void handleStdFunctionCtor(Module &M, CallInst *CI, CaptureProfileMap Profiles) {
    if (CI->arg_size() < 2)
        return;

    Value *CaptureArg = CI->getArgOperand(1); // i64 %coerce.val.ii
    if (auto *ST = getLambdaStructFromArg(CaptureArg)) {
        errs() << "Lambda closure struct: " << ST->getName() << "\n";

        auto name = ST->getName().str();
        LambdaCaptureKey LambdaCaptureKey{name, 0};

        if (!Profiles.contains(LambdaCaptureKey)) {
            errs() << "Did not find profile data for " << name << "\n";
            return;
        }

        auto data = Profiles[LambdaCaptureKey];

        Profiles.erase(LambdaCaptureKey);

        errs() << "Found profile data for " << name << "\n";

        Function *LambdaBody = FindLambdaBody(*CI->getModule(), ST);
        if (!LambdaBody) {
            errs() << "ERROR: No lambda body found for closure: " << ST->getName() << "\n";
            return;
        }
        errs() << "Lambda body: " << LambdaBody->getName() << "\n";

        int HotValue = data.HotValue;
        Function *HotClone = cloneAndSpecializeLambdaBody(LambdaBody, ST, HotValue);
        errs() << "Created hot clone: " << HotClone->getName() << "\n";

        Module &Mod = *CI->getModule();
        LLVMContext &Ctx = Mod.getContext();

        // ====== NEW: rewrite this ctor call to use HotClone on the hot path ======

        Function *ParentF = CI->getFunction();

        // Only support: returning void + ctor result unused (make_lambdai case)
        if (!ParentF->getReturnType()->isVoidTy()) {
            errs() << "lambdaopt: non-void function; skipping rewrite.\n";
            return;
        }
        if (!CI->use_empty()) {
            errs() << "lambdaopt: ctor result has uses; skipping rewrite.\n";
            return;
        }

        // === Create a trampoline: int (int) that calls the hot clone ===

        Type *IntTy = Type::getInt32Ty(Ctx);
        FunctionType *TrampTy = FunctionType::get(IntTy, {IntTy}, false);

        std::string TrampName = (HotClone->getName() + ".fp_tramp").str();
        Function *Tramp = Mod.getFunction(TrampName);
        if (!Tramp) {
            Tramp = Function::Create(TrampTy, GlobalValue::InternalLinkage, TrampName, &Mod);

            BasicBlock *TBB = BasicBlock::Create(Ctx, "entry", Tramp);
            IRBuilder<> BT(TBB);

            // Y = input argument
            Value *Y = Tramp->arg_begin();

            // Allocate dummy closure with correct size and alignment
            AllocaInst *Dummy = BT.CreateAlloca(ST, nullptr, "lambdaopt.dummy.closure");
            const DataLayout &DL = Mod.getDataLayout();
            uint64_t Size = DL.getTypeAllocSize(ST);
            BT.CreateMemSet(Dummy, ConstantInt::get(Type::getInt8Ty(Ctx), 0), Size, MaybeAlign(Dummy->getAlign()));

            // Call hot clone: i32 (%class.anon*, i32)
            CallInst *CallHot = BT.CreateCall(HotClone, {Dummy, Y});
            BT.CreateRet(CallHot);
        }

        // === Split original block ===

        BasicBlock *OrigBB = CI->getParent();
        BasicBlock *ColdBB = OrigBB->splitBasicBlock(CI, "lambdaopt.cold");

        ReturnInst *ColdRet = dyn_cast<ReturnInst>(ColdBB->getTerminator());
        if (!ColdRet) {
            errs() << "lambdaopt: unexpected terminator (not ret);\n";
            return;
        }

        // New hot block
        BasicBlock *HotBB = BasicBlock::Create(Ctx, "lambdaopt.hot", ParentF);

        // Replace unconditional branch with conditional
        Instruction *OldTerm = OrigBB->getTerminator();
        IRBuilder<> BCond(OldTerm);

        auto *CapTy = cast<IntegerType>(CaptureArg->getType()); // i64
        ConstantInt *HotI64 = ConstantInt::get(CapTy, HotValue);
        Value *IsHot = BCond.CreateICmpEQ(CaptureArg, HotI64, "lambdaopt_is_hot");

        BCond.CreateCondBr(IsHot, HotBB, ColdBB);
        OldTerm->eraseFromParent();

        // === Cold path ===
        // remains untouched except its predecessor is now conditional
        //      [CI call]
        //      ret void

        // === Hot path ===

        IRBuilder<> BHot(HotBB);

        // "this" for std::function
        Value *ThisPtr = CI->getArgOperand(0);

        // FP type: int(*)(int)
        FunctionType *FnTy = FunctionType::get(IntTy, {IntTy}, false);
        PointerType *FnPtrTy = PointerType::getUnqual(FnTy);

        // Tramp is already correct signature
        Value *HotFnPtr = Tramp;
        if (Tramp->getType() != FnPtrTy)
            HotFnPtr = BHot.CreateBitCast(Tramp, FnPtrTy);

        // helper: std::function<int(int)>* (std::function<int(int)>*, int(*)(int))
        FunctionType *HelperTy = FunctionType::get(CI->getType(), {ThisPtr->getType(), FnPtrTy}, false);

        FunctionCallee Helper = Mod.getOrInsertFunction("lambdaopt_init_function_from_fp", HelperTy);

        CallInst *HotCall = BHot.CreateCall(Helper, {ThisPtr, HotFnPtr});
        HotCall->setTailCallKind(CI->getTailCallKind());

        BHot.CreateRetVoid();

        // Original ctor call is redundant now
        // CI->eraseFromParent();

        errs() << "Finished rewrite of the boi\n";
    }
}

llvm::PreservedAnalyses LambdaEscapeOptimizer::run(llvm::Module &M, llvm::ModuleAnalysisManager &) {
    CaptureProfileMap Profiles = loadCaptureProfiles("../../logs/lambda_logs_lambda_profdata.txt");

    for (Function &F : M) {
        if (F.isDeclaration())
            continue;

        // First: collect all ctor callsites we care about.
        SmallVector<CallInst *, 8> Ctors;

        for (BasicBlock &BB : F) {
            for (Instruction &I : BB) {
                auto *CI = dyn_cast<CallInst>(&I);
                if (!CI)
                    continue;

                Function *Callee = CI->getCalledFunction();
                if (!Callee)
                    continue;

                auto Name = Callee->getName();
                if (Name.empty())
                    continue;

                if (Name.starts_with("_ZNSt3__18function") && Name.contains("EEC") && Name.contains("Z")) {
                    errs() << "func_name: " << demangleName(Name) << "\n";
                    Ctors.push_back(CI);
                }
            }
        }

        // Second: rewrite those callsites. NOW it's safe to erase/split/etc.
        for (CallInst *CI : Ctors) {
            handleStdFunctionCtor(M, CI, Profiles);
        }
    }

    return PreservedAnalyses::none();
}

} // namespace lambdaopt
