/*
 * Copyright (c) 2025 Huawei Technologies Co., Ltd. All rights reserved.
 * libobmm is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 *
 * See the Mulan PSL v2 for more details.
 *
 * Description: libobmm main api
 * Author: Gao Chao
 * Create: 2025-10-28
 */

#ifndef _VENDOR_ADAPTOR_H
#define _VENDOR_ADAPTOR_H

#include <libobmm.h>

int vendor_adapt_export(struct obmm_mem_desc *desc, const void **vendor_info,
            uint16_t *vendor_len, int *numa);
void free_vendor_info(void *vendor_info);

int vendor_fixup_import_cmd(struct obmm_cmd_import *cmd);
void vendor_cleanup_import_cmd(struct obmm_cmd_import *cmd);

int vendor_fixup_preimport_cmd(struct obmm_cmd_preimport *cmd);
void vendor_cleanup_preimport_cmd(struct obmm_cmd_preimport *cmd);

#endif