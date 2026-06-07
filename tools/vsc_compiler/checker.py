"""Semantic checker — validates AST against all E/W/I rules."""

from typing import List

from vsc_compiler.ast_nodes import (
    RegistryAST, DriverAST, PropertyAST, TransformAST,
    VALID_TYPES, VALID_FLAGS, VALID_TRANSFORM_TYPES,
    TRANSFORM_REQUIRED_PARAMS, MAX_MULTI_STAGE_DEPTH,
)


# ═══════════════════════════════════════════════════════════════════════
#  Diagnostic types
# ═══════════════════════════════════════════════════════════════════════

class Diag:
    FATAL = "FATAL"
    WARNING = "WARNING"
    INFO = "INFO"

    def __init__(self, level: str, code: str, message: str):
        self.level = level
        self.code = code
        self.message = message

    def __str__(self):
        return f"[{self.level}] {self.code}: {self.message}"


# ═══════════════════════════════════════════════════════════════════════
#  Main entry
# ═══════════════════════════════════════════════════════════════════════

def check(registry: RegistryAST) -> List[Diag]:
    """Run all semantic checks. Returns list of diagnostics."""
    diags: List[Diag] = []

    _check_registry(registry, diags)
    _check_drivers(registry, diags)
    _check_dependencies(registry, diags)
    _check_aliases(registry, diags)

    # info
    active = [d for d in registry.drivers if d.status == "active"]
    total_props = sum(len(d.properties) for d in active)
    diags.append(Diag(Diag.INFO, "I0002",
                      f"Input: {registry.file_path} "
                      f"({len(registry.drivers)} drivers, "
                      f"{len([d for d in registry.drivers if d.status == 'deprecated'])} deprecated)"))
    diags.append(Diag(Diag.INFO, "I0003",
                      f"Processed: {len(active)} active drivers, {total_props} properties"))

    return diags


# ═══════════════════════════════════════════════════════════════════════
#  Registry-level checks
# ═══════════════════════════════════════════════════════════════════════

def _check_registry(registry: RegistryAST, diags: List[Diag]) -> None:
    seen_ids: dict[int, DriverAST] = {}
    seen_names: dict[str, DriverAST] = {}

    for drv in registry.drivers:
        did = drv.driver_id
        name = drv.name

        # E1001 — duplicate Driver_ID
        if did in seen_ids:
            diags.append(Diag(Diag.FATAL, "E1001",
                              f"Duplicate Driver_ID 0x{did:02X}: "
                              f"'{seen_ids[did].name}' and '{name}'"))
        else:
            seen_ids[did] = drv

        # E1002 — duplicate Driver name
        if name in seen_names:
            diags.append(Diag(Diag.FATAL, "E1002",
                              f"Duplicate Driver name '{name}'"))
        else:
            seen_names[name] = drv

        # E1003 — ID out of range
        if not (0x01 <= did <= 0xFE):
            diags.append(Diag(Diag.FATAL, "E1003",
                              f"Driver_ID 0x{did:02X} out of range [0x01, 0xFE] "
                              f"for driver '{name}'"))

        # E1004 — deprecated ID reused (checked by E1001 already —
        #         if an ID is in seen_ids from a deprecated driver,
        #         E1001 fires.  We add a specific message.)
        if did in seen_ids:
            prev = seen_ids[did]
            if prev.status == "deprecated" and drv.status == "active":
                diags.append(Diag(Diag.FATAL, "E1004",
                                  f"Driver_ID 0x{did:02X} was deprecated by "
                                  f"'{prev.name}' and cannot be reused by '{name}'"))


# ═══════════════════════════════════════════════════════════════════════
#  Per-driver checks
# ═══════════════════════════════════════════════════════════════════════

def _check_drivers(registry: RegistryAST, diags: List[Diag]) -> None:
    for drv in registry.drivers:
        if drv.status != "active":
            continue
        _check_properties(drv, diags)
        _check_transform(drv, drv.transform, diags)

        # W2001 — property count warning
        if len(drv.properties) > 128:
            diags.append(Diag(Diag.WARNING, "W2001",
                              f"Driver '{drv.name}' has {len(drv.properties)} "
                              f"properties (recommend < 128)"))


def _check_properties(drv: DriverAST, diags: List[Diag]) -> None:
    seen_indices: dict[int, PropertyAST] = {}
    prop_names: dict[str, PropertyAST] = {}

    for prop in drv.properties:
        idx = prop.index
        name = prop.name

        # E2001 — duplicate property index
        if idx in seen_indices:
            diags.append(Diag(Diag.FATAL, "E2001",
                              f"Driver '{drv.name}': duplicate property index "
                              f"0x{idx:02X} ('{seen_indices[idx].name}' and '{name}')"))
        else:
            seen_indices[idx] = prop

        # E2002 — index out of range
        if not (0x00 <= idx <= 0xFE):
            diags.append(Diag(Diag.FATAL, "E2002",
                              f"Driver '{drv.name}': property index 0x{idx:02X} "
                              f"out of range [0x00, 0xFE]"))

        # E2004 — invalid type
        if prop.ptype not in VALID_TYPES:
            diags.append(Diag(Diag.FATAL, "E2004",
                              f"Driver '{drv.name}', property '{name}': "
                              f"unknown type '{prop.ptype}'"))

        # E2005 — unknown flag
        for flag in prop.flags:
            if flag not in VALID_FLAGS:
                diags.append(Diag(Diag.FATAL, "E2005",
                                  f"Driver '{drv.name}', property '{name}': "
                                  f"unknown flag '{flag}'"))

        # W2002 — default out of range
        _check_default_range(drv, prop, diags)

        prop_names[name] = prop

    # E2003 — max_ref / min_ref validity (deferred until all props seen)
    for prop in drv.properties:
        for ref_kind, ref_name in [("max_ref", prop.max_ref),
                                    ("min_ref", prop.min_ref)]:
            if ref_name and ref_name not in prop_names:
                diags.append(Diag(Diag.FATAL, "E2003",
                                  f"Driver '{drv.name}', property '{prop.name}': "
                                  f"{ref_kind} references unknown property '{ref_name}'"))


