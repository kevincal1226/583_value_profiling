
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Support/Casting.h>

#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Analysis/BranchProbabilityInfo.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/ProfileData/InstrProfData.inc"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"

using namespace llvm;

namespace {

struct LambdaCaptureKey {
    std::string Name;    // e.g. "class.anon"
    unsigned FieldIndex; // capture index in the struct

    bool operator==(const LambdaCaptureKey &Other) const {
        return FieldIndex == Other.FieldIndex && Name == Other.Name;
    }
};

struct LambdaCaptureKeyHash {
    std::size_t operator()(const LambdaCaptureKey &Key) const noexcept {
        std::size_t h1 = std::hash<std::string>{}(Key.Name);
        std::size_t h2 = std::hash<unsigned>{}(Key.FieldIndex);
        // simple combine
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};

struct CaptureProfile {
    int64_t HotValue; // value we will bake in
    double Freq;      // frequency (0–1)
    uint64_t Count;   // occurrences (debug/info)
};

using CaptureProfileMap = std::unordered_map<LambdaCaptureKey, CaptureProfile, LambdaCaptureKeyHash>;

CaptureProfileMap loadCaptureProfiles(const std::string &Path) {
    CaptureProfileMap Profiles;

    std::ifstream in(Path);
    if (!in) {
        llvm::errs() << "[lambda-prof] Could not open profile file: " << Path << "\n";
        return Profiles;
    }

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;

        // Allow comment lines starting with '#'
        if (!line.empty() && line[0] == '#')
            continue;

        std::stringstream ss(line);
        std::string lambdaName, fieldStr, valueStr, countStr, freqStr;

        if (!std::getline(ss, lambdaName, ','))
            continue;
        if (!std::getline(ss, fieldStr, ','))
            continue;
        if (!std::getline(ss, valueStr, ','))
            continue;
        if (!std::getline(ss, countStr, ','))
            continue;
        if (!std::getline(ss, freqStr, ','))
            continue;

        unsigned fieldIndex = static_cast<unsigned>(std::stoul(fieldStr));
        int64_t value = static_cast<int64_t>(std::stoll(valueStr));
        uint64_t count = static_cast<uint64_t>(std::stoull(countStr));
        double freq = std::stod(freqStr);

        // Apply threshold: only keep hot captures
        if (freq < 0.8)
            continue;

        LambdaCaptureKey key{lambdaName, fieldIndex};
        CaptureProfile prof{value, freq, count};

        auto it = Profiles.find(key);
        if (it == Profiles.end() || freq > it->second.Freq) {
            Profiles[key] = prof; // keep hottest for this (lambda, field)
        }
    }

    // Optional debug dump:
    llvm::errs() << "[lambda-prof] Loaded " << Profiles.size() << " hot capture entries from " << Path << "\n";
    for (const auto &Entry : Profiles) {
        const auto &key = Entry.first;
        const auto &prof = Entry.second;
        llvm::errs() << "  - " << key.Name << ", field " << key.FieldIndex << " -> value " << prof.HotValue
                     << " (freq=" << prof.Freq << ", count=" << prof.Count << ")\n";
    }

    return Profiles;
}

class LambdaOptimizer : public PassInfoMixin<LambdaOptimizer> {

    // Find the StructType representing the lambda closure for this key.
    // Returns nullptr if not found.
    static StructType *findClosureType(const Module &M, const LambdaCaptureKey &Key) {
        const std::string &TargetName = Key.Name;

        for (StructType *ST : M.getIdentifiedStructTypes()) {
            // Skip opaque types — they have no fields and can't hold captures
            if (ST->isOpaque()) {
                continue;
            }

            // StructType names do NOT include the '%' IR prefix
            if (ST->hasName() && ST->getName() == TargetName) {
                return ST;
            }
        }

        // Not found — print debug info
        llvm::errs() << "[lambda-opt] Could not find StructType for lambda '" << TargetName << "' in module.\n";

        return nullptr;
    }

