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
#include "llvm/Analysis/LoopPass.h"
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

std::unordered_map<std::string, std::string> optimized_func_map{};

// construct map of var name -> {value, highest_frequency}
auto parse_data(std::string &&filename) -> prof_map_t {
    prof_map_t prof_data;

    std::ifstream ifs{filename};

    std::string func_name;
    std::string var_name;
    std::string value_str;
    std::string frequency_str;
    std::string probability_str;

    while (std::getline(ifs, func_name, ','), std::getline(ifs, var_name, ','), std::getline(ifs, value_str, ','),
           std::getline(ifs, frequency_str, ','), std::getline(ifs, probability_str)) {
        int value{std::stoi(value_str)};
        double probability{std::stod(probability_str)};

        if (probability > prof_data[func_name][var_name].second) {
            prof_data[func_name][var_name] = {value, probability};
        }
    }

    // erase non-hot vars
    std::erase_if(prof_data, [](auto &k) {
        // erase inner keys that don't matter
        std::erase_if(k.second, [](auto &kv) { return kv.second.second < HOT_FREQUENCY_THRESHOLD; });

        // 482 moment, erase outer key
        return k.second.empty();
    });

    return prof_data;
}

struct ValueProfiler : public PassInfoMixin<ValueProfiler> {
    prof_map_t prof_map;

    auto should_optimize_call(Function *const called_function) -> bool {
        return prof_map.contains(demangle_func_name(called_function->getName().str()));
    }

    std::unordered_set<std::string> discovered{};

    ValueProfiler(prof_map_t &&prof_map) : prof_map(std::move(prof_map)) {}

    auto demangle_func_name(const std::string &s) -> std::string {
        std::string demanged_func_name = llvm::demangle(s);
        std::string func_name = demanged_func_name.contains('(')
                                    ? demanged_func_name.erase(demanged_func_name.find_first_of('('))
                                    : demanged_func_name;

        return func_name;
    }

    auto clone_func_if_hot(Function &F) {
        std::string func_name = demangle_func_name(F.getName().str());

        // erase all params where the highest frequency < HOT_FREQUENCY_THRESHOLD

        // if nothing to optimize, don't clone
        if (prof_map[func_name].empty()) {
            return;
        }

        // idk what any of this is
        std::vector<Type *> params;
        for (auto &arg : F.args()) {
            if (!prof_map[func_name].contains(arg.getName().str())) {
                params.push_back(arg.getType());
            }
        }

        auto newFTy = FunctionType::get(F.getReturnType(), params, F.isVarArg());

        // rename function to <name>_opt
        Function *NewF = Function::Create(newFTy, F.getLinkage(), F.getName() + "_opt", F.getParent());

        NewF->copyAttributesFrom(&F);
        ValueToValueMapTy ValueMap;

        // clone functions args???
        auto NFArgIt = NewF->arg_begin();
        for (const Argument &Arg : F.args()) {
            if (!prof_map[func_name].contains(Arg.getName().str())) {
                NFArgIt->setName(Arg.getName());
                ValueMap[&Arg] = &*NFArgIt++;
            }
            else {
                // replace arg with a constant
                ValueMap[&Arg] =
                    ConstantInt::get(Type::getInt32Ty(F.getContext()), prof_map[func_name][Arg.getName().str()].first);
            }
        }

        SmallVector<ReturnInst *, 8> Returns;
        CloneFunctionInto(NewF, &F, ValueMap, CloneFunctionChangeType::DifferentModule, Returns);
        // end of idk what any of this is
    }

    auto try_clone_func(Function &F) {
        std::string mangled_func_name = F.getName().str();

        if (mangled_func_name.contains("_opt") || !discovered.insert(mangled_func_name).second) {
            return;
        }

        std::string func_name = demangle_func_name(mangled_func_name);

        // if it's a function with arguments, clone it
        if (prof_map.contains(func_name)) {
            clone_func_if_hot(F);
        }
    }

