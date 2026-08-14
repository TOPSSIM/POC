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

#include "topssim-sdm-cache.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static bool topssim_env_enabled(const char *name)
{
    const char *value = getenv(name);

    if (!value || !*value)
        return false;

    return !strcmp(value, "1") ||
        !ogs_strcasecmp(value, "true") ||
        !ogs_strcasecmp(value, "yes") ||
        !ogs_strcasecmp(value, "on");
}

static const char *topssim_cache_dir(void)
{
    const char *dir = getenv("TOPSSIM_SDM_CACHE_DIR");

    if (dir && *dir)
        return dir;

    return "build/topssim/sdm-cache";
}

static bool topssim_sdm_cache_enabled(void)
{
    return topssim_env_enabled("TOPSSIM_SDM_CF");
}

static bool topssim_sdm_cache_plmn_allowed(const char *plmn)
{
    const char *allowed_plmn = getenv("TOPSSIM_SDM_CF_PLMN");

    if (!allowed_plmn || !*allowed_plmn)
        return true;

    if (!plmn || !*plmn)
        return false;

    return !strcmp(plmn, allowed_plmn);
}

static bool topssim_sdm_cache_resource_allowed(const char *resource)
{
    if (!resource)
        return false;

    return !strcmp(resource, OGS_SBI_RESOURCE_NAME_AM_DATA) ||
        !strcmp(resource, OGS_SBI_RESOURCE_NAME_SMF_SELECT_DATA) ||
        !strcmp(resource, OGS_SBI_RESOURCE_NAME_SM_DATA) ||
        !strcmp(resource, OGS_SBI_RESOURCE_NAME_UE_CONTEXT_IN_SMF_DATA);
}

static void topssim_sanitize_component(
        const char *in, char *out, size_t out_size)
{
    size_t i, pos = 0;

    ogs_assert(out);
    ogs_assert(out_size > 0);

    if (!in)
        in = "";

    for (i = 0; in[i] && pos + 1 < out_size; i++) {
        unsigned char ch = (unsigned char)in[i];

        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.')
            out[pos++] = ch;
        else
            out[pos++] = '_';
    }

    out[pos] = '\0';
}

static void topssim_plmn_string(
        ogs_sbi_message_t *recvmsg, char *out, size_t out_size)
{
    char buf[OGS_PLMNIDSTRLEN];

    ogs_assert(out);
    ogs_assert(out_size > 0);

    out[0] = '\0';

    if (!recvmsg || !recvmsg->param.plmn_id_presence)
        return;

    ogs_cpystrn(out,
            ogs_plmn_id_to_string(&recvmsg->param.plmn_id, buf), out_size);
}

static char *topssim_cache_path_for(
        const char *supi, const char *resource, const char *plmn)
{
    char safe_supi[128];
    char safe_resource[64];
    char safe_plmn[32];
    const char *dir = topssim_cache_dir();

    topssim_sanitize_component(supi, safe_supi, sizeof(safe_supi));
    topssim_sanitize_component(resource, safe_resource, sizeof(safe_resource));
    topssim_sanitize_component(plmn, safe_plmn, sizeof(safe_plmn));

    if (safe_plmn[0])
        return ogs_msprintf("%s/%s__%s__%s.json",
                dir, safe_supi, safe_resource, safe_plmn);

    return ogs_msprintf("%s/%s__%s.json", dir, safe_supi, safe_resource);
}

static bool topssim_cache_expired(const char *path)
{
    char expires_path[OGS_MAX_FILEPATH_LEN];
    FILE *fp = NULL;
    long long expires_at_ms = 0;
    long long now_ms = (long long)time(NULL) * 1000;
    int rv;

    ogs_assert(path);

    rv = ogs_snprintf(expires_path, sizeof(expires_path), "%s.expires", path);
    if (rv < 0 || rv >= (int)sizeof(expires_path))
        return true;

    fp = fopen(expires_path, "r");
    if (!fp)
        return false;

    rv = fscanf(fp, "%lld", &expires_at_ms);
    fclose(fp);

    if (rv != 1 || expires_at_ms <= 0)
        return true;

    return expires_at_ms <= now_ms;
}