    // Find the lambda's operator() function given the closure StructType.
    // We look for a function whose first argument is ClosureTy*,
    // preferring one whose demangled name contains "operator()".
    static Function *findLambdaOperatorFunc(Module &M, StructType *ClosureTy) {
        if (ClosureTy == nullptr) {
            llvm::errs() << "[lambda-opt] No ClosureType provided.\n";
            return nullptr;
        }

        PointerType *ThisPtrTy = ClosureTy->getPointerTo();
        Function *Fallback = nullptr;

        for (Function &F : M) {
            // We only care about functions with bodies
            if (F.isDeclaration()) {
                continue;
            }

            // Must have at least one argument
            if (F.arg_empty()) {
                continue;
            }

            Argument &FirstArg = *F.arg_begin();
            Type *ArgTy = FirstArg.getType();

            // Does the first parameter match "%class.anon*" ?
            if (ArgTy != ThisPtrTy) {
                continue;
            }

            // --- Now this is a candidate operator() ---

            // Try demangling for a more precise match
            std::string Demangled = llvm::demangle(F.getName().str());

            if (Demangled.find("operator()") != std::string::npos) {
                // Strong match: this is almost certainly the lambda operator
                return &F;
            }

            // Otherwise keep the first match as a fallback
            if (Fallback == nullptr) {
                Fallback = &F;
            }
        }

        if (Fallback == nullptr) {
            llvm::errs() << "[lambda-opt] Could not find operator() for lambda struct '" << ClosureTy->getName()
                         << "'\n";
        }
        else {
            llvm::errs() << "[lambda-opt] Using fallback operator for lambda struct '" << ClosureTy->getName()
                         << "': " << Fallback->getName() << "\n";
        }

        return Fallback;
    }

    // Clone the original operator() into a new function with the same type.
    // We will then specialize the clone to bake in the hot capture value.
    static Function *cloneLambdaOperator(Function *OriginalFunc, const LambdaCaptureKey &Key,
                                         const CaptureProfile &Prof) {
        if (OriginalFunc == nullptr) {
            return nullptr;
        }

        Module *M = OriginalFunc->getParent();

        // Build a unique specialized name
        std::string NewName = OriginalFunc->getName().str() + ".hot.field" + std::to_string(Key.FieldIndex) + ".value" +
                              std::to_string(Prof.HotValue);

        // Get original function type
        FunctionType *FTy = OriginalFunc->getFunctionType();

        // Create the new function shell
        Function *NewF = Function::Create(FTy, OriginalFunc->getLinkage(), NewName, M);

        NewF->setCallingConv(OriginalFunc->getCallingConv());

        // Map original args -> new args
        ValueToValueMapTy VMap;
        {
            auto A = OriginalFunc->arg_begin();
            auto B = NewF->arg_begin();
            for (; A != OriginalFunc->arg_end(); ++A, ++B) {
                B->setName(A->getName());
                VMap[&*A] = &*B;
            }
        }

        // Clone the body
        SmallVector<ReturnInst *, 8> Returns; // unused but required by API
        CloneFunctionInto(NewF, OriginalFunc, VMap, CloneFunctionChangeType::LocalChangesOnly, Returns);

        llvm::errs() << "[lambda-opt] Cloned operator(): " << OriginalFunc->getName() << " -> " << NewF->getName()
                     << "\n";

        return NewF;
    }

    // Strip away simple pointer casts so we can see the underlying value.
    static Value *stripPointerCasts(Value *V) {
        while (true) {
            if (auto *BC = dyn_cast<BitCastInst>(V)) {
                V = BC->getOperand(0);
            }
            else if (auto *ASC = dyn_cast<AddrSpaceCastInst>(V)) {
                V = ASC->getOperand(0);
            }
            else {
                break;
            }
        }
        return V;
    }

