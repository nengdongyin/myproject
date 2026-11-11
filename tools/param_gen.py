#!/usr/bin/env python3
"""param_gen.py — 参数模块 YAML → C 生成器

用法:
    python tools/param_gen.py params gen/params            # 生成
    python tools/param_gen.py params gen/params --check    # 只校验是否与源一致

输入: <src_dir>/*.params.yaml  (一个模块一个文件)
输出: <out_dir>/<module>_params.h   PID 常量枚举 + 参数表声明 + 计数宏
      <out_dir>/<module>_params.c   缓冲区 + 条目定义 + 参数表

设计约束 (与 gen/vsc 生成链同构):
  - 生成物入库, CI 用 --check 卡 staleness;
  - 生成物不得手改 (文件头有 GENERATED 标记);
  - 输出不含时间戳, 同输入必得同字节 (保证 --check 可靠)。

为什么生成而不是用宏:
  生成物是**普通 C 代码** —— 定义看得见、grep 得到、断点能停、报错指向具体行。
  宏方案能省掉生成步骤, 但把"定义在哪"变成了"展开后才知道"。

退出码: 0 = 成功/一致; 1 = 校验失败或生成物过期。
"""

import argparse
import glob
import json
import os
import re
import sys

try:
    import yaml
except ImportError:  # pragma: no cover
    sys.exit("FATAL E0001: 需要 PyYAML —— python -m pip install pyyaml")

# ── 常量表 ──────────────────────────────────────────────────────────────

NUMERIC_TYPES = ("UINT", "INT", "FLOAT")
ALL_TYPES = NUMERIC_TYPES + ("BOOL", "ENUM", "STRING", "BLOB", "EXEC")

# 参数类型标签与公开宏名一一对应: UINT → PARAM_UINT / PARAM_IP_UINT

FLAGS = {
    "PERSIST": "PARAM_FLAG_PERSIST",
    "READONLY": "PARAM_FLAG_READONLY",
    "HIDDEN": "PARAM_FLAG_HIDDEN",
    "DEPRECATED": "PARAM_FLAG_DEPRECATED",
    "EXEC": "PARAM_FLAG_EXEC",
    "FIXED": "PARAM_FLAG_FIXED",
}

VTBL = {"app": "app_vtable", "ip": "ip_vtable"}

VALUE_TYPES = tuple(t for t in ALL_TYPES if t != "EXEC")

# ── 诊断 ────────────────────────────────────────────────────────────────


class Diag:
    def __init__(self, path):
        self.path = path
        self.items = []

    def err(self, code, msg):
        self.items.append((code, "E", msg))

    def warn(self, code, msg):
        self.items.append((code, "W", msg))

    @property
    def fatal(self):
        return sum(1 for _, lvl, _ in self.items if lvl == "E")

    def dump(self):
        for code, lvl, msg in self.items:
            print(f"{'FATAL' if lvl == 'E' else 'WARN '} {code}: {self.path}: {msg}")


# ── 小工具 ──────────────────────────────────────────────────────────────


def is_ident(s):
    return isinstance(s, str) and s.isidentifier() and not s.startswith("__")


def num_literal(ptype, v):
    """把 YAML 数值渲染成 C 字面量 (FLOAT 补 'f', 保证是浮点常量)。"""
    if ptype == "FLOAT":
        s = repr(float(v))
        if "e" in s or "E" in s:
            s = f"{float(v):.6f}".rstrip("0")
            if s.endswith("."):
                s += "0"
        return s + "f"
    return str(int(v))


def bool_literal(v):
    return "true" if v else "false"


def field_or_expr(p, key, ptype):
    """取 <key> 的 C 字面量；若给了 <key>_expr 则原样透传。"""
    expr = p.get(key + "_expr")
    if expr is not None:
        return str(expr)
    return num_literal(ptype, p[key])


# ── 校验 ────────────────────────────────────────────────────────────────


