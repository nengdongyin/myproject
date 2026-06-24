/**
 * @file mkv.c
 * @brief 极简 KV 存储库实现 — 基于 NOR Flash 的追加写键值存储
 *
 * 双 sector 轮转，单调递增 seq 标记活跃 sector。
 * 追加写 + 顺序扫读 + 按需 GC。
 * 掉电安全: 追加写天然安全，GC 期间双 sector 保证至少一个完整。
 */

#include "mkv.h"
#include "fal.h"
#include <string.h>

/* ═══════════════════════════════════════════════ CRC16 (CCITT-FALSE) ═══ */

#if MKV_CRC16_TABLE
static const uint16_t crc16_table[256] = {
    0x0000,0x1021,0x2042,0x3063,0x4084,0x50A5,0x60C6,0x70E7,
    0x8108,0x9129,0xA14A,0xB16B,0xC18C,0xD1AD,0xE1CE,0xF1EF,
    0x1231,0x0210,0x3273,0x2252,0x52B5,0x4294,0x72F7,0x62D6,
    0x9339,0x8318,0xB37B,0xA35A,0xD3BD,0xC39C,0xF3FF,0xE3DE,
    0x2462,0x3443,0x0420,0x1401,0x64E6,0x74C7,0x44A4,0x5485,
    0xA56A,0xB54B,0x8528,0x9509,0xE5EE,0xF5CF,0xC5AC,0xD58D,
    0x3653,0x2672,0x1611,0x0630,0x76D7,0x66F6,0x5695,0x46B4,
    0xB75B,0xA77A,0x9719,0x8738,0xF7DF,0xE7FE,0xD79D,0xC7BC,
    0x48C4,0x58E5,0x6886,0x78A7,0x0840,0x1861,0x2802,0x3823,
    0xC9CC,0xD9ED,0xE98E,0xF9AF,0x8948,0x9969,0xA90A,0xB92B,
    0x5AF5,0x4AD4,0x7AB7,0x6A96,0x1A71,0x0A50,0x3A33,0x2A12,
    0xDBFD,0xCBDC,0xFBBF,0xEB9E,0x9B79,0x8B58,0xBB3B,0xAB1A,
    0x6CA6,0x7C87,0x4CE4,0x5CC5,0x2C22,0x3C03,0x0C60,0x1C41,
    0xEDAE,0xFD8F,0xCDEC,0xDDCD,0xAD2A,0xBD0B,0x8D68,0x9D49,
    0x7E97,0x6EB6,0x5ED5,0x4EF4,0x3E13,0x2E32,0x1E51,0x0E70,
    0xFF9F,0xEFBE,0xDFDD,0xCFFC,0xBF1B,0xAF3A,0x9F59,0x8F78,
    0x9188,0x81A9,0xB1CA,0xA1EB,0xD10C,0xC12D,0xF14E,0xE16F,
    0x1080,0x00A1,0x30C2,0x20E3,0x5004,0x4025,0x7046,0x6067,
    0x83B9,0x9398,0xA3FB,0xB3DA,0xC33D,0xD31C,0xE37F,0xF35E,
    0x02B1,0x1290,0x22F3,0x32D2,0x4235,0x5214,0x6277,0x7256,
    0xB5EA,0xA5CB,0x95A8,0x8589,0xF56E,0xE54F,0xD52C,0xC50D,
    0x34E2,0x24C3,0x14A0,0x0481,0x7466,0x6447,0x5424,0x4405,
    0xA7DB,0xB7FA,0x8799,0x97B8,0xE75F,0xF77E,0xC71D,0xD73C,
    0x26D3,0x36F2,0x0691,0x16B0,0x6657,0x7676,0x4615,0x5634,
    0xD94C,0xC96D,0xF90E,0xE92F,0x99C8,0x89E9,0xB98A,0xA9AB,
    0x5844,0x4865,0x7806,0x6827,0x18C0,0x08E1,0x3882,0x28A3,
    0xCB7D,0xDB5C,0xEB3F,0xFB1E,0x8BF9,0x9BD8,0xABBB,0xBB9A,
    0x4A75,0x5A54,0x6A37,0x7A16,0x0AF1,0x1AD0,0x2AB3,0x3A92,
    0xFD2E,0xED0F,0xDD6C,0xCD4D,0xBDAA,0xAD8B,0x9DE8,0x8DC9,
    0x7C26,0x6C07,0x5C64,0x4C45,0x3CA2,0x2C83,0x1CE0,0x0CC1,
    0xEF1F,0xFF3E,0xCF5D,0xDF7C,0xAF9B,0xBFBA,0x8FD9,0x9FF8,
    0x6E17,0x7E36,0x4E55,0x5E74,0x2E93,0x3EB2,0x0ED1,0x1EF0,
};

