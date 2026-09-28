#include <fstream>
#include <ranges>
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
    std::erase_if(prof_data, [](auto& k) {
        // erase inner keys that don't matter
        std::erase_if(k.second, [](auto& kv) { return kv.second.second < HOT_FREQUENCY_THRESHOLD; });

        // 482 moment, erase outer key
        return k.second.empty();
    });

    return prof_data;
}

struct FrequentValueSpecializer : public PassInfoMixin<FrequentValueSpecializer> {
    prof_map_t prof_map;

    auto should_optimize_call(Function* const called_function) -> bool {
        return prof_map.contains(demangle_func_name(called_function->getName().str()));
    }

    std::unordered_set<std::string> discovered {};

    FrequentValueSpecializer(prof_map_t&& prof_map)
        : prof_map(std::move(prof_map)) {}

    auto demangle_func_name(std::string const& s) -> std::string {
        std::string demanged_func_name = llvm::demangle(s);
        std::string func_name = demanged_func_name.contains('(')
                                ? demanged_func_name.erase(demanged_func_name.find_first_of('('))
                                : demanged_func_name;

        return func_name;
    }

    auto clone_func_if_hot(Function& F) {
        std::string func_name = demangle_func_name(F.getName().str());

        // erase all params where the highest frequency < HOT_FREQUENCY_THRESHOLD

        // if nothing to optimize, don't clone
        if (prof_map[func_name].empty()) {
            return;
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
        ValueToValueMapTy ValueMap;

        // clone functions args???
        auto NFArgIt = NewF->arg_begin();
        for (Argument const& Arg : F.args()) {
            if (!prof_map[func_name].contains(Arg.getName().str())) {
                NFArgIt->setName(Arg.getName());
                ValueMap[&Arg] = &*NFArgIt++;
            } else {
                // replace arg with a constant
                ValueMap[&Arg]
                  = ConstantInt::get(Type::getInt32Ty(F.getContext()), prof_map[func_name][Arg.getName().str()].first);
            }
        }

        SmallVector<ReturnInst*, 8> Returns;
        CloneFunctionInto(NewF, &F, ValueMap, CloneFunctionChangeType::DifferentModule, Returns);
        // end of idk what any of this is
    }

    auto try_clone_func(Function& F) {
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

    auto insert_phi(CallInst* call_inst, Function& F) {
        Function* called_func = call_inst->getCalledFunction();

        // indirect calls i.e. fun ptrs
        assert(called_func != nullptr);

        std::string called_func_name = called_func->getName().str();
        std::string demangled_called_func_name = demangle_func_name(called_func_name);
        std::string opt_func_name = called_func_name + "_opt";

        std::vector<std::pair<Value*, int>> hot_values;
        std::vector<Value*> cold_values;

        int i = 0;
        for (auto& arg : called_func->args()) {
            Value* val = call_inst->getArgOperand(i);

            if (prof_map[demangled_called_func_name].contains(arg.getName().str())) {
                hot_values.emplace_back(val, prof_map[demangled_called_func_name][arg.getName().str()].first);
            } else {
                cold_values.push_back(val);
            }
            ++i;
        }

        // some random boilerplate idk
        LLVMContext& Ctx = F.getContext();
        Instruction* I = call_inst;
        BasicBlock* OrigBB = I->getParent();

        // Split at the call instruction
        BasicBlock* AfterBB = OrigBB->splitBasicBlock(I, "after_call");

        // Remove the branch from the original split (it will be replaced by conditional branch)
        OrigBB->getTerminator()->eraseFromParent();

        // Create Then/Else blocks
        BasicBlock* ThenBB = BasicBlock::Create(Ctx, "then", &F, AfterBB);
        BasicBlock* ElseBB = BasicBlock::Create(Ctx, "else", &F, AfterBB);

        IRBuilder<> B(OrigBB);

        // conditional branch with multiple conds
        Value* cmp = nullptr;
        for (auto& [val, comp_val] : hot_values) {
            Value* tmp_cmp = B.CreateICmpEQ(val, ConstantInt::get(val->getType(), comp_val));
            if (cmp == nullptr) {
                cmp = tmp_cmp;
            } else {
                cmp = B.CreateAnd(cmp, tmp_cmp);
            }
        }

        B.CreateCondBr(cmp, ThenBB, ElseBB);

        // Fill Then block
        B.SetInsertPoint(ThenBB);
        Function* optF = F.getParent()->getFunction(opt_func_name);
        Value* tmp1 = B.CreateCall(optF, cold_values);
        B.CreateBr(AfterBB);   // terminator

        // Fill Else block
        B.SetInsertPoint(ElseBB);
        // use the original call instruction in the else branch
        Value* tmp2 = call_inst->clone();
        B.Insert(tmp2);
        B.CreateBr(AfterBB);   // terminator

        // Insert PHI in AfterBB
        B.SetInsertPoint(AfterBB, AfterBB->begin());
        PHINode* phi = B.CreatePHI(tmp1->getType(), 2);
        phi->addIncoming(tmp1, ThenBB);
        phi->addIncoming(tmp2, ElseBB);

        // Replace uses of original call
        call_inst->replaceAllUsesWith(phi);
        call_inst->eraseFromParent();
    }

    auto optimize_calls(Function& F) {
        std::vector<llvm::CallInst*> func_calls_to_opt;

        for (auto& basic_block : F) {
            for (auto& instruction : basic_block) {
                auto* call_inst = llvm::dyn_cast<llvm::CallInst>(&instruction);

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

        for (auto const& inst : func_calls_to_opt) {
            insert_phi(inst, F);
        }
    }

    PreservedAnalyses run(Function& F, FunctionAnalysisManager& FAM) {
        optimize_calls(F);

        try_clone_func(F);

        return PreservedAnalyses::none();
    }
};

struct IndirectCallOptimizer : public PassInfoMixin<IndirectCallOptimizer> {
    static constexpr double optimize_threshold = 0.5;

    using instruction_name_t = std::string;
    using function_name_t = std::string;
    std::unordered_map<instruction_name_t, std::vector<function_name_t>> optimizable_indirect_calls;
    std::unordered_set<Instruction*> optimized_call_instr;
    // prof_map_t prof_map;

    // IndirectCallOptimizer(prof_map_t&& prof_map)
    //     : prof_map(std::move(prof_map)) {}

    auto parse_indirect_call_data(std::string const& file) {
        std::unordered_map<std::string, function_name_t> func_ptr_to_func_name;
        std::unordered_map<instruction_name_t, std::unordered_map<function_name_t, uint32_t>> indirect_call_counts;

        // parse the log data
        std::ifstream fs { file };
        std::string log_type;
        while (fs >> log_type) {
            if (log_type == "FMAP") {
                std::string fptr;
                std::string func_name;
                std::string trash;

                if (!(fs >> fptr) || !(fs >> trash) || !(fs >> func_name)) {
                    break;
                }

                assert(!func_ptr_to_func_name.contains(fptr));
                assert(indirect_call_counts.empty());

                func_ptr_to_func_name[fptr] = func_name;
            } else if (log_type == "ICALL") {
                std::string var_name;
                std::string f_ptr;

                if (!(fs >> var_name) || !(fs >> f_ptr)) {
                    break;
                }

                assert(func_ptr_to_func_name.contains(f_ptr));

                auto const& func_name = func_ptr_to_func_name[f_ptr];

                indirect_call_counts[var_name][func_name] += 1;
            }
        }

        optimizable_indirect_calls
          = indirect_call_counts
          | std::ranges::views::transform(
              [](std::pair<instruction_name_t, std::unordered_map<function_name_t, uint32_t>> const& func_calls)
                -> std::pair<instruction_name_t, std::vector<function_name_t>> {
                  auto const total_calls
                    = std::ranges::fold_left(func_calls.second | std::ranges::views::values, 0,
                                             [](uint32_t curr, uint32_t v) -> uint32_t { return curr + v; });

                  return { func_calls.first,
                           func_calls.second
                             | std::ranges::views::filter(
                               [total_calls](std::pair<function_name_t, uint32_t> const& kv) -> bool {
                                   return kv.second / static_cast<double>(total_calls) >= optimize_threshold;
                               })
                             | std::ranges::views::keys | std::ranges::to<std::vector<function_name_t>>() };
              })
          | std::ranges::to<std::unordered_map<instruction_name_t, std::vector<function_name_t>>>();
    }

    IndirectCallOptimizer(std::string const& file) { parse_indirect_call_data(file); }

    auto replace_indirect_call_with(Function& func, CallInst* const call_instr,
                                    std::vector<Function*> const& functions) {
        if (functions.empty()) {
            return;
        }

        auto const before_bb = call_instr->getParent();

        BasicBlock* const after_bb = before_bb->splitBasicBlock(call_instr, "after_call");

        auto& context = before_bb->getContext();

        IRBuilder<> builder { after_bb, after_bb->begin() };

        PHINode* const phi
          = call_instr->getType()->isVoidTy() ? nullptr : builder.CreatePHI(call_instr->getType(), functions.size());

        auto const indirect_call_target = call_instr->getCalledOperand();

        // all this basically does is generate an if ... else if ... else ... based on the
        // number of functions to optimize
        auto const substitute_functions
          = [&](this auto&& self, BasicBlock* const replace_bb, size_t const function_i = 0) -> auto {
            auto const actual_function = functions[function_i];

            BasicBlock* const then_bb = BasicBlock::Create(context, "then_bb", &func, after_bb);
            BasicBlock* const else_bb = BasicBlock::Create(context, "else_bb", &func, after_bb);

            replace_bb->getTerminator()->eraseFromParent();


            // create the function pointer check
            builder.SetInsertPoint(replace_bb);
            auto const cmp = builder.CreateICmpEQ(indirect_call_target, actual_function);
            builder.CreateCondBr(cmp, then_bb, else_bb);


            // create and insert the fixed call instruction
            auto const fixed_call_instr = dyn_cast<CallInst>(call_instr->clone());
            fixed_call_instr->setCalledFunction(actual_function);

            builder.SetInsertPoint(then_bb);
            builder.Insert(fixed_call_instr);
            builder.CreateBr(after_bb);

            if (phi != nullptr) {
                phi->addIncoming(fixed_call_instr, then_bb);
            }

            // create the else block
            auto const new_call_instr = call_instr->clone();
            optimized_call_instr.insert(new_call_instr);

            builder.SetInsertPoint(else_bb);
            builder.Insert(new_call_instr);
            builder.CreateBr(after_bb);

            // create the branch for the next instruction
            if (function_i + 1 >= functions.size()) {
                if (phi != nullptr) {
                    phi->addIncoming(new_call_instr, else_bb);
                }

                return;
            }
            self(else_bb, function_i + 1);
        };

        substitute_functions(before_bb);

        if (phi != nullptr) {
            call_instr->replaceAllUsesWith(phi);
        }

        call_instr->eraseFromParent();
    }

    auto run(Function& F, FunctionAnalysisManager& FAM) -> PreservedAnalyses {
        auto module = F.getParent();

        std::vector<std::pair<CallInst*, std::vector<Function*>>> calls_to_optimize;
        for (auto& bb : F) {
            for (auto& instruction : bb) {
                auto const call_instr = dyn_cast<CallInst>(&instruction);

                if (call_instr == nullptr) {
                    continue;
                }

                auto const is_indirect_call = call_instr->getCalledFunction() == nullptr;
                if (!is_indirect_call) {
                    continue;
                }


                auto const call_name = call_instr->getNameOrAsOperand();
                if (!optimizable_indirect_calls.contains(call_name)) {
                    continue;
                }

                if (optimized_call_instr.contains(&instruction)) {
                    continue;
                }

                auto const functions
                  = optimizable_indirect_calls[call_name]
                  | std::ranges::views::transform(
                      [module](std::string const& func_name) -> Function* { return module->getFunction(func_name); })
                  | std::ranges::to<std::vector<Function*>>();

                assert(!functions.empty());

                calls_to_optimize.emplace_back(call_instr, functions);
            }
        }

        for (auto& [call_instr, functions] : calls_to_optimize) {
            replace_indirect_call_with(F, call_instr, functions);
        }

        return PreservedAnalyses::none();
    }
};

struct ValueProfiler : PassInfoMixin<ValueProfiler> {
    GlobalValue* global_fileptr {};
    std::string log_directory;

    ValueProfiler(std::string log_directory)
        : log_directory(std::move(log_directory)) {}

    static auto get_or_insert_fprintf_func(llvm::Module& M) -> llvm::FunctionCallee {
        LLVMContext& ctx = M.getContext();

        FunctionType* fprintf_type = FunctionType::get(llvm::Type::getInt32Ty(ctx),
                                                       { PointerType::get(ctx, 0), PointerType::get(ctx, 0) }, true);

        return M.getOrInsertFunction("fprintf", fprintf_type);
    }

    static auto make_fmt(Module& module, LLVMContext& context, std::string const& str) -> GlobalVariable* {
        Constant* const fmt_data = ConstantDataArray::getString(context, str, true);
        auto* const fmt
          = new GlobalVariable(module, fmt_data->getType(), true, GlobalValue::PrivateLinkage, fmt_data, ".fmt");

        return fmt;
    }

    /*
     * Basically just inserts a thing to print this function's name and a pointer to it at the start of the main
     * function
     */
    auto insert_ptr_to_func_mapping(Function& F, Module& module, LLVMContext& context, BasicBlock& insert_into_bb,
                                    BasicBlock::iterator const& insert_instr_at) -> void {
        // build the formatting string
        auto* const fmt = make_fmt(module, context, "FMAP %p -> " + F.getNameOrAsOperand() + "\n");

        IRBuilder<> builder(&insert_into_bb, insert_instr_at);

        // get pointers to the fmt string and function
        Value* const fmt_ptr = builder.CreateBitCast(fmt, PointerType::get(context, 0));
        Value* const func_ptr = builder.CreateBitCast(&F, PointerType::get(context, 0));

        // insert the actual call
        Value* file_ptr = builder.CreateLoad(builder.getPtrTy(), global_fileptr);
        auto call = builder.CreateCall(get_or_insert_fprintf_func(module), { file_ptr, fmt_ptr, func_ptr });
    }

    auto print_indirect_call(Module& module, LLVMContext& context, CallInst* call_inst) {
        IRBuilder<> builder { call_inst };

        // get the variables name and create the fmt string for it
        std::string const name = call_inst->getNameOrAsOperand();
        GlobalVariable* const fmt = make_fmt(module, context, "ICALL " + name + " %p\n");

        // get pointers to the fmt string and function
        Value* const fmt_ptr = builder.CreateBitCast(fmt, PointerType::get(context, 0));
        Value* const indirect_target = call_inst->getCalledOperand();

        // insert the actual call
        Value* file_ptr = builder.CreateLoad(builder.getPtrTy(), global_fileptr);
        builder.CreateCall(get_or_insert_fprintf_func(module), { file_ptr, fmt_ptr, indirect_target });
    }

    auto insert_fopen(Module& module, LLVMContext& context, IRBuilder<>& builder, std::string const& filename) -> void {
        // create a global pointer for the output file
        global_fileptr
          = new GlobalVariable(module, PointerType::get(context, 0), false, GlobalValue::ExternalLinkage,
                               llvm::ConstantPointerNull::get(PointerType::get(context, 0)), "global_file_ptr");

        PointerType* const ptr_ty = PointerType::get(context, 0);
        FunctionType* const fopen_type = FunctionType::get(ptr_ty, { ptr_ty, ptr_ty }, false);
        FunctionCallee const fopen_func = module.getOrInsertFunction("fopen", fopen_type);

        // Format string literals
        auto* const filename_str = builder.CreateGlobalString(filename);
        auto* const mode_str = builder.CreateGlobalString("w");

        // Call fopen
        Value* const file_ptr_value = builder.CreateCall(fopen_func, { filename_str, mode_str });

        // save it in global variable
        builder.CreateStore(file_ptr_value, global_fileptr);
    }

    auto run(Function& F, FunctionAnalysisManager& FAM) -> PreservedAnalyses {
        Module* const module = F.getParent();
        LLVMContext& context = module->getContext();
        BasicBlock& main_func_bb = *module->getFunction("main")->begin();

        // Insert a call to fopen at the very start of main
        if (global_fileptr == nullptr) {
            IRBuilder<> builder(&main_func_bb, main_func_bb.begin());
            insert_fopen(*module, context, builder, log_directory);
        }

        // We need to insert everything else after the fopen
        auto main_insert_pos = main_func_bb.begin();
        std::advance(main_insert_pos, 2);
        insert_ptr_to_func_mapping(F, *module, context, main_func_bb, main_insert_pos);

        for (auto& bb : F) {
            for (auto& instr : bb) {
                // profile indirect function call
                auto* const call_inst = dyn_cast<CallInst>(&instr);
                auto const is_indirect_call = call_inst != nullptr && call_inst->getCalledFunction() == nullptr;
                if (is_indirect_call) {
                    print_indirect_call(*module, context, call_inst);
                }
            }
        }

        return PreservedAnalyses::none();
    }
};

}   // namespace
