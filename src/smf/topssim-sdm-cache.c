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

bool smf_topssim_sdm_cache_enabled(void)
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

static bool configured_plmn(char *plmn, size_t plmn_len)
{
    const char *allowed = getenv("TOPSSIM_SDM_CF_PLMN");

    ogs_assert(plmn);
    ogs_assert(plmn_len);

    if (!allowed || !strlen(allowed))
        return false;

    ogs_cpystrn(plmn, allowed, plmn_len);
    return true;
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
        smf_sess_t *sess, const char *resource,
        char *path, size_t path_len, char *plmn, size_t plmn_len)
{
    smf_ue_t *smf_ue = NULL;
    char supi[128];
    char safe_resource[128];
    char plmn_buf[OGS_PLMNIDSTRLEN];
    const char *cache_dir = getenv("TOPSSIM_SDM_CACHE_DIR");
    int rv;

    ogs_assert(sess);
    ogs_assert(resource);
    ogs_assert(path);
    ogs_assert(plmn);

    smf_ue = smf_ue_find_by_id(sess->smf_ue_id);
    ogs_assert(smf_ue);
    ogs_assert(smf_ue->supi);

    if (!cache_dir || !strlen(cache_dir))
        cache_dir = TOPSSIM_DEFAULT_SDM_CACHE_DIR;

    if (!configured_plmn(plmn_buf, sizeof(plmn_buf))) {
        ogs_plmn_id_to_string(&sess->serving_plmn_id, plmn_buf);
        if (!plmn_allowed(plmn_buf))
            return false;
    }

    sanitize_component(smf_ue->supi, supi, sizeof(supi));
    sanitize_component(resource, safe_resource, sizeof(safe_resource));

    rv = snprintf(path, path_len, "%s/%s__%s__%s.json",
            cache_dir, supi, safe_resource, plmn_buf);
    if (rv < 0 || rv >= (int)path_len)
        return false;

    ogs_cpystrn(plmn, plmn_buf, plmn_len);
    return true;
}

static bool parse_sm_data(char *body, ogs_sbi_message_t *message)
{
    cJSON *item = NULL;
    cJSON *smsubJSON = NULL;

    ogs_assert(body);
    ogs_assert(message);

    item = cJSON_Parse(body);
    if (!item)
        return false;

    if (!cJSON_IsArray(item)) {
        cJSON_Delete(item);
        return false;
    }

    message->SessionManagementSubscriptionDataList = OpenAPI_list_create();
    ogs_assert(message->SessionManagementSubscriptionDataList);

    cJSON_ArrayForEach(smsubJSON, item) {
        OpenAPI_session_management_subscription_data_t *smsub_item = NULL;

        if (!cJSON_IsObject(smsubJSON)) {
            cJSON_Delete(item);
            return false;
        }

        smsub_item =
            OpenAPI_session_management_subscription_data_parseFromJSON(
                    smsubJSON);
        if (!smsub_item) {
            cJSON_Delete(item);
            return false;
        }
        OpenAPI_list_add(
                message->SessionManagementSubscriptionDataList, smsub_item);
    }

    cJSON_Delete(item);
    return message->SessionManagementSubscriptionDataList->count > 0;
}

bool smf_topssim_sdm_cache_try_handle(
        smf_sess_t *sess, ogs_sbi_stream_t *stream, const char *resource)
{
    bool handled = false;
    char path[OGS_MAX_FILEPATH_LEN];
    char plmn[OGS_PLMNIDSTRLEN];
    char *body = NULL;
    ogs_sbi_message_t message;
    smf_ue_t *smf_ue = NULL;
    ogs_time_t source_started_at;
    ogs_time_t retrieval_started_at;

    ogs_assert(sess);
    ogs_assert(stream);
    ogs_assert(resource);

    source_started_at = ogs_time_now();

    if (!smf_topssim_sdm_cache_enabled())
        return false;

    if (strcmp(resource, OGS_SBI_RESOURCE_NAME_SM_DATA))
        return false;

    smf_ue = smf_ue_find_by_id(sess->smf_ue_id);
    ogs_assert(smf_ue);
    ogs_assert(smf_ue->supi);

    if (!build_cache_path(sess, resource, path, sizeof(path),
                plmn, sizeof(plmn)))
        return false;

    ogs_info("[TOPSSIM][SMF-SDM-CF][SOURCE] SUPI[%s] resource[%s] "
            "plmn[%s] source[sdm-cf] elapsed_ms[%.3f]",
            smf_ue->supi, resource, plmn,
            (double)(ogs_time_now() - source_started_at) / 1000.0);

    retrieval_started_at = ogs_time_now();

    if (!read_text_file(path, &body)) {
        ogs_info("[TOPSSIM][SMF-SDM-CF][MISS] SUPI[%s] resource[%s] "
                "plmn[%s] path[%s] elapsed_ms[%.3f]",
                smf_ue->supi, resource, plmn, path,
                (double)(ogs_time_now() - retrieval_started_at) / 1000.0);
        return false;
    }

    memset(&message, 0, sizeof(message));
    message.res_status = OGS_SBI_HTTP_STATUS_OK;
    message.h.method = (char *)OGS_SBI_HTTP_METHOD_GET;
    message.h.service.name = (char *)OGS_SBI_SERVICE_NAME_NUDM_SDM;
    message.h.api.version = (char *)OGS_SBI_API_V2;
    message.h.resource.component[0] = smf_ue->supi;
    message.h.resource.component[1] = (char *)resource;

    if (!parse_sm_data(body, &message)) {
        ogs_warn("[TOPSSIM][SMF-SDM-CF][MISS] SUPI[%s] resource[%s] "
                "plmn[%s] invalid_json path[%s] elapsed_ms[%.3f]",
                smf_ue->supi, resource, plmn, path,
                (double)(ogs_time_now() - retrieval_started_at) / 1000.0);
        ogs_free(body);
        ogs_sbi_message_free(&message);
        return false;
    }

    ogs_free(body);

    handled = smf_nudm_sdm_handle_get(sess, stream, &message);
    ogs_sbi_message_free(&message);

    if (!handled) {
        ogs_error("[TOPSSIM][SMF-SDM-CF] local handler failed SUPI[%s] "
                "resource[%s] elapsed_ms[%.3f]",
                smf_ue->supi, resource,
                (double)(ogs_time_now() - retrieval_started_at) / 1000.0);
        return false;
    }

    ogs_info("[TOPSSIM][SMF-SDM-CF][HIT] SUPI[%s] resource[%s] plmn[%s] "
            "path[%s] elapsed_ms[%.3f]",
            smf_ue->supi, resource, plmn, path,
            (double)(ogs_time_now() - retrieval_started_at) / 1000.0);

    return true;
}