def validate(schema, d):
    mod = schema.get("module") or {}
    if not is_ident(mod.get("name")):
        d.err("E1001", "module.name 缺失或不是合法标识符")
    if not is_ident(mod.get("enum")) and mod.get("enum") is not None:
        d.err("E1001", "module.enum 不是合法标识符")
    if not isinstance(mod.get("id"), int) or not (0 <= mod["id"] <= 0xFFFF):
        d.err("E1001", "module.id 缺失或超出 0..0xFFFF")
    if mod.get("vtbl") not in VTBL:
        d.err("E1002", f"module.vtbl 必须是 {list(VTBL)} 之一")
    if not isinstance(mod.get("label"), str) or not mod["label"]:
        d.err("E1001", "module.label 缺失 (dump 用的调试名)")

    ps = schema.get("params")
    if not isinstance(ps, list) or not ps:
        d.err("E1003", "params 必须是非空列表")
        return

    seen_names, seen_ids = {}, {}
    for i, p in enumerate(ps):
        where = f"params[{i}]"
        if not isinstance(p, dict):
            d.err("E1003", f"{where} 必须是映射")
            continue
        name = p.get("name")
        if not is_ident(name):
            d.err("E1004", f"{where}.name 缺失或不是合法标识符")
        elif name in seen_names:
            d.err("E1004", f"参数名重复: {name}")
        else:
            seen_names[name] = i

        pid = p.get("id")
        if not isinstance(pid, int) or not (0 <= pid <= 0xFFFF):
            d.err("E1005", f"{where}.id 缺失或超出 0..0xFFFF")
        elif pid in seen_ids:
            d.err("E1005", f"local_id 重复: 0x{pid:02X} (与 {seen_ids[pid]} 冲突)")
        else:
            seen_ids[pid] = name

        t = p.get("type")
        if t not in ALL_TYPES:
            d.err("E1006", f"{where}.type 必须是 {list(ALL_TYPES)} 之一")
            continue

        flags = p.get("flags") or []
        if not isinstance(flags, list):
            d.err("E1007", f"{where}.flags 必须是列表")
            flags = []
        for f in flags:
            if f not in FLAGS:
                d.err("E1007", f"{where}.flags 含未知旗标 {f!r} (可用: {list(FLAGS)})")

        # EXEC: 不允许任何值字段
        if t == "EXEC":
            for bad in ("default", "default_expr", "min", "min_expr", "max",
                        "max_expr", "values", "buf"):
                if bad in p:
                    d.err("E1008", f"{where} 是 EXEC, 不允许字段 {bad!r}")
            continue

        if "default" not in p and "default_expr" not in p:
            if t in NUMERIC_TYPES or t == "ENUM":
                d.err("E1009", f"{where} 缺少 default")
            elif t == "BOOL":
                p["default"] = False  # BOOL 默认 false 可接受
            # STRING / BLOB 的默认值就是 buf 的初始内容, 不需要 default 字段

        # 数值范围
        if t in NUMERIC_TYPES:
            has_min = "min" in p or "min_expr" in p
            has_max = "max" in p or "max_expr" in p
            if has_min != has_max:
                d.err("E1010", f"{where} min/max 必须成对出现 (缺一个时无法判断范围语义)")
            elif has_min:
                if "min_expr" not in p and "max_expr" not in p:
                    if not (p["min"] < p["max"]):
                        d.warn("W2002",
                               f"{where} min({p['min']}) >= max({p['max']}), "
                               f"该参数将不启用范围校验 (has_range=0)")
            if flags and "FIXED" in flags and has_min:
                if "min_expr" not in p and "max_expr" not in p and p["min"] < p["max"]:
                    d.warn("W2003", f"{where} 同时用 FIXED 与 min<max, 语义矛盾")

        if t == "ENUM":
            vals = p.get("values")
            if not isinstance(vals, list) or not vals:
                d.err("E1011", f"{where} (ENUM) 必须有非空 values 列表")
            else:
                if len(set(vals)) != len(vals):
                    d.err("E1011", f"{where} (ENUM) values 有重复值")
                df = p.get("default")
                if isinstance(df, int) and df not in vals:
                    d.err("E1011", f"{where} (ENUM) default={df} 不在 values 中")

        if t in ("STRING", "BLOB"):
            buf = p.get("buf")
            if not isinstance(buf, dict):
                d.err("E1012", f"{where} ({t}) 必须有 buf: {{name, size}}")
            else:
                if not is_ident(buf.get("name")):
                    d.err("E1012", f"{where}.buf.name 缺失或不是合法标识符")
                sz = buf.get("size")
                if not isinstance(sz, int) or sz <= 0:
                    d.err("E1012", f"{where}.buf.size 必须是正整数")
                elif t == "STRING" and sz < 2:
                    d.err("E1012", f"{where}.buf.size 至少 2 (需容纳 '\\0')")


