#include <fstream>
#include <sstream>
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

#include "LambdaCommon.hpp"

using namespace llvm;

namespace lambdaopt {

// ================= Profile loading =================

CaptureProfileMap loadCaptureProfiles(const std::string &Path) {
    CaptureProfileMap Profiles;

    std::ifstream in(Path);
    if (!in) {
        llvm::errs() << "[lambda-prof] Could not open profile file: " << Path << "\n";
        return Profiles;
    }

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;

        // Allow comment lines starting with '#'
        if (!line.empty() && line[0] == '#')
            continue;

        std::stringstream ss(line);
        std::string lambdaName, fieldStr, valueStr, countStr, freqStr;

        if (!std::getline(ss, lambdaName, ','))
            continue;
        if (!std::getline(ss, fieldStr, ','))
            continue;
        if (!std::getline(ss, valueStr, ','))
            continue;
        if (!std::getline(ss, countStr, ','))
            continue;
        if (!std::getline(ss, freqStr, ','))
            continue;

        unsigned fieldIndex = static_cast<unsigned>(std::stoul(fieldStr));
        int64_t value = static_cast<int64_t>(std::stoll(valueStr));
        uint64_t count = static_cast<uint64_t>(std::stoull(countStr));
        double freq = std::stod(freqStr);

        // Apply threshold: only keep hot captures
        if (freq < 0.8)
            continue;

        LambdaCaptureKey key{lambdaName, fieldIndex};
        CaptureProfile prof{value, freq, count};

        auto it = Profiles.find(key);
        if (it == Profiles.end() || freq > it->second.Freq) {
            Profiles[key] = prof; // keep hottest for this (lambda, field)
        }
    }

    llvm::errs() << "[lambda-prof] Loaded " << Profiles.size() << " hot capture entries from " << Path << "\n";
    for (const auto &Entry : Profiles) {
        const auto &key = Entry.first;
        const auto &prof = Entry.second;
        llvm::errs() << "  - " << key.Name << ", field " << key.FieldIndex << " -> value " << prof.HotValue
                     << " (freq=" << prof.Freq << ", count=" << prof.Count << ")\n";
    }

    return Profiles;
}

} // namespace lambdaopt
