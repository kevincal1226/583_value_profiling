#!/usr/bin/env bash
# Run script for Homework 2 CSE 583 Fall 2025
# e.g. sh run.sh benchmarks/correctness/hw2correct1.c
set -e
set -Eeuo pipefail

# ACTION NEEDED: If the path is different, please update it here.
LIB="build/value_profile/ValueProfilingPass.dylib"

mkdir -p build
cd build
cmake -D CMAKE_C_CMPILER=/usr/bin/clang -D CMAKE_CXX_COMPILER=/usr/bin/clang++ ..
make
cd ..

./value_profile/run_profiler $1

if [ ! -f "$LIB" ]; then
    echo "Could not find $LIB. Please build your pass or correct the path in the script."
    exit 1
fi

PATH2LIB=$(realpath "$LIB")

CURRENT_DIR=$(pwd)

# Default to correctness pass
SELECTED_PASS=value_specialize
flag_set=false
generate_viz=false

show_help() {
    cat <<EOF
Usage: $0 [-c|-p][-v] <source_file>

Options:
  -v                Generate visualization of CFG
  -h                Show this help message and exit

Arguments:
  <source_file>     Path to benchmark file (.c)

Examples:
  $0 benchmarks/correctness/hw2correct1.c
  $0 -c benchmarks/correctness/hw2correct2.c
  $0 -v benchmarks/correctness/hw2correct1.c
  $0 -p benchmarks/performance/hw2perf1.c
EOF
}

# Parse options
while getopts ":cpvh" opt; do
    case $opt in
    v)
        generate_viz=true
        ;;
    h)
        show_help
        exit 0
        ;;
    \?)
        echo "Invalid option: -$OPTARG" >&2
        show_help
        exit 1
        ;;
    esac
done

# Shift away parsed options
shift $((OPTIND - 1))

