#!/usr/bin/env python3
"""Summarise a CPU Counters export (macOS, Instruments' MetricTable).

Usage: counters_parse.py <MetricTable-export.xml>

Prints the median of each metric over all 1 ms samples of the target
process. Metric values that Instruments formats with thousands separators
(cycle counts) are unformatted first.
"""
import collections
import sys
import xml.etree.ElementTree as ET


def main(path):
    root = ET.parse(path).getroot()
    refs = {}
    for el in root.iter():
        i = el.get("id")
        if i is not None:
            refs[i] = el

    def resolve(el):
        while el is not None and el.get("ref"):
            el = refs.get(el.get("ref"))
        return el

    stats = collections.defaultdict(list)
    for row in root.iter("row"):
        name = value = None
        for child in row:
            el = resolve(child)
            if el is None:
                continue
            if el.tag == "string" and name is None:
                name = el.get("fmt")
            elif el.tag == "fixed-decimal":
                value = float(el.get("fmt").replace(",", ""))
        if name is not None and value is not None:
            stats[name].append(value)

    for name, vals in sorted(stats.items(), key=lambda kv: -len(kv[1])):
        if len(vals) < 2:
            continue
        vals.sort()
        n = len(vals)
        print("%-38s n=%-6d median=%-14.4f mean=%-14.4f min=%-10.4f max=%-10.4f"
              % (name, n, vals[n // 2], sum(vals) / n, vals[0], vals[-1]))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
