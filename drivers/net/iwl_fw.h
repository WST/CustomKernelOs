#ifndef IWL_FW_H
#define IWL_FW_H
#include "types.h"

/* Файл iwlwifi-1000-5.ucode: с 88-го байта идут TLV, заголовок — два u32. */
struct iwl_slice { const u8 *p; u32 n; };
struct iwl_fw {
    struct iwl_slice inst, data, init, init_data;
};
int iwl_fw_parse(const u8 *buf, u32 len, struct iwl_fw *out);

#endif
