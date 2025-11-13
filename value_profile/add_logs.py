#!/usr/bin/env python3
import re
import sys
import os

if len(sys.argv) != 3:
    print("usage: add_logs.py <input.cpp> <output.cpp>")
    sys.exit(1)

inp = sys.argv[1]
out = sys.argv[2]

# match: returnType funcName(params) {
func_re = re.compile(
    r"""^
        ([a-zA-Z_][\w:\<\>\s\*&]+?)      # return type
        \s+
        ([a-zA-Z_]\w*)                   # function name
        \s*
        \(
            ([^\)]*)                     # parameters
        \)
        \s*
        \{
    """,
    re.VERBOSE,
)

filename = ""

with open(inp) as f:
    lines = f.readlines()

    filename = os.path.basename(f.name)


new = []

for i, line in enumerate(lines):
    stripped = line.strip()
    m = func_re.match(stripped)

    if not m:
        new.append(line)
        continue

    # IDempotency check:
    j = i + 1
    while j < len(lines) and lines[j].strip() == "":
        j += 1
    if j < len(lines) and ("g_log_file" in lines[j] or "g_param_freq" in lines[j]):
        new.append(line)
        continue

    fname = m.group(2).strip()
    params = m.group(3).strip()

    names = []
    for p in params.split(","):
        p = p.strip()
        if p:
            names.append(p.split()[-1])

    # detect single-line body: has "}" on same line after "{"
    if "}" in stripped and stripped.index("{") < stripped.index("}"):
        # split into: prefix before "{", inner body, suffix "}"
        idx_open = line.index("{")
        idx_close = line.rindex("}")
        before = line[: idx_open + 1]  # up to '{'
        body = line[idx_open + 1 : idx_close].strip()
        after = line[idx_close:]  # including '}'

        new.append(before + "\n")

        if names:
            chain = ' << " " << '.join([f'"{n}=" << {n}' for n in names])
            new.append(f'    g_log_file << "{fname}" << " " << {chain} << std::endl;\n')
            for n in names:
                new.append(f'    g_param_freq["{fname}::{n}"]++;\n')

        # original body
        new.append("    " + body + "\n")
        new.append(after + "\n")
        continue

    # multi-line
    new.append(line)
    if names:
        chain = ' << "," << '.join([f'"{n}=" << {n}' for n in names])
        new.append(f'    g_log_file << "{fname}" << " " << {chain} << std::endl;\n')
        for n in names:
            new.append(f'    g_param_freq["{fname}::{n}"]++;\n')

# inject global setup if missing
if not any("std::ofstream g_log_file" in l for l in new):
    setup = [
        "#include <fstream>\n",
        "#include <unordered_map>\n",
        "#include <string>\n",
        f'std::ofstream g_log_file("logs/{filename}.txt");\n',
        "std::unordered_map<std::string, size_t> g_param_freq;\n",
        "\n",
    ]
    new = setup + new

with open(out, "w") as f:
    f.writelines(new)