static uint16_t crc16_update(uint16_t crc, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        crc = (uint16_t)((crc << 8) ^ crc16_table[((crc >> 8) ^ data[i]) & 0xFF]);
    return crc;
}
#else  /* !MKV_CRC16_TABLE: 运行时计算 */
static uint16_t crc16_update(uint16_t crc, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint16_t)(data[i]) << 8;
        for (int j = 8; j > 0; j--)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}
#endif /* MKV_CRC16_TABLE */

/* ═══════════════════════════════════════════════ 内部辅助 ═══════════ */

static uint32_t rec_total(uint16_t vlen) {
    if (vlen == MKV_TOMBSTONE) return MKV_REC_OVERHEAD; /* 墓碑无 value */
    return MKV_REC_OVERHEAD + vlen;
}
static void w16(uint8_t *p, uint16_t v)  { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static uint16_t r16(const uint8_t *p)    { return (uint16_t)(p[0]|((uint16_t)p[1]<<8)); }
static void w32(uint8_t *p, uint32_t v)  { p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24); }
static uint32_t r32(const uint8_t *p)    { return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }

/* 读 sector header，验证 magic + CRC */
static bool read_sec_hdr(mkv_t *kv, int sec_id, uint32_t *seq)
{
    uint8_t b[MKV_SEC_HDR_SIZE];
    if (fal_partition_read(kv->part, (uint32_t)sec_id * kv->sector_size, b, 8) < 0)
        return false;
    if (r16(b) != MKV_SEC_MAGIC) return false;
    if (r16(b+6) != crc16_update(0, b+2, 4)) return false;
    *seq = r32(b+2);
    return true;
}

/* 写 sector header */
static int write_sec_hdr(mkv_t *kv, int sec_id, uint32_t seq)
{
    uint8_t b[8];
    w16(b, MKV_SEC_MAGIC); w32(b+2, seq); w16(b+6, crc16_update(0, b+2, 4));
    return fal_partition_write(kv->part, (uint32_t)sec_id * kv->sector_size, b, 8);
}

/**
 * @brief 从 flash 读取并验证一条记录
 * @param base    sector 基地址
 * @param off     记录偏移
 * @param out_id  输出 param_id
 * @param out_vlen 输出 value 长度
 * @return true 记录完整且 CRC 正确
 *
 * 记录布局: [magic:2][id:4][vlen:2][value:N][crc16:2]
 * CRC 覆盖: [id:4][vlen:2][value:N]
 */
static bool read_and_verify(mkv_t *kv, uint32_t base, uint32_t off,
                            uint32_t *out_id, uint16_t *out_vlen)
{
    uint8_t head[MKV_REC_HEAD_SIZE];
    if (fal_partition_read(kv->part, base + off, head, 8) < 0)
        return false;
    if (r16(head) != MKV_REC_MAGIC)
        return false;

    uint32_t id   = r32(head+2);
    uint16_t vlen = r16(head+6);
    bool is_tomb  = (vlen == MKV_TOMBSTONE);
    uint16_t crc  = crc16_update(0, head+2, 6); /* id + vlen */

    /* 读 value 并继续算 CRC（墓碑无 value） */
    if (!is_tomb && vlen > 0)
    {
        uint8_t buf[64];
        uint32_t remain = vlen;
        uint32_t voff   = base + off + MKV_REC_HEAD_SIZE;
        while (remain > 0)
        {
            uint32_t n = remain > 64 ? 64 : remain;
            if (fal_partition_read(kv->part, voff, buf, n) < 0)
                return false;
            crc = crc16_update(crc, buf, n);
            voff   += n;
            remain -= n;
        }
    }

    /* CRC 位置: 墓碑时紧接 head，否则在 value 之后 */
    uint32_t crc_off = base + off + MKV_REC_HEAD_SIZE + (is_tomb ? 0 : vlen);
    uint8_t crc_buf[2];
    if (fal_partition_read(kv->part, crc_off, crc_buf, 2) < 0)
        return false;
    if (crc != r16(crc_buf))
        return false;

    *out_id   = id;
    *out_vlen = vlen;
    return true;
}

/* 写一条完整记录到 flash
 * 布局: [magic:2][id:4][vlen:2][value:N][crc16:2] */