def _check_default_range(drv: DriverAST, prop: PropertyAST,
                         diags: List[Diag]) -> None:
    """W2002: default value outside min/max range."""
    if prop.default is None:
        return
    try:
        default = float(prop.default) if not isinstance(prop.default, bool) else None
    except (TypeError, ValueError):
        return
    if default is None:
        return
    if prop.min_val is not None and default < float(prop.min_val):
        diags.append(Diag(Diag.WARNING, "W2002",
                          f"Driver '{drv.name}', property '{prop.name}': "
                          f"default {prop.default} < min {prop.min_val}"))
    if prop.max_val is not None and default > float(prop.max_val):
        diags.append(Diag(Diag.WARNING, "W2002",
                          f"Driver '{drv.name}', property '{prop.name}': "
                          f"default {prop.default} > max {prop.max_val}"))


# ═══════════════════════════════════════════════════════════════════════
#  Transform checks
# ═══════════════════════════════════════════════════════════════════════

def _check_transform(drv: DriverAST, t: TransformAST | None,
                     diags: List[Diag], depth: int = 0) -> None:
    if t is None:
        return

    # E3001 — invalid transform type
    if t.ttype not in VALID_TRANSFORM_TYPES:
        diags.append(Diag(Diag.FATAL, "E3001",
                          f"Driver '{drv.name}': unknown transform type '{t.ttype}'"))
        return

    # E3003 — MULTI_STAGE depth
    if depth > MAX_MULTI_STAGE_DEPTH:
        diags.append(Diag(Diag.FATAL, "E3003",
                          f"Driver '{drv.name}': MULTI_STAGE depth {depth} "
                          f"exceeds max {MAX_MULTI_STAGE_DEPTH}"))
        return

    # E3002 — missing required params
    required = TRANSFORM_REQUIRED_PARAMS.get(t.ttype, [])
    for rp in required:
        if rp not in t.params:
            # CROP has optional *_ref alternatives to *_w / *_h
            if t.ttype == "CROP":
                # max_w_ref can substitute for max_w; min_w is still required
                if rp in ("max_w",) and "max_w_ref" in t.params:
                    continue
                if rp in ("max_h",) and "max_h_ref" in t.params:
                    continue
            diags.append(Diag(Diag.FATAL, "E3002",
                              f"Driver '{drv.name}', transform '{t.ttype}': "
                              f"missing required param '{rp}'"))

    # recurse into MULTI_STAGE
    if t.ttype == "MULTI_STAGE" and t.stages:
        for stage in t.stages:
            _check_transform(drv, stage, diags, depth + 1)


# ═══════════════════════════════════════════════════════════════════════
#  Dependency graph checks (E4001)
# ═══════════════════════════════════════════════════════════════════════

def _check_dependencies(registry: RegistryAST, diags: List[Diag]) -> None:
    """E4001 — detect cycles in max_ref / min_ref across all drivers."""
    # Build adjacency: (driver_name, prop_name) → list of (driver_name, prop_name)
    adj: dict[tuple[str, str], list[tuple[str, str]]] = {}
    all_nodes: set[tuple[str, str]] = set()

    for drv in registry.drivers:
        if drv.status != "active":
            continue
        for prop in drv.properties:
            node = (drv.name, prop.name)
            all_nodes.add(node)
            adj.setdefault(node, [])

            for ref_name in [prop.max_ref, prop.min_ref]:
                if ref_name:
                    target = (drv.name, ref_name)  # refs are same-driver only
                    adj[node].append(target)

    # Kahn topological sort
    in_degree: dict[tuple[str, str], int] = {n: 0 for n in all_nodes}
    for src, targets in adj.items():
        for tgt in targets:
            in_degree[tgt] = in_degree.get(tgt, 0) + 1

    queue = [n for n in all_nodes if in_degree.get(n, 0) == 0]
    sorted_count = 0

    while queue:
        u = queue.pop(0)
        sorted_count += 1
        for v in adj.get(u, []):
            in_degree[v] -= 1
            if in_degree[v] == 0:
                queue.append(v)

    if sorted_count != len(all_nodes):
        # find cycle nodes
        cycle_nodes = [n for n, d in in_degree.items() if d > 0]
        path_str = " → ".join(f"{d}.{p}" for d, p in cycle_nodes[:6])
        diags.append(Diag(Diag.FATAL, "E4001",
                          f"Dependency cycle detected: {path_str}"))


# ═══════════════════════════════════════════════════════════════════════
#  Alias uniqueness (E5001)
# ═══════════════════════════════════════════════════════════════════════

def _check_aliases(registry: RegistryAST, diags: List[Diag]) -> None:
    seen_aliases: dict[str, DriverAST] = {}

    for drv in registry.drivers:
        for alias in drv.aliases:
            if alias in seen_aliases:
                diags.append(Diag(Diag.FATAL, "E5001",
                                  f"Duplicate alias '{alias}': used by "
                                  f"'{seen_aliases[alias].name}' and '{drv.name}'"))
            else:
                seen_aliases[alias] = drv
