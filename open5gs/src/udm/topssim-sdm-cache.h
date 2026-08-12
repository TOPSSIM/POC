/*
 * Copyright (C) 2026
 *
 * This file is part of Open5GS.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UDM_TOPSSIM_SDM_CACHE_H
#define UDM_TOPSSIM_SDM_CACHE_H

#include "context.h"

#ifdef __cplusplus
extern "C" {
#endif

bool udm_topssim_sdm_cache_try_send(
        udm_ue_t *udm_ue, ogs_sbi_stream_t *stream,
        ogs_sbi_message_t *recvmsg);

#ifdef __cplusplus
}
#endif

#endif /* UDM_TOPSSIM_SDM_CACHE_H */