static int write_rec(mkv_t *kv, uint32_t id, const uint8_t *data, uint16_t vlen,
                     uint32_t base, uint32_t off)
{
    uint8_t head[MKV_REC_HEAD_SIZE];
    w16(head, MKV_REC_MAGIC);
    w32(head+2, id);
    w16(head+6, vlen);

    if (fal_partition_write(kv->part, base + off, head, MKV_REC_HEAD_SIZE) < 0)
        return -1;

    /* CRC: id(4B) + vlen(2B) + value(N) */
    uint16_t crc = crc16_update(0, head+2, 6);
    crc = crc16_update(crc, data, vlen);

    /* 写 value */
    if (vlen > 0)
    {
        if (fal_partition_write(kv->part, base + off + MKV_REC_HEAD_SIZE,
                                data, vlen) < 0)
            return -1;
    }

    /* 写 CRC（value 之后 2 字节） */
    uint8_t crc_buf[2];
    w16(crc_buf, crc);
    if (fal_partition_write(kv->part,
                            base + off + MKV_REC_HEAD_SIZE + vlen,
                            crc_buf, 2) < 0)
        return -1;

    return 0;
}

/* ═══════════════════════════════════════════════ 扫描辅助 ═══════ */

/**
 * @brief 探测 off 位置是否为擦除区（连续 16 字节全 0xFF）
 *
 * NOR Flash 擦除后全为 0xFF。16 字节全 0xFF 判定为已到达扇区末尾，
 * 外部扫描循环可安全终止。误判概率 < 2^-128。
 */
static bool mkv_probe_erased(mkv_t *kv, uint32_t base, uint32_t off)
{
    uint8_t buf[16];
    if (fal_partition_read(kv->part, base + off, buf, 16) < 0)
        return false;
    for (int i = 0; i < 16; i++)
        if (buf[i] != 0xFF) return false;
    return true;
}

/**
 * @brief 尝试按 header 的 vlen 跳过半写记录
 *
 * 追加写下掉电可能产生半写记录（header 完整但 CRC 错误）。
 * 若 header magic 匹配则按 vlen 返回跳跃步长；
 * header 也损坏时返回 1，外循环逐字节寻找下一条记录或擦除区。
 *
 * @return 外循环应前进的字节数（至少 1）
 */
static uint32_t mkv_try_skip(mkv_t *kv, uint32_t base, uint32_t off)
{
    uint8_t head[MKV_REC_HEAD_SIZE];
    if (fal_partition_read(kv->part, base + off, head, 8) >= 0
        && r16(head) == MKV_REC_MAGIC)
    {
        uint16_t v = r16(head + 6);
        uint32_t total = (v == MKV_TOMBSTONE)
            ? MKV_REC_OVERHEAD
            : MKV_REC_OVERHEAD + v;
        if (off + total <= kv->sector_size) return total;
    }
    return 1; /* header 损坏，逐字节前进 */
}

/* ═══════════════════════════════════════════════ GC ══════════════ */

#define GC_MAP_MAX 256

static int mkv_gc(mkv_t *kv)
{
    uint32_t old_base = kv->active_base;
    int      old_id   = (int)(old_base / kv->sector_size);
    int      new_id   = (old_id + 1) % (int)kv->sector_count;
    uint32_t new_base = (uint32_t)new_id * kv->sector_size;
    uint32_t new_seq  = kv->active_seq + 1;

    struct { uint32_t id; uint32_t off; } map[GC_MAP_MAX];
    int map_count = 0;

    if (fal_partition_erase(kv->part, new_base, kv->sector_size) < 0)
        return -1;

    /* 扫描旧 sector, 记录每个 id 最后一次出现 */
    uint32_t scan = MKV_SEC_HDR_SIZE;
    while (scan + MKV_REC_OVERHEAD <= kv->sector_size)
    {
        uint32_t rid; uint16_t vlen;
        if (!read_and_verify(kv, old_base, scan, &rid, &vlen))
        {
            if (mkv_probe_erased(kv, old_base, scan)) break;
            scan += mkv_try_skip(kv, old_base, scan);
            continue;
        }

        int found = -1;
        for (int i = 0; i < map_count; i++)
            if (map[i].id == rid) { found = i; break; }
        if (found >= 0)
            map[found].off = scan;
        else if (map_count < GC_MAP_MAX)
            { map[map_count].id=rid; map[map_count].off=scan; map_count++; }

        scan += rec_total(vlen);
    }

    /* 复制有效记录到新 sector */
    uint32_t off = MKV_SEC_HDR_SIZE;
    uint8_t buf[512];
    for (int i = 0; i < map_count; i++)
    {
        uint32_t rid; uint16_t vlen;
        if (!read_and_verify(kv, old_base, map[i].off, &rid, &vlen))
            continue;
        if (vlen == MKV_TOMBSTONE) continue;

        uint32_t total = rec_total(vlen);
        if (off + total > kv->sector_size || total > sizeof(buf))
            continue;

        if (fal_partition_read(kv->part, old_base + map[i].off, buf, total) < 0)
            continue;
        if (fal_partition_write(kv->part, new_base + off, buf, total) < 0)
            continue;
        off += total;
    }

    if (write_sec_hdr(kv, new_id, new_seq) < 0) return -1;

    kv->active_base  = new_base;
    kv->active_seq   = new_seq;
    kv->write_offset = off;

    fal_partition_erase(kv->part, old_base, kv->sector_size);
    return 0;
}

