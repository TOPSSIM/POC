/*
 * Copyright (C) 2026
 *
 * This file is part of Open5GS.
 */

#ifndef AMF_TOPSSIM_SDM_CACHE_H
#define AMF_TOPSSIM_SDM_CACHE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "context.h"

bool amf_topssim_sdm_cache_enabled(void);
bool amf_topssim_sdm_cache_try_handle(
        amf_ue_t *amf_ue, int state, const char *resource);

#ifdef __cplusplus
}
#endif

#endif /* AMF_TOPSSIM_SDM_CACHE_H */
