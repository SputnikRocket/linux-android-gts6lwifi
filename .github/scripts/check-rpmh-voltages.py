#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check the RPMh regulators in a DTB against what the hardware can make.

qcom-rpmh-regulator registers a PMIC's regulators all together: when one of
them asks for a voltage range with no selectable voltage in it (for example
3.3 V on an LDO that steps 1.504 V + n * 8 mV), the regulator core fails it
with "unsupportable voltage constraints" and every regulator of that PMIC is
gone, along with everything they supply. Catch that at build time.

Usage: check-rpmh-voltages.py <kernel source> <dtb>...
"""

import re
import struct
import sys


def driver_tables(src):
    code = open(f"{src}/drivers/regulator/qcom-rpmh-regulator.c").read()

    voltages = {}
    for m in re.finditer(r"static const struct rpmh_vreg_hw_data (\w+) = \{(.*?)\n\};",
                         code, re.S):
        volts = set()
        for lo, first, last, step in re.findall(
                r"REGULATOR_LINEAR_RANGE\((\d+),\s*(\d+),\s*(\d+),\s*(\d+)\)", m.group(2)):
            lo, first, last, step = map(int, (lo, first, last, step))
            volts.update(lo + (sel - first) * step for sel in range(first, last + 1))
        voltages[m.group(1)] = volts

    tables = {}
    for m in re.finditer(r"static const struct rpmh_vreg_init_data (\w+)\[\] = \{(.*?)\n\};",
                         code, re.S):
        tables[m.group(1)] = dict(re.findall(r'RPMH_VREG\("(\w+)",[^&]*&(\w+),', m.group(2)))

    compatibles = dict(re.findall(r'\.compatible = "([^"]+)",\s*\.data = (\w+)', code))
    return voltages, tables, compatibles


def dt_nodes(dtb):
    """Yield (path, properties) for every node of a flattened device tree."""
    data = open(dtb, "rb").read()
    magic, _, off_struct, off_strings = struct.unpack_from(">4I", data, 0)
    if magic != 0xd00dfeed:
        sys.exit(f"{dtb}: not a DTB")

    pos, stack = off_struct, []
    while True:
        (token,) = struct.unpack_from(">I", data, pos)
        pos += 4
        if token == 1:  # FDT_BEGIN_NODE
            end = data.index(b"\0", pos)
            stack.append((data[pos:end].decode(), {}))
            pos = (end + 4) & ~3
        elif token == 2:  # FDT_END_NODE
            name, props = stack.pop()
            yield "/".join(n for n, _ in stack[1:]) + "/" + name, props
        elif token == 3:  # FDT_PROP
            length, nameoff = struct.unpack_from(">2I", data, pos)
            pos += 8
            key_end = data.index(b"\0", off_strings + nameoff)
            stack[-1][1][data[off_strings + nameoff:key_end].decode()] = data[pos:pos + length]
            pos = (pos + length + 3) & ~3
        elif token == 4:  # FDT_NOP
            continue
        elif token == 9:  # FDT_END
            return
        else:
            sys.exit(f"{dtb}: bad FDT token {token:#x}")


def cell(value):
    return struct.unpack(">I", value[:4])[0]


def strings(value):
    return [v.decode() for v in value.split(b"\0") if v]


def main():
    src, dtbs = sys.argv[1], sys.argv[2:]
    voltages, tables, compatibles = driver_tables(src)
    failed = False

    for dtb in dtbs:
        nodes = list(dt_nodes(dtb))
        pmics = {path: compatibles.get(compat)
                 for path, props in nodes
                 for compat in strings(props.get("compatible", b""))
                 if compat.endswith("-rpmh-regulators")}
        checked = 0
        for path, props in nodes:
            parent, _, name = path.rpartition("/")
            table = pmics.get(parent)
            if not table or "regulator-min-microvolt" not in props:
                continue
            hw = tables.get(table, {}).get(name)
            if hw is None:
                print(f"::error::{dtb}: {path}: no regulator \"{name}\" in {table}")
                failed = True
                continue
            lo = cell(props["regulator-min-microvolt"])
            hi = cell(props.get("regulator-max-microvolt", props["regulator-min-microvolt"]))
            usable = sorted(v for v in voltages[hw] if lo <= v <= hi)
            checked += 1
            if not usable:
                near = sorted(voltages[hw], key=lambda v: min(abs(v - lo), abs(v - hi)))[:2]
                print(f"::error::{dtb}: {path}: {lo}-{hi} uV has no voltage {hw} can make "
                      f"(nearest: {', '.join(map(str, sorted(near)))}); the regulator core "
                      f"would fail every regulator of this PMIC")
                failed = True
        print(f"{dtb}: {checked} RPMh regulators checked")

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
