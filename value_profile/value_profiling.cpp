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

#include <string_view>
#include <unordered_map>

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

using prof_map_t = std::unordered_map<std::string, std::unordered_map<std::string, std::unordered_map<int, double>>>;

namespace {

auto parse_data() -> prof_map_t {
    prof_map_t prof_data;
    std::string filename { "../../profile_stats.txt" };
    std::ifstream ifs { filename };
    std::string func_name;
    std::string var_name;
    std::string value_str;
    std::string frequency_str;
    std::string probability_str;
    while (std::getline(ifs, func_name, ','), std::getline(ifs, var_name, ','), std::getline(ifs, value_str, ','),
           std::getline(ifs, frequency_str, ','), std::getline(ifs, probability_str)) {
        int value { std::stoi(value_str) };
        double probability { std::stod(probability_str) };
        prof_data[func_name][var_name][value] = probability;
    }

    return prof_data;
}

struct ValueProfiler : public PassInfoMixin<ValueProfiler> {
    prof_map_t prof_map {};
    ValueProfiler() = default;
    ValueProfiler(prof_map_t&& prof_map)
        : prof_map(std::move(prof_map)) {}
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
                           FPM.addPass(ValueProfiler(parse_data()));
                           return true;
                       }
                       return false;
                   });
             } };
}
