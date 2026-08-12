/*
 * Copyright (C) 2026
 *
 * This file is part of Open5GS.
 */

#ifndef SMF_TOPSSIM_SDM_CACHE_H
#define SMF_TOPSSIM_SDM_CACHE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "context.h"

bool smf_topssim_sdm_cache_enabled(void);
bool smf_topssim_sdm_cache_try_handle(
        smf_sess_t *sess, ogs_sbi_stream_t *stream, const char *resource);

#ifdef __cplusplus
}
#endif

#endif /* SMF_TOPSSIM_SDM_CACHE_H */
