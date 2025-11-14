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

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Support/Casting.h>

#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Analysis/BranchProbabilityInfo.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/ProfileData/InstrProfData.inc"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/LoopUtils.h"

/* *******Implementation Starts Here******* */
// You can include more Header files here
/* *******Implementation Ends Here******* */
using namespace llvm;

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {

struct ValueProfiler : public PassInfoMixin<ValueProfiler> {
    PreservedAnalyses run(Function& F, FunctionAnalysisManager& FAM) { return PreservedAnalyses::none(); }
};

}   // namespace

extern "C" auto LLVM_ATTRIBUTE_WEAK llvmGetPassPluginInfo() -> ::llvm::PassPluginLibraryInfo {
    return { .APIVersion = LLVM_PLUGIN_API_VERSION,
             .PluginName = "ValueProfilingPass",
             .PluginVersion = "v0.1",
             .RegisterPassBuilderCallbacks = [](PassBuilder& PB) -> void {
                 PB.registerPipelineParsingCallback(
                   [](StringRef Name, FunctionPassManager& FPM, ArrayRef<PassBuilder::PipelineElement>) -> bool {
                       if (Name == "value_profiler") {
                           FPM.addPass(ValueProfiler());
                           return true;
                       }
                       if (Name == "value_profile_opt") {
                           FPM.addPass(ValueProfiler());
                           return true;
                       }
                       return false;
                   });
             } };
}
