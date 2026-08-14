/*
 * Copyright (C) 2026
 *
 * This file is part of Open5GS.
 */

#include "topssim-sdm-cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nudm-handler.h"

#define TOPSSIM_DEFAULT_SDM_CACHE_DIR "build/topssim/sdm-cache"

static bool truthy_env(const char *name)
{
    const char *value = getenv(name);

    if (!value || !strlen(value))
        return false;

    return !strcmp(value, "1") ||
        !ogs_strcasecmp(value, "true") ||
        !ogs_strcasecmp(value, "yes") ||
        !ogs_strcasecmp(value, "on");
}

bool amf_topssim_sdm_cache_enabled(void)
{
    return truthy_env("TOPSSIM_SDM_CF");
}

static bool plmn_allowed(const char *plmn)
{
    const char *allowed = getenv("TOPSSIM_SDM_CF_PLMN");

    if (!allowed || !strlen(allowed))
        return true;

    return plmn && !strcmp(allowed, plmn);
}

static void sanitize_component(const char *src, char *dst, size_t dst_len)
{
    size_t i;

    ogs_assert(src);
    ogs_assert(dst);
    ogs_assert(dst_len);

    for (i = 0; src[i] && i + 1 < dst_len; i++) {
        char ch = src[i];
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.')
            dst[i] = ch;
        else
            dst[i] = '_';
    }
    dst[i] = '\0';
}

static bool read_text_file(const char *path, char **out)
{
    FILE *file = NULL;
    long size;
    size_t read_size;
    char *buf = NULL;

    ogs_assert(path);
    ogs_assert(out);

    file = fopen(path, "rb");
    if (!file)
        return false;

    if (fseek(file, 0, SEEK_END) != 0)
        goto fail;
    size = ftell(file);
    if (size < 0)
        goto fail;
    if (fseek(file, 0, SEEK_SET) != 0)
        goto fail;

    buf = ogs_malloc((size_t)size + 1);
    ogs_assert(buf);

    read_size = fread(buf, 1, (size_t)size, file);
    if (read_size != (size_t)size)
        goto fail;
    buf[size] = '\0';

    fclose(file);
    *out = buf;
    return true;

fail:
    if (file)
        fclose(file);
    if (buf)
        ogs_free(buf);
    return false;
}

static bool build_cache_path(
        amf_ue_t *amf_ue, const char *resource,
        char *path, size_t path_len, char *plmn, size_t plmn_len)
{
    char supi[128];
    char safe_resource[128];
    char plmn_buf[OGS_PLMNIDSTRLEN];
    const char *cache_dir = getenv("TOPSSIM_SDM_CACHE_DIR");
    int rv;

    ogs_assert(amf_ue);
    ogs_assert(amf_ue->supi);
    ogs_assert(resource);
    ogs_assert(path);
    ogs_assert(plmn);

    if (!cache_dir || !strlen(cache_dir))
        cache_dir = TOPSSIM_DEFAULT_SDM_CACHE_DIR;

    ogs_plmn_id_to_string(&amf_ue->nr_tai.plmn_id, plmn_buf);
    if (!plmn_allowed(plmn_buf))
        return false;

    sanitize_component(amf_ue->supi, supi, sizeof(supi));
    sanitize_component(resource, safe_resource, sizeof(safe_resource));

    rv = snprintf(path, path_len, "%s/%s__%s__%s.json",
            cache_dir, supi, safe_resource, plmn_buf);
    if (rv < 0 || rv >= (int)path_len)
        return false;

    ogs_cpystrn(plmn, plmn_buf, plmn_len);
    return true;
}

static void free_cached_message(ogs_sbi_message_t *message)
{
    ogs_assert(message);

    if (message->AccessAndMobilitySubscriptionData)
        OpenAPI_access_and_mobility_subscription_data_free(
                message->AccessAndMobilitySubscriptionData);
    if (message->SmfSelectionSubscriptionData)
        OpenAPI_smf_selection_subscription_data_free(
                message->SmfSelectionSubscriptionData);
    if (message->UeContextInSmfData)
        OpenAPI_ue_context_in_smf_data_free(message->UeContextInSmfData);
}

