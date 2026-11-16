/*
 * @file shell_sections_gcc.ld
 * @brief letter-shell 命令导出段 —— 通用 GNU ld 片段 (FreeRTOS / 裸机 GCC)
 *
 * @par 用法
 *
 * **方式 A (推荐, 最稳): 不用本文件, 直接把三条语句贴进你自己的 .rodata**
 * —— 不引入 INCLUDE 的路径问题, 也不用操心输出段的 region, 因为 .rodata
 * 已经在你的脚本里被指派到 ROM 了:
 * @code
 *   .rodata :
 *   {
 *       *(.rodata .rodata.*)
 *       . = ALIGN(4);
 *       _shell_command_start = .;
 *       KEEP(*(shellCommand))
 *       _shell_command_end = .;
 *   } > FLASH AT> FLASH        /* 换成你脚本里的 region 名 *\/
 * @endcode
 * `. = ALIGN(4)` 不是装饰: SHELL_SECTION 用的是 `aligned(1)`, 段的起始对齐
 * 决定了整批描述符的对齐; 描述符里有指针, 起始不对齐就会产生非对齐访问。
 *
 * **方式 B: INCLUDE 本文件, 但路径必须是绝对路径**
 * @code
 *   INCLUDE "/abs/path/to/project/src/core/shell/shell_sections_gcc.ld"
 * @endcode
 * 为什么不能用相对路径: GNU ld 的 INCLUDE 只在**链接器当前工作目录**和
 * `-L` 指定的目录里找文件, 与 C 的 `#include`(相对当前文件)完全不同。
 * 而 `arm-xxx-gcc -T xxx.ld` 通常是在 build 目录里执行的, 于是
 * `INCLUDE "src/core/shell/..."` 会直接报
 * `cannot open linker script file ...: No such file or directory`。
 * 变通: 构建系统拼绝对路径 (Make 用 `$(abspath ...)`, CMake 用
 * `${CMAKE_CURRENT_SOURCE_DIR}` 生成一行), 或给链接命令加
 * `-Wl,-L<工程根>`。
 *
 * **方式 C: 本文件单独 INCLUDE 时要注意 region**
 * 本片段刻意**不写** `> REGION`, 它跟随 INCLUDE 处的位置计数器。所以必须
 * INCLUDE 在 `.rodata` 之后 (还在 ROM 段内); 若你的脚本用 MEMORY + 每段显式
 * `> FLASH`, 更建议用方式 A 把内容贴进去, 让它继承 `.rodata` 的 region。
 *
 * @par 为什么必须有这一段
 * SHELL_EXPORT_CMD 用 `__attribute__((section("shellCommand")))` 把命令
 * 描述符放进 shellCommand 输入段, shellInit() 靠 `_shell_command_start` /
 * `_shell_command_end` 求段边界。若链接时开了 `--gc-sections`
 * (配合 -ffunction-sections/-fdata-sections, GCC 工程几乎默认开) 而没有
 * KEEP, 整段会被当垃圾回收 —— 现象是"命令明明编译进去了, 上电后
 * help 里却看不到", 且没有任何链接期报错。
 *
 * @note 输出段故意叫 `.shell_command` 而不是 `shellCommand`: 输入/输出段
 *       同名虽然也能工作, 但排查 map 文件时极难分辨"这是收集进来的还是
 *       我自己定义的", 而本段存在的意义只有一个 —— 提供边界符号。
 * @note Zephyr 构建请改用 shell_sections_zephyr.ld (C 预处理器宏形式),
 *       两者内容等价, 只是语法各自适配。
 * @note ARMCC (Keil): 不用本文件, 在 target/Linker 选项加 `--keep shellCommand*`。
 * @note IAR: 不用本文件, shell.h 的 SHELL_SECTION 已用 `@ shellCommand` 处理。
 */

.shell_command ALIGN(4) :
{
    _shell_command_start = .;
    KEEP(*(shellCommand))
    _shell_command_end = .;
}
