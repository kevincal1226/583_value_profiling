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
#include <llvm-20/llvm/IR/BasicBlock.h>
#include <llvm-20/llvm/IR/IRBuilder.h>
#include <llvm-20/llvm/IR/InstrTypes.h>
#include <llvm-20/llvm/IR/Instruction.h>
#include <llvm-20/llvm/IR/IntrinsicInst.h>
#include <llvm-20/llvm/Support/Casting.h>

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

struct VariableStats {
    std::unordered_map<int, int> values;

    int total = 0;

    auto add(int value, int count) -> void {
        total += count;
        values[value] += count;
    }

    auto get_frequent_value() -> std::optional<int> {

        for (auto [value, count] : values) {

            if (count > total * 0.9) {
                return value;
            }
        }

        return {};
    }
};

struct ProfileResult {
    std::unordered_map<std::string, VariableStats> results;

    int total = 0;

    auto add(const std::string &variable, int value, int count) -> void {
        total += count;
        results[variable].add(value, count);
    }

    auto get_variable_stats(const std::string &func) -> VariableStats & { return results[func]; }
};

std::unordered_map<std::string, ProfileResult> parseStats(const std::string &filename) {
    std::ifstream file(filename);

    std::string line;

    std::unordered_map<std::string, ProfileResult> output;

    while (std::getline(file, line)) {
        std::stringstream ss(line);

        std::string func_name;
        std::string variable;
        int value, count;
        double freq;

        std::string temp;

        std::getline(ss, func_name, ','); // foo

        std::getline(ss, variable, ','); // x

        std::getline(ss, temp, ','); // 0
        value = std::stoi(temp);

        std::getline(ss, temp, ','); // 1
        count = std::stoi(temp);

        std::getline(ss, temp, ','); // 0.5
        freq = std::stod(temp);

        output[func_name].add(variable, value, count);

        errs() << "FUNCTION: " << func_name << " " << variable << " " << count << "\n ";
    }

    return output;
}