/* ═══════════════════════════════════════════════ API ═════════════ */

int mkv_init(mkv_t *kv, const char *fal_part_name)
{
    if (!kv || !fal_part_name) return -1;
    memset(kv, 0, sizeof(*kv));

    fal_init();
    kv->part = fal_partition_find(fal_part_name);
    if (!kv->part) return -1;

    /* 以物理擦除块 × MKV_SECTOR_MULT 为 sector 大小 */
    {
        const struct fal_flash_dev *flash =
            fal_flash_device_find(kv->part->flash_name);
        uint32_t erase = flash ? (uint32_t)flash->blk_size : 4096;
        kv->sector_size = erase * MKV_SECTOR_MULT;
    }
    if (kv->sector_size < MKV_MIN_SECTOR_SIZE) return -1;

    kv->sector_count = (uint8_t)(kv->part->len / kv->sector_size);
    if (kv->sector_count < 2) return -1;  /* GC 至少需要 2 个 sector */

    /* 扫描全部 sector header，找到 seq 最大的活跃 sector */
    int      best_id = 0;
    uint32_t best_seq = 0;
    bool     any_ok = false;

    for (uint8_t i = 0; i < kv->sector_count; i++)
    {
        uint32_t seq;
        if (read_sec_hdr(kv, i, &seq))
        {
            any_ok = true;
            if (!any_ok || seq >= best_seq)
                { best_id = i; best_seq = seq; }
        }
    }

    if (!any_ok)
    {
        /* 全新分区：擦除全部 sector，从 sector 0 开始 */
        for (uint8_t i = 0; i < kv->sector_count; i++)
            fal_partition_erase(kv->part, (uint32_t)i * kv->sector_size,
                                kv->sector_size);
        write_sec_hdr(kv, 0, 1);
        kv->active_base  = 0;
        kv->active_seq   = 1;
        kv->write_offset = MKV_SEC_HDR_SIZE;
        kv->initialized  = true;
        return 0;
    }

    kv->active_base = (uint32_t)best_id * kv->sector_size;
    kv->active_seq  = best_seq;

    /* 扫描活跃 sector 找到 write_offset */
    uint32_t off = MKV_SEC_HDR_SIZE;
    uint16_t cons_fail = 0;
    while (off + MKV_REC_OVERHEAD <= kv->sector_size)
    {
        uint32_t rid; uint16_t vlen;
        if (!read_and_verify(kv, kv->active_base, off, &rid, &vlen))
        {
            if (mkv_probe_erased(kv, kv->active_base, off)) break;
            uint32_t step = mkv_try_skip(kv, kv->active_base, off);
            if (step == 1 && ++cons_fail > 256)
            {
                /* 扇区内容为脏数据，擦除重建 */
                fal_partition_erase(kv->part, kv->active_base, kv->sector_size);
                write_sec_hdr(kv, best_id, best_seq);
                kv->write_offset = MKV_SEC_HDR_SIZE;
                kv->initialized = true;
                return 0;
            }
            if (step != 1) cons_fail = 0;
            off += step;
            continue;
        }
        cons_fail = 0;
        off += rec_total(vlen);
    }
    kv->write_offset = off;
    kv->initialized = true;
    return 0;
}