# ── 生成 ────────────────────────────────────────────────────────────────

BANNER = """\
/* ─────────────────────────────────────────────────────────────────────
 *  GENERATED FILE — DO NOT EDIT / 生成文件，请勿手改
 *
 *  源:       {src}
 *  生成器:   tools/param_gen.py
 *  重新生成: python tools/param_gen.py params gen/params
 *  一致性校验: python tools/param_gen.py params gen/params --check
 * ───────────────────────────────────────────────────────────────────── */
"""


def emit_header(mod, ps, src):
    enum_name = mod.get("enum")
    mod_id = mod["id"]
    table = f"{mod['name']}_params"
    count = f"{mod['name'].upper()}_PARAM_COUNT"
    guard = f"{mod['name'].upper()}_PARAMS_H"

    out = [BANNER.format(src=src), f"#ifndef {guard}\n#define {guard}\n"]
    out.append('#include "param_manager.h"\n#include "module_ids.h"\n')

    if enum_name:
        out.append(
            f"/* 模块 ID —— module_ids.h 已登记则用其枚举, 未登记时给模板占位 */\n"
            f"#ifndef {enum_name}\n"
            f"#define {enum_name} 0x{mod_id:02X}u\n"
            f"#endif\n"
        )
        id_ref = enum_name
    else:
        id_ref = f"0x{mod_id:02X}u"

    names = [f"PID_{mod['name'].upper()}_{p['name'].upper()}" for p in ps]
    width = max(len(n) for n in names)
    out.append(f"/* 参数 ID 宏 —— 模块 {mod['name']} 的每条参数一个 */\n")
    for n, p in zip(names, ps):
        out.append(f"#define {n:<{width}}  MAKE_PARAM_ID({id_ref}, 0x{p['id']:02X})\n")
    out.append("\n")

    out.append(
        f"/* 参数表 (定义在 {mod['name']}_params.c, 供 PARAM_MODULE_INIT 使用) */\n"
        f"#define {count} {len(ps)}\n"
        f"extern param_entry_t *{table}[{count}];\n\n"
    )
    out.append(f"#endif /* {guard} */\n")
    return "".join(out)


def emit_entry(mod, p):
    """生成一条条目 —— 直接调用公开宏, 不展开结构体初始化器。

    用宏而不是字面量初始化器的三个好处:
      1. 条目结构体布局只在框架一处定义 —— 布局变更无需重新生成;
      2. 生成物短、可读, 与手写代码完全同形 (文档/示例可直接对照);
      3. has_range / dirty / 调试名 / 单缓冲等隐式语义仍由框架宏负责。

    IP 模块自动换用 PARAM_IP_* 宏 (由 module.vtbl 决定)。
    """
    t = p["type"]
    name = p["name"]
    pid = f"PID_{mod['name'].upper()}_{name.upper()}"
    mac = ("PARAM_APP_" if mod["vtbl"] == "app" else "PARAM_IP_") + t

    if t == "EXEC":
        # PARAM_EXEC 自带 PARAM_FLAG_EXEC, 不接受 flags/值
        return f"{mac}({name}, {pid});\n"

    flags = list(p.get("flags") or [])
    flags_c = " | ".join(FLAGS[f] for f in flags) if flags else "0"

    if t in NUMERIC_TYPES:
        d = field_or_expr(p, "default", t)
        lo = field_or_expr(p, "min", t) if ("min" in p or "min_expr" in p) else "0"
        hi = field_or_expr(p, "max", t) if ("max" in p or "max_expr" in p) else "0"
        return f"{mac}({name}, {pid}, {flags_c}, {d}, {lo}, {hi});\n"

    if t == "BOOL":
        return f"{mac}({name}, {pid}, {flags_c}, {bool_literal(bool(p['default']))});\n"

    if t == "ENUM":
        return (f"{mac}({name}, {pid}, {flags_c}, {int(p['default'])}, "
                f"{enum_table_name(mod, name)}, {len(p['values'])});\n")

    if t == "STRING":
        b = p["buf"]
        # 用 sizeof(buf)-1 而不是写死数字: 缓冲区尺寸仍由 YAML 决定,
        # 但生成物里没有第二个可漂移的常量
        return f"{mac}({name}, {pid}, {flags_c}, {b['name']}, sizeof({b['name']}) - 1);\n"

    if t == "BLOB":
        b = p["buf"]
        return f"{mac}({name}, {pid}, {flags_c}, {b['name']}, sizeof({b['name']}));\n"

    raise AssertionError(f"未处理的类型 {t}")


