#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"

#include "lambda_capture_collector.cpp"
// #include "lambda_optimizer.cpp"
#include "value_profiling.cpp"

#include "LambdaEscapingSpecializer.hpp"

#include "LambdaOptimizer.cpp"

using namespace llvm;

extern "C" auto LLVM_ATTRIBUTE_WEAK llvmGetPassPluginInfo() -> ::llvm::PassPluginLibraryInfo {
    return {.APIVersion = LLVM_PLUGIN_API_VERSION,
            .PluginName = "ValueProfilingPass",
            .PluginVersion = "v0.1",
            .RegisterPassBuilderCallbacks = [](PassBuilder &PB) -> void {
                // Register FUNCTION PASS: value_profiler
                PB.registerPipelineParsingCallback(
                    [](StringRef Name, FunctionPassManager &FPM, ArrayRef<PassBuilder::PipelineElement>) -> bool {
                        if (Name == "value_profiler") {
                            FPM.addPass(ValueProfiler(parse_data("../../profile_stats.txt")));
                            return true;
                        }
                        return false;
                    });

                // Register MODULE PASS: lambda_capture_collector
                PB.registerPipelineParsingCallback(
                    [](StringRef Name, ModulePassManager &MPM, ArrayRef<PassBuilder::PipelineElement>) -> bool {
                        if (Name == "lambda_capture_collector") {
                            MPM.addPass(LambdaCaptureCollector());
                            return true;
                        }
                        return false;
                    });

                // Register MODULE PASS: lambda_capture_collector
                PB.registerPipelineParsingCallback(
                    [](StringRef Name, ModulePassManager &MPM, ArrayRef<PassBuilder::PipelineElement>) -> bool {
                        if (Name == "lambda_capture_collector") {
                            MPM.addPass(lambdaopt::LambdaOptimizer());
                            return true;
                        }
                        return false;
                    });

                // PB.registerPipelineParsingCallback(
                //     [](StringRef Name, ModulePassManager &MPM, ArrayRef<PassBuilder::PipelineElement>) -> bool {
                //         if (Name == "lambda_optimizer") {
                //             MPM.addPass(LambdaOptimizer());
                //             return true;
                //         }
                //         return false;
                //     });
            }};
}
