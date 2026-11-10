/**
 * @file mock_fal.c
 * @brief RAM-backed FAL mock 实现 — 见 mock_fal.h
 *
 * 仅提供 mkv.c 依赖的 6 个 fal_* 符号，不链接真实 FAL。
 * 读/写/擦除严格校验分区边界 (addr + size > len 即返回 -1)，
 * 因此任何越过分区末尾的探测都会在这里暴露为读失败。
 */

#include "mock_fal.h"
#include <string.h>

#define MOCK_FLASH_NAME "mock_flash"
#define MOCK_RAM_SIZE   (64 * 1024)

static uint8_t g_ram[MOCK_RAM_SIZE];

static struct fal_flash_dev g_flash = {
    .name = MOCK_FLASH_NAME,
    .addr = 0,
    .len  = MOCK_RAM_SIZE,
    .blk_size = 4096,
    .write_gran = 1,
};

static struct fal_partition g_part = {
    .magic_word = 0x45503130u,
    .name = MOCK_PART_NAME,
    .flash_name = MOCK_FLASH_NAME,
    .offset = 0,
    .len = 64 * 1024,
    .reserved = 0,
};

void mock_fal_reset(uint32_t blk_size, uint32_t partition_len)
{
    memset(g_ram, 0xFF, sizeof(g_ram));
    g_flash.blk_size = blk_size;
    g_flash.len = partition_len;
    g_part.len = partition_len;
}

const uint8_t *mock_fal_ram(void) { return g_ram; }
uint32_t mock_fal_ram_size(void) { return MOCK_RAM_SIZE; }

int fal_init(void)
{
    return 1;
}

const struct fal_flash_dev *fal_flash_device_find(const char *name)
{
    if (name && strcmp(name, MOCK_FLASH_NAME) == 0)
        return &g_flash;
    return NULL;
}

const struct fal_partition *fal_partition_find(const char *name)
{
    if (name && strcmp(name, MOCK_PART_NAME) == 0)
        return &g_part;
    return NULL;
}

int fal_partition_read(const struct fal_partition *part, uint32_t addr,
                       uint8_t *buf, size_t size)
{
    if (!part || !buf || (uint64_t)addr + size > part->len)
        return -1;
    memcpy(buf, g_ram + part->offset + addr, size);
    return (int)size;
}

int fal_partition_write(const struct fal_partition *part, uint32_t addr,
                        const uint8_t *buf, size_t size)
{
    if (!part || !buf || (uint64_t)addr + size > part->len)
        return -1;
    memcpy(g_ram + part->offset + addr, buf, size);
    return (int)size;
}

int fal_partition_erase(const struct fal_partition *part, uint32_t addr, size_t size)
{
    if (!part || (uint64_t)addr + size > part->len)
        return -1;
    memset(g_ram + part->offset + addr, 0xFF, size);
    return (int)size;
}
