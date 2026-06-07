"""VSC Schema Compiler — main entry point."""

import sys
import os

from vsc_compiler.parser import parse_all
from vsc_compiler.checker import check, Diag
from vsc_compiler.depgraph import build as build_depgraph
from vsc_compiler.codegen import generate as generate_code
from vsc_compiler.checksum import compute as compute_checksum

__version__ = "1.0.0"


def main() -> int:
    """Main entry point. Returns 0 on success, 1 on fatal errors."""
    import argparse

    parser = argparse.ArgumentParser(description="VSC Schema Compiler")
    parser.add_argument("drivers_dir", help="path to drivers/ directory")
    parser.add_argument("output_dir", help="path to output directory")
    parser.add_argument("--graph", default=None,
                        help="path to system_graph.json")
    parser.add_argument("--board", default=None,
                        help="path to board.json")
    args = parser.parse_args()

    drivers_dir = args.drivers_dir
    output_dir = args.output_dir

    if not os.path.isdir(drivers_dir):
        print(f"Error: drivers_dir not found: {drivers_dir}")
        return 1

    # ── Step 1: Parse ──
    print(f"I0001  Compiler version: vsc_prop_gen.py v{__version__}")
    try:
        registry = parse_all(drivers_dir)
    except FileNotFoundError as e:
        print(f"FATAL E0001: {e}")
        return 1
    except Exception as e:
        print(f"FATAL E0002: YAML parse error: {e}")
        return 1

    # ── Step 2: Semantic checks ──
    diags = check(registry)

    fatal_count = 0
    warn_count = 0
    for d in diags:
        print(str(d))
        if d.level == Diag.FATAL:
            fatal_count += 1
        elif d.level == Diag.WARNING:
            warn_count += 1

    if fatal_count > 0:
        print(f"\nFATAL: {fatal_count} error(s) — no output generated.")
        return 1

    # ── Step 3: Dependency graph ──
    dep_edges = build_depgraph(registry)

    # ── Step 4: Checksum ──
    checksum = compute_checksum(registry, drivers_dir)
    print(f"I0004  Checksum: 0x{checksum:08X}")

    # ── Step 5: Code generation ──
    generate_code(registry, dep_edges, checksum, __version__, output_dir)

    # ── Step 6: system_init generation (optional) ──
    if args.graph:
        with open(args.graph, "r", encoding="utf-8") as f:
            graph_json = f.read()
        board_json = ""
        if args.board:
            with open(args.board, "r", encoding="utf-8") as f:
                board_json = f.read()
        from vsc_compiler.codegen import generate_system_init
        generate_system_init(graph_json, board_json, output_dir,
                             checksum, __version__)

    fcount = len(os.listdir(output_dir))
    total_kb = sum(os.path.getsize(os.path.join(output_dir, f))
                   for f in os.listdir(output_dir)) // 1024
    print(f"I0005  Output: {output_dir}/ ({fcount} files, {total_kb} KiB)")

    if warn_count > 0:
        print(f"\nWARNING: {warn_count} warning(s) — output generated with warnings.")
    else:
        print(f"\nOK — output generated successfully.")

    return 0


if __name__ == "__main__":
    sys.exit(main())
