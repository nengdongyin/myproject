/**
 * @file test_vfs_format.c
 * @brief vfs_format 单元测试 —— 路径解析 / 能力校验 / 挂载失败后仍可格式化 / EBUSY
 *
 * 用一个只认 vfs_fs_ops_t 的内存 mock 后端驱动核心的 vfs_format：
 * 覆盖格式化的核心场景——「卷挂载失败（如 SD 乱数据）后仍能按路径解析并格式化」，
 * 以及 -EBUSY（卷已挂载）、-ENOTSUP（后端不支持）、-ENOENT、-EINVAL 等前置校验。
 */

#include "unity.h"
#include "test_suite.h"
#include "vfs.h"
#include "mock_storage.h"
#include <string.h>
#include <errno.h>

/* ================================================================
 *  Mock 后端（带 format）
 * ================================================================ */

#define FMT_FSTYPE_NORMAL VFS_FSTYPE_EXTERNAL_BASE
#define FMT_FSTYPE_FAIL   (VFS_FSTYPE_EXTERNAL_BASE + 1)
#define FMT_FSTYPE_NOFMT  (VFS_FSTYPE_EXTERNAL_BASE + 2)

static vfs_mock_flash_t g_flash;

static int     g_format_calls;          /* format 回调调用次数 */
static const vfs_bdev_t *g_format_bdev; /* 后端收到的 bdev 指针（验证解析） */

static int fmt_mount_ok(vfs_mount_t *mp)     { (void)mp; return 0; }
static int fmt_mount_fail(vfs_mount_t *mp)   { (void)mp; return -EIO; }
static int fmt_unmount(vfs_mount_t *mp)      { (void)mp; return 0; }

static int fmt_open(vfs_file_t *f, const char *rel, int flags)
{
    (void)rel; (void)flags;
    f->priv = NULL; /* 不需要真实句柄 */
    return 0;
}

static int fmt_format(const vfs_bdev_t *bdev)
{
    g_format_calls++;
    g_format_bdev = bdev;
    return 0;
}

/* 支持 format 的常规后端（mount 成功） */
static const vfs_fs_ops_t fmt_ops = {
    .name = "fmt",
    .caps = VFS_CAP_READ | VFS_CAP_WRITE | VFS_CAP_FORMAT,
    .mount = fmt_mount_ok,
    .unmount = fmt_unmount,
    .format = fmt_format,
    .open = fmt_open,
};

/* 支持 format 但 mount 失败（模拟 SD 乱数据挂载失败） */
static const vfs_fs_ops_t fmt_fail_ops = {
    .name = "fmt_fail",
    .caps = VFS_CAP_READ | VFS_CAP_WRITE | VFS_CAP_FORMAT,
    .mount = fmt_mount_fail,
    .unmount = fmt_unmount,
    .format = fmt_format,
    .open = fmt_open,
};

/* 不支持 format（无 VFS_CAP_FORMAT、无 format 回调） */
static const vfs_fs_ops_t no_fmt_ops = {
    .name = "no_fmt",
    .caps = VFS_CAP_READ | VFS_CAP_WRITE,
    .mount = fmt_mount_ok,
    .unmount = fmt_unmount,
    .open = fmt_open,
};

/* ================================================================
 *  setUp / tearDown
 * ================================================================ */

static void fmt_setup(void)
{
    g_format_calls = 0;
    g_format_bdev = NULL;

    TEST_ASSERT_EQUAL_INT(0, vfs_mock_flash_init(&g_flash, 512, 16)); /* 8KB，512B 块模拟 SD 扇区 */
    vfs_mock_make_bdev(&g_flash, "mock_bdev", 0, 16);

    vfs_deinit(); /* 清空核心状态（幂等安全） */
    TEST_ASSERT_EQUAL_INT(0, vfs_init());

    TEST_ASSERT_EQUAL_INT(0, vfs_bdev_register("mock_bdev", &g_flash.bdev));

    TEST_ASSERT_EQUAL_INT(0, vfs_register(FMT_FSTYPE_NORMAL, &fmt_ops));
    TEST_ASSERT_EQUAL_INT(0, vfs_register(FMT_FSTYPE_FAIL, &fmt_fail_ops));
    TEST_ASSERT_EQUAL_INT(0, vfs_register(FMT_FSTYPE_NOFMT, &no_fmt_ops));
}

