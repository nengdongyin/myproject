/*
 * @file shell_sections_zephyr.ld
 * @brief letter-shell 命令导出段的链接器片段 (Zephyr 专用)
 *
 * letter-shell 的 SHELL_EXPORT_CMD/... 用 `__attribute__((section("shellCommand")))`
 * 把命令描述符放进名为 shellCommand 的段, shellInit() 依赖
 * `_shell_command_start` / `_shell_command_end` 两个符号确定段边界。
 *
 * GCC/ARMCC/IAR 的等价做法 (letter-shell README 移植说明):
 *   - GCC (裸机 ld):   _shell_command_start = .; KEEP(*(shellCommand)); _shell_command_end = .;
 *   - ARMCC (keil):     target option 增加 --keep shellCommand*
 *   - IAR:              shell.h 里 SHELL_SECTION 已用 @ shellCommand 处理
 *
 * 本文件是 **Zephyr 专用**形态: 由 CMakeLists 的
 *   zephyr_linker_sources(SECTIONS shell_sections_zephyr.ld)
 * 注入到全局链接脚本的 SECTIONS 块里。这里能用 SECTION_PROLOGUE /
 * GROUP_ROM_LINK_IN, 是因为 Zephyr 的链接脚本会过一遍 C 预处理器。
 *
 * 非 Zephyr (FreeRTOS + GCC / 裸机) 用 **同目录的 shell_sections_gcc.ld**:
 * 那份是纯 ld 语法, 由工程自己的链接脚本 `INCLUDE` 进去即可。
 *
 * @note 这里必须用 SECTIONS 而不是 RODATA: 本文件自带输出段
 *       (SECTION_PROLOGUE(shellCommand,,)), 而 RODATA 的片段是被 #include 到
 *       .rodata 输出段**内部**的 —— 输出段套输出段会链接失败。
 * @note Zephyr 4.x 没有 RODATA_SECTIONS 这个 location 名 (旧版本/cmake 文档里
 *       容易写错, 错误信息是 "Must choose valid location for linker snippet")。
 *
 * @note 若使用的 Zephyr 版本对 SECTION_PROLOGUE / GROUP_ROM_LINK_IN 宏有差异,
 *       只需在此文件内按对应版本调整段声明, core/shell 的 C 代码不受影响。
 */

SECTION_PROLOGUE(shellCommand,,)
{
	KEEP(*(shellCommand))
} GROUP_ROM_LINK_IN(RAMABLE_REGION, ROMABLE_REGION)

_shell_command_start = ADDR(shellCommand);
_shell_command_end = ADDR(shellCommand) + SIZEOF(shellCommand);