bool amf_topssim_sdm_cache_try_handle(
        amf_ue_t *amf_ue, int state, const char *resource)
{
    int rv;
    char path[OGS_MAX_FILEPATH_LEN];
    char plmn[OGS_PLMNIDSTRLEN];
    char *body = NULL;
    cJSON *item = NULL;
    ogs_sbi_message_t message;
    ogs_time_t source_started_at;
    ogs_time_t retrieval_started_at;

    ogs_assert(amf_ue);
    ogs_assert(resource);

    source_started_at = ogs_time_now();

    if (!amf_topssim_sdm_cache_enabled())
        return false;

    if (!build_cache_path(amf_ue, resource, path, sizeof(path),
                plmn, sizeof(plmn)))
        return false;

    ogs_info("[TOPSSIM][AMF-SDM-CF][SOURCE] SUPI[%s] resource[%s] "
            "plmn[%s] source[sdm-cf] elapsed_ms[%.3f]",
            amf_ue->supi, resource, plmn,
            (double)(ogs_time_now() - source_started_at) / 1000.0);

    retrieval_started_at = ogs_time_now();

    if (!read_text_file(path, &body)) {
        ogs_info("[TOPSSIM][AMF-SDM-CF][MISS] SUPI[%s] resource[%s] "
                "plmn[%s] path[%s] elapsed_ms[%.3f]",
                amf_ue->supi, resource, plmn, path,
                (double)(ogs_time_now() - retrieval_started_at) / 1000.0);
        return false;
    }

    item = cJSON_Parse(body);
    if (!item) {
        ogs_warn("[TOPSSIM][AMF-SDM-CF][MISS] SUPI[%s] resource[%s] "
                "plmn[%s] invalid_json path[%s] elapsed_ms[%.3f]",
                amf_ue->supi, resource, plmn, path,
                (double)(ogs_time_now() - retrieval_started_at) / 1000.0);
        ogs_free(body);
        return false;
    }

    memset(&message, 0, sizeof(message));
    message.res_status = OGS_SBI_HTTP_STATUS_OK;
    message.h.method = (char *)OGS_SBI_HTTP_METHOD_GET;
    message.h.service.name = (char *)OGS_SBI_SERVICE_NAME_NUDM_SDM;
    message.h.api.version = (char *)OGS_SBI_API_V2;
    message.h.resource.component[0] = amf_ue->supi;
    message.h.resource.component[1] = (char *)resource;

    if (!strcmp(resource, OGS_SBI_RESOURCE_NAME_AM_DATA)) {
        message.AccessAndMobilitySubscriptionData =
            OpenAPI_access_and_mobility_subscription_data_parseFromJSON(item);
    } else if (!strcmp(resource, OGS_SBI_RESOURCE_NAME_SMF_SELECT_DATA)) {
        message.SmfSelectionSubscriptionData =
            OpenAPI_smf_selection_subscription_data_parseFromJSON(item);
    } else if (!strcmp(resource, OGS_SBI_RESOURCE_NAME_UE_CONTEXT_IN_SMF_DATA)) {
        message.UeContextInSmfData =
            OpenAPI_ue_context_in_smf_data_parseFromJSON(item);
    } else {
        ogs_warn("[TOPSSIM][AMF-SDM-CF][MISS] SUPI[%s] unsupported "
                "resource[%s] elapsed_ms[%.3f]",
                amf_ue->supi, resource,
                (double)(ogs_time_now() - retrieval_started_at) / 1000.0);
        cJSON_Delete(item);
        ogs_free(body);
        return false;
    }

    cJSON_Delete(item);
    ogs_free(body);

    rv = amf_nudm_sdm_handle_provisioned(amf_ue, state, &message);
    free_cached_message(&message);

    if (rv != OGS_OK) {
        ogs_error("[TOPSSIM][AMF-SDM-CF] local handler failed SUPI[%s] "
                "resource[%s] elapsed_ms[%.3f]",
                amf_ue->supi, resource,
                (double)(ogs_time_now() - retrieval_started_at) / 1000.0);
        return false;
    }

    ogs_info("[TOPSSIM][AMF-SDM-CF][HIT] SUPI[%s] resource[%s] plmn[%s] "
            "path[%s] elapsed_ms[%.3f]",
            amf_ue->supi, resource, plmn, path,
            (double)(ogs_time_now() - retrieval_started_at) / 1000.0);

    return true;
}
