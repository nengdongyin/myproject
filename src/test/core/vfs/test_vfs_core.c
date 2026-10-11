/**
 * @file test_vfs_core.c
 * @brief VFS 核心单元测试 —— 注册 / 挂载 / 路径解析 / 句柄池 / 分派 / 前置校验
 *
 * 用一个内存 mock 后端（只认 vfs_fs_ops_t）驱动核心，无需任何真实文件系统，
 * 覆盖：最长前缀解析、RO 挂载拦截、caps 前置校验、fd 生命周期、ENFILE、EBADF 等。
 */

#include "unity.h"
#include "test_suite.h"
#include "vfs.h"
#include <string.h>
#include <errno.h>

/* ================================================================
 *  Mock 后端
 * ================================================================ */

#define MOCK_FILE_CAP 256

static uint8_t g_data[MOCK_FILE_CAP];
static int     g_len;                    /* 当前"文件"长度 */
static char    g_last_rel[64];           /* 后端收到的相对路径（验证解析） */
static int     g_mount_calls, g_unmount_calls, g_unlink_calls;

typedef struct { int used; int cursor; } mock_handle_t;
static mock_handle_t g_handles[VFS_MAX_OPEN_FILES];

static mock_handle_t *h_alloc(void)
{
    for (int i = 0; i < VFS_MAX_OPEN_FILES; i++)
        if (!g_handles[i].used)
        {
            g_handles[i].used = 1;
            g_handles[i].cursor = 0;
            return &g_handles[i];
        }
    return NULL;
}
static void h_free(mock_handle_t *h) { if (h) h->used = 0; }

static int mock_mount(vfs_mount_t *mp)   { (void)mp; g_mount_calls++;   return 0; }
static int mock_unmount(vfs_mount_t *mp) { (void)mp; g_unmount_calls++; return 0; }

static int mock_open(vfs_file_t *f, const char *rel, int flags)
{
    mock_handle_t *h = h_alloc();
    if (!h)
        return -ENFILE;
    strncpy(g_last_rel, rel, sizeof(g_last_rel) - 1);
    g_last_rel[sizeof(g_last_rel) - 1] = '\0';
    if (flags & VFS_O_TRUNC)
        g_len = 0;
    f->priv = h;
    return 0;
}

static ssize_t mock_read(vfs_file_t *f, void *buf, size_t n)
{
    mock_handle_t *h = (mock_handle_t *)f->priv;
    if (!h)
        return -EBADF;
    size_t avail = (h->cursor < g_len) ? (size_t)(g_len - h->cursor) : 0;
    if (n > avail)
        n = avail;
    memcpy(buf, g_data + h->cursor, n);
    h->cursor += (int)n;
    return (ssize_t)n;
}

static ssize_t mock_write(vfs_file_t *f, const void *buf, size_t n)
{
    mock_handle_t *h = (mock_handle_t *)f->priv;
    if (!h)
        return -EBADF;
    if (h->cursor >= MOCK_FILE_CAP)
        return -ENOSPC;
    size_t room = (size_t)(MOCK_FILE_CAP - h->cursor);
    if (n > room)
        n = room;
    memcpy(g_data + h->cursor, buf, n);
    h->cursor += (int)n;
    if (h->cursor > g_len)
        g_len = h->cursor;
    return (ssize_t)n;
}

static off_t mock_lseek(vfs_file_t *f, off_t off, int whence)
{
    mock_handle_t *h = (mock_handle_t *)f->priv;
    if (!h)
        return -EBADF;
    off_t base = (whence == VFS_SEEK_SET) ? 0
               : (whence == VFS_SEEK_CUR) ? (off_t)h->cursor
               : (whence == VFS_SEEK_END) ? (off_t)g_len
                                          : -1;
    if (base < 0)
        return -EINVAL;
    off_t np = base + off;
    if (np < 0)
        return -EINVAL;
    h->cursor = (int)np;
    return np;
}

static off_t mock_tell(vfs_file_t *f)
{
    mock_handle_t *h = (mock_handle_t *)f->priv;
    return h ? (off_t)h->cursor : -EBADF;
}

static int mock_close(vfs_file_t *f)
{
    h_free((mock_handle_t *)f->priv);
    f->priv = NULL;
    return 0;
}

static int mock_stat(vfs_mount_t *mp, const char *rel, vfs_stat_t *st)
{
    (void)mp; (void)rel;
    st->type = VFS_TYPE_FILE;
    st->size = (uint32_t)g_len;
    return 0;
}

static int mock_unlink(vfs_mount_t *mp, const char *rel)
{
    (void)mp; (void)rel;
    g_len = 0;
    g_unlink_calls++;
    return 0;
}