    auto insert_phi(CallInst *call_inst, Function &F) {
        Function *called_func = call_inst->getCalledFunction();

        // indirect calls i.e. fun ptrs
        assert(called_func != nullptr);

        std::string called_func_name = called_func->getName().str();
        std::string demangled_called_func_name = demangle_func_name(called_func_name);
        std::string opt_func_name = called_func_name + "_opt";

        std::vector<std::pair<Value *, int>> hot_values;
        std::vector<Value *> cold_values;

        int i = 0;
        for (auto &arg : called_func->args()) {
            Value *val = call_inst->getArgOperand(i);

            if (prof_map[demangled_called_func_name].contains(arg.getName().str())) {
                hot_values.emplace_back(val, prof_map[demangled_called_func_name][arg.getName().str()].first);
            }
            else {
                cold_values.push_back(val);
            }
            ++i;
        }

        // some random boilerplate idk
        LLVMContext &Ctx = F.getContext();
        Instruction *I = call_inst;
        BasicBlock *OrigBB = I->getParent();

        // Split at the call instruction
        BasicBlock *AfterBB = OrigBB->splitBasicBlock(I, "after_call");

        // Remove the branch from the original split (it will be replaced by conditional branch)
        OrigBB->getTerminator()->eraseFromParent();

        // Create Then/Else blocks
        BasicBlock *ThenBB = BasicBlock::Create(Ctx, "then", &F, AfterBB);
        BasicBlock *ElseBB = BasicBlock::Create(Ctx, "else", &F, AfterBB);

        IRBuilder<> B(OrigBB);

        // conditional branch with multiple conds
        Value *cmp = nullptr;
        for (auto &[val, comp_val] : hot_values) {
            Value *tmp_cmp = B.CreateICmpEQ(val, ConstantInt::get(val->getType(), comp_val));
            if (cmp == nullptr) {
                cmp = tmp_cmp;
            }
            else {
                cmp = B.CreateAnd(cmp, tmp_cmp);
            }
        }

        B.CreateCondBr(cmp, ThenBB, ElseBB);

        // Fill Then block
        B.SetInsertPoint(ThenBB);
        Function *optF = F.getParent()->getFunction(opt_func_name);
        Value *tmp1 = B.CreateCall(optF, cold_values);
        B.CreateBr(AfterBB); // terminator

        // Fill Else block
        B.SetInsertPoint(ElseBB);
        // use the original call instruction in the else branch
        Value *tmp2 = call_inst->clone();
        B.Insert(tmp2);
        B.CreateBr(AfterBB); // terminator

        // Insert PHI in AfterBB
        B.SetInsertPoint(AfterBB, AfterBB->begin());
        PHINode *phi = B.CreatePHI(tmp1->getType(), 2);
        phi->addIncoming(tmp1, ThenBB);
        phi->addIncoming(tmp2, ElseBB);

        // Replace uses of original call
        call_inst->replaceAllUsesWith(phi);
        call_inst->eraseFromParent();
    }

    auto optimize_calls(Function &F) {
        std::vector<llvm::CallInst *> func_calls_to_opt;

        for (auto &basic_block : F) {
            for (auto &instruction : basic_block) {
                auto *call_inst = llvm::dyn_cast<llvm::CallInst>(&instruction);

                if (call_inst == nullptr) {
                    continue;
                }

                auto const called_function = call_inst->getCalledFunction();

                // called function might be indirect
                if (called_function == nullptr) {
                    continue;
                }

                if (!should_optimize_call(called_function)) {
                    continue;
                }

                func_calls_to_opt.push_back(call_inst);
            }
        }

        for (auto const &inst : func_calls_to_opt) {
            insert_phi(inst, F);
        }
    }

    PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM) {
        optimize_calls(F);

        try_clone_func(F);

        return PreservedAnalyses::none();
    }
};

} // namespace
