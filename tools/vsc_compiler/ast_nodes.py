"""AST node definitions for the VSC Schema Compiler."""

from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional


# ── supported types ──
VALID_TYPES = frozenset({"u32", "i32", "f32", "bool", "enum", "string"})

# ── supported flags ──
VALID_FLAGS = frozenset({
    "readonly", "runtime", "persist", "transaction", "atomic", "lazy", "debug"
})

# ── transform types ──
VALID_TRANSFORM_TYPES = frozenset({
    "PASS_THROUGH", "BINNING", "CROP", "PIXEL_FMT_CONV", "MULTI_STAGE"
})

# ── required params per transform type ──
TRANSFORM_REQUIRED_PARAMS: Dict[str, List[str]] = {
    "PASS_THROUGH":   [],
    "BINNING":        ["factor_x", "factor_y"],
    "CROP":           ["min_w", "min_h", "align_w", "align_h"],
    "PIXEL_FMT_CONV": ["fmt_in", "fmt_out"],
    "MULTI_STAGE":    ["stages"],
}

MAX_MULTI_STAGE_DEPTH = 4


# ═══════════════════════════════════════════════════════════════════════
#  AST nodes
# ═══════════════════════════════════════════════════════════════════════

@dataclass
class PropertyAST:
    name: str
    index: int          # 0x00 – 0xFE
    ptype: str          # u32 | i32 | f32 | bool | enum | string
    flags: List[str] = field(default_factory=list)
    default: Any = None
    min_val: Optional[Any] = None
    max_val: Optional[Any] = None
    max_ref: Optional[str] = None   # name of referenced property (same driver)
    min_ref: Optional[str] = None
    enum_values: Optional[List[str]] = None
    description: str = ""


@dataclass
class TransformAST:
    ttype: str                            # PASS_THROUGH | BINNING | …
    params: Dict[str, Any] = field(default_factory=dict)
    stages: Optional[List['TransformAST']] = None  # MULTI_STAGE only


@dataclass
class DriverAST:
    name: str
    schema_version: int
    driver_id: int       # from registry (0x01 – 0xFE)
    category: str
    status: str          # active | deprecated
    version: str         # semantic version string
    aliases: List[str] = field(default_factory=list)
    replaced_by: Optional[str] = None
    description: str = ""
    caps: List[str] = field(default_factory=list)   # capability strings
    properties: List[PropertyAST] = field(default_factory=list)
    transform: Optional[TransformAST] = None
    compatibility: Optional[Dict[str, Any]] = None
    file_path: str = ""  # source YAML path (set by parser)


@dataclass
class RegistryAST:
    drivers: List[DriverAST] = field(default_factory=list)
    file_path: str = ""
