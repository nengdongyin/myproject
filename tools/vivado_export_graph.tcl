# vivado_export_graph.tcl
# ============================================================================
# VSC Bitstream Descriptor 导出脚本
#
# 用法（在 Vivado Tcl Console 中）：
#   source tools/vivado_export_graph.tcl
#   vsc_export_graph <block_design_name> [output_path]
#
# 输出：system_graph.json — VSC Pipeline 拓扑描述文件
# ============================================================================

namespace eval vsc {

# ═══════════════════════════════════════════════════════════════════════════
#  Vivado IP VLNV → VSC driver name 映射表
#
#  键 = Vivado 完整 VLNJ 字符串
#  值 = VSC driver name (对应 drivers/registry.yaml 中的 name)
#
#  新增 IP 在此添加映射条目。
# ═══════════════════════════════════════════════════════════════════════════

array set type_map {
    # ── FPGA IP ──
    "xilinx.com:ip:v_crop:*"                      "crop"
    "xilinx.com:ip:v_demosaic:*"                  "decoder"
    "xilinx.com:ip:axis_binning_2x2:*"            "binning"
    "user.org:ip:crop:*"                           "crop"
    "user.org:ip:binning:*"                        "binning"
    "user.org:ip:decoder:*"                        "decoder"
    "user.org:ip:histogram:*"                      "histogram"
    "user.org:ip:scaler:*"                         "scaler"
    "user.org:ip:lvds_rx:*"                        "lvds_rx"
    "user.org:ip:cl_output:*"                      "cl_output"
}

# ═══════════════════════════════════════════════════════════════════════════
#  IP instance name → VSC type override（匹配优先级高于 VLNV 映射）
# ═══════════════════════════════════════════════════════════════════════════

array set name_override {}

# ═══════════════════════════════════════════════════════════════════════════
#  IP type → stream link direction（由 Tcl 脚本自动判定，无需手动配置）
# ═══════════════════════════════════════════════════════════════════════════

proc resolve_type {vlnv inst_name} {
    variable type_map
    variable name_override

    # 先查实例名覆盖
    if {[info exists name_override($inst_name)]} {
        return $name_override($inst_name)
    }

    # 再查 VLNV 表（支持通配符 *）
    foreach {pattern vsc_name} [array get type_map] {
        if {[string match $pattern $vlnv]} {
            return $vsc_name
        }
    }

    # 回退：从 VLNV 中提取短名
    set parts [split $vlnv ":"]
    set short [lindex $parts end-1]
    return $short
}

# ═══════════════════════════════════════════════════════════════════════════
#  获取 IP 的 AXI 基地址
# ═══════════════════════════════════════════════════════════════════════════

proc get_base_addr {bd_name cell_name} {
    # 遍历 address segments 查找匹配的 cell
    set segs [get_bd_addr_segs -of_objects [get_bd_cells $cell_name] -quiet]
    if {$segs == ""} { return "0x0" }

    # 返回第一个 segment 的 offset（通常只有一个）
    set offset [get_property offset [lindex $segs 0]]
    return [format "0x%08X" $offset]
}

# ═══════════════════════════════════════════════════════════════════════════
#  获取 AXI Stream 连接
#  返回：{src_inst src_port dst_inst dst_port} 列表的列表
# ═══════════════════════════════════════════════════════════════════════════

proc get_stream_links {bd_name} {
    set links {}
    set intfs [get_bd_intf_pins -of_objects [get_bd_cells] -quiet]

    foreach intf $intfs {
        set mode [get_property mode $intf]
        if {$mode != "Master"} { continue }

        # 找到该 Master 连接的 Slave
        set conn [get_bd_intf_pins -of_objects [get_bd_intf_net -of_objects $intf] -quiet]
        foreach slave_intf $conn {
            set smode [get_property mode $slave_intf]
            if {$smode != "Slave"} { continue }

            set src_cell [get_bd_cells -of_objects $intf]
            set dst_cell [get_bd_cells -of_objects $slave_intf]

            if {$src_cell == "" || $dst_cell == ""} { continue }
            if {$src_cell == $dst_cell} { continue }

            lappend links [list \
                [get_property name $src_cell] \
                [get_property name $intf] \
                [get_property name $dst_cell] \
                [get_property name $slave_intf]]
        }
    }
    return $links
}

# ═══════════════════════════════════════════════════════════════════════════
#  判断 link type：STREAM 或 TAP
#  TAP = 目标 IP 是 ANALYZER 类型（histogram / ae_stats / awb_stats）
# ═══════════════════════════════════════════════════════════════════════════

proc classify_link {vsc_type} {
    set analyzers {histogram ae_stats awb_stats roi_stats}
    if {[lsearch -exact $analyzers $vsc_type] >= 0} {
        return "TAP"
    }
    return "STREAM"
}

# ═══════════════════════════════════════════════════════════════════════════
#  主入口
# ═══════════════════════════════════════════════════════════════════════════

proc export_graph {bd_name {output_path "system_graph.json"}} {
    puts "=== VSC Graph Export ==="
    puts "Block Design : $bd_name"
    puts "Output       : $output_path"

    # 打开 Block Design
    if {[get_bd_designs -quiet $bd_name] == ""} {
        open_bd_design [get_files "$bd_name.bd"]
    }

    set cells [get_bd_cells -quiet]
    if {$cells == ""} {
        error "No cells found in Block Design '$bd_name'"
    }

    # ── 构建 nodes ──
    set nodes_json {}
    set inst_names {}
    foreach cell $cells {
        set inst_name [get_property name $cell]
        set vlnv      [get_property vlnv $cell]
        set vsc_type  [resolve_type $vlnv $inst_name]
        set base_addr [get_base_addr $bd_name $inst_name]

        # 跳过非 VSC 相关的 IP（处理器、总线等）
        if {$vsc_type == ""} { continue }

        lappend inst_names $inst_name

        # 判断是否 optional（默认为 false）
        set optional "false"
        if {[string match "*_opt*" $inst_name]} { set optional "true" }

        append nodes_json "    \{\"type\": \"$vsc_type\", \"id\": \"$inst_name\", \"base\": \"$base_addr\", \"optional\": $optional\},\n"
        puts "  node: $inst_name → type=$vsc_type, base=$base_addr"
    }

    # ── 构建 links ──
    set stream_links [get_stream_links $bd_name]
    set links_json {}
    foreach link $stream_links {
        lassign $link src_inst src_port dst_inst dst_port

        # 两边都必须是已导出的 VSC 节点
        if {[lsearch -exact $inst_names $src_inst] < 0} { continue }
        if {[lsearch -exact $inst_names $dst_inst] < 0} { continue }

        # 获取目标 IP 的 VSC 类型，判断 link type
        set dst_cell [get_bd_cells $dst_inst]
        set dst_vlnv [get_property vlnv $dst_cell]
        set dst_type [resolve_type $dst_vlnv $dst_inst]
        set link_type [classify_link $dst_type]

        append links_json "    \{\"src\": \"$src_inst\", \"dst\": \"$dst_inst\", \"type\": \"$link_type\"\},\n"
        puts "  link: $src_inst → $dst_inst ($link_type)"
    }

    # ── 输出 JSON ──
    set fd [open $output_path w]
    puts $fd "\{"
    puts $fd "  \"nodes\": \["
    puts -nonewline $fd [string trimright $nodes_json ",\n"]
    puts $fd "\n  \],"
    puts $fd "  \"links\": \["
    puts -nonewline $fd [string trimright $links_json ",\n"]
    puts $fd "\n  \]"
    puts $fd "\}"
    close $fd

    puts "=== Export complete ==="
    puts "[llength $inst_names] nodes, [llength $stream_links] links → $output_path"
}

};  # namespace vsc

# ── 快捷命令 ──
proc vsc_export_graph {bd_name {output_path "system_graph.json"}} {
    vsc::export_graph $bd_name $output_path
}

puts "vsc_export_graph loaded. Usage: vsc_export_graph <bd_name> \[output_path\]"
