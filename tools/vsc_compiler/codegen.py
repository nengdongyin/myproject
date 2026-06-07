"""Code generator — produces the 6 output files from validated AST."""

import os
from datetime import datetime, timezone
from typing import List

from vsc_compiler.ast_nodes import RegistryAST, DriverAST, PropertyAST, TransformAST
from vsc_compiler.depgraph import DepEdge

# ── capability string → C macro mapping ──
CAP_MAP = {
    "SENSOR":        "VSC_CAP_SENSOR",
    "EXPOSURE_CTRL": "VSC_CAP_EXPOSURE_CTRL",
    "STATISTICS":    "VSC_CAP_STATISTICS",
    "HDR":           "VSC_CAP_HDR",
    "TRIGGER":       "VSC_CAP_TRIGGER",
    "CROP":          "VSC_CAP_CROP",
    "BINNING":       "VSC_CAP_BINNING",
    "FORMAT_CONV":   "VSC_CAP_FORMAT_CONV",
}


# ═══════════════════════════════════════════════════════════════════════
#  Public entry
# ═══════════════════════════════════════════════════════════════════════

def generate(registry: RegistryAST,
             dep_edges: List[DepEdge],
             schema_checksum: int,
             compiler_version: str,
             output_dir: str) -> None:
    """Generate all 6 output files into output_dir."""

    os.makedirs(output_dir, exist_ok=True)
    active = [d for d in registry.drivers if d.status == "active"]

    _write_ids_h(active, output_dir, schema_checksum, compiler_version)
    _write_strings_c(active, output_dir, schema_checksum, compiler_version)
    _write_schema_c(active, output_dir, schema_checksum, compiler_version)
    _write_registry_c(active, output_dir, schema_checksum, compiler_version)
    _write_depmap_c(active, dep_edges, output_dir, schema_checksum, compiler_version)
    _write_checksum_h(schema_checksum, output_dir, compiler_version)


# ═══════════════════════════════════════════════════════════════════════
#  File 1: vsc_prop_ids.h
# ═══════════════════════════════════════════════════════════════════════

def _write_ids_h(active: List[DriverAST], out_dir: str,
                 checksum: int, compiler_ver: str) -> None:
    lines = _header("vsc_prop_ids.h", checksum, compiler_ver)
    lines.append("#ifndef VSC_PROP_IDS_H")
    lines.append("#define VSC_PROP_IDS_H")
    lines.append("")
    lines.append("/* ── Driver ID constants ── */")
    for drv in active:
        macro = _driver_id_macro(drv.name)
        lines.append(f"#define {macro}  0x{drv.driver_id:02X}")
    lines.append("")

    for drv in active:
        lines.append(f"/* ── {drv.name} ── */")
        lines.append(f"#define _{drv.name.upper()}_PROP_COUNT  {len(drv.properties)}")
        for prop in drv.properties:
            macro = _prop_id_macro(drv.name, prop.name)
            val = (drv.driver_id << 8) | prop.index
            lines.append(f"#define {macro}  ((0x{drv.driver_id:02X} << 8) | 0x{prop.index:02X})")
        lines.append("")

    lines.append("#endif /* VSC_PROP_IDS_H */")
    lines.append("")
    _write(os.path.join(out_dir, "vsc_prop_ids.h"), lines)


# ═══════════════════════════════════════════════════════════════════════
#  File 2: vsc_prop_strings.c
# ═══════════════════════════════════════════════════════════════════════

def _write_strings_c(active: List[DriverAST], out_dir: str,
                     checksum: int, compiler_ver: str) -> None:
    lines = _header("vsc_prop_strings.c", checksum, compiler_ver)
    lines.append('#include "vsc_prop_ids.h"')
    lines.append("")

    # Build name → ID lookup table
    entries: list[tuple[str, int]] = []
    for drv in active:
        for prop in drv.properties:
            name = f"{drv.name}.{prop.name}"
            val = (drv.driver_id << 8) | prop.index
            entries.append((name, val))

    lines.append("/* name → Property ID lookup (sorted by name) */")
    entries.sort(key=lambda e: e[0])
    lines.append("static const struct {")
    lines.append("    const char *name;")
    lines.append("    uint16_t    id;")
    lines.append(f"}} _vsc_prop_strings[{len(entries)}] = {{")
    for name, val in entries:
        macro = _prop_id_macro_from_val(name, val)
        lines.append(f'    {{ "{name}", {macro} }},')
    lines.append("};")
    lines.append(f"const uint16_t _vsc_prop_string_count = {len(entries)};")
    lines.append("")
    _write(os.path.join(out_dir, "vsc_prop_strings.c"), lines)