def enum_table_name(mod, param_name):
    return f"s_{mod['name']}_{param_name}_values"


def emit_source(mod, ps, src):
    table = f"{mod['name']}_params"
    count = f"{mod['name'].upper()}_PARAM_COUNT"
    enum_name = mod.get("enum")

    out = [BANNER.format(src=src)]
    out.append('#include "app_param_manager.h"\n')
    if mod["vtbl"] == "ip":
        out.append('#include "ip_param_manager.h"\n')
    out.append('#include "module_ids.h"\n')
    out.append(f'#include "{mod["name"]}_params.h"\n\n')

    if enum_name:
        out.append(
            f"/* module.id 与 module_ids.h 的枚举值必须一致 —— 漂移即编译失败 */\n"
            f"_Static_assert({enum_name} == 0x{mod['id']:02X},\n"
            f'               "{src}: module.id 与 {enum_name} 已漂移, 请同步");\n\n'
        )

    enums = [p for p in ps if p["type"] == "ENUM"]
    if enums:
        out.append("/* ── ENUM 合法值表 ── */\n")
        for p in enums:
            vals = ", ".join(str(int(v)) for v in p["values"])
            out.append(f"static const int32_t {enum_table_name(mod, p['name'])}[] = {{{vals}}};\n")
        out.append("\n")

    bufs = [p for p in ps if p["type"] in ("STRING", "BLOB")]
    if bufs:
        out.append("/* ── STRING / BLOB 缓冲区 (单缓冲: 静态可写, 生命周期 = 运行期) ── */\n")
        for p in bufs:
            b = p["buf"]
            if p["type"] == "STRING":
                init = json.dumps(str(b.get("init", "")))
                out.append(f"static char {b['name']}[{b['size']}] = {init};\n")
            else:
                out.append(f"static uint8_t {b['name']}[{b['size']}] = {{0}};\n")
        out.append("\n")

    out.append("/* ── 参数条目 (公开宏调用, 与手写代码同形) ── */\n")
    for p in ps:
        out.append(emit_entry(mod, p))
        out.append("\n")

    out.append("/* ── 参数表 ── */\n")
    out.append(f"param_entry_t *{table}[{count}] = {{\n")
    for p in ps:
        out.append(f"    &{p['name']}.base,\n")
    out.append("};\n")
    return "".join(out)


# ── 用户骨架 (scaffold, 只在文件不存在时生成一次) ───────────────────────

SCAFFOLD_BANNER = """\
/* ─────────────────────────────────────────────────────────────────────
 *  用户文件 — 由 param_gen.py --scaffold 首次生成, 之后**归你所有**
 *
 *  源:       {src}
 *  数据:     {base}.h / {base}.c  (生成物, 请勿手改)
 *  检查缺失项: python tools/param_gen.py params gen/params --scaffold <dir>
 *
 *  生成器不会覆盖本文件; 回调里的 switch 按 YAML 初次展开, 内容由你填。
 * ───────────────────────────────────────────────────────────────────── */
"""


def _switch_cases(mod, plist, default_ret):
    """生成 switch (id) { case PID_x: break; ... default: return X; } return PARAM_OK;"""
    L = ["    switch (id)\n", "    {\n"]
    for p in plist:
        L.append(f"    case PID_{mod['name'].upper()}_{p['name'].upper()}:\n")
        L.append("        break;\n")
    L.append(f"    default:\n        return {default_ret};\n")
    L.append("    }\n    return PARAM_OK;\n")
    return "".join(L)


