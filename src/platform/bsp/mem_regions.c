/**
 * @file mem_regions.c
 * @brief 板级内存白名单表 (rpi_pico2 / RP2350A)
 *
 * 地址取自本工程的设备树 (build/zephyr/zephyr.dts):
 *   flash0: reg = <0x10000000 0x00400000>   (4 MiB, XIP)
 *   sram0 : reg = <0x20000000 0x00082000>   (520 KiB)
 * 换板子时改下面这张表即可; 迁移到 FreeRTOS 工程时同表照抄, 与 RTOS 无关。
 *
 * 只读段 (writable = 0) 上 `mw` 会被拒绝; `md` 不受 writable 影响。
 */

#include "mem_regions.h"

/** @brief 本板可访问地址段 (顺序无关, 判定用"整段落在同一段内") */
static const mem_region_t s_regions[] = {
    /* XIP flash: 只读。写它要走 flash 控制器编程流程, 不是 memcpy 能做的事 */
    { "flash", 0x10000000u, 0x00400000u, 0 },

    /* SRAM: 可读写 (调试时看栈/全局变量、改临时变量都在这) */
    { "sram",  0x20000000u, 0x00082000u, 1 },

    /* ---- 外设段: 默认不开放 ----
     * 打开前请确认: (1) 该地址在本板确实存在; (2) 读它没有副作用;
     * (3) 写它不会把总线/时钟锁死。示例:
     *   { "periph", 0x40000000u, 0x00100000u, 1 },
     *   { "sio",    0xd0000000u, 0x00001000u, 1 },
     */
};

const mem_region_t *mem_regions_get(size_t *count)
{
    if (count != NULL)
        *count = sizeof(s_regions) / sizeof(s_regions[0]);
    return s_regions;
}

int mem_regions_configure(void)
{
    size_t n = 0;
    const mem_region_t *tbl = mem_regions_get(&n);

    return mem_access_configure(tbl, n);
}