# ═══════════════════════════════════════════════════════════════════════
#  File 3: vsc_prop_schema.c
# ═══════════════════════════════════════════════════════════════════════

def _write_schema_c(active: List[DriverAST], out_dir: str,
                    checksum: int, compiler_ver: str) -> None:
    lines = _header("vsc_prop_schema.c", checksum, compiler_ver)
    lines.append('#include "vsc_types.h"')
    lines.append('#include "vsc_prop_ids.h"')
    lines.append("")

    for drv in active:
        lines.append(f"/* ═══════════ {drv.name} ═══════════ */")
        lines.append("")

        # transform_desc initializer
        if drv.transform:
            lines.append(f"const vsc_fmt_transform_desc_t "
                         f"_{drv.name}_transform = {{")
            _emit_transform_struct(lines, drv.transform, indent=4, drv=drv)
            lines.append("};")
            lines.append("")

        # property meta array
        lines.append(f"const vsc_prop_meta_t _{drv.name}_schema[] = {{")
        for prop in drv.properties:
            _emit_prop_meta(lines, drv, prop, indent=4)
        lines.append("};")
        lines.append("")

    _write(os.path.join(out_dir, "vsc_prop_schema.c"), lines)


def _emit_transform_struct(lines: List[str], t: TransformAST, indent: int,
                          drv: DriverAST | None = None) -> None:
    pref = " " * indent
    lines.append(f"{pref}.type = VSC_TRANSFORM_{t.ttype},")

    if t.ttype == "BINNING":
        lines.append(f"{pref}.params.binning = {{")
        lines.append(f"{pref}    .factor_x = {t.params.get('factor_x', 1)},")
        lines.append(f"{pref}    .factor_y = {t.params.get('factor_y', 1)},")
        lines.append(f"{pref}}},")
    elif t.ttype == "CROP":
        max_w = _resolve_ref_param(drv, t.params, "max_w", "max_w_ref", 8192)
        max_h = _resolve_ref_param(drv, t.params, "max_h", "max_h_ref", 8192)
        lines.append(f"{pref}.params.crop = {{")
        lines.append(f"{pref}    .min_w  = {t.params.get('min_w', 0)},")
        lines.append(f"{pref}    .min_h  = {t.params.get('min_h', 0)},")
        lines.append(f"{pref}    .max_w  = {max_w},")
        lines.append(f"{pref}    .max_h  = {max_h},")
        lines.append(f"{pref}    .align_w = {t.params.get('align_w', 1)},")
        lines.append(f"{pref}    .align_h = {t.params.get('align_h', 1)},")
        lines.append(f"{pref}}},")
    elif t.ttype == "PIXEL_FMT_CONV":
        lines.append(f"{pref}.params.pixel_fmt_conv = {{")
        lines.append(f"{pref}    .fmt_in  = VSC_FMT_{t.params.get('fmt_in', 'INVALID')},")
        lines.append(f"{pref}    .fmt_out = VSC_FMT_{t.params.get('fmt_out', 'INVALID')},")
        lines.append(f"{pref}}},")
    elif t.ttype == "PASS_THROUGH":
        pass  # no params
    elif t.ttype == "MULTI_STAGE" and t.stages:
        # emit inline sub-transform array
        sub_name = f"_{drv.name}_sub_transforms" if drv else "_sub_transforms"
        lines.append(f"{pref}.params.multi_stage = {{")
        lines.append(f"{pref}    .count = {len(t.stages)},")
        lines.append(f"{pref}    .subs  = {sub_name},")
        lines.append(f"{pref}}},")
        # generate sub-transform array before this block
        # (for now, emit as separate static array)
    elif t.ttype == "PASS_THROUGH":
        pass  # no params


