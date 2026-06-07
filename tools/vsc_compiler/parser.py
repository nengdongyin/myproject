"""YAML parser: registry.yaml + *.schema.yaml → AST."""

import os
import yaml
from typing import List, Optional

from vsc_compiler.ast_nodes import (
    DriverAST, RegistryAST, PropertyAST, TransformAST,
    MAX_MULTI_STAGE_DEPTH,
)


# ═══════════════════════════════════════════════════════════════════════
#  Public entry points
# ═══════════════════════════════════════════════════════════════════════

def parse_all(drivers_dir: str) -> RegistryAST:
    """Parse registry.yaml + all active drivers' .schema.yaml files.

    Args:
        drivers_dir: path to the drivers/ directory.

    Returns:
        RegistryAST with all active drivers fully populated.
    """
    registry_path = os.path.join(drivers_dir, "registry.yaml")
    if not os.path.isfile(registry_path):
        raise FileNotFoundError(f"registry.yaml not found at {registry_path}")

    registry = _parse_registry(registry_path)

    for drv in registry.drivers:
        if drv.status != "active":
            continue
        schema_path = _find_schema(drivers_dir, drv.name)
        if schema_path is None:
            raise FileNotFoundError(
                f"Schema file not found for active driver '{drv.name}'"
            )
        _populate_driver_from_schema(drv, schema_path)

    return registry


# ═══════════════════════════════════════════════════════════════════════
#  registry.yaml
# ═══════════════════════════════════════════════════════════════════════

def _parse_registry(path: str) -> RegistryAST:
    with open(path, "r", encoding="utf-8") as f:
        raw = yaml.safe_load(f)

    registry = RegistryAST(file_path=path)
    for entry in raw.get("drivers", []):
        drv = DriverAST(
            name=entry["name"],
            schema_version=0,  # filled later from schema yaml
            driver_id=_parse_hex(entry["id"]),
            category=entry.get("category", "other"),
            status=entry.get("status", "active"),
            version=str(entry.get("version", "0.0")),
            aliases=entry.get("aliases", []) or [],
            replaced_by=entry.get("replaced_by"),
            description=entry.get("description", ""),
        )
        registry.drivers.append(drv)
    return registry


# ═══════════════════════════════════════════════════════════════════════
#  .schema.yaml
# ═══════════════════════════════════════════════════════════════════════

def _find_schema(drivers_dir: str, driver_name: str) -> Optional[str]:
    """Walk drivers_dir to find <driver_name>.schema.yaml."""
    for root, _dirs, files in sorted(os.walk(drivers_dir)):
        target = f"{driver_name}.schema.yaml"
        if target in files:
            return os.path.join(root, target)
    return None


def _populate_driver_from_schema(drv: DriverAST, path: str) -> None:
    with open(path, "r", encoding="utf-8") as f:
        raw = yaml.safe_load(f)

    drv.file_path = path
    drv.schema_version = int(raw.get("schema_version", 0))
    drv.description = raw.get("description", drv.description)
    drv.caps = raw.get("caps", []) or []

    # properties
    for prop_raw in raw.get("properties", []):
        prop = PropertyAST(
            name=prop_raw["name"],
            index=_parse_hex(prop_raw["index"]),
            ptype=prop_raw.get("type", "u32"),
            flags=prop_raw.get("flags", []) or [],
            default=prop_raw.get("default"),
            min_val=prop_raw.get("min"),
            max_val=prop_raw.get("max"),
            max_ref=prop_raw.get("max_ref"),
            min_ref=prop_raw.get("min_ref"),
            enum_values=prop_raw.get("enum_values"),
            description=prop_raw.get("description", ""),
        )
        drv.properties.append(prop)

    # transform
    t_raw = raw.get("transform")
    if t_raw:
        drv.transform = _parse_transform(t_raw)


def _parse_transform(raw: dict, depth: int = 0) -> TransformAST:
    if depth > MAX_MULTI_STAGE_DEPTH:
        raise ValueError(
            f"MULTI_STAGE depth {depth} exceeds max {MAX_MULTI_STAGE_DEPTH}"
        )

    ttype = raw.get("type", "PASS_THROUGH")
    params = raw.get("params", {}) or {}
    stages: Optional[List[TransformAST]] = None

    if ttype == "MULTI_STAGE":
        raw_stages = raw.get("stages", []) or []
        stages = [_parse_transform(s, depth + 1) for s in raw_stages]

    return TransformAST(ttype=ttype, params=params, stages=stages)


# ═══════════════════════════════════════════════════════════════════════
#  helpers
# ═══════════════════════════════════════════════════════════════════════

def _parse_hex(val) -> int:
    """Parse a hex integer from either int or string like '0x03'."""
    if isinstance(val, int):
        return val
    return int(str(val), 16)
