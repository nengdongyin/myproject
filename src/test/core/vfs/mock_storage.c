/**
 * @file mock_storage.c
 * @brief 内存 flash 介质实现（见 mock_storage.h）
 */

#include "mock_storage.h"
#include <stdlib.h>
#include <string.h>

/** @brief 介质读：越界返回 -1，否则按字节拷贝 */
static int mock_read(vfs_storage_t *s, uint32_t off, void *buf, size_t n)
{
    vfs_mock_flash_t *m = (vfs_mock_flash_t *)s->ctx;
    if ((size_t)off + n > (size_t)m->total)
        return -1;
    memcpy(buf, m->mem + off, n);
    m->read_calls++;
    return 0;
}

/**
 * @brief 介质编程：模拟 flash「按位与」语义（1→0 只能，0→1 无效）
 *
 * 因此要正确写入数据，目标区域必须是 0xFF（已擦除）；否则数据被静默损坏，
 * 用于验证 rawflash 的擦除纪律与 append 的 0xFF 脏尾检测。
 */
static int mock_prog(vfs_storage_t *s, uint32_t off, const void *buf, size_t n)
{
    vfs_mock_flash_t *m = (vfs_mock_flash_t *)s->ctx;
    if ((size_t)off + n > (size_t)m->total)
        return -1;
    const uint8_t *p = (const uint8_t *)buf;
    for (size_t i = 0; i < n; i++)
        m->mem[off + i] &= p[i];
    m->prog_calls++;
    return 0;
}

/** @brief 介质擦除：置 0xFF */
static int mock_erase(vfs_storage_t *s, uint32_t off, size_t n)
{
    vfs_mock_flash_t *m = (vfs_mock_flash_t *)s->ctx;
    if ((size_t)off + n > (size_t)m->total)
        return -1;
    memset(m->mem + off, 0xFF, n);
    m->erase_calls++;
    return 0;
}

int vfs_mock_flash_init(vfs_mock_flash_t *m, uint32_t blk_size, uint32_t blk_count)
{
    if (!m || blk_size == 0 || blk_count == 0)
        return -1;
    memset(m, 0, sizeof(*m));
    m->total = blk_size * blk_count;
    m->mem = (uint8_t *)malloc(m->total);
    if (!m->mem)
        return -1;
    memset(m->mem, 0xFF, m->total); /* 初始为擦除态 */

    m->storage.name = "mock_flash";
    m->storage.blk_size = blk_size;
    m->storage.blk_count = blk_count;
    m->storage.read = mock_read;
    m->storage.prog = mock_prog;
    m->storage.erase = mock_erase;
    m->storage.ctx = m;
    return 0;
}

void vfs_mock_flash_free(vfs_mock_flash_t *m)
{
    if (!m)
        return;
    free(m->mem);
    m->mem = NULL;
}

void vfs_mock_make_bdev(vfs_mock_flash_t *m, const char *name,
                        uint32_t byte_offset, uint32_t blk_count)
{
    vfs_bdev_init(&m->bdev, name, &m->storage, byte_offset,
                  m->storage.blk_size, blk_count);
}