def _emit_prop_meta(lines: List[str], drv: DriverAST,
                    prop: PropertyAST, indent: int) -> None:
    pref = " " * indent
    id_macro = _prop_id_macro(drv.name, prop.name)
    type_enum = f"VSC_TYPE_{prop.ptype.upper()}"
    flags = "0"
    for f in prop.flags:
        flags += f" | VSC_PROP_{f.upper()}"

    max_ref = "0"
    if prop.max_ref:
        max_ref = _prop_id_macro(drv.name, prop.max_ref)

    def_val = _fmt_c_val(prop.default, prop.ptype)
    min_val = _fmt_c_val(prop.min_val, prop.ptype) if prop.min_val is not None else "0"
    max_val = _fmt_c_val(prop.max_val, prop.ptype) if prop.max_val is not None else "0"

    lines.append(f"{pref}{{")
    lines.append(f"{pref}    .prop_id     = {id_macro},")
    lines.append(f'{pref}    .name        = "{drv.name}.{prop.name}",')
    lines.append(f"{pref}    .type        = {type_enum},")
    lines.append(f"{pref}    .flags       = {flags},")
    lines.append(f"{pref}    .default_val = {{ .{_c_union_field(prop.ptype)} = {def_val} }},")
    lines.append(f"{pref}    .min_val     = {{ .{_c_union_field(prop.ptype)} = {min_val} }},")
    lines.append(f"{pref}    .max_val     = {{ .{_c_union_field(prop.ptype)} = {max_val} }},")
    lines.append(f"{pref}    .max_ref_id  = {max_ref},")
    lines.append(f"{pref}}},")


# ═══════════════════════════════════════════════════════════════════════
#  File 4: vsc_driver_registry.c
# ═══════════════════════════════════════════════════════════════════════

def _write_registry_c(active: List[DriverAST], out_dir: str,
                      checksum: int, compiler_ver: str) -> None:
    lines = _header("vsc_driver_registry.c", checksum, compiler_ver)
    lines.append('#include "vsc_types.h"')
    lines.append('#include "vsc_prop_ids.h"')
    lines.append("")
    lines.append("/* external schema + transform declarations */")
    for drv in active:
        lines.append(f"extern const vsc_prop_meta_t _{drv.name}_schema[];")
        if drv.transform:
            lines.append(f"extern const vsc_fmt_transform_desc_t "
                         f"_{drv.name}_transform;")
    lines.append("")

    lines.append("/* ── driver registry ── */")
    lines.append(f"const vsc_driver_t _vsc_drivers[] = {{")
    for drv in active:
        tptr = f"&_{drv.name}_transform" if drv.transform else "NULL"
        caps = "0"
        for c in drv.caps:
            if c in CAP_MAP:
                caps += f" | {CAP_MAP[c]}"
        lines.append(f"    {{")
        lines.append(f'        .name               = "{drv.name}",')
        lines.append(f"        .driver_id          = "
                     f"{_driver_id_macro(drv.name)},")
        lines.append(f"        .capabilities       = {caps},")
        lines.append(f"        .schema             = _{drv.name}_schema,")
        lines.append(f"        .prop_count         = _{drv.name.upper()}_PROP_COUNT,")
        lines.append(f"        .transform_template = {tptr},")
        lines.append(f"        .ops                = {{ NULL, NULL, NULL, NULL }},")
        lines.append(f"    }},")
    lines.append(f"    {{ NULL, 0, 0, NULL, 0, NULL, {{ NULL, NULL, NULL, NULL }} }},")
    lines.append("};")
    lines.append("")

    # vsc_driver_find() and vsc_driver_by_index() are now in vsc_feature.c
    # to support both generated _vsc_drivers[] and manually registered drivers.
    lines.append("")

    _write(os.path.join(out_dir, "vsc_driver_registry.c"), lines)


# ═══════════════════════════════════════════════════════════════════════
#  File 5: vsc_dependency_map.c
# ═══════════════════════════════════════════════════════════════════════