static char *topssim_read_file(const char *path, size_t *length)
{
    FILE *fp = NULL;
    long size;
    char *content = NULL;
    size_t nread;

    ogs_assert(path);
    ogs_assert(length);

    *length = 0;

    fp = fopen(path, "rb");
    if (!fp)
        return NULL;

    if (fseek(fp, 0, SEEK_END) != 0)
        goto error;
    size = ftell(fp);
    if (size < 0)
        goto error;
    if (fseek(fp, 0, SEEK_SET) != 0)
        goto error;

    content = ogs_malloc((size_t)size + 1);
    ogs_assert(content);

    nread = fread(content, 1, (size_t)size, fp);
    if (nread != (size_t)size)
        goto error;

    content[size] = '\0';
    *length = (size_t)size;
    fclose(fp);

    return content;

error:
    if (content)
        ogs_free(content);
    fclose(fp);
    return NULL;
}

static bool topssim_send_raw_json(
        ogs_sbi_stream_t *stream, const char *content, size_t content_length)
{
    ogs_sbi_response_t *response = NULL;

    ogs_assert(stream);
    ogs_assert(content);

    response = ogs_sbi_response_new();
    if (!response) {
        ogs_error("ogs_sbi_response_new() failed");
        return false;
    }

    response->status = OGS_SBI_HTTP_STATUS_OK;
    response->http.content = ogs_strndup(content, content_length);
    ogs_assert(response->http.content);
    response->http.content_length = content_length;
    ogs_sbi_header_set(
            response->http.headers, OGS_SBI_CONTENT_TYPE,
            OGS_SBI_CONTENT_JSON_TYPE);

    return ogs_sbi_server_send_response(stream, response);
}

static double topssim_elapsed_ms(ogs_time_t start)
{
    return (double)(ogs_get_monotonic_time() - start) / 1000.0;
}

bool udm_topssim_sdm_cache_try_send(
        udm_ue_t *udm_ue, ogs_sbi_stream_t *stream,
        ogs_sbi_message_t *recvmsg)
{
    const char *resource = NULL;
    char plmn[OGS_PLMNIDSTRLEN];
    char *path = NULL;
    char *fallback_path = NULL;
    char *content = NULL;
    size_t content_length = 0;
    bool sent = false;
    ogs_time_t start = ogs_get_monotonic_time();

    ogs_assert(udm_ue);
    ogs_assert(stream);
    ogs_assert(recvmsg);

    if (!topssim_sdm_cache_enabled())
        return false;

    resource = recvmsg->h.resource.component[1];
    if (!topssim_sdm_cache_resource_allowed(resource))
        return false;

    topssim_plmn_string(recvmsg, plmn, sizeof(plmn));
    if (!topssim_sdm_cache_plmn_allowed(plmn))
        return false;

    path = topssim_cache_path_for(udm_ue->supi, resource, plmn);
    ogs_assert(path);

    if (access(path, R_OK) != 0 || topssim_cache_expired(path)) {
        if (plmn[0]) {
            fallback_path = topssim_cache_path_for(udm_ue->supi, resource, "");
            ogs_assert(fallback_path);
            if (access(fallback_path, R_OK) == 0 &&
                    topssim_cache_expired(fallback_path) == false) {
                ogs_free(path);
                path = fallback_path;
                fallback_path = NULL;
                goto read_cache;
            }
        }
        ogs_info("[TOPSSIM][SDM-CF][MISS] SUPI[%s] resource[%s] plmn[%s] "
                "path[%s] errno[%d] elapsed_ms[%.3f]",
                udm_ue->supi ? udm_ue->supi : "(null)",
                resource ? resource : "(null)",
                plmn[0] ? plmn : "-",
                path, errno, topssim_elapsed_ms(start));
        goto cleanup;
    }

read_cache:
    content = topssim_read_file(path, &content_length);
    if (!content || content_length == 0) {
        ogs_warn("[TOPSSIM][SDM-CF][MISS] SUPI[%s] resource[%s] "
                "empty-cache[%s] elapsed_ms[%.3f]",
                udm_ue->supi ? udm_ue->supi : "(null)",
                resource ? resource : "(null)", path, topssim_elapsed_ms(start));
        goto cleanup;
    }

    sent = topssim_send_raw_json(stream, content, content_length);
    ogs_info("[TOPSSIM][SDM-CF][HIT] SUPI[%s] resource[%s] plmn[%s] "
            "bytes[%d] path[%s] elapsed_ms[%.3f]",
            udm_ue->supi ? udm_ue->supi : "(null)",
            resource ? resource : "(null)",
            plmn[0] ? plmn : "-",
            (int)content_length, path, topssim_elapsed_ms(start));

cleanup:
    if (content)
        ogs_free(content);
    if (fallback_path)
        ogs_free(fallback_path);
    ogs_free(path);

    return sent;
}