def emit_scaffold(mod, ps, src, base):
    """用户侧骨架: 模块上下文 + 5 个回调 (switch 按清单展开) + 定义/注册。

    "一次性脚手架": 生成器只在文件不存在时创建, 之后归用户所有。
    它不在生成物目录里、不受 --check 约束, 所以可以自由填业务代码。
    """
    name = mod["name"]
    ctx_var = mod.get("ctx") or f"g_{name}"
    ctx_type = mod.get("ctx_type") or f"{name}_ctx_t"
    ip = mod["vtbl"] == "ip"
    define_mac = "PARAM_IP_MODULE_DEFINE" if ip else "PARAM_APP_MODULE_DEFINE"
    init_mac = "PARAM_MODULE_INIT"
    mod_id = mod.get("enum") or f"0x{mod['id']:02X}u"

    writable = [p for p in ps if p["type"] != "EXEC"]
    execs = [p for p in ps if p["type"] == "EXEC"]

    o = [SCAFFOLD_BANNER.format(src=src, base=base)]
    o.append('#include "app_param_manager.h"\n')
    if ip:
        o.append('#include "ip_param_manager.h"\n')
    o.append('#include "module_ids.h"\n')
    o.append(f'#include "{base}.h"\n\n')

    o.append("/* ── 模块私有上下文 (按需换成真实类型/字段) ── */\n")
    o.append("typedef struct\n{\n")
    o.append("    int reserved; /* TODO */\n")
    o.append(f"}} {ctx_type};\n\n")
    o.append(f"static {ctx_type} {ctx_var};\n\n")

    o.append("/** @brief 模块初始化 (param_load_all 的 INIT 阶段) */\n")
    o.append(f"static int {name}_init(void *ctx)\n{{\n")
    o.append(f"    {ctx_type} *e = ({ctx_type} *)ctx;\n")
    o.append("    (void)e;\n")
    o.append("    return PARAM_OK;\n}\n\n")

    o.append("/** @brief 读取: 返回 PARAM_OK 才覆盖缓存; 默认让框架回退缓存 */\n")
    o.append(f"static int {name}_read(void *ctx, uint32_t id, param_value_t *value)\n{{\n")
    o.append("    (void)ctx;\n    (void)id;\n    (void)value;\n")
    o.append("    return PARAM_ERR_INVALID_ID;\n}\n\n")

    o.append("/** @brief 写入校验/转换 (apply): 返回非 OK = 拒绝且缓存回滚 */\n")
    o.append(f"static int {name}_write(void *ctx, uint32_t id, param_value_t value)\n{{\n")
    o.append("    (void)ctx;\n    (void)value;\n\n")
    o.append(_switch_cases(mod, writable, "PARAM_OK"))
    o.append("}\n\n")

    o.append("/** @brief 命令执行 (EXEC) */\n")
    o.append(f"static int {name}_exec(void *ctx, uint32_t id, param_value_t arg)\n{{\n")
    o.append("    (void)ctx;\n    (void)arg;\n\n")
    o.append(_switch_cases(mod, execs, "PARAM_ERR_NOT_FOUND"))
    o.append("}\n\n")

    o.append("/** @brief 批量下发 (App): 失败保留 dirty, 下一轮 param_flush 重试 */\n")
    o.append(f"static int {name}_flush(void *ctx)\n{{\n")
    o.append("    (void)ctx;\n    return PARAM_OK;\n}\n\n")

    o.append("/* ── 模块定义 + 注册 ── */\n")
    o.append(f'{define_mac}({name}, {mod_id}, "{mod["label"]}",\n')
    o.append(f"                    .ctx = &{ctx_var},\n")
    o.append(f"                    .init = {name}_init,\n")
    o.append(f"                    .read = {name}_read,\n")
    o.append(f"                    .write = {name}_write,\n")
    o.append(f"                    .exec = {name}_exec,\n")
    o.append(f"                    .flush = {name}_flush);\n\n")
    o.append(f"{init_mac}({name}, {base});\n")
    return "".join(o)


def scaffold_missing(target, mod, ps):
    """已存在的骨架里, 哪些 PID 没出现 (提示可能需要补 case)。"""
    with open(target, encoding="utf-8") as f:
        txt = f.read()
    return [f"PID_{mod['name'].upper()}_{p['name'].upper()}" for p in ps
            if not re.search(r"\b" + re.escape(
                f"PID_{mod['name'].upper()}_{p['name'].upper()}") + r"\b", txt)]


# ── 主流程 ──────────────────────────────────────────────────────────────


