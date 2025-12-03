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

    GlobalValue *global_fileptr{};
    std::string log_directory = "../../logs/lambda_logs.txt";

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
    static bool isLambdaCaptureStore(StoreInst *SI) {
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
    static StructType *getLambdaStruct(StoreInst *SI) {
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
    /// Precondition: SI is a lambda capture store (checked by caller).
    static Value *getCapturedValue(StoreInst *SI) {
        if (!SI)
            return nullptr;
        return SI->getValueOperand();
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

    void handleCaptureStore(Function &F, StoreInst *SI, Module &module, LLVMContext &context) {
        auto *ST = getLambdaStruct(SI);
        auto *value = SI->getValueOperand(); // no type filter here
        int field = getCaptureFieldIndex(SI);

        if (!value) {

            errs() << "[lambda] capture does not have value" << F.getName() << "\n";
        }

        // Debug log to stderr
        errs() << "[lambda] capture in function " << F.getName() << "\n";
        errs() << "  Struct: " << ST->getName() << "\n";
        errs() << "  Field: " << field << "\n";
        errs() << "  Value IR: ";
        value->print(errs());
        errs() << "\n";

        IRBuilder<> builder(SI);
        Type *ty = value->getType();
        const std::string lambda_name = ST->getName().str();

        // Load FILE*
        Value *file_ptr = builder.CreateLoad(builder.getPtrTy(), global_fileptr);
        Value *field_val = builder.getInt32(field);

        // Helpers
        auto fprintfFunc = get_or_insert_fprintf_func(module);

        // --- Case 1: Integer captures -----------------------
        if (ty->isIntegerTy()) {
            unsigned bw = ty->getIntegerBitWidth();
            Value *val = value;

            if (bw < 64)
                val = builder.CreateZExt(val, builder.getInt64Ty());
            else if (bw > 64)
                val = builder.CreateTrunc(val, builder.getInt64Ty());

            auto *fmt = make_fmt(module, context, "LAMBDA " + lambda_name + " %d %ld\n");
            Value *fmt_ptr = builder.CreateBitCast(fmt, builder.getPtrTy());

            builder.CreateCall(fprintfFunc, {file_ptr, fmt_ptr, field_val, val});
            return;
        }

        // --- Case 2: Floating captures (float, double) ------
        if (ty->isFloatingPointTy()) {
            Value *dbl = value;
            if (ty->isFloatTy())
                dbl = builder.CreateFPExt(dbl, builder.getDoubleTy()); // promote float → double

            auto *fmt = make_fmt(module, context, "LAMBDA " + lambda_name + " %d %f\n");
            Value *fmt_ptr = builder.CreateBitCast(fmt, builder.getPtrTy());

            builder.CreateCall(fprintfFunc, {file_ptr, fmt_ptr, field_val, dbl});
            return;
        }

        // --- Case 3: Pointer captures → print %p ------------
        if (ty->isPointerTy()) {
            Value *ptrInt = builder.CreatePtrToInt(value, builder.getInt64Ty());

            auto *fmt = make_fmt(module, context, "LAMBDA " + lambda_name + " %d %p\n");
            Value *fmt_ptr = builder.CreateBitCast(fmt, builder.getPtrTy());

            builder.CreateCall(fprintfFunc, {file_ptr, fmt_ptr, field_val, ptrInt});
            return;
        }

        // --- Case 4: Unknown scalar → raw bits as hex -------
        unsigned bits = ty->getScalarSizeInBits();
        if (bits == 0 || bits > 64) {
            errs() << "  Unsupported non-scalar capture type, skipping\n";
            return;
        }

        Value *raw = builder.CreateBitCast(value, builder.getIntNTy(bits));
        if (bits < 64)
            raw = builder.CreateZExt(raw, builder.getInt64Ty());

        auto *fmt = make_fmt(module, context, "LAMBDA " + lambda_name + " %d 0x%lX\n");
        Value *fmt_ptr = builder.CreateBitCast(fmt, builder.getPtrTy());

        builder.CreateCall(fprintfFunc, {file_ptr, fmt_ptr, field_val, raw});
    }

    static auto make_fmt(Module &module, LLVMContext &context, std::string const &str) -> GlobalVariable * {
        Constant *const fmt_data = ConstantDataArray::getString(context, str, true);
        auto *const fmt =
            new GlobalVariable(module, fmt_data->getType(), true, GlobalValue::PrivateLinkage, fmt_data, ".fmt");

        return fmt;
    }

    auto insert_fopen(Module &module, LLVMContext &context, IRBuilder<> &builder, std::string const &filename)
        -> void { // create a global pointer for the output file
        global_fileptr =
            new GlobalVariable(module, PointerType::get(context, 0), false, GlobalValue::ExternalLinkage,
                               llvm::ConstantPointerNull::get(PointerType::get(context, 0)), "global_file_ptr");

        PointerType *const ptr_ty = PointerType::get(context, 0);
        FunctionType *const fopen_type = FunctionType::get(ptr_ty, {ptr_ty, ptr_ty}, false);
        FunctionCallee const fopen_func = module.getOrInsertFunction("fopen", fopen_type);

        // Format string literals
        auto *const filename_str = builder.CreateGlobalString(filename);
        auto *const mode_str = builder.CreateGlobalString("w");

        // Call fopen
        Value *const file_ptr_value = builder.CreateCall(fopen_func, {filename_str, mode_str});

        // save it in global variable
        builder.CreateStore(file_ptr_value, global_fileptr);
    }

    static auto get_or_insert_fprintf_func(llvm::Module &M) -> llvm::FunctionCallee {
        LLVMContext &ctx = M.getContext();

        FunctionType *fprintf_type =
            FunctionType::get(llvm::Type::getInt32Ty(ctx), {PointerType::get(ctx, 0), PointerType::get(ctx, 0)}, true);

        return M.getOrInsertFunction("fprintf", fprintf_type);
    }

  public:
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
        LLVMContext &context = M.getContext();

        BasicBlock &main_func_bb = *M.getFunction("main")->begin();

        if (global_fileptr == nullptr) {
            IRBuilder<> builder(&main_func_bb, main_func_bb.begin());
            insert_fopen(M, context, builder, log_directory);
        }

        for (Function &F : M) {
            for (BasicBlock &BB : F) {
                for (Instruction &I : BB) {

                    // detect all store operations since storing a capture into a lambda is a store instruction
                    if (auto *SI = dyn_cast<StoreInst>(&I)) {
                        // we only care about lamabda capture stores
                        if (isLambdaCaptureStore(SI)) {
                            handleCaptureStore(F, SI, M, context);
                        }
                    }
                }
            }
        }

        return PreservedAnalyses::all();
    }
};

} // namespace