/* 故意不含 VFS_CAP_APPEND，用于验证 caps 前置校验 */
static const vfs_fs_ops_t mock_ops = {
    .name = "mock",
    .caps = VFS_CAP_READ | VFS_CAP_WRITE | VFS_CAP_SEEK | VFS_CAP_TRUNCATE,
    .mount = mock_mount,
    .unmount = mock_unmount,
    .open = mock_open,
    .read = mock_read,
    .write = mock_write,
    .lseek = mock_lseek,
    .close = mock_close,
    .tell = mock_tell,
    .stat = mock_stat,
    .unlink = mock_unlink,
};

/* ================================================================
 *  setUp / tearDown
 * ================================================================ */

static void core_setup(void)
{
    memset(g_data, 0, sizeof(g_data));
    g_len = 0;
    g_last_rel[0] = '\0';
    memset(g_handles, 0, sizeof(g_handles));
    g_mount_calls = g_unmount_calls = g_unlink_calls = 0;

    vfs_deinit();          /* 清空核心状态（幂等安全） */
    TEST_ASSERT_EQUAL_INT(0, vfs_init());
    TEST_ASSERT_EQUAL_INT(0, vfs_register(VFS_FSTYPE_EXTERNAL_BASE, &mock_ops));

    static const vfs_mount_cfg_t mnts[] = {
        {"/mnt", VFS_FSTYPE_EXTERNAL_BASE, NULL, 0, NULL},
        {"/ro",  VFS_FSTYPE_EXTERNAL_BASE, NULL, VFS_MNT_RO, NULL},
    };
    TEST_ASSERT_EQUAL_INT(0, vfs_mount_table(mnts, 2));
}

static void core_teardown(void)
{
    vfs_deinit();
}

/* ================================================================
 *  Case: 挂载 / 打开
 * ================================================================ */

static void test_mount_called(void)
{
    TEST_ASSERT_EQUAL_INT(2, g_mount_calls);
}

static void test_open_ok(void)
{
    vfs_fd_t fd = vfs_open("/mnt/foo", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT(0, vfs_close(fd));
}

static void test_open_relative_path_rejected(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, vfs_open("foo", VFS_O_RDONLY));
}

static void test_open_unmounted_returns_enoent(void)
{
    TEST_ASSERT_EQUAL_INT(-ENOENT, vfs_open("/nomnt/foo", VFS_O_RDONLY));
}

/* 边界：/mntx 不应匹配挂载点 /mnt */
static void test_prefix_boundary_not_matched(void)
{
    TEST_ASSERT_EQUAL_INT(-ENOENT, vfs_open("/mntx/foo", VFS_O_RDONLY));
}

/* 最长前缀：/mnt/deep 比 /mnt 更长，应命中前者，rel = "x" */
static void test_longest_prefix_resolve(void)
{
    static const vfs_mount_cfg_t deep[] = {
        {"/mnt/deep", VFS_FSTYPE_EXTERNAL_BASE, NULL, 0, NULL},
    };
    TEST_ASSERT_EQUAL_INT(0, vfs_mount_table(deep, 1));

    vfs_fd_t fd = vfs_open("/mnt/deep/x", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_STRING("x", g_last_rel); /* 后端收到的是去前缀相对路径 */
    vfs_close(fd);
}

/* ================================================================
 *  Case: 前置校验（RO 挂载 / caps / 非法标志）
 * ================================================================ */

static void test_readonly_mount_rejects_write(void)
{
    TEST_ASSERT_EQUAL_INT(-EROFS, vfs_open("/ro/x", VFS_O_WRONLY));
    TEST_ASSERT_EQUAL_INT(-EROFS, vfs_open("/ro/x", VFS_O_RDONLY | VFS_O_CREAT));
    /* 合法写组合（WRONLY|TRUNC）；注意 RDONLY|TRUNC 属非法组合，在挂载检查前即 -EINVAL */
    TEST_ASSERT_EQUAL_INT(-EROFS, vfs_open("/ro/x", VFS_O_WRONLY | VFS_O_TRUNC));
    /* 只读访问允许 */
    vfs_fd_t fd = vfs_open("/ro/x", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    vfs_close(fd);
}

static void test_append_cap_unsupported(void)
{
    TEST_ASSERT_EQUAL_INT(-ENOTSUP, vfs_open("/mnt/x", VFS_O_WRONLY | VFS_O_APPEND));
}

static void test_invalid_access_mode(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, vfs_open("/mnt/x", 0x3));
}

static void test_rdonly_trunc_rejected(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, vfs_open("/mnt/x", VFS_O_RDONLY | VFS_O_TRUNC));
}

/* ================================================================
 *  Case: 读 / 写 / 定位 分派
 * ================================================================ */