def _write_depmap_c(active: List[DriverAST], dep_edges: List[DepEdge],
                    out_dir: str, checksum: int, compiler_ver: str) -> None:
    lines = _header("vsc_dependency_map.c", checksum, compiler_ver)
    lines.append('#include "vsc_types.h"')
    lines.append('#include "vsc_prop_ids.h"')
    lines.append("")

    lines.append("/* topologically-sorted dependency map */")
    lines.append("static const vsc_prop_dep_t _global_dependencies[] = {")
    for sd, sp, td, tp in dep_edges:
        sm = _prop_id_macro(sd, sp)
        tm = _prop_id_macro(td, tp)
        lines.append(f"    {{ {sm}, {tm} }},")
    lines.append("};")
    lines.append(f"const uint8_t _global_dep_count = {len(dep_edges)};")
    lines.append("")
    _write(os.path.join(out_dir, "vsc_dependency_map.c"), lines)


# ═══════════════════════════════════════════════════════════════════════
#  File 6: vsc_schema_checksum.h
# ═══════════════════════════════════════════════════════════════════════

# ═══════════════════════════════════════════════════════════════════════
#  File 7: vsc_system_init.c (generated from system_graph.json + board.json)
# ═══════════════════════════════════════════════════════════════════════

def generate_system_init(graph_json: str, board_json: str,
                         out_dir: str, checksum: int,
                         compiler_ver: str) -> None:
    """Generate vsc_system_init.c with pre-initialized descriptor data."""
    import json as _json

    graph = _json.loads(graph_json) if graph_json else {"nodes": [], "links": []}
    board = _json.loads(board_json) if board_json else {}

    lines = _header("vsc_system_init.c", checksum, compiler_ver)
    lines.append('#include "vsc_loader.h"')
    lines.append("")

    # ── static system_desc ──
    lines.append("/* ═══════════ system_graph.json ═══════════ */")
    lines.append("static const vsc_system_desc_t _vsc_system_desc = {")

    nodes = graph.get("nodes", [])
    lines.append(f"    .num_nodes = {len(nodes)},")
    lines.append("    .nodes = {")
    for nd in nodes:
        ov = nd.get("prop_overrides", {}) or {}
        ov_count = len(ov)
        lines.append("        {")
        lines.append(f'            .type       = "{nd["type"]}",')
        lines.append(f'            .id         = "{nd["id"]}",')
        lines.append(f'            .base_addr  = 0x{_parse_json_hex(nd.get("base", 0)):08X},')
        lines.append(f'            .optional   = {str(nd.get("optional", False)).lower()},')
        lines.append(f"            .num_overrides = {ov_count},")
        if ov_count > 0:
            lines.append("            .overrides = {")
            for k, v in ov.items():
                lines.append(f'                {{ "{k}", {_parse_json_hex(v)} }},')
            lines.append("            },")
        else:
            lines.append("            /* overrides */")
        lines.append("        },")
    lines.append("    },")

    links = graph.get("links", [])
    lines.append(f"    .num_links = {len(links)},")
    if links:
        lines.append("    .links = {")
        for lk in links:
            lt = "VSC_LINK_TAP" if lk.get("type") == "TAP" else "VSC_LINK_STREAM"
            lines.append(f'        {{ "{lk["src"]}", "{lk["dst"]}", {lt} }},')
        lines.append("    },")
    else:
        lines.append("    /* links */")
    lines.append("};")
    lines.append("")

    # ── static board_config ──
    lines.append("/* ═══════════ board.json ═══════════ */")
    sensor = board.get("sensor", "")
    i2c_bus = board.get("i2c_bus", 0)
    i2c_addr = _parse_json_hex(board.get("i2c_addr", 0))
    lines.append("static const vsc_board_config_t _vsc_board_config = {")
    lines.append(f'    .sensor_type = "{sensor}",')
    lines.append(f"    .i2c_bus     = {i2c_bus},")
    lines.append(f"    .i2c_addr    = {i2c_addr},")
    lines.append("};")
    lines.append("")

    # ── init function ──
    lines.append("int vsc_system_init_default(vsc_pipeline_t *pipeline)")
    lines.append("{")
    lines.append("    return vsc_system_init(&_vsc_system_desc,")
    lines.append("                           &_vsc_board_config, pipeline);")
    lines.append("}")

    _write(os.path.join(out_dir, "vsc_system_init.c"), lines)


