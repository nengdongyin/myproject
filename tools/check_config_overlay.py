#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""校验工程配置覆盖层 config.h 是否安全。

背景
----
工程根目录的 `config.h` 由构建系统强制预处理（`-include config.h`），用于覆盖
各模块头里的默认值。模块头的写法必须是：

    #ifndef X
    #define X 默认值
    #endif

这样"在 config.h 里定义一次"才会生效。若覆盖了一个**裸 #define** 的宏，要么编译器
报宏重定义告警、要么静默无效；若覆盖了"接口契约类"宏（模块 ID / 编码 / 分区布局），
会破坏升级兼容性。本脚本把这两类风险变成 CI 可判定的规则。

规则
----
R1 config.h 里生效的每个宏，必须在 src/（排除 src/lib/ 第三方库）里定义过；
R2 该定义处必须被 `#ifndef 同名` 守卫（向前 15 行内、且中间不夹另一个同名 #define）；
R3 命中禁止清单（DENY_* / DENY_PREFIXES）直接报错。

用法
----
    python tools/check_config_overlay.py [--config config.h] [--root .]

退出码：0 = 全部合规；1 = 存在违规；2 = 用法/文件问题。
"""

from __future__ import annotations

import argparse
import os
import re
import sys

# ---------------------------------------------------------------- 禁止覆盖
# 接口契约 / 兼容性: 覆盖它们等于换协议或破坏升级路径
DENY_EXACT = {
    "SHELL_CFG_USER",        # letter-shell 自己的 overlay 入口
    "PARAM_MODULE_LIST",     # 模块注册清单 (驱动 ID 与初始化顺序)
    "MAKE_PARAM_ID",         # 参数 ID 编码
}
DENY_PREFIXES = (
    "MODULE_",               # 模块 ID: "ID 永不漂移"
    "PARAM_PARTITION_",      # 存储分区布局派生值
)

DEFINE_RE = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)")
IFNDEF_RE = re.compile(r"^\s*#\s*ifndef\s+([A-Za-z_]\w*)")
COMMENT_LINE_RE = re.compile(r"^\s*(/\*|\*|//)")

SEARCH_BACK_UP = 15  # 允许 #ifndef 与 #define 之间夹注释


def strip_block_comments(text: str) -> str:
    """去掉 /* ... */ 块注释（保留行数，便于报行号）。"""
    out = []
    in_block = False
    for line in text.splitlines():
        if in_block:
            if "*/" in line:
                in_block = False
                line = line.split("*/", 1)[1]
            else:
                out.append("")
                continue
        # 单行内可能有多段块注释
        while "/*" in line:
            head, rest = line.split("/*", 1)
            if "*/" in rest:
                line = head + rest.split("*/", 1)[1]
            else:
                in_block = True
                line = head
                break
        out.append(line)
    return "\n".join(out)


def active_defines(path: str) -> dict[str, int]:
    """收集文件中生效的 `#define NAME`（忽略注释与续行细节）。"""
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        raw = f.read()
    text = strip_block_comments(raw)
    found: dict[str, int] = {}
    for no, line in enumerate(text.splitlines(), start=1):
        if COMMENT_LINE_RE.match(line):
            continue
        code = line.split("//", 1)[0]
        m = DEFINE_RE.match(code)
        if m:
            found.setdefault(m.group(1), no)
    return found


def detect_own_guard(path: str) -> str | None:
    """取覆盖层文件自己的 include guard 名（第一个 `#ifndef X` 后的 `#define X`）。"""
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        text = strip_block_comments(f.read())
    guard = None
    for line in text.splitlines():
        code = line.split("//", 1)[0]
        m = IFNDEF_RE.match(code)
        if m and guard is None:
            guard = m.group(1)
            continue
        m = DEFINE_RE.match(code)
        if guard and m and m.group(1) == guard:
            return guard
    return None


def find_home(name: str, src_root: str) -> tuple[str, int] | None:
    """在 src/（排除 src/lib/）里找 `#define name` 的定义位置。"""
    pat = re.compile(r"^\s*#\s*define\s+" + re.escape(name) + r"\b")
    lib_dir = os.path.join(src_root, "lib") + os.sep
    for dirpath, _dirnames, filenames in os.walk(src_root):
        if dirpath.startswith(lib_dir):
            continue
        for fn in filenames:
            if not fn.endswith((".h", ".hpp", ".c")):
                continue
            path = os.path.join(dirpath, fn)
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as f:
                    lines = f.readlines()
            except OSError:
                continue
            for idx, line in enumerate(lines):
                if pat.match(line):
                    return path, idx  # 0-based
    return None


def guarded_above(lines: list[str], idx: int, name: str) -> bool:
    """判断 lines[idx] 的 #define 是否被上方的 `#ifndef name` 守卫。"""
    back = 0
    for j in range(idx - 1, max(idx - 1 - SEARCH_BACK_UP, -1), -1):
        back += 1
        line = lines[j]
        if COMMENT_LINE_RE.match(line):
            continue
        m = IFNDEF_RE.match(line)
        if m and m.group(1) == name:
            return True
        # 中间夹了别的预处理指令（含另一个同名 #define）→ 视为未守卫
        if line.lstrip().startswith("#"):
            return False
    return False


def main() -> int:
    ap = argparse.ArgumentParser(description="校验 config.h 覆盖层是否安全")
    ap.add_argument("--config", default="config.h")
    ap.add_argument("--root", default=".")
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    cfg = args.config if os.path.isabs(args.config) else os.path.join(root, args.config)
    src_root = os.path.join(root, "src")

    if not os.path.isfile(cfg):
        print(f"[ERR] 找不到覆盖层文件: {cfg}")
        return 2
    if not os.path.isdir(src_root):
        print(f"[ERR] 找不到源码目录: {src_root}")
        return 2

    defines = active_defines(cfg)

    # 跳过覆盖层自身的 include guard（它不是要给任何模块用的旋钮）
    guard = detect_own_guard(cfg)
    if guard:
        defines.pop(guard, None)

    if not defines:
        print(f"[OK] {args.config}: 未定义任何宏（全部沿用模块默认值）")
        return 0
    violations: list[str] = []
    for name, lineno in sorted(defines.items(), key=lambda kv: kv[1]):
        where = f"{args.config}:{lineno}  {name}"

        if name in DENY_EXACT or name.startswith(DENY_PREFIXES):
            violations.append(f"{where}\n    ✗ R3 禁止覆盖（接口契约类宏）")
            continue

        hit = find_home(name, src_root)
        if hit is None:
            violations.append(
                f"{where}\n    ✗ R1 src/ 中找不到定义（是否属于 src/lib 第三方库？）"
            )
            continue

        path, idx = hit
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
        if not guarded_above(lines, idx, name):
            rel = os.path.relpath(path, root)
            violations.append(
                f"{where}\n    ✗ R2 {rel}:{idx + 1} 的定义没有 #ifndef {name} 守卫"
                f" → 覆盖会变成宏重定义或静默失效"
            )

    if violations:
        print(f"[FAIL] {args.config}: {len(violations)} 处违规\n")
        for v in violations:
            print("  " + v)
        print(
            "\n提示: 把该模块头的默认值改成\n"
            "    #ifndef NAME\n    #define NAME 默认值\n    #endif\n"
            "之后再在 config.h 里覆盖。"
        )
        return 1

    print(f"[OK] {args.config}: {len(defines)} 个宏全部合规")
    for name in sorted(defines):
        print(f"    ✓ {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