static void fmt_teardown(void)
{
    vfs_deinit();
    vfs_mock_flash_free(&g_flash);
}

/* ================================================================
 *  Case: 挂载失败后仍可格式化（核心场景：SD 乱数据）
 * ================================================================ */

static void test_format_after_mount_fail(void)
{
    static const vfs_mount_cfg_t mnt[] = {
        {"/bad", FMT_FSTYPE_FAIL, "mock_bdev", 0, NULL},
    };
    /* mount 失败（模拟 SD 乱数据），但配置已登记，vfs_format 仍能按路径解析 */
    TEST_ASSERT_EQUAL_INT(-EIO, vfs_mount_table(mnt, 1));
    TEST_ASSERT_EQUAL_INT(0, vfs_format("/bad"));
    TEST_ASSERT_EQUAL_INT(1, g_format_calls);
    TEST_ASSERT_EQUAL_PTR(&g_flash.bdev, g_format_bdev);
}

/* 卷已挂载 → -EBUSY，format 回调不被调用 */
static void test_format_mounted_returns_ebusy(void)
{
    static const vfs_mount_cfg_t mnt[] = {
        {"/ok", FMT_FSTYPE_NORMAL, "mock_bdev", 0, NULL},
    };
    TEST_ASSERT_EQUAL_INT(0, vfs_mount_table(mnt, 1));
    TEST_ASSERT_EQUAL_INT(-EBUSY, vfs_format("/ok"));
    TEST_ASSERT_EQUAL_INT(0, g_format_calls);
}

/* 后端不支持 format → -ENOTSUP */
static void test_format_unsupported(void)
{
    static const vfs_mount_cfg_t mnt[] = {
        {"/nf", FMT_FSTYPE_NOFMT, "mock_bdev", 0, NULL},
    };
    TEST_ASSERT_EQUAL_INT(0, vfs_mount_table(mnt, 1));
    TEST_ASSERT_EQUAL_INT(-ENOTSUP, vfs_format("/nf"));
    TEST_ASSERT_EQUAL_INT(0, g_format_calls);
}

/* 未知挂载点 → -ENOENT */
static void test_format_unknown_mountpoint(void)
{
    TEST_ASSERT_EQUAL_INT(-ENOENT, vfs_format("/nope"));
}

/* 非绝对路径 → -EINVAL */
static void test_format_relative_path(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, vfs_format("sd"));
}

/* 介质未就绪（无 bdev）→ -ENOENT */
static void test_format_no_bdev(void)
{
    static const vfs_mount_cfg_t mnt[] = {
        {"/nobdev", FMT_FSTYPE_NORMAL, NULL, 0, NULL},
    };
    TEST_ASSERT_EQUAL_INT(0, vfs_mount_table(mnt, 1));
    TEST_ASSERT_EQUAL_INT(-ENOENT, vfs_format("/nobdev"));
    TEST_ASSERT_EQUAL_INT(0, g_format_calls);
}

/* 尾斜杠容忍："/bad/" 与 "/bad" 应解析到同一挂载点 */
static void test_format_trailing_slash(void)
{
    static const vfs_mount_cfg_t mnt[] = {
        {"/bad", FMT_FSTYPE_FAIL, "mock_bdev", 0, NULL},
    };
    TEST_ASSERT_EQUAL_INT(-EIO, vfs_mount_table(mnt, 1));
    TEST_ASSERT_EQUAL_INT(0, vfs_format("/bad/"));
    TEST_ASSERT_EQUAL_INT(1, g_format_calls);
    TEST_ASSERT_EQUAL_PTR(&g_flash.bdev, g_format_bdev);
}

/* ================================================================
 *  Runner
 * ================================================================ */

int vfs_format_run_all_tests(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_format_after_mount_fail);
    RUN_TEST(test_format_mounted_returns_ebusy);
    RUN_TEST(test_format_unsupported);
    RUN_TEST(test_format_unknown_mountpoint);
    RUN_TEST(test_format_relative_path);
    RUN_TEST(test_format_no_bdev);
    RUN_TEST(test_format_trailing_slash);

    return UNITY_END();
}

TEST_SUITE_DEFINE(vfs_format, fmt_setup, fmt_teardown, vfs_format_run_all_tests);
