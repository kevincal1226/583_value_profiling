#!/usr/bin/env python3


import sys
from collections import defaultdict
from icecream import ic

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

    _, func, vars = line.split(" ")
    for v in vars.split(","):
        varname, val = v.split("=")
        val = int(val)

        call_cnt[func] += 1

        tnv_table[func][varname][val] += 1

        if len(tnv_table[func][varname]) > top_n:
            tnv_table[func][varname].pop(min(tnv_table[func][varname].items(), key=lambda b: b[1])[0])

ic(tnv_table)
ic(call_cnt)