def load_registry(src_dir):
    """读 <src_dir>/registry.yaml → [(name, path, status)]; 文件不存在返回 None。

    登记表只回答"有哪些模块、是否参与生成", 参数内容仍在各自 *.params.yaml。
    与 drivers/registry.yaml 同构 (name + status + description)。
    """
    reg = os.path.join(src_dir, "registry.yaml")
    if not os.path.isfile(reg):
        return None

    with open(reg, encoding="utf-8") as f:
        doc = yaml.safe_load(f) or {}
    mods = doc.get("modules")
    if not isinstance(mods, list) or not mods:
        raise ValueError(f"{reg}: 必须有非空的 modules 列表")

    out, seen_name, seen_file = [], set(), set()
    for i, e in enumerate(mods):
        if not isinstance(e, dict):
            raise ValueError(f"{reg}: modules[{i}] 必须是映射")
        name = e.get("name")
        if not is_ident(name):
            raise ValueError(f"{reg}: modules[{i}].name 缺失或不是合法标识符")
        if name in seen_name:
            raise ValueError(f"{reg}: 重复的模块名 {name!r}")
        seen_name.add(name)

        fname = e.get("file") or f"{name}.params.yaml"
        if fname in seen_file:
            raise ValueError(f"{reg}: 重复的 file {fname!r}")
        seen_file.add(fname)

        status = e.get("status", "active")
        if status not in ("active", "deprecated", "disabled"):
            raise ValueError(f"{reg}: {name} 的 status 非法: {status!r} "
                             f"(可用 active / deprecated / disabled)")
        out.append((name, os.path.join(src_dir, fname), status))
    return out


def generate_one(yaml_path, src_display):
    with open(yaml_path, encoding="utf-8") as f:
        schema = yaml.safe_load(f)
    if not isinstance(schema, dict):
        raise ValueError("顶层必须是映射 (module / params)")

    d = Diag(src_display)
    validate(schema, d)
    d.dump()
    if d.fatal:
        # 与成功分支保持同一元组形状, 否则调用处解包会抛 TypeError
        return None, None, None, None

    mod, ps = schema["module"], schema["params"]
    base = f"{mod['name']}_params"
    return base, mod, ps, {
        base + ".h": emit_header(mod, ps, src_display),
        base + ".c": emit_source(mod, ps, src_display),
    }