static void test_write_read_seek_roundtrip(void)
{
    vfs_fd_t fd = vfs_open("/mnt/f", VFS_O_RDWR);
    TEST_ASSERT_TRUE(fd >= 0);

    TEST_ASSERT_EQUAL_INT(2, (int)vfs_write(fd, "hi", 2));
    TEST_ASSERT_EQUAL_INT(0, (int)vfs_lseek(fd, 0, VFS_SEEK_SET));
    char buf[4] = {0};
    TEST_ASSERT_EQUAL_INT(2, (int)vfs_read(fd, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_MEMORY("hi", buf, 2);
    TEST_ASSERT_EQUAL_INT(2, (int)vfs_tell(fd));

    vfs_close(fd);
}

/* ================================================================
 *  Case: fd 生命周期 / EBADF / ENFILE
 * ================================================================ */

static void test_ebadf_after_close(void)
{
    vfs_fd_t fd = vfs_open("/mnt/f", VFS_O_RDWR);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT(0, vfs_close(fd));
    TEST_ASSERT_EQUAL_INT(-EBADF, (int)vfs_read(fd, (void *)"x", 1));
    TEST_ASSERT_EQUAL_INT(-EBADF, (int)vfs_write(fd, "x", 1));
    TEST_ASSERT_EQUAL_INT(-EBADF, vfs_close(fd));
}

static void test_bad_fd_values(void)
{
    TEST_ASSERT_EQUAL_INT(-EBADF, (int)vfs_read(-1, (void *)"x", 1));
    TEST_ASSERT_EQUAL_INT(-EBADF, (int)vfs_read(9999, (void *)"x", 1));
}

static void test_enfile_on_pool_exhaustion(void)
{
    vfs_fd_t fds[VFS_MAX_OPEN_FILES];
    for (int i = 0; i < VFS_MAX_OPEN_FILES; i++)
    {
        fds[i] = vfs_open("/mnt/f", VFS_O_RDONLY);
        TEST_ASSERT_TRUE(fds[i] >= 0);
    }
    /* 池已满 */
    TEST_ASSERT_EQUAL_INT(-ENFILE, vfs_open("/mnt/f", VFS_O_RDONLY));
    for (int i = 0; i < VFS_MAX_OPEN_FILES; i++)
        vfs_close(fds[i]);
}

/* ================================================================
 *  Case: stat / unlink
 * ================================================================ */

static void test_stat_root_is_dir(void)
{
    vfs_stat_t st;
    TEST_ASSERT_EQUAL_INT(0, vfs_stat("/mnt", &st));
    TEST_ASSERT_EQUAL_UINT8(VFS_TYPE_DIR, st.type);
}

static void test_stat_file_reports_size(void)
{
    vfs_fd_t fd = vfs_open("/mnt/f", VFS_O_RDWR);
    vfs_write(fd, "abcd", 4);
    vfs_close(fd);

    vfs_stat_t st;
    TEST_ASSERT_EQUAL_INT(0, vfs_stat("/mnt/f", &st));
    TEST_ASSERT_EQUAL_UINT8(VFS_TYPE_FILE, st.type);
    TEST_ASSERT_EQUAL_UINT32(4, st.size);
}

static void test_unlink_file(void)
{
    TEST_ASSERT_EQUAL_INT(0, vfs_unlink("/mnt/f"));
    TEST_ASSERT_EQUAL_INT(1, g_unlink_calls);
}

static void test_unlink_root_rejected(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, vfs_unlink("/mnt"));
}

/* ================================================================
 *  Runner
 * ================================================================ */

int vfs_core_run_all_tests(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_mount_called);
    RUN_TEST(test_open_ok);
    RUN_TEST(test_open_relative_path_rejected);
    RUN_TEST(test_open_unmounted_returns_enoent);
    RUN_TEST(test_prefix_boundary_not_matched);
    RUN_TEST(test_longest_prefix_resolve);

    RUN_TEST(test_readonly_mount_rejects_write);
    RUN_TEST(test_append_cap_unsupported);
    RUN_TEST(test_invalid_access_mode);
    RUN_TEST(test_rdonly_trunc_rejected);

    RUN_TEST(test_write_read_seek_roundtrip);

    RUN_TEST(test_ebadf_after_close);
    RUN_TEST(test_bad_fd_values);
    RUN_TEST(test_enfile_on_pool_exhaustion);

    RUN_TEST(test_stat_root_is_dir);
    RUN_TEST(test_stat_file_reports_size);
    RUN_TEST(test_unlink_file);
    RUN_TEST(test_unlink_root_rejected);

    return UNITY_END();
}

TEST_SUITE_DEFINE(vfs_core, core_setup, core_teardown, vfs_core_run_all_tests);
