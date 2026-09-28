#!/usr/bin/env python3


import sys
from collections import defaultdict

if len(sys.argv) != 3 and len(sys.argv) != 2:
    print("usage: add_logs.py [logfile.txt] <optional n (default = 5)>")
    sys.exit(1)

top_n = 5

logfile: str = sys.argv[1]

# func -> var -> val -> cnt
tnv_table = defaultdict(lambda: defaultdict(lambda: defaultdict(int)))

call_cnt = defaultdict(int)

with open(logfile) as f:
    lines = f.readlines()

for line in lines:
    if "no args" in line:
        continue

    func, _, vars = line.partition(" ")
    call_cnt[func] += 1

    for v in vars.split(","):
        varname, var = v.split("=")
        var = int(var)

        tnv_table[func][varname][var] += 1

        # if len(tnv_table[func][varname]) > top_n:
        #     tnv_table[func][varname].pop(
        #         min(tnv_table[func][varname].items(), key=lambda b: b[1])[0]
        #     )

        # if 0 in tnv_table["multiply"]["y"]:
        #     print(tnv_table["multiply"]["y"])

outfile = logfile.removesuffix(".txt") + "_profdata.txt"
with open(outfile, "w") as f:
    for func, var_map in tnv_table.items():
        for var, val_map in var_map.items():
            for val, cnt in sorted(val_map.items(), key=lambda b: -b[1]):
                f.write(f"{func},{var},{val},{cnt},{cnt / call_cnt[func]}\n")