def main():
    ap = argparse.ArgumentParser(
        description="参数模块 YAML → C 生成器",
        epilog="例: python tools/param_gen.py            "
               "# 等价于 python tools/param_gen.py params gen/params\n"
               "    python tools/param_gen.py params gen/params --check\n"
               "    python tools/param_gen.py params gen/params --scaffold examples",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    # 两个位置参数都给了默认值: 在参数目录下直接跑脚本 (如 IDE 的 Run 按钮)
    # 不再因为少传参数而报 argparse 错误 + 回溯
    ap.add_argument("src_dir", nargs="?", default="params",
                    help="params/*.params.yaml 所在目录 (默认: params)")
    ap.add_argument("out_dir", nargs="?", default=os.path.join("gen", "params"),
                    help="生成物输出目录 (默认: gen/params)")
    ap.add_argument("--check", action="store_true",
                    help="不写文件, 只校验磁盘上的生成物是否与源一致")
    ap.add_argument("--scaffold", default=None, metavar="DIR",
                    help="另外生成用户骨架 <DIR>/<module>_module.c "
                         "(仅在文件不存在时创建, 绝不覆盖)")
    try:
        args = ap.parse_args()
    except SystemExit as e:  # argparse 已打印 usage; 不再抛回溯
        return int(e.code or 0)

    if not os.path.isdir(args.src_dir):
        print(f"FATAL E0002: 目录不存在: {args.src_dir}")
        print("提示: 请在仓库根目录运行, 或显式给出 <src_dir> <out_dir>;")
        print("      例: python tools/param_gen.py params gen/params")
        return 1

    try:
        registry = load_registry(args.src_dir)
    except ValueError as e:
        print(f"FATAL E0004: {e}")
        return 1

    registered = {}   # 文件名 → 登记的模块名
    if registry is None:
        # 没有登记表: 退回"全量 glob", 保持向后兼容
        files = sorted(glob.glob(os.path.join(args.src_dir, "*.params.yaml")))
        if not files:
            print(f"FATAL E0002: {args.src_dir} 下没有 *.params.yaml")
            return 1
        print(f"I0000  未发现 {args.src_dir}/registry.yaml, 按 *.params.yaml 全量生成")
    else:
        files = []
        skipped = []
        for name, path, status in registry:
            registered[os.path.basename(path)] = name
            (files if status == "active" else skipped).append(path if status == "active" else name)
        if skipped:
            print(f"I0000  跳过非 active 模块: {', '.join(skipped)}")
        if not files:
            print(f"FATAL E0002: registry.yaml 里没有 status=active 的模块")
            return 1

        # 先报"登记了但文件不在"——这条最能定位笔误 (name 与文件名对不上)
        missing = [p for p in files if not os.path.isfile(p)]
        if missing:
            for m in missing:
                print(f"FATAL E0004: registry.yaml 登记的文件不存在: {m}")
            print("  处理: 检查 registry 里的 name (默认推导 <name>.params.yaml) "
                  "或用 file: 显式指定")
            return 1

        # 再报"文件在但没登记" —— 这正是"写完忘接入"的静默失败场景, 直接拒绝
        on_disk = {os.path.basename(p)
                   for p in glob.glob(os.path.join(args.src_dir, "*.params.yaml"))}
        extra = sorted(on_disk - set(registered))
        if extra:
            print("FATAL E0004: 以下 *.params.yaml 未在 registry.yaml 登记 "
                  "(不登记就会被静默跳过, 故拒绝生成):")
            for x in extra:
                print(f"  - {args.src_dir}/{x}")
            print("  处理: 在 registry.yaml 的 modules 里加一行, 或删除/改名该文件")
            return 1

    if not args.check:
        os.makedirs(args.out_dir, exist_ok=True)

    outputs, failed = {}, False
    for path in files:
        src_display = path.replace(os.sep, "/")
        rel = os.path.relpath(path, os.path.dirname(os.path.dirname(src_display)))
        rel = rel.replace(os.sep, "/")
        base, mod, ps, generated = generate_one(path, rel)
        if generated is None:
            failed = True
            continue
        # 登记名与 yaml 里的 module.name 必须一致 —— 否则生成的符号名和登记表对不上
        want = registered.get(os.path.basename(path))
        if want and mod["name"] != want:
            print(f"FATAL E0004: {rel}: module.name={mod['name']!r} "
                  f"与 registry.yaml 登记的 {want!r} 不一致")
            failed = True
            continue
        # module.name 冲突会让两个 YAML 写同一个 _params.h/.c, 静默覆盖
        clash = [f for f in generated if f in outputs]
        if clash:
            print(f"FATAL E0003: {rel} 的 module.name 与其它 YAML 冲突 "
                  f"(输出 {', '.join(clash)})")
            failed = True
            continue
        outputs.update(generated)
        print(f"I0001  {rel} → {args.out_dir}/{base}.{{h,c}}  "
              f"({len(generated[base + '.c'])} + {len(generated[base + '.h'])} bytes)")

        if args.scaffold:
            target = os.path.join(args.scaffold, f"{mod['name']}_module.c")
            if os.path.isfile(target):
                miss = scaffold_missing(target, mod, ps)
                if miss:
                    print(f"W2004  {target} 已存在, 未覆盖; 这些 PID 在文件里没找到 "
                          f"(可能需要补 case): {', '.join(miss)}")
                else:
                    print(f"I0002  {target} 已存在, 未覆盖 (PID 齐全)")
            elif args.check:
                print(f"SCAFFOLD {target} 尚未生成 (--check 不会创建)")
            else:
                os.makedirs(args.scaffold, exist_ok=True)
                with open(target, "w", encoding="utf-8", newline="\n") as f:
                    f.write(emit_scaffold(mod, ps, rel, base))
                print(f"I0002  {rel} → {target} (用户骨架, 首次生成)")

    if failed:
        print("\nFATAL: 存在校验错误, 未生成任何文件。")
        return 1

    stale = []
    for fname, text in outputs.items():
        target = os.path.join(args.out_dir, fname)
        if args.check:
            old = None
            if os.path.isfile(target):
                with open(target, encoding="utf-8") as f:
                    old = f.read()
            if old != text:
                stale.append(fname)
        else:
            with open(target, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)

    if args.check:
        if stale:
            print("\nSTALE: 以下生成物与 YAML 不一致:")
            for s in stale:
                print(f"  {args.out_dir}/{s}")
            print("\n请:")
            print("  1. 审查 YAML 改动")
            print(f"  2. 运行: python tools/param_gen.py {args.src_dir} {args.out_dir}")
            print(f"  3. 提交更新后的 {args.out_dir}/ 文件")
            return 1
        print(f"\nOK: {len(outputs)} 个生成物与 YAML 一致。")
    else:
        print(f"\nOK: 已写出 {len(outputs)} 个文件到 {args.out_dir}/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