    // In the cloned operator(), replace loads of the captured field with a constant.
    static bool specializeCaptureInClone(Function *CloneF, StructType *ClosureTy, const LambdaCaptureKey &Key,
                                         const CaptureProfile &Prof) {
        if ((CloneF == nullptr) || (ClosureTy == nullptr)) {
            return false;
        }

        if (CloneF->arg_empty()) {
            return false;
        }

        bool Changed = false;
        SmallVector<Instruction *, 8> ToErase;

        for (BasicBlock &BB : *CloneF) {
            for (Instruction &I : BB) {
                auto *LI = dyn_cast<LoadInst>(&I);
                if (LI == nullptr) {
                    continue;
                }

                // We care only about loads from GEPs on the closure
                auto *GEP = dyn_cast<GetElementPtrInst>(LI->getPointerOperand());
                if (GEP == nullptr) {
                    continue;
                }

                // With opaque pointers, the source element type is what tells us
                // which struct we're indexing into.
                if (GEP->getSourceElementType() != ClosureTy) {
                    continue;
                }

                // Expect at least two indices: [0, FieldIndex]
                if (GEP->getNumIndices() < 2) {
                    continue;
                }

                auto IdxIt = GEP->idx_begin();
                auto *Idx0 = dyn_cast<ConstantInt>(IdxIt->get());
                auto *Idx1 = dyn_cast<ConstantInt>((++IdxIt)->get());

                if ((Idx0 == nullptr) || (Idx1 == nullptr)) {
                    continue;
                }

                if (Idx0->getSExtValue() != 0) {
                    continue;
                }

                if (Idx1->getZExtValue() != Key.FieldIndex) {
                    continue;
                }

                // This is a load from the captured field we care about.
                Type *FieldTy = LI->getType();

                if (!FieldTy->isIntegerTy()) {
                    llvm::errs() << "[lambda-opt] Capture field " << Key.FieldIndex << " in lambda '"
                                 << ClosureTy->getName() << "' is not an integer type; skipping specialization.\n";
                    continue;
                }

                // Build constant with same bit-width as the field
                auto *IntTy = cast<IntegerType>(FieldTy);
                Constant *ConstVal = ConstantInt::get(IntTy, Prof.HotValue);

                LI->replaceAllUsesWith(ConstVal);
                ToErase.push_back(LI);
                Changed = true;

                llvm::errs() << "[lambda-opt] Specialized capture field " << Key.FieldIndex << " in clone '"
                             << CloneF->getName() << "' with value " << Prof.HotValue << "\n";
            }
        }

        // Clean up dead loads
        for (Instruction *I : ToErase) {
            I->eraseFromParent();
        }

        return Changed;
    }

    // Find the store that writes the captured field for this call.
    // Returns nullptr if we can't find a matching pattern in the same basic block.
    static StoreInst *findCaptureStoreForCall(CallInst *CI, StructType *ClosureTy, const LambdaCaptureKey &Key) {
        if (!CI || !ClosureTy) {
            return nullptr;
        }

        BasicBlock *BB = CI->getParent();
        if (!BB) {
            return nullptr;
        }

        // First argument is the closure pointer ("this")
        if (CI->arg_empty()) {
            return nullptr;
        }

        Value *ThisArg = CI->getArgOperand(0);
        Value *ThisBase = stripPointerCasts(ThisArg);

        // Walk backwards from the call within the same basic block
        for (auto It = BasicBlock::iterator(CI); It != BB->begin();) {
            --It;
            Instruction &I = *It;

            auto *SI = dyn_cast<StoreInst>(&I);
            if (!SI) {
                continue;
            }

            auto *GEP = dyn_cast<GetElementPtrInst>(SI->getPointerOperand());
            if (!GEP) {
                continue;
            }

            // We only care about GEPs indexing into the lambda closure struct.
            if (GEP->getSourceElementType() != ClosureTy) {
                continue;
            }

            // Check indices: expect [0, FieldIndex]
            if (GEP->getNumIndices() < 2) {
                continue;
            }

            auto IdxIt = GEP->idx_begin();
            auto *Idx0 = dyn_cast<ConstantInt>(IdxIt->get());
            auto *Idx1 = dyn_cast<ConstantInt>((++IdxIt)->get());

            if (!Idx0 || !Idx1) {
                continue;
            }

            if (Idx0->getSExtValue() != 0) {
                continue;
            }

            if (Idx1->getZExtValue() != Key.FieldIndex) {
                continue;
            }

            // Check that the GEP base ultimately comes from the "this" argument.
            Value *GEPBase = stripPointerCasts(GEP->getPointerOperand());
            if (GEPBase != ThisBase) {
                continue;
            }

            // This looks like the capture store we want.
            return SI;
        }

        return nullptr;
    }

