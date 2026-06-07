#!/usr/bin/env python3
"""CI validation script — checks that generated files match expected checksum.

Usage:
    python tools/ci_check_vsc_gen.py

Exit code 0 = generated files match.
Exit code 1 = mismatch (CI fails).
"""

import sys
import os

def main():
    gen_dir = os.path.join(os.path.dirname(__file__), "..", "gen", "vsc")
    expected_file = os.path.join(gen_dir, "expected_checksum.txt")
    drivers_dir = os.path.join(os.path.dirname(__file__), "..", "drivers")

    if not os.path.isfile(expected_file):
        print("ERROR: expected_checksum.txt not found")
        return 1

    with open(expected_file) as f:
        expected = f.read().strip()

    # Run the compiler to compute current checksum (dry-run)
    sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
    from vsc_compiler.parser import parse_all
    from vsc_compiler.checksum import compute as compute_checksum

    try:
        registry = parse_all(drivers_dir)
    except Exception as e:
        print(f"ERROR: parse failed: {e}")
        return 1

    current_checksum = compute_checksum(registry, drivers_dir)
    current_hex = f"0x{current_checksum:08X}"

    if current_hex != expected:
        print(f"CHECKSUM MISMATCH:")
        print(f"  Expected: {expected}")
        print(f"  Current:  {current_hex}")
        print(f"")
        print(f"  Schema YAML files have changed. Please:")
        print(f"  1. Review the changes")
        print(f"  2. Run: python tools/vsc_prop_gen.py drivers gen/vsc")
        print(f"  3. Commit the updated gen/vsc/ files")
        return 1

    print(f"OK — checksum matches: {expected}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