struct ValueProfiler : public PassInfoMixin<ValueProfiler> {
    PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM) {
        // NOTE: i do not owe anyone $5; i added ALL consts AFTER writing the code

        errs() << "STARTING PASS\n";

        std::string cwd = std::filesystem::current_path().string();
        llvm::errs() << "Pass running in directory: " << cwd << "\n";

        auto stats = parseStats("../../profile_stats.txt");

        errs() << "COLLECTED STATS\n";

        auto const module = F.getParent();
        auto const foo_function = module->getFunction("foo");

        for (auto bb_it = F.begin(); bb_it != F.end();) {
            auto &next_bb = *bb_it++;

            for (auto instruction_it = next_bb.begin(); instruction_it != next_bb.end();) {
                auto &instruction = *instruction_it++;
                auto const call_instr = dyn_cast<CallInst>(&instruction);

                if (call_instr != nullptr && call_instr->getCalledFunction() != nullptr &&
                    stats.contains(call_instr->getCalledFunction()->getName().str())) {

                    errs() << "here: " << call_instr->getCalledFunction()->getName() << "\n";

                    BasicBlock *first_half_bb = next_bb.splitBasicBlockBefore(&instruction);

                    IRBuilder<> bb_builder(first_half_bb);
                    first_half_bb->getTerminator()->eraseFromParent();

                    // use profile data here

                    const auto func = call_instr->getCalledFunction();

                    errs() << func->getName() << "\n";

                    if (func->isDeclaration())
                        continue; // = skip printf, memcpy, profiler, etc.
                    if (func->isDeclaration())
                        continue;

                    // OR: only do functions defined in this module (has body)
                    if (!func->size())
                        continue;

                    SmallVector<Type *, 8> newParamTys;

                    auto &funcStats = stats[call_instr->getCalledFunction()->getName().str()];

                    for (unsigned i = 0; i < func->arg_size(); ++i) {
                        Argument &srcArg = *(func->arg_begin() + i);

                        auto &variableStats = funcStats.get_variable_stats(srcArg.getName().str());

                        errs() << "CHECKING VARIABLE " << srcArg.getName() << "\n";

                        if (!variableStats.get_frequent_value()) {
                            newParamTys.push_back(srcArg.getType());
                        }
                    }

                    FunctionType *new_function_type =
                        FunctionType::get(func->getReturnType(), newParamTys, func->isVarArg());

                    const auto new_function =
                        Function::Create(new_function_type, func->getLinkage(), func->getName() + "_clone", module);

                    ValueToValueMapTy VMap;

                    auto dstAI = new_function->arg_begin();
                    for (unsigned i = 0; i < func->arg_size(); ++i) {
                        Argument &srcArg = *(func->arg_begin() + i);

                        auto &variableStats = funcStats.get_variable_stats(srcArg.getName().str());

                        auto value = variableStats.get_frequent_value();

                        if (value) {
                            VMap[&srcArg] = ConstantInt::get(Type::getInt32Ty(func->getContext()), *value);
                        }
                    }
                    SmallVector<ReturnInst *, 4> returns;
                    CloneFunctionInto(new_function, func, VMap, CloneFunctionChangeType::DifferentModule, returns);

                    auto const new_call_instr = CallInst::Create(new_function_type, new_function);

                    auto const bb_true = BasicBlock::Create(F.getContext(), "new_func", &F, &next_bb);
                    IRBuilder<> bb_true_builder(bb_true);
                    bb_true_builder.Insert(new_call_instr);
                    bb_true_builder.CreateBr(&next_bb);

                    // create the standard branch
                    auto const bb_false = BasicBlock::Create(F.getContext(), "old_func", &F, &next_bb);
                    IRBuilder<> bb_false_builder(bb_false);
                    auto cloned_call = call_instr->clone();
                    bb_false_builder.Insert(cloned_call);
                    bb_false_builder.CreateBr(&next_bb);

                    // join the two branches w/ a phi, replace uses, and get rid of the orignal call
                    auto const phi = PHINode::Create(cloned_call->getType(), 2, "", &*next_bb.begin());
                    phi->addIncoming(new_call_instr, bb_true);
                    phi->addIncoming(cloned_call, bb_false);

                    call_instr->replaceAllUsesWith(phi);
                    call_instr->eraseFromParent();

                    // add the branch from the first half to the true/false blocks
                    auto const cmp = bb_builder.CreateICmpEQ(new_call_instr->getCalledOperand(), foo_function);
                    bb_builder.CreateCondBr(cmp, bb_true, bb_false);
                }

                if (call_instr != nullptr && call_instr->getCalledFunction() == nullptr && foo_function != nullptr) {
                    // split returns the first half
                    BasicBlock *first_half_bb = next_bb.splitBasicBlockBefore(&instruction);

                    IRBuilder<> bb_builder(first_half_bb);
                    first_half_bb->getTerminator()->eraseFromParent();

                    auto const indirect_call_target = call_instr->getCalledOperand();

                    // create the specialized call
                    auto const fixed_indirect_call_instr = dyn_cast<CallInst>(call_instr->clone());
                    fixed_indirect_call_instr->setCalledFunction(foo_function);

                    // create the specialized branch
                    auto const bb_true = BasicBlock::Create(F.getContext(), "true", &F, &next_bb);
                    IRBuilder<> bb_true_builder(bb_true);
                    bb_true_builder.Insert(fixed_indirect_call_instr);
                    bb_true_builder.CreateBr(&next_bb);

                    // create the standard branch
                    auto const bb_false = BasicBlock::Create(F.getContext(), "false", &F, &next_bb);
                    IRBuilder<> bb_false_builder(bb_false);
                    auto cloned_call = call_instr->clone();
                    bb_false_builder.Insert(cloned_call);
                    bb_false_builder.CreateBr(&next_bb);

                    // join the two branches w/ a phi, replace uses, and get rid of the orignal call
                    auto const phi = PHINode::Create(cloned_call->getType(), 2, "", &*next_bb.begin());
                    phi->addIncoming(fixed_indirect_call_instr, bb_true);
                    phi->addIncoming(cloned_call, bb_false);

                    call_instr->replaceAllUsesWith(phi);
                    call_instr->eraseFromParent();

                    // add the branch from the first half to the true/false blocks
                    auto const cmp = bb_builder.CreateICmpEQ(indirect_call_target, foo_function);
                    bb_builder.CreateCondBr(cmp, bb_true, bb_false);
                }
            }
        }

        return PreservedAnalyses::none();
    }
};

struct ValueProfilePass : public PassInfoMixin<ValueProfilePass> {
    PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM) {
        // TODO: actually do some silly fucking optimizations
        return PreservedAnalyses::none();
    }
};
} // namespace

extern "C" auto LLVM_ATTRIBUTE_WEAK llvmGetPassPluginInfo() -> ::llvm::PassPluginLibraryInfo {
    return {.APIVersion = LLVM_PLUGIN_API_VERSION,
            .PluginName = "ValueProfilingPass",
            .PluginVersion = "v0.1",
            .RegisterPassBuilderCallbacks = [](PassBuilder &PB) -> void {
                PB.registerPipelineParsingCallback(
                    [](StringRef Name, FunctionPassManager &FPM, ArrayRef<PassBuilder::PipelineElement>) -> bool {
                        if (Name == "value_profiler") {
                            FPM.addPass(ValueProfiler());
                            return true;
                        }
                        if (Name == "value_profile_opt") {
                            FPM.addPass(ValueProfilePass());
                            return true;
                        }
                        return false;
                    });
            }};
}