    // For each direct call to OriginalOp, insert a guard:
    //
    //    if (capture == HotValue)
    //       call HotClone(...)
    //    else
    //       call OriginalOp(...)
    //
    // and replace the original call with a PHI that merges the results.
    //
    // Returns true if any call sites were rewritten.
    static bool specializeConstructionAndCall(Function *OriginalOp, Function *HotClone, StructType *ClosureTy,
                                              const LambdaCaptureKey &Key, const CaptureProfile &Prof) {
        if (!OriginalOp || !HotClone || !ClosureTy) {
            return false;
        }

        bool Changed = false;
        LLVMContext &Ctx = OriginalOp->getContext();

        // Snapshot the call sites first so we don't trip over CFG edits.
        SmallVector<CallInst *, 8> CallSites;
        for (User *U : OriginalOp->users()) {
            if (auto *CI = dyn_cast<CallInst>(U)) {
                if (CI->getCalledFunction() == OriginalOp) {
                    CallSites.push_back(CI);
                }
            }
        }

        for (CallInst *CI : CallSites) {
            // Find the capture store for this call.
            StoreInst *CaptureStore = findCaptureStoreForCall(CI, ClosureTy, Key);
            if (!CaptureStore) {
                llvm::errs() << "[lambda-opt] Could not find capture store for call to '" << OriginalOp->getName()
                             << "'; skipping this call site.\n";
                continue;
            }

            Value *CapturedVal = CaptureStore->getValueOperand();
            auto *IntTy = dyn_cast<IntegerType>(CapturedVal->getType());
            if (!IntTy) {
                llvm::errs() << "[lambda-opt] Capture value at call site is not an integer; skipping.\n";
                continue;
            }

            // Build constant HotValue with same bit-width as the capture.
            APInt HotAP(IntTy->getBitWidth(), static_cast<uint64_t>(Prof.HotValue),
                        /*isSigned=*/true);
            Constant *HotConst = ConstantInt::get(IntTy, HotAP);

            // Split the basic block at the call.
            BasicBlock *OrigBB = CI->getParent();
            BasicBlock *MergeBB = OrigBB->splitBasicBlock(CI, "lambda.merge");

            // OrigBB now ends with an unconditional branch to MergeBB; we replace it.
            Instruction *OldTerm = OrigBB->getTerminator();

            // Create the comparison in OrigBB, right before the old terminator.
            IRBuilder<> CmpBuilder(OldTerm);
            Value *IsHot = CmpBuilder.CreateICmpEQ(CapturedVal, HotConst, "lambda.is_hot");

            // Create hot and cold blocks, inserted before MergeBB.
            Function *ParentF = OrigBB->getParent();
            auto *HotBB = BasicBlock::Create(Ctx, "lambda.hot", ParentF, MergeBB);
            auto *ColdBB = BasicBlock::Create(Ctx, "lambda.cold", ParentF, MergeBB);

            // Remove the store from OrigBB and place it at the beginning of ColdBB.
            CaptureStore->removeFromParent();
            CaptureStore->insertBefore(&*ColdBB->begin());

            // Now the hot path has *no store*.
            // The cold path still performs the store before calling the original operator.

            // Replace the unconditional branch with a conditional branch.
            OldTerm->eraseFromParent();
            IRBuilder<> BrBuilder(OrigBB);
            BrBuilder.SetInsertPoint(OrigBB);
            BrBuilder.CreateCondBr(IsHot, HotBB, ColdBB);

            // Rebuild the argument list for the calls.
            SmallVector<Value *, 8> Args;
            Args.reserve(CI->arg_size());
            for (unsigned i = 0; i < CI->arg_size(); ++i) {
                Args.push_back(CI->getArgOperand(i));
            }

            Type *RetTy = OriginalOp->getReturnType();
            Value *HotCall = nullptr;
            Value *ColdCall = nullptr;

            // Emit hot path: call the specialized clone, then branch to merge.
            {
                IRBuilder<> HotBuilder(HotBB);
                if (RetTy->isVoidTy()) {
                    HotBuilder.CreateCall(HotClone, Args);
                    HotBuilder.CreateBr(MergeBB);
                }
                else {
                    HotCall = HotBuilder.CreateCall(HotClone, Args, "lambda.call.hot");
                    HotBuilder.CreateBr(MergeBB);
                }
            }

            // Emit cold path: call the original operator, then branch to merge.
            {
                IRBuilder<> ColdBuilder(ColdBB);
                if (RetTy->isVoidTy()) {
                    ColdBuilder.CreateCall(OriginalOp, Args);
                    ColdBuilder.CreateBr(MergeBB);
                }
                else {
                    ColdCall = ColdBuilder.CreateCall(OriginalOp, Args, "lambda.call.cold");
                    ColdBuilder.CreateBr(MergeBB);
                }
            }

            if (!RetTy->isVoidTy()) {
                // In MergeBB, replace the old call with a PHI that selects hot vs cold.
                IRBuilder<> MergeBuilder(MergeBB);
                MergeBuilder.SetInsertPoint(&*MergeBB->begin());

                PHINode *PHI = MergeBuilder.CreatePHI(RetTy, 2, "lambda.call.sel");
                PHI->addIncoming(HotCall, HotBB);
                PHI->addIncoming(ColdCall, ColdBB);

                CI->replaceAllUsesWith(PHI);
            }

            // Erase the original direct call in MergeBB; we now have hot/cold calls.
            CI->eraseFromParent();

            llvm::errs() << "[lambda-opt] Rewrote call site of '" << OriginalOp->getName()
                         << "' to guard on capture == " << Prof.HotValue << "\n";

            Changed = true;
        }

        return Changed;
    }

