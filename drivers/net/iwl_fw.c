#include "iwl_fw.h"
#include "string.h"

int iwl_fw_parse(const u8 *buf, u32 len, struct iwl_fw *out) {
    if (!buf || !out || len < 96) return -1;
    u32 magic = (u32)buf[4] | ((u32)buf[5] << 8) | ((u32)buf[6] << 16) | ((u32)buf[7] << 24);
    if (magic != 0x0a4c5749u) return -1;
    memset(out, 0, sizeof(*out));
    u32 off = 88;
    while (off + 8 <= len) {
        u32 type = (u32)buf[off] | ((u32)buf[off+1] << 8) | ((u32)buf[off+2] << 16) | ((u32)buf[off+3] << 24);
        u32 n = (u32)buf[off+4] | ((u32)buf[off+5] << 8) | ((u32)buf[off+6] << 16) | ((u32)buf[off+7] << 24);
        off += 8;
        if (!type || n > len - off) return -1;
        struct iwl_slice s = { buf + off, n };
        if (type == 1) out->inst = s;
        else if (type == 2) out->data = s;
        else if (type == 3) out->init = s;
        else if (type == 4) out->init_data = s;
        off += (n + 3u) & ~3u;
    }
    if (!out->inst.n || !out->data.n || !out->init.n || !out->init_data.n) return -1;
    return 0;
}
