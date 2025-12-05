#!/usr/bin/env python3

from pathlib import Path

import sys
from collections import defaultdict

if len(sys.argv) != 3 and len(sys.argv) != 2:
    print("usage: add_logs.py [logfile.txt] <optional n (default = 5)>")
    sys.exit(1)

top_n = 5

logfile: str = sys.argv[1]

# func -> var -> val -> cnt
tnv_table = defaultdict(lambda: defaultdict(lambda: defaultdict(int)))

lambda_construction_count = defaultdict(lambda: defaultdict(int))

with open(logfile) as f:
    lines = f.readlines()

for line in lines:
    if "no args" in line:
        continue

    try:
        _, lambda_name, capture_arg, value = line.strip().split(" ")
    except:
        break

    lambda_construction_count[lambda_name][capture_arg] += 1

    tnv_table[lambda_name][capture_arg][value] += 1

print(tnv_table.keys())


outfile = logfile.removesuffix(".txt") + "_lambda_profdata.txt"

if Path(outfile).exists():
    Path(outfile).unlink()


with open(outfile, "w") as f:
    for lambda_name, var_map in tnv_table.items():
        for var, val_map in var_map.items():
            for val, cnt in sorted(val_map.items(), key=lambda b: -b[1]):
                f.write(
                    f"{lambda_name},{var},{val},{cnt},{cnt / lambda_construction_count[lambda_name][var]}\n"
                )