# Check for required positional argument
if [ $# -lt 1 ]; then
    echo "Error: Missing required argument" >&2
    show_help
    exit 1
fi

if [ ! -f $1 ]; then
    echo "Error: $1 is not a valid file" >&2
    show_help
    exit 1
fi

SRC_FILE=$1
FILE_BASENAME=$(basename $SRC_FILE)
# this is to run cpp files and not just c files
# FILENAME=$FILE_BASENAME
FILENAME=${FILE_BASENAME%.*}

cd $(dirname $SRC_FILE)

# Delete outputs from previous runs. Update this when you want to retain some files.
rm -f default.profraw *_prof *_fplicm *.bc *.profdata *_output *.ll

# Convert source code to bitcode (IR).
# clang -emit-llvm -fno-discard-value-names -c ${FILENAME}.cpp -Xclang -disable-O0-optnone -o ${FILENAME}.bc
#
# # Canonicalize natural loops (Ref: llvm.org/doxygen/LoopSimplify_8h_source.html)
# opt -passes='loop-simplify' ${FILENAME}.bc -o ${FILENAME}.ls.bc
#
# # Instrument profiler passes.
# opt -passes='pgo-instr-gen,instrprof' ${FILENAME}.ls.bc -o ${FILENAME}.ls.prof.bc
#
# # Generate binary executable with profiler embedded
# clang -fprofile-instr-generate ${FILENAME}.ls.prof.bc -o ${FILENAME}_prof
#
# # When we run the profiler embedded executable, it generates a default.profraw file that contains the profile data.
# ./${FILENAME}_prof >correct_output
#
# # Converting it to LLVM form. This step can also be used to combine multiple profraw files,
# # in case you want to include different profile runs together.
# llvm-profdata merge -o ${FILENAME}.profdata default.profraw
#
# # The "Profile Guided Optimization Use" pass attaches the profile data to the bc file.
# opt -passes="pgo-instr-use" -o ${FILENAME}.profdata.bc -pgo-test-profile-file=${FILENAME}.profdata <${FILENAME}.ls.prof.bc >/dev/null
#
# # We now use the profile augmented bc file as input to your pass.
# opt -S -load-pass-plugin="${PATH2LIB}" -passes="${SELECTED_PASS}" ${FILENAME}.profdata.bc -o ${FILENAME}.value_profiled.bc >/dev/null

# Convert source code to bitcode (IR).
clang -emit-llvm -fno-discard-value-names -c ${FILENAME}.cpp -Xclang -disable-O0-optnone -o ${FILENAME}.bc

# Canonicalize natural loops (Ref: llvm.org/doxygen/LoopSimplify_8h_source.html)
opt -passes='loop-simplify' ${FILENAME}.bc -o ${FILENAME}.bc

##############################################
#            RUN YOUR PASS
##############################################

# We now use the profile augmented bc file as input to your pass.
opt -S -load-pass-plugin="${PATH2LIB}" -passes="${SELECTED_PASS}" ${FILENAME}.bc -o ${FILENAME}.value_profiled.bc >/dev/null

##############################################
#     RUN O2 OPTIMIZATIONS AFTER YOUR PASS
##############################################
opt -S -passes='default<O2>' ${FILENAME}.value_profiled.bc -o ${FILENAME}.value_profiled.O2.bc

##############################################
#     RUN O2 OPTIMIZATIONS WITHOUT PASS
##############################################
opt -S -passes='default<O2>' ${FILENAME}.bc -o ${FILENAME}.not_value_profiled.O2.bc

# Generate binary excutable before FPLICM: Unoptimzed code
clang++ -fprofile-instr-generate ${FILENAME}.bc -o ${FILENAME}_not_value_profiled
# Generate binary executable after FPLICM: Optimized code
clang++ -fprofile-instr-generate ${FILENAME}.value_profiled.bc -o ${FILENAME}_value_profiled

# Binary AFTER your pass + O2
clang++ -fprofile-instr-generate ${FILENAME}.value_profiled.O2.bc -o ${FILENAME}_value_profiled_O2

# Binary O2 only
clang++ -fprofile-instr-generate ${FILENAME}.not_value_profiled.O2.bc -o ${FILENAME}_O2

generate_cfg_viz() {
    VIZ_TYPE=cfg
    OUTPUT_DIR=$CURRENT_DIR/dot # will put .pdf file here
    TMP_DIR=$OUTPUT_DIR/tmp     # will put .dot files here
    BITCODE_DIR=$(pwd)
    BENCH=$1

    mkdir -p $OUTPUT_DIR
    mkdir -p $TMP_DIR
    cd $TMP_DIR

    # If profile data available, use it
    PROF_FLAGS=""
    PROF_DATA=$BITCODE_DIR/$BENCH.profdata
    if [ $VIZ_TYPE = "cfg" ]; then
        if [ -f $PROF_DATA ]; then
            echo "Using prof data in visualization"
            PROF_FLAGS="-cfg-weights"
        else
            echo "No prof data, not including it in visualization"
        fi
    fi

    if [ -f $PROF_DATA ]; then
        BITCODE=$BITCODE_DIR/$BENCH.profdata.bc
    else
        BITCODE=$BITCODE_DIR/$BENCH.bc
    fi

    # Generate .dot files in tmp dir
    opt $PROF_FLAGS -passes="dot-$VIZ_TYPE" $BITCODE >/dev/null

    # Combine .dot files into PDF
    if [ $VIZ_TYPE = "cfg" ]; then
        DOT_FILES=$(ls .*.dot)
    else
        DOT_FILES=$(ls *.dot)
    fi
    cat $DOT_FILES | dot -Tpdf >$OUTPUT_DIR/$BENCH.$VIZ_TYPE.pdf
    echo "Created $BENCH.$VIZ_TYPE.pdf"
    cd - >/dev/null
    rm -rf $TMP_DIR

}

if $generate_viz; then
    generate_cfg_viz $FILENAME        # Generate CFG visualization without FPLICM
    generate_cfg_viz $FILENAME.fplicm # Generate CFG visualization after FPLICM
fi

# Cleanup: Remove this if you want to retain the created files.
# rm -f default.profraw *_prof *_fplicm *.bc *.profdata *_output *.ll

cd $CURRENT_DIR
