#include <fal.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/device.h>
#include <string.h>

#define RP2350_FLASH_DEV_NAME   "rp2350_flash"
#define RP2350_FLASH_BASE       0x10000000u
#define RP2350_FLASH_SIZE       (4 * 1024 * 1024)
#define RP2350_FLASH_BLK_SIZE   4096u

static const struct device *get_flash_dev(void)
{
    return DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_flash_controller));
}

static int rp2350_flash_read(long offset, uint8_t *buf, size_t size)
{
    const struct device *dev = get_flash_dev();
    if (!dev) return -1;
    return flash_read(dev, (off_t)offset, buf, size);
}

static int rp2350_flash_write(long offset, const uint8_t *buf, size_t size)
{
    const struct device *dev = get_flash_dev();
    if (!dev) return -1;
    return flash_write(dev, (off_t)offset, buf, size);
}

static int rp2350_flash_erase(long offset, size_t size)
{
    const struct device *dev = get_flash_dev();
    if (!dev) return -1;
    return flash_erase(dev, (off_t)offset, size);
}

const struct fal_flash_dev rp2350_onchip_flash = {
    .name       = RP2350_FLASH_DEV_NAME,
    .addr       = RP2350_FLASH_BASE,
    .len        = RP2350_FLASH_SIZE,
    .blk_size   = RP2350_FLASH_BLK_SIZE,
    .ops        = { NULL, rp2350_flash_read, rp2350_flash_write, rp2350_flash_erase },
    .write_gran = 1,
};
