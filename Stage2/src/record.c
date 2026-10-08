#include "simplekv.h"

static uint32_t crc_table[256];
static int      crc_ready = 0;

static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
    crc_ready = 1;
}

uint32_t crc32(const void *data, size_t len)
{
    if (!crc_ready)
        crc_init();
    const unsigned char *p = data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void put_u32(unsigned char *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}

uint32_t get_u32(const unsigned char *p)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        v |= (uint32_t)p[i] << (8 * i);
    return v;
}

void put_i64(unsigned char *p, int64_t v)
{
    uint64_t u = (uint64_t)v;
    for (int i = 0; i < 8; i++)
        p[i] = (unsigned char)(u >> (8 * i));
}

int64_t get_i64(const unsigned char *p)
{
    uint64_t u = 0;
    for (int i = 0; i < 8; i++)
        u |= (uint64_t)p[i] << (8 * i);
    return (int64_t)u;
}

size_t record_size(uint32_t key_len, uint32_t value_len)
{
    return RECORD_HEADER + (size_t)key_len + value_len;
}

size_t record_encode(const struct record *r, unsigned char *out)
{
    out[4] = r->type;
    put_u32(out + 5, r->key_len);
    put_u32(out + 9, r->value_len);
    put_i64(out + 13, r->expires_at);
    memcpy(out + RECORD_HEADER, r->key, r->key_len);
    if (r->value_len)
        memcpy(out + RECORD_HEADER + r->key_len, r->value, r->value_len);
    size_t total = record_size(r->key_len, r->value_len);
    put_u32(out, crc32(out + 4, total - 4));
    return total;
}

int record_decode_header(const unsigned char *buf, size_t available, struct record *r, uint32_t *crc)
{
    if (available < RECORD_HEADER)
        return RECORD_SHORT;
    *crc = get_u32(buf);
    r->type = buf[4];
    r->key_len = get_u32(buf + 5);
    r->value_len = get_u32(buf + 9);
    r->expires_at = get_i64(buf + 13);
    if (r->type != RECORD_PUT && r->type != RECORD_DELETE)
        return RECORD_BAD;
    if (r->key_len == 0 || r->key_len > MAX_KEY || r->value_len > MAX_VALUE)
        return RECORD_BAD;
    if (r->type == RECORD_DELETE && r->value_len != 0)
        return RECORD_BAD;
    return RECORD_OK;
}

int record_check(const unsigned char *whole, size_t len)
{
    return len >= RECORD_HEADER && get_u32(whole) == crc32(whole + 4, len - 4);
}
