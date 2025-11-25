#include <fstream>
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

class LambdaCaptureCollector : public PassInfoMixin<LambdaCaptureCollector> {

    /// Determine whether a StructType looks like a lambda closure.
    /// This is intentionally simple and only checks Clang-style naming.
    /// We can refine later if needed.
    static bool isLambdaStruct(StructType *ST) {
        if (ST == nullptr || !ST->hasName()) {
            return false;
        }

        StringRef name = ST->getName();

        // Case 1: Clang mangled lambda types
        if (name.contains("ZZ") && name.contains("ENK")) {
            return true;
        }

        // Case 2: Clang anonymous lambda structs
        if (name.contains("class.anon") || name.contains("struct.anon")) {
            return true;
        }

        return false;
    }

    /// Determine whether a StoreInst writes into a lambda closure struct.
    /// Pattern:
    ///     store V, (gep %lambda_struct, ...)
    static bool isLambdaCaptureStore(const StoreInst *SI) {
        if (SI == nullptr) {
            return false;
        }

        // 1. The pointer operand must be a GEP
        const Value *Ptr = SI->getPointerOperand();
        const auto *GEP = dyn_cast<GetElementPtrInst>(Ptr);
        if (GEP == nullptr) {
            return false; // stores directly to alloca or array are not lambda captures
        }

        // 2. The GEP's source element type must be a struct type
        Type *SourceTy = GEP->getSourceElementType();
        auto *ST = dyn_cast<StructType>(SourceTy);

        if (!ST) {
            return false;
        }

        // 3. Check if the struct looks like a Clang lambda closure
        return isLambdaStruct(ST);
    }

    /// Extract the lambda closure StructType from a StoreInst.
    /// Precondition: SI must satisfy isLambdaCaptureStore(SI).
    static StructType *getLambdaStruct(const StoreInst *SI) {
        if (SI == nullptr) {
            return nullptr;
        }

        // Pointer must be a GEP
        const Value *Ptr = SI->getPointerOperand();
        const auto *GEP = dyn_cast<GetElementPtrInst>(Ptr);
        if (GEP == nullptr) {
            return nullptr;
        }

        // The source element type is the closure struct
        Type *SourceTy = GEP->getSourceElementType();
        auto *ST = dyn_cast<StructType>(SourceTy);

        // Double-check safety: ensure it's actually a lambda struct
        if (!isLambdaStruct(ST)) {
            return nullptr;
        }

        return ST;
    }

    /// Extract the captured value from a lambda capture store.
    /// Only supports integer captures (i32 or i64). Returns nullptr otherwise.
    /// Precondition: SI is a lambda capture store (checked by caller).
    static const Value *getCapturedValue(const StoreInst *SI) {
        if (SI == nullptr) {
            return nullptr;
        }

        const Value *V = SI->getValueOperand();
        if (V == nullptr) {
            return nullptr;
        }

        Type *Ty = V->getType();
        if (!Ty->isIntegerTy(32) && !Ty->isIntegerTy(64)) {
            return nullptr;
        }

        return V; // valid integer capture
    }

    /// Extract the field index being written in a lambda capture store.
    /// Example:
    ///   %gep = getelementptr %lambda, %lambda* %p, i32 0, i32 0
    ///   store ..., i32* %gep
    ///
    /// Returns:
    ///   field index (0-based), or -1 if unavailable.
    static int getCaptureFieldIndex(const StoreInst *SI) {
        if (SI == nullptr) {
            return -1;
        }

        // Must be a GEP because we already know (from isLambdaCaptureStore)
        const auto *GEP = dyn_cast<GetElementPtrInst>(SI->getPointerOperand());
        if (GEP == nullptr) {
            return -1;
        }

        // GEP for struct fields always has:
        //   gep %struct, %struct* %ptr, i32 0, i32 <field>
        if (GEP->getNumIndices() < 2) {
            return -1;
        }

        // The last index is the field index
        auto IdxIt = GEP->idx_end();
        --IdxIt; // move to the last index

        const Value *IdxVal = IdxIt->get();
        if (const auto *CI = dyn_cast<ConstantInt>(IdxVal)) {
            return CI->getSExtValue();
        }

        return -1; // non-constant indexing (unlikely for lambda captures)
    }

    static void handleCaptureStore(Function &F, const StoreInst *SI) {
        auto *ST = getLambdaStruct(SI);
        auto *value = getCapturedValue(SI);
        int field = getCaptureFieldIndex(SI);

        errs() << "[lambda] capture in function " << F.getName() << "\n";
        errs() << "  Struct: " << ST->getName() << "\n";
        errs() << "  Field: " << field << "\n";
        errs() << "  Value: ";
        value->print(errs());
        errs() << "\n";
    }

  public:
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
        errs() << "HERE\n";
        for (Function &F : M) {
            for (BasicBlock &BB : F) {
                for (Instruction &I : BB) {

                    // detect all store operations since storing a capture into a lambda is a store instruction
                    if (auto *SI = dyn_cast<StoreInst>(&I)) {
                        // we only care about lamabda capture stores
                        if (isLambdaCaptureStore(SI)) {
                            handleCaptureStore(F, SI);
                        }
                    }
                }
            }
        }

        return PreservedAnalyses::all();
    }
};

} // namespace