int mkv_scan(mkv_t *kv, mkv_scan_cb cb, void *user)
{
    if (!kv || !kv->initialized || !cb) return -1;

    uint32_t off = MKV_SEC_HDR_SIZE;
    uint8_t buf[128];  /* 栈缓冲区: value >128B 时需多次读取 */

    while (off + MKV_REC_OVERHEAD <= kv->sector_size)
    {
        uint32_t rid; uint16_t vlen;
        if (!read_and_verify(kv, kv->active_base, off, &rid, &vlen))
        {
            if (mkv_probe_erased(kv, kv->active_base, off)) break;
            off += mkv_try_skip(kv, kv->active_base, off);
            continue;
        }

        if (vlen == MKV_TOMBSTONE) {
            cb(rid, NULL, 0, user);
        } else if (vlen > 0) {
            /* 分块读取 value 并直接传给回调 — 不做缓存聚合 */
            if (vlen <= sizeof(buf)) {
                fal_partition_read(kv->part,
                                   kv->active_base + off + MKV_REC_HEAD_SIZE,
                                   buf, vlen);
                cb(rid, buf, vlen, user);
            }
        }
        off += rec_total(vlen);
    }
    return 0;
}

int mkv_get(mkv_t *kv, uint32_t id, uint8_t *buf, uint16_t max_len)
{
    if (!kv || !kv->initialized || !buf || max_len==0) return -1;

    bool found = false, deleted = false;
    uint16_t last_vlen = 0;
    uint32_t last_off = 0, off = MKV_SEC_HDR_SIZE;

    while (off + MKV_REC_OVERHEAD <= kv->sector_size)
    {
        uint32_t rid; uint16_t vlen;
        if (!read_and_verify(kv, kv->active_base, off, &rid, &vlen))
        {
            if (mkv_probe_erased(kv, kv->active_base, off)) break;
            off += mkv_try_skip(kv, kv->active_base, off);
            continue;
        }
        if (rid == id)
        {
            found    = true;
            last_off = off;
            last_vlen = vlen;
            deleted  = (vlen == MKV_TOMBSTONE);
        }
        off += rec_total(vlen);
    }

    if (!found || deleted) return 0;
    if (last_vlen > max_len) last_vlen = max_len;
    if (last_vlen == 0) return 0;

    if (fal_partition_read(kv->part,
                           kv->active_base + last_off + MKV_REC_HEAD_SIZE,
                           buf, last_vlen) < 0)
        return -1;
    return (int)last_vlen;
}

int mkv_set(mkv_t *kv, uint32_t id, const uint8_t *data, uint16_t len)
{
    if (!kv || !kv->initialized || !data) return -1;
    if (len == MKV_TOMBSTONE) return -1;

    uint32_t total = rec_total(len);
    if (kv->write_offset + total > kv->sector_size)
    {
        if (mkv_gc(kv) < 0) return -1;
        if (kv->write_offset + total > kv->sector_size) return -1;
    }

    if (write_rec(kv, id, data, len, kv->active_base, kv->write_offset) < 0)
        return -1;

    kv->write_offset += total;
    return 0;
}

int mkv_del(mkv_t *kv, uint32_t id)
{
    if (!kv || !kv->initialized) return -1;

    uint32_t total = rec_total(0);
    if (kv->write_offset + total > kv->sector_size)
    {
        if (mkv_gc(kv) < 0) return -1;
        if (kv->write_offset + total > kv->sector_size) return -1;
    }

    /* 墓碑: vlen=0xFFFF, 无 value。CRC 在 head 之后 */
    uint8_t head[MKV_REC_HEAD_SIZE];
    w16(head, MKV_REC_MAGIC);
    w32(head+2, id);
    w16(head+6, MKV_TOMBSTONE);

    if (fal_partition_write(kv->part, kv->active_base + kv->write_offset,
                            head, MKV_REC_HEAD_SIZE) < 0)
        return -1;

    /* CRC: id(4B) + vlen(2B) */
    uint8_t crc_buf[2];
    w16(crc_buf, crc16_update(0, head+2, 6));
    if (fal_partition_write(kv->part,
                            kv->active_base + kv->write_offset + MKV_REC_HEAD_SIZE,
                            crc_buf, 2) < 0)
        return -1;

    kv->write_offset += total;
    return 0;
}

int mkv_erase_all(mkv_t *kv)
{
    if (!kv || !kv->initialized) return -1;
    for (uint8_t i = 0; i < kv->sector_count; i++)
        fal_partition_erase(kv->part, (uint32_t)i * kv->sector_size,
                            kv->sector_size);
    write_sec_hdr(kv, 0, 1);
    kv->active_base=0; kv->active_seq=1; kv->write_offset=MKV_SEC_HDR_SIZE;
    return 0;
}

void mkv_deinit(mkv_t *kv) { (void)kv; }