def _parse_json_hex(v) -> int:
    """Parse a JSON value that may be a hex string '0x...' or integer."""
    if isinstance(v, str):
        return int(v, 0)
    return int(v)


def _write_checksum_h(checksum: int, out_dir: str,
                      compiler_ver: str) -> None:
    lines = _header("vsc_schema_checksum.h", checksum, compiler_ver)
    lines.append("#ifndef VSC_SCHEMA_CHECKSUM_H")
    lines.append("#define VSC_SCHEMA_CHECKSUM_H")
    lines.append("")
    lines.append(f"#define VSC_SCHEMA_CHECKSUM  0x{checksum:08X}")
    lines.append("")
    lines.append("#endif /* VSC_SCHEMA_CHECKSUM_H */")
    lines.append("")
    _write(os.path.join(out_dir, "vsc_schema_checksum.h"), lines)


# ═══════════════════════════════════════════════════════════════════════
#  Helpers
# ═══════════════════════════════════════════════════════════════════════

_ID_MACRO_CACHE: dict[str, str] = {}

def _driver_id_macro(name: str) -> str:
    return f"VSC_DRIVER_ID_{name.upper()}"

def _prop_id_macro(drv_name: str, prop_name: str) -> str:
    key = f"{drv_name}.{prop_name}"
    if key in _ID_MACRO_CACHE:
        return _ID_MACRO_CACHE[key]
    # transform dots and special chars to underscores
    safe = prop_name.replace(".", "_").replace("-", "_").upper()
    macro = f"VSC_PROP_{drv_name.upper()}_{safe}"
    _ID_MACRO_CACHE[key] = macro
    return macro

def _prop_id_macro_from_val(name: str, val: int) -> str:
    """Build a macro name from the full property name string."""
    # Parse "driver.prop" from the name
    parts = name.split(".", 1)
    if len(parts) == 2:
        return _prop_id_macro(parts[0], parts[1])
    return f"0x{val:04X}"

def _fmt_c_val(val, ptype: str) -> str:
    if val is None:
        return "0"
    if ptype == "bool":
        return "1" if val else "0"
    if ptype in ("u32", "i32"):
        return str(int(val))
    if ptype == "f32":
        v = float(val)
        if v == int(v):
            return f"{v:.1f}f"
        return f"{v:.6g}f"
    if ptype == "enum":
        return str(int(val))
    if ptype == "string":
        return f'"{val}"'
    return "0"

def _c_union_field(ptype: str) -> str:
    return {"u32": "u32", "i32": "i32", "f32": "f32",
            "bool": "b", "enum": "u32", "string": "str"}.get(ptype, "u32")

def _resolve_ref_param(drv: DriverAST | None, params: dict,
                       direct_key: str, ref_key: str,
                       fallback: int) -> str:
    """Resolve a param that can be either a direct value or a ref to a property.

    If `direct_key` is present in params, return its value (int).
    If `ref_key` is present, look up that property's default value in the driver.
    Otherwise return `fallback`.
    """
    if direct_key in params:
        return str(int(params[direct_key]))
    if ref_key in params and drv is not None:
        ref_name = params[ref_key]
        for prop in drv.properties:
            if prop.name == ref_name:
                return str(int(prop.default)) if prop.default is not None else str(fallback)
    return str(fallback)


def _header(filename: str, checksum: int, compiler_ver: str) -> List[str]:
    ts = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    return [
        "/* ═══════════════════════════════════════════════════════════",
        f" *  GENERATED FILE — DO NOT EDIT",
        f" *",
        f" *  Compiler:  vsc_prop_gen.py  v{compiler_ver}",
        f" *  File:      {filename}",
        f" *  Checksum:  0x{checksum:08X}",
        f" *  Generated: {ts}",
        " * ═══════════════════════════════════════════════════════════ */",
        "",
    ]

def _write(path: str, lines: List[str]) -> None:
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