  public:
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
        CaptureProfileMap Profiles = loadCaptureProfiles("../../logs/lambda_logs_lambda_profdata.txt");

        for (auto &[Key, Prof] : Profiles) {

            StructType *CT = findClosureType(M, Key);
            if (!CT) {
                continue;
            }

            Function *OriginalOp = findLambdaOperatorFunc(M, CT);
            if (!OriginalOp) {
                continue;
            }

            Function *HotClone = cloneLambdaOperator(OriginalOp, Key, Prof);
            if (!HotClone) {
                continue;
            }

            bool SpecializedBody = specializeCaptureInClone(HotClone, CT, Key, Prof);

            if (!SpecializedBody) {
                llvm::errs() << "[lambda-opt] No specialization in clone '" << HotClone->getName()
                             << "', erasing it.\n";
                HotClone->eraseFromParent();
                continue;
            }

            llvm::errs() << "[lambda-opt] Created hot operator clone '" << HotClone->getName() << "' for lambda '"
                         << Key.Name << "', field " << Key.FieldIndex << " = " << Prof.HotValue << "\n";

            // NEW: rewrite call sites to branch between OriginalOp and HotClone.
            bool RewroteCalls = specializeConstructionAndCall(OriginalOp, HotClone, CT, Key, Prof);
            if (!RewroteCalls) {
                llvm::errs() << "[lambda-opt] No call sites rewritten for '" << OriginalOp->getName()
                             << "'; hot clone may remain unused.\n";
            }
        }

        return PreservedAnalyses::none();
    }
};
} // namespace
