#!/usr/bin/env python3
"""CI validation — check system_graph.json types against drivers/registry.yaml.

Usage:
    python tools/check_graph_types.py [system_graph.json] [drivers_dir]

Exit 0: all types valid.
Exit 1: unknown types found (CI fails).
"""

import sys
import os
import json
import yaml


def main() -> int:
    graph_path = sys.argv[1] if len(sys.argv) > 1 else "drivers/system_graph.json"
    drivers_dir = sys.argv[2] if len(sys.argv) > 2 else "drivers"

    # 加载 registry
    registry_path = os.path.join(drivers_dir, "registry.yaml")
    if not os.path.isfile(registry_path):
        print(f"ERROR: registry.yaml not found at {registry_path}")
        return 1

    with open(registry_path) as f:
        registry = yaml.safe_load(f)

    # 收集所有已知 type name + aliases
    known_types = set()
    for drv in registry.get("drivers", []):
        if drv.get("status") != "deprecated":
            known_types.add(drv["name"])
            for alias in drv.get("aliases", []):
                known_types.add(alias)

    # 加载 graph
    if not os.path.isfile(graph_path):
        print(f"ERROR: system_graph.json not found at {graph_path}")
        return 1

    with open(graph_path) as f:
        graph = json.load(f)

    # 校验
    errors = 0
    for node in graph.get("nodes", []):
        t = node.get("type", "")
        if t not in known_types:
            print(f"ERROR: node '{node.get('id', '?')}' has unknown type '{t}'")
            print(f"       Known types: {', '.join(sorted(known_types))}")
            errors += 1

    if errors:
        print(f"\n{errors} unknown type(s) found. Update registry.yaml or fix the Block Design.")
        return 1

    print(f"OK — all {len(graph.get('nodes', []))} node types are valid.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
