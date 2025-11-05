//===-- Frequent Path Loop Invariant Code Motion Pass --------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===---------------------------------------------------------------------===//
//
// CSE583 F25 - This pass can be used as a template for your FPLICM homework
//               assignment.
//               The passes get registered as "fplicm-correctness" and
//               "fplicm-performance".
//
//
////===-------------------------------------------------------------------===//
#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Analysis/BranchProbabilityInfo.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/LoopIterator.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Utils/SSAUpdater.h"

/* *******Implementation Starts Here******* */
// You can include more Header files here
/* *******Implementation Ends Here******* */
using namespace llvm;

namespace {
struct ValueProfile : public PassInfoMixin<ValueProfile> {
    PreservedAnalyses run(Function& F, FunctionAnalysisManager& FAM) { return PreservedAnalyses::none(); }
};
}   // namespace

extern "C" ::llvm::PassPluginLibraryInfo LLVM_ATTRIBUTE_WEAK llvmGetPassPluginInfo() {
    return { .APIVersion = LLVM_PLUGIN_API_VERSION,
             .PluginName = "HW1Pass",
             .PluginVersion = "v0.1",
             .RegisterPassBuilderCallbacks = [](PassBuilder& PB) {
                 PB.registerPipelineParsingCallback(
                   [](StringRef Name, FunctionPassManager& FPM, ArrayRef<PassBuilder::PipelineElement>) {
                       if (Name == "value_profile") {
                           FPM.addPass(ValueProfile());
                           return true;
                       }
                       return false;
                   });
             } };
}
