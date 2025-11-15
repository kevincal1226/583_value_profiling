#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string_view>
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

using namespace llvm;

using value_frequency_pair_t = std::pair<int, double>;
using prof_map_t = std::unordered_map<std::string, std::unordered_map<std::string, value_frequency_pair_t>>;


namespace {
constexpr double HOT_FREQUENCY_THRESHOLD = 0.8;

std::unordered_map<std::string, std::string> optimized_func_map {};

// construct map of var name -> {value, highest_frequency}
auto parse_data(std::string&& filename) -> prof_map_t {
    prof_map_t prof_data;

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

        if (probability > prof_data[func_name][var_name].second) {
            prof_data[func_name][var_name] = { value, probability };
        }
    }

    // erase non-hot vars
    std::for_each(prof_data.begin(), prof_data.end(), [](auto& k) {
        std::erase_if(k.second, [](auto& kv) { return kv.second.second < HOT_FREQUENCY_THRESHOLD; });
    });

    return prof_data;
}

struct ValueProfiler : public PassInfoMixin<ValueProfiler> {
    prof_map_t prof_map;
    std::unordered_set<std::string> discovered {};

    ValueProfiler(prof_map_t&& prof_map)
        : prof_map(std::move(prof_map)) {}

    auto demangle_func_name(const std::string& s) -> std::string {
        std::string demanged_func_name = llvm::demangle(s);
        std::string func_name = demanged_func_name.contains('(')
                                ? demanged_func_name.erase(demanged_func_name.find_first_of('('))
                                : demanged_func_name;

        return func_name;
    }

    void wipe_uses(llvm::Function* F, const std::string& var_name, const int new_val) {
        llvm::AllocaInst* yAddr = nullptr;

        // 1. Find %y.addr (the alloca for argument y)
        for (auto& BB : *F) {
            for (auto& I : BB) {
                if (auto* AI = llvm::dyn_cast<llvm::AllocaInst>(&I)) {
                    if (AI->getName() == var_name + ".addr") {
                        yAddr = AI;
                        break;
                    }
                }
            }
            if (yAddr) break;
        }

        if (!yAddr) return;   // not found

        llvm::SmallVector<llvm::Instruction*, 8> toErase;

        // 2. Iterate through all uses of y.addr
        for (llvm::User* U : llvm::make_early_inc_range(yAddr->users())) {
            if (auto* SI = llvm::dyn_cast<llvm::StoreInst>(U)) {
                // store x -> y.addr  REMOVE IT
                toErase.push_back(SI);
            } else if (auto* LI = llvm::dyn_cast<llvm::LoadInst>(U)) {
                // load from y.addr -> REPLACE WITH CONSTANT
                llvm::IRBuilder<> builder(LI);
                llvm::Value* constant = llvm::ConstantInt::get(LI->getType(), new_val);
                LI->replaceAllUsesWith(constant);

                toErase.push_back(LI);
            } else {
                llvm::errs() << "Unexpected *.addr user: " << *U << "\n";
            }
        }

        // 3. Erase the loads + stores
        for (auto* I : toErase) I->eraseFromParent();

        // 4. Remove the alloca itself
        if (yAddr->use_empty()) yAddr->eraseFromParent();
    }


    auto clone_func_if_hot(Function& F) -> Function* {
        std::string func_name = demangle_func_name(F.getName().str());

        // erase all params where the highest frequency < HOT_FREQUENCY_THRESHOLD

        // if nothing to optimize, don't clone
        if (prof_map[func_name].empty()) {
            return nullptr;
        }

        // idk what any of this is
        std::vector<Type*> params;
        for (auto& arg : F.args()) {
            if (!prof_map[func_name].contains(arg.getName().str())) {
                params.push_back(arg.getType());
            }
        }

        auto newFTy = FunctionType::get(F.getReturnType(), params, F.isVarArg());

        // rename function to <name>_opt
        Function* NewF = Function::Create(newFTy, F.getLinkage(), F.getName() + "_opt", F.getParent());

        NewF->copyAttributesFrom(&F);
        ValueToValueMapTy VMap;

        // clone functions args???
        auto NFArgIt = NewF->arg_begin();
        for (const Argument& Arg : F.args()) {
            if (!prof_map[func_name].contains(Arg.getName().str())) {
                NFArgIt->setName(Arg.getName());
                VMap[&Arg] = &*NFArgIt++;
            } else {
                // ????
                VMap[&Arg] = ConstantInt::get(Type::getInt32Ty(F.getContext()), 0);
            }
        }

        SmallVector<ReturnInst*, 8> Returns;
        CloneFunctionInto(NewF, &F, VMap, CloneFunctionChangeType::DifferentModule, Returns);
        // end of idk what any of this is


        // wipe all uses of things we've profiled
        for (const auto& [var_name, value_and_freq] : prof_map[func_name]) {
            wipe_uses(NewF, var_name, value_and_freq.first);
        }

        return NewF;
    }

    auto try_clone_func(Function& F) -> Function* {
        std::string mangled_func_name = F.getName().str();

        if (mangled_func_name.contains("_opt") || !discovered.insert(mangled_func_name).second) {
            return nullptr;
        }

        std::string func_name = demangle_func_name(mangled_func_name);

        // if it's a function with arguments, clone it
        if (prof_map.contains(func_name)) {
            return clone_func_if_hot(F);
        }

        return nullptr;
    }

    PreservedAnalyses run(Function& F, FunctionAnalysisManager& FAM) {
        auto NewF = try_clone_func(F);
        if (NewF == nullptr) {
            return PreservedAnalyses::none();
        }

        return PreservedAnalyses::none();
    }
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
                           FPM.addPass(ValueProfiler(parse_data("../../profile_stats.txt")));
                           return true;
                       }
                       return false;
                   });
             } };
}
