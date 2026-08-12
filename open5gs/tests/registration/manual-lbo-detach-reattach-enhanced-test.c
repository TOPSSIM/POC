/*
 * Copyright (C) 2026
 *
 * This file is part of Open5GS.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "test-common.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define SCENARIO_NAME                "LBO (full detach + reattach)"

#ifndef TOPSSIM_SOURCE_DIR
#define TOPSSIM_SOURCE_DIR "."
#endif

#ifndef TOPSSIM_BUILD_DIR
#define TOPSSIM_BUILD_DIR "build/topssim"
#endif

#ifndef TOPSSIM_PYTHON
#define TOPSSIM_PYTHON "python3"
#endif

#define HOME_AMF_ADDR                "172.16.85.128"
#define VISITED_AMF_ADDR             "172.16.85.148"
#define HOME_MCC                     1
#define HOME_MNC                     1
#define HOME_MNC_LEN                 2
#define HOME_TAC                     1
#define VISITED_MCC                  1
#define VISITED_MNC                  2
#define VISITED_MNC_LEN              2
#define VISITED_TAC                  1

#define RSTEP(_step, _fmt, ...) \
    abts_log_message("[MANUAL-ROAM][STEP %02d] " _fmt, (_step), ##__VA_ARGS__)

static bson_t *test_db_new_manual_lbo_detach_reattach(test_ue_t *test_ue)
{
    bson_t *doc = NULL;
    bool lbo_allowed = true;

    ogs_assert(test_ue);

    doc = BCON_NEW(
            "imsi", BCON_UTF8(test_ue->imsi),
            "msisdn", "[",
                BCON_UTF8(TEST_MSISDN),
                BCON_UTF8(TEST_ADDITIONAL_MSISDN),
            "]",
            "ambr", "{",
                "downlink", "{",
                    "value", BCON_INT32(1),
                    "unit", BCON_INT32(3),
                "}",
                "uplink", "{",
                    "value", BCON_INT32(1),
                    "unit", BCON_INT32(3),
                "}",
            "}",
            "slice", "[", "{",
                "sst", BCON_INT32(1),
                "default_indicator", BCON_BOOL(true),
                "session", "[", "{",
                    "name", BCON_UTF8("internet"),
                    "type", BCON_INT32(3),
                    "ambr", "{",
                        "downlink", "{",
                            "value", BCON_INT32(1),
                            "unit", BCON_INT32(3),
                        "}",
                        "uplink", "{",
                            "value", BCON_INT32(1),
                            "unit", BCON_INT32(3),
                        "}",
                    "}",
                    "qos", "{",
                        "index", BCON_INT32(9),
                        "arp", "{",
                            "priority_level", BCON_INT32(8),
                            "pre_emption_vulnerability", BCON_INT32(1),
                            "pre_emption_capability", BCON_INT32(1),
                        "}",
                    "}",
                    "lbo_roaming_allowed", BCON_BOOL(lbo_allowed),
                "}", "]",
            "}", "]",
            "security", "{",
                "k", BCON_UTF8(test_ue->k_string),
                "opc", BCON_UTF8(test_ue->opc_string),
                "amf", BCON_UTF8("8000"),
                "sqn", BCON_INT64(64),
            "}",
            "subscribed_rau_tau_timer", BCON_INT32(12),
            "network_access_mode", BCON_INT32(0),
            "subscriber_status", BCON_INT32(0),
            "operator_determined_barring", BCON_INT32(0),
            "access_restriction_data", BCON_INT32(32)
          );
    ogs_assert(doc);

    return doc;
}

static void wait_for_enter(const char *prompt)
{
    const char *manual_wait = getenv("TOPSSIM_MANUAL_WAIT");

    abts_log_message("[MANUAL-ROAM] %s", prompt);
    if (!manual_wait || !strlen(manual_wait) ||
            (!strcmp(manual_wait, "0") ||
             !ogs_strcasecmp(manual_wait, "false") ||
             !ogs_strcasecmp(manual_wait, "no") ||
             !ogs_strcasecmp(manual_wait, "off"))) {
        abts_log_message("[MANUAL-ROAM] Auto-continue without manual wait");
        return;
    }

    if (!isatty(STDIN_FILENO)) {
        abts_log_message("[MANUAL-ROAM] Non-interactive stdin, continue automatically");
        return;
    }

    while (true) {
        int c = getchar();
        if (c == '\n' || c == EOF)
            break;
    }
}

static const char *home_amf_addr(void)
{
    const char *addr = getenv("TOPSSIM_HOME_AMF_ADDR");

    return addr && strlen(addr) ? addr : HOME_AMF_ADDR;
}

static const char *visited_amf_addr(void)
{
    const char *addr = getenv("TOPSSIM_VISITED_AMF_ADDR");

    return addr && strlen(addr) ? addr : VISITED_AMF_ADDR;
}

static bool skip_gtpu_ping(void)
{
    const char *value = getenv("TOPSSIM_SKIP_GTPU_PING");

    if (!value || !strlen(value))
        return false;

    return !strcmp(value, "1") ||
        !ogs_strcasecmp(value, "true") ||
        !ogs_strcasecmp(value, "yes") ||
        !ogs_strcasecmp(value, "on");
}

static bool skip_pdu_session(void)
{
    const char *value = getenv("TOPSSIM_SKIP_PDU_SESSION");

    if (!value || !strlen(value))
        return false;

    return !strcmp(value, "1") ||
        !ogs_strcasecmp(value, "true") ||
        !ogs_strcasecmp(value, "yes") ||
        !ogs_strcasecmp(value, "on");
}

static bool topssim_sdm_cf_enabled(void)
{
    const char *value = getenv("TOPSSIM_SDM_CF");

    if (!value || !strlen(value))
        return false;

    return !strcmp(value, "1") ||
        !ogs_strcasecmp(value, "true") ||
        !ogs_strcasecmp(value, "yes") ||
        !ogs_strcasecmp(value, "on");
}

static const char *visited_db_uri(void)
{
    const char *uri = getenv("TOPSSIM_VISITED_DB_URI");

    return uri && strlen(uri) ? uri : NULL;
}

static int64_t now_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void append_topssim_audit_elapsed(
        const char *event, const char *component, int64_t elapsed_ms)
{
    char path[OGS_MAX_FILEPATH_LEN];
    const char *cache_dir = getenv("TOPSSIM_SDM_CACHE_DIR");
    FILE *file = NULL;
    int rv;

    ogs_assert(event);
    ogs_assert(component);

    if (!cache_dir || !strlen(cache_dir))
        cache_dir = TOPSSIM_BUILD_DIR "/sdm-cache";

    rv = snprintf(path, sizeof(path), "%s/audit.jsonl", cache_dir);
    if (rv < 0 || rv >= (int)sizeof(path))
        return;

    file = fopen(path, "a");
    if (!file)
        return;

    fprintf(file,
            "{\"event\":\"%s\",\"timestamp_ms\":%" PRId64 ","
            "\"payload\":{\"component\":\"%s\",\"elapsed_ms\":%" PRId64 "}}\n",
            event, now_ms(), component, elapsed_ms);
    fclose(file);
}

static int mirror_subscriber_to_uri(
        const char *db_uri, test_ue_t *test_ue, const bson_t *doc)
{
    mongoc_client_t *client = NULL;
    mongoc_collection_t *collection = NULL;
    const mongoc_uri_t *uri = NULL;
    const char *db_name = NULL;
    bson_t *key = NULL;
    bson_t *doc_copy = NULL;
    int64_t count = 0;
    bson_error_t error;
    int rv = OGS_ERROR;

    ogs_assert(db_uri);
    ogs_assert(test_ue);
    ogs_assert(doc);

    client = mongoc_client_new(db_uri);
    if (!client) {
        abts_log_message("[MANUAL-ROAM][DB] Failed to parse visited DB URI");
        return OGS_ERROR;
    }

#if MONGOC_CHECK_VERSION(1, 4, 0)
    mongoc_client_set_error_api(client, 2);
#endif

    uri = mongoc_client_get_uri(client);
    db_name = mongoc_uri_get_database(uri);
    if (!db_name || !strlen(db_name))
        db_name = "open5gs";

    collection = mongoc_client_get_collection(client, db_name, "subscribers");
    if (!collection) {
        abts_log_message("[MANUAL-ROAM][DB] Failed to open visited subscribers collection");
        goto cleanup;
    }

    key = BCON_NEW("imsi", BCON_UTF8(test_ue->imsi));
    ogs_assert(key);

    count = mongoc_collection_count(
            collection, MONGOC_QUERY_NONE, key, 0, 0, NULL, &error);
    if (count > 0 &&
            mongoc_collection_remove(collection,
                MONGOC_REMOVE_SINGLE_REMOVE, key, NULL, &error) != true) {
        abts_log_message("[MANUAL-ROAM][DB] Failed to remove stale visited subscriber IMSI[%s]: %s",
                test_ue->imsi, error.message);
        goto cleanup;
    }

    doc_copy = bson_copy(doc);
    ogs_assert(doc_copy);

    if (mongoc_collection_insert(
                collection, MONGOC_INSERT_NONE, doc_copy, NULL, &error) != true) {
        abts_log_message("[MANUAL-ROAM][DB] Failed to insert visited subscriber IMSI[%s]: %s",
                test_ue->imsi, error.message);
        goto cleanup;
    }

    rv = OGS_OK;
    abts_log_message("[MANUAL-ROAM][DB] Mirrored subscriber into visited DB IMSI[%s]",
            test_ue->imsi);

cleanup:
    if (doc_copy)
        bson_destroy(doc_copy);
    if (key)
        bson_destroy(key);
    if (collection)
        mongoc_collection_destroy(collection);
    if (client)
        mongoc_client_destroy(client);

    return rv;
}

static int remove_subscriber_from_uri(const char *db_uri, test_ue_t *test_ue)
{
    mongoc_client_t *client = NULL;
    mongoc_collection_t *collection = NULL;
    const mongoc_uri_t *uri = NULL;
    const char *db_name = NULL;
    bson_t *key = NULL;
    bson_error_t error;
    int rv = OGS_ERROR;

    ogs_assert(db_uri);
    ogs_assert(test_ue);

    client = mongoc_client_new(db_uri);
    if (!client)
        return OGS_ERROR;

#if MONGOC_CHECK_VERSION(1, 4, 0)
    mongoc_client_set_error_api(client, 2);
#endif

    uri = mongoc_client_get_uri(client);
    db_name = mongoc_uri_get_database(uri);
    if (!db_name || !strlen(db_name))
        db_name = "open5gs";

    collection = mongoc_client_get_collection(client, db_name, "subscribers");
    if (!collection)
        goto cleanup;

    key = BCON_NEW("imsi", BCON_UTF8(test_ue->imsi));
    ogs_assert(key);

    if (mongoc_collection_remove(collection,
                MONGOC_REMOVE_SINGLE_REMOVE, key, NULL, &error) != true) {
        abts_log_message("[MANUAL-ROAM][DB] Failed to remove visited subscriber IMSI[%s]: %s",
                test_ue->imsi, error.message);
        goto cleanup;
    }

    rv = OGS_OK;

cleanup:
    if (key)
        bson_destroy(key);
    if (collection)
        mongoc_collection_destroy(collection);
    if (client)
        mongoc_client_destroy(client);

    return rv;
}

static bool build_visited_nrf_fqdn(char *fqdn, size_t fqdn_len)
{
    int rv;

    ogs_assert(fqdn);

    rv = snprintf(fqdn, fqdn_len,
            "nrf.5gc.mnc%03d.mcc%03d.3gppnetwork.org",
            VISITED_MNC, VISITED_MCC);
    return (rv >= 0 && rv < (int)fqdn_len);
}

static bool build_home_udm_uri(char *uri, size_t uri_len)
{
    int rv;

    ogs_assert(uri);

    rv = snprintf(uri, uri_len,
            "http://udm.5gc.mnc%03d.mcc%03d.3gppnetwork.org",
            HOME_MNC, HOME_MCC);
    return (rv >= 0 && rv < (int)uri_len);
}

static void trigger_visited_pre_discovery(const char *services_csv)
{
    int rv;
    char command[2048];
    char nrf_fqdn[128];
    const char *services = services_csv;

    if (!services || !strlen(services))
        services = "nudm-uecm,nudm-sdm";

    if (!build_visited_nrf_fqdn(nrf_fqdn, sizeof(nrf_fqdn))) {
        abts_log_message("[MANUAL-ROAM][PREP] Failed to build visited NRF FQDN");
        return;
    }

    rv = snprintf(command, sizeof(command),
            "curl --http2-prior-knowledge --fail-with-body -sS -G "
            "--data-urlencode 'target-nf-type=UDM' "
            "--data-urlencode 'requester-nf-type=AMF' "
            "--data-urlencode 'service-names=%s' "
            "--data-urlencode 'target-plmn-list=[{\"mcc\":\"%03d\",\"mnc\":\"%0*d\"}]' "
            "--data-urlencode 'requester-plmn-list=[{\"mcc\":\"%03d\",\"mnc\":\"%0*d\"}]' "
            "--data-urlencode 'pre-discovery=true' "
            "'http://%s/nnrf-disc/v1/nf-instances' "
            "> /dev/null",
            services,
            HOME_MCC, HOME_MNC_LEN, HOME_MNC,
            VISITED_MCC, VISITED_MNC_LEN, VISITED_MNC,
            nrf_fqdn);
    if (rv < 0 || rv >= (int)sizeof(command)) {
        abts_log_message("[MANUAL-ROAM][PREP] Failed to build pre-discovery command");
        return;
    }

    rv = system(command);
    if (rv == 0) {
        abts_log_message("[MANUAL-ROAM][PREP] Triggered visited NRF pre-discovery successfully [fqdn:%s services:%s]",
                nrf_fqdn, services);
    } else {
        abts_log_message("[MANUAL-ROAM][PREP] Pre-discovery trigger failed (curl/system rv=%d) [fqdn:%s services:%s], continue",
                rv, nrf_fqdn, services);
    }
}

static void trigger_sdm_prefetch(const char *supi)
{
    int rv;
    char command[4096];
    char h_udm_uri[192];
    const char *cache_dir = getenv("TOPSSIM_SDM_CACHE_DIR");

    ogs_assert(supi);

    if (!cache_dir || !strlen(cache_dir))
        cache_dir = TOPSSIM_BUILD_DIR "/sdm-cache";

    if (!build_home_udm_uri(h_udm_uri, sizeof(h_udm_uri))) {
        abts_log_message("[MANUAL-ROAM][PREP] Failed to build home UDM URI");
        return;
    }

    rv = snprintf(command, sizeof(command),
            "'%s' '%s/topssim/tools/prefetch_sdm.py' "
            "--supi '%s' "
            "--h-udm '%s' "
            "--vplmn-mcc '%03d' "
            "--vplmn-mnc '%0*d' "
            "--cache-dir '%s' "
            "--resources 'am-data,smf-select-data,ue-context-in-smf-data,sm-data' "
            "--ttl-seconds 1800 "
            "--allow-empty-ue-context",
            TOPSSIM_PYTHON,
            TOPSSIM_SOURCE_DIR,
            supi,
            h_udm_uri,
            VISITED_MCC,
            VISITED_MNC_LEN, VISITED_MNC,
            cache_dir);
    if (rv < 0 || rv >= (int)sizeof(command)) {
        abts_log_message("[MANUAL-ROAM][PREP] Failed to build SDM prefetch command");
        return;
    }

    rv = system(command);
    if (rv == 0) {
        abts_log_message("[MANUAL-ROAM][PREP] Prefetched SDM data into SDM-CF cache [supi:%s cache:%s]",
                supi, cache_dir);
    } else {
        abts_log_message("[MANUAL-ROAM][PREP] SDM prefetch failed (system rv=%d) [supi:%s], continue",
                rv, supi);
    }
}

static void trigger_sdm_cache_lifecycle(const char *supi, const char *action)
{
    int rv;
    char command[4096];
    char h_udm_uri[192];
    const char *cache_dir = getenv("TOPSSIM_SDM_CACHE_DIR");

    ogs_assert(supi);
    ogs_assert(action);

    if (!cache_dir || !strlen(cache_dir))
        cache_dir = TOPSSIM_BUILD_DIR "/sdm-cache";

    if (!strcmp(action, "subscribe")) {
        if (!build_home_udm_uri(h_udm_uri, sizeof(h_udm_uri))) {
            abts_log_message("[MANUAL-ROAM][POST] Failed to build home UDM URI");
            return;
        }

        rv = snprintf(command, sizeof(command),
                "'%s' '%s/topssim/tools/sdm_cache_lifecycle.py' "
                "%s "
                "--supi '%s' "
                "--cache-dir '%s' "
                "--resources 'am-data,smf-select-data,ue-context-in-smf-data,sm-data' "
                "--h-udm '%s' "
                "--callback-reference 'http://sdm-cf.localdomain/topssim/sdm-subscription-notify/%s' "
                "--ttl-seconds 1800",
                TOPSSIM_PYTHON,
                TOPSSIM_SOURCE_DIR,
                action,
                supi,
                cache_dir,
                h_udm_uri,
                supi);
    } else {
        rv = snprintf(command, sizeof(command),
                "'%s' '%s/topssim/tools/sdm_cache_lifecycle.py' "
                "%s "
                "--supi '%s' "
                "--cache-dir '%s' "
                "--resources 'am-data,smf-select-data,ue-context-in-smf-data,sm-data' "
                "--ttl-seconds 1800",
                TOPSSIM_PYTHON,
                TOPSSIM_SOURCE_DIR,
                action,
                supi,
                cache_dir);
    }
    if (rv < 0 || rv >= (int)sizeof(command)) {
        abts_log_message("[MANUAL-ROAM][POST] Failed to build SDM-CF lifecycle command");
        return;
    }

    rv = system(command);
    if (rv == 0) {
        abts_log_message("[MANUAL-ROAM][POST] SDM-CF cache lifecycle action completed [action:%s supi:%s cache:%s]",
                action, supi, cache_dir);
    } else {
        abts_log_message("[MANUAL-ROAM][POST] SDM-CF cache lifecycle action failed (system rv=%d) [action:%s supi:%s], continue",
                rv, action, supi);
    }
}

static void run_preparation_phase(int *step, const char *supi)
{
    int64_t started_ms;

    ogs_assert(step);
    ogs_assert(supi);

    wait_for_enter("Press Enter to start preparation phase");

    RSTEP((*step)++, "Preparation phase started");
    abts_log_message("[MANUAL-ROAM][PREP] Initial preparation phase is active");

    started_ms = now_ms();
    abts_log_message("[MANUAL-ROAM][PREP] Preparation-scope determination completed");
    append_topssim_audit_elapsed(
            "preparation.scope_determination.completed",
            "Preparation-scope determination",
            now_ms() - started_ms);

    abts_log_message("[MANUAL-ROAM][PREP] Triggering UDM pre-discovery on visited NRF");
    started_ms = now_ms();
    trigger_visited_pre_discovery("nudm-uecm,nudm-sdm");
    append_topssim_audit_elapsed(
            "preparation.pre_discovery.completed",
            "Subscriber database pre-discovery",
            now_ms() - started_ms);

    abts_log_message("[MANUAL-ROAM][PREP] Prefetching UDM SDM resources into SDM-CF cache");
    trigger_sdm_prefetch(supi);
    RSTEP((*step)++, "Preparation phase completed");

    wait_for_enter("Press Enter to continue to normal flow");
}

static void run_post_setup_phase(int *step, const char *supi)
{
    ogs_assert(step);
    ogs_assert(supi);

    RSTEP((*step)++, "Post-setup phase started");
    abts_log_message("[MANUAL-ROAM][POST] Creating H-UDM SDM subscriptions for prepared resources");
    trigger_sdm_cache_lifecycle(supi, "subscribe");
    abts_log_message("[MANUAL-ROAM][POST] Refreshing local SDM-CF cache after visited setup");
    trigger_sdm_cache_lifecycle(supi, "refresh");
    RSTEP((*step)++, "Post-setup phase completed");
}

static void run_post_setup_cleanup(const char *supi)
{
    ogs_assert(supi);

    abts_log_message("[MANUAL-ROAM][POST] Removing local SDM-CF cache artifacts");
    trigger_sdm_cache_lifecycle(supi, "delete");
}

static void log_ue_ipv4(const char *prefix, test_sess_t *sess)
{
    char *ip = NULL;

    ogs_assert(prefix);
    ogs_assert(sess);

    if (!sess->ue_ip.ipv4) {
        abts_log_message("[MANUAL-ROAM] %s UE IPv4: (not assigned)", prefix);
        return;
    }

    ip = ogs_ipv4_to_string(sess->ue_ip.addr);
    if (ip) {
        abts_log_message("[MANUAL-ROAM] %s UE IPv4: %s", prefix, ip);
        ogs_free(ip);
    } else {
        abts_log_message("[MANUAL-ROAM] %s UE IPv4: (conversion failed)", prefix);
    }
}

static void set_access_context(
        uint16_t mcc, uint16_t mnc, uint16_t mnc_len, uint32_t tac,
        test_ue_t *test_ue, const char *label)
{
    test_context_t *ctx = test_self();
    ogs_plmn_id_t plmn_id;

    ogs_assert(ctx);
    ogs_assert(label);

    memset(&plmn_id, 0, sizeof(plmn_id));
    ogs_plmn_id_build(&plmn_id, mcc, mnc, mnc_len);

    ctx->num_of_plmn_support = 1;
    memset(&ctx->plmn_support[0], 0, sizeof(ctx->plmn_support[0]));
    memcpy(&ctx->plmn_support[0].plmn_id, &plmn_id, OGS_PLMN_ID_LEN);
    ctx->plmn_support[0].num_of_s_nssai = 1;
    ctx->plmn_support[0].s_nssai[0].sst = 1;
    ctx->plmn_support[0].s_nssai[0].sd.v = OGS_S_NSSAI_NO_SD_VALUE;

    ctx->num_of_nr_served_tai = 1;
    memset(&ctx->nr_served_tai[0], 0, sizeof(ctx->nr_served_tai[0]));
    ctx->nr_served_tai[0].list2.num = 1;
    memcpy(&ctx->nr_served_tai[0].list2.tai[0].plmn_id, &plmn_id, OGS_PLMN_ID_LEN);
    ctx->nr_served_tai[0].list2.tai[0].tac.v = tac;

    memcpy(&ctx->nr_tai, &ctx->nr_served_tai[0].list2.tai[0], sizeof(ctx->nr_tai));
    memcpy(&ctx->nr_cgi.plmn_id, &plmn_id, OGS_PLMN_ID_LEN);
    ctx->nr_cgi.cell_id = 0x40001;

    if (test_ue) {
        memcpy(&test_ue->nr_tai, &ctx->nr_tai, sizeof(test_ue->nr_tai));
        memcpy(&test_ue->nr_cgi.plmn_id, &ctx->nr_cgi.plmn_id, OGS_PLMN_ID_LEN);
        test_ue->nr_cgi.cell_id = ctx->nr_cgi.cell_id;
    }

    abts_log_message("[MANUAL-ROAM] Set access context to %s PLMN[%03u/%03u] TAC[%u]",
            label, mcc, mnc, tac);
}

static void send_ngap_checked(abts_case *tc, ogs_socknode_t *ngap,
        ogs_pkbuf_t *sendbuf, const char *plmn, const char *label)
{
    int rv;
    ogs_assert(tc);
    ogs_assert(ngap);
    ABTS_PTR_NOTNULL(tc, sendbuf);

    abts_log_message("[MANUAL-ROAM][TRACE][%s][TX] %s",
            plmn ? plmn : "N/A", label ? label : "(null)");
    rv = testgnb_ngap_send(ngap, sendbuf);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);
}

static void recv_ngap_and_trace(abts_case *tc, ogs_socknode_t *ngap,
        test_ue_t *test_ue, const char *plmn, const char *label)
{
    ogs_pkbuf_t *recvbuf = NULL;

    ogs_assert(tc);
    ogs_assert(ngap);
    ogs_assert(test_ue);

    recvbuf = testgnb_ngap_read(ngap);
    ABTS_PTR_NOTNULL(tc, recvbuf);
    testngap_recv(test_ue, recvbuf);
    abts_log_message("[MANUAL-ROAM][TRACE][%s][RX] %s",
            plmn ? plmn : "N/A", label ? label : "(null)");
}

static void run_registration_flow(
        abts_case *tc, ogs_socknode_t *ngap, test_ue_t *test_ue, int *step,
        const char *plmn)
{
    char prompt[192];
    ogs_pkbuf_t *gmmbuf = NULL;
    ogs_pkbuf_t *nasbuf = NULL;
    ogs_pkbuf_t *sendbuf = NULL;

    ogs_assert(tc);
    ogs_assert(ngap);
    ogs_assert(test_ue);
    ogs_assert(step);

    test_ue->nas.registration.tsc = 0;
    test_ue->nas.registration.ksi = OGS_NAS_KSI_NO_KEY_IS_AVAILABLE;
    test_ue->nas.registration.follow_on_request = 1;
    test_ue->nas.registration.value = OGS_NAS_5GS_REGISTRATION_TYPE_INITIAL;

    memset(&test_ue->registration_request_param, 0,
            sizeof(test_ue->registration_request_param));

    /* 1) UE starts registration with SUCI in InitialUEMessage. */
    RSTEP((*step)++, "Send Registration Request (SUCI)");
    gmmbuf = testgmm_build_registration_request(test_ue, NULL, false, false);
    ABTS_PTR_NOTNULL(tc, gmmbuf);

    test_ue->registration_request_param.gmm_capability = 1;
    test_ue->registration_request_param.s1_ue_network_capability = 1;
    test_ue->registration_request_param.requested_nssai = 1;
    test_ue->registration_request_param.last_visited_registered_tai = 1;
    test_ue->registration_request_param.ue_usage_setting = 1;
    nasbuf = testgmm_build_registration_request(test_ue, NULL, false, false);
    ABTS_PTR_NOTNULL(tc, nasbuf);

    sendbuf = testngap_build_initial_ue_message(test_ue, gmmbuf,
                NGAP_RRCEstablishmentCause_mo_Signalling, false, true);
    send_ngap_checked(tc, ngap, sendbuf, plmn,
            "InitialUEMessage(RegistrationRequest)");

    /*
     * 2) Authentication pre-phase:
     *    AMF may ask for Identity first, or directly send AuthenticationRequest.
     */
    RSTEP((*step)++, "Wait Identity Request or Authentication Request");
    recv_ngap_and_trace(tc, ngap, test_ue, plmn,
            "Downlink NAS after RegistrationRequest");
    if (test_ue->gmm_message_type == OGS_NAS_5GS_IDENTITY_REQUEST) {
        RSTEP((*step)++, "Send Identity Response");
        gmmbuf = testgmm_build_identity_response(test_ue);
        ABTS_PTR_NOTNULL(tc, gmmbuf);
        sendbuf = testngap_build_uplink_nas_transport(test_ue, gmmbuf);
        send_ngap_checked(tc, ngap, sendbuf, plmn,
                "UplinkNASTransport(IdentityResponse)");

        RSTEP((*step)++, "Wait Authentication Request");
        recv_ngap_and_trace(tc, ngap, test_ue, plmn,
                "DownlinkNAS(AuthenticationRequest)");
    } else {
        RSTEP((*step)++, "Identity step skipped (AMF already has SUCI)");
    }

    /* 3) Authentication challenge/response (UE proves credentials). */
    ABTS_INT_EQUAL(tc, OGS_NAS_5GS_AUTHENTICATION_REQUEST, test_ue->gmm_message_type);
    RSTEP((*step)++, "Send Authentication Response");

    gmmbuf = testgmm_build_authentication_response(test_ue);
    ABTS_PTR_NOTNULL(tc, gmmbuf);
    sendbuf = testngap_build_uplink_nas_transport(test_ue, gmmbuf);
    send_ngap_checked(tc, ngap, sendbuf, plmn,
            "UplinkNASTransport(AuthenticationResponse)");

    /*
     * 4) Security phase:
     *    AMF sends SecurityModeCommand, UE confirms with SecurityModeComplete.
     */
    RSTEP((*step)++, "Wait Security Mode Command / Send Security Mode Complete");
    recv_ngap_and_trace(tc, ngap, test_ue, plmn,
            "DownlinkNAS(SecurityModeCommand)");

    snprintf(prompt, sizeof(prompt),
            "Press Enter to send SecurityModeComplete [%s] (this triggers AMF Nudm_UECM PUT)",
            plmn ? plmn : "N/A");
    wait_for_enter(prompt);

    gmmbuf = testgmm_build_security_mode_complete(test_ue, nasbuf);
    ABTS_PTR_NOTNULL(tc, gmmbuf);
    sendbuf = testngap_build_uplink_nas_transport(test_ue, gmmbuf);
    send_ngap_checked(tc, ngap, sendbuf, plmn,
            "UplinkNASTransport(SecurityModeComplete)");

    /*
     * 5) Access setup + registration acceptance:
     *    AMF sends InitialContextSetup carrying RegistrationAccept.
     */
    RSTEP((*step)++, "Wait InitialContextSetup + Registration Accept");
    recv_ngap_and_trace(tc, ngap, test_ue, plmn,
            "InitialContextSetup(+RegistrationAccept)");
    ABTS_INT_EQUAL(tc,
            NGAP_ProcedureCode_id_InitialContextSetup,
            test_ue->ngap_procedure_code);

    sendbuf = testngap_build_ue_radio_capability_info_indication(test_ue);
    send_ngap_checked(tc, ngap, sendbuf, plmn,
            "UERadioCapabilityInfoIndication");

    sendbuf = testngap_build_initial_context_setup_response(test_ue, false);
    send_ngap_checked(tc, ngap, sendbuf, plmn,
            "InitialContextSetupResponse");

    /* 6) UE finalizes registration and handles Configuration Update Command. */
    RSTEP((*step)++, "Send Registration Complete / Wait Configuration Update Command");
    gmmbuf = testgmm_build_registration_complete(test_ue);
    ABTS_PTR_NOTNULL(tc, gmmbuf);
    sendbuf = testngap_build_uplink_nas_transport(test_ue, gmmbuf);
    send_ngap_checked(tc, ngap, sendbuf, plmn,
            "UplinkNASTransport(RegistrationComplete)");

    recv_ngap_and_trace(tc, ngap, test_ue, plmn,
            "DownlinkNAS(ConfigurationUpdateCommand)");
}

static test_sess_t *run_pdu_session_and_get_ip(
        abts_case *tc, ogs_socknode_t *ngap, ogs_socknode_t *gtpu,
        test_ue_t *test_ue, uint8_t psi, int *step, const char *plmn)
{
    int rv;
    ogs_pkbuf_t *gmmbuf = NULL;
    ogs_pkbuf_t *gsmbuf = NULL;
    ogs_pkbuf_t *sendbuf = NULL;
    ogs_pkbuf_t *recvbuf = NULL;
    test_sess_t *sess = NULL;
    test_bearer_t *qos_flow = NULL;

    ogs_assert(tc);
    ogs_assert(ngap);
    ogs_assert(gtpu);
    ogs_assert(test_ue);
    ogs_assert(step);

    RSTEP((*step)++, "Send PDU Session Establishment Request [PSI:%d]", psi);
    sess = test_sess_add_by_dnn_and_psi(test_ue, "internet", psi);
    ogs_assert(sess);

    sess->ul_nas_transport_param.request_type = OGS_NAS_5GS_REQUEST_TYPE_INITIAL;
    sess->ul_nas_transport_param.dnn = 1;
    sess->ul_nas_transport_param.s_nssai = 0;
    sess->pdu_session_establishment_param.ssc_mode = 1;
    sess->pdu_session_establishment_param.epco = 1;

    gsmbuf = testgsm_build_pdu_session_establishment_request(sess);
    ABTS_PTR_NOTNULL(tc, gsmbuf);
    gmmbuf = testgmm_build_ul_nas_transport(sess,
            OGS_NAS_PAYLOAD_CONTAINER_N1_SM_INFORMATION, gsmbuf);
    ABTS_PTR_NOTNULL(tc, gmmbuf);
    sendbuf = testngap_build_uplink_nas_transport(test_ue, gmmbuf);
    send_ngap_checked(tc, ngap, sendbuf, plmn,
            "UplinkNASTransport(PduSessionEstablishmentRequest)");

    RSTEP((*step)++, "Wait PDU Session Resource Setup + Establishment Accept");
    recv_ngap_and_trace(tc, ngap, test_ue, plmn,
            "PDUSessionResourceSetup(+EstablishmentAccept)");
    ABTS_INT_EQUAL(tc,
            NGAP_ProcedureCode_id_PDUSessionResourceSetup,
            test_ue->ngap_procedure_code);

    qos_flow = test_qos_flow_find_by_qfi(sess, 1);
    ogs_assert(qos_flow);

    sendbuf = testngap_sess_build_pdu_session_resource_setup_response(sess);
    send_ngap_checked(tc, ngap, sendbuf, plmn,
            "PDUSessionResourceSetupResponse");

    if (skip_gtpu_ping()) {
        abts_log_message("[MANUAL-ROAM][TRACE][%s][SKIP] GTP-U IPv4 ping",
                plmn ? plmn : "N/A");
    } else {
        abts_log_message("[MANUAL-ROAM][TRACE][%s][TX] GTP-U IPv4 ping",
                plmn ? plmn : "N/A");
        rv = test_gtpu_send_ping(gtpu, qos_flow, TEST_PING_IPV4);
        ABTS_INT_EQUAL(tc, OGS_OK, rv);

        recvbuf = testgnb_gtpu_read(gtpu);
        ABTS_PTR_NOTNULL(tc, recvbuf);
        abts_log_message("[MANUAL-ROAM][TRACE][%s][RX] GTP-U IPv4 ping reply", plmn);
        ogs_pkbuf_free(recvbuf);
    }

    return sess;
}

static void run_disconnect_flow(
        abts_case *tc, ogs_socknode_t *ngap, test_ue_t *test_ue, int *step,
        const char *plmn)
{
    ogs_pkbuf_t *gmmbuf = NULL;
    ogs_pkbuf_t *sendbuf = NULL;

    ogs_assert(tc);
    ogs_assert(ngap);
    ogs_assert(test_ue);
    ogs_assert(step);

    RSTEP((*step)++, "Send UEContextReleaseRequest");
    sendbuf = testngap_build_ue_context_release_request(test_ue,
            NGAP_Cause_PR_radioNetwork, NGAP_CauseRadioNetwork_user_inactivity,
            true);
    send_ngap_checked(tc, ngap, sendbuf, plmn, "UEContextReleaseRequest");

    RSTEP((*step)++, "Wait UEContextReleaseCommand / Send UEContextReleaseComplete");
    recv_ngap_and_trace(tc, ngap, test_ue, plmn, "UEContextReleaseCommand");
    ABTS_INT_EQUAL(tc,
            NGAP_ProcedureCode_id_UEContextRelease,
            test_ue->ngap_procedure_code);

    sendbuf = testngap_build_ue_context_release_complete(test_ue);
    send_ngap_checked(tc, ngap, sendbuf, plmn, "UEContextReleaseComplete");

    RSTEP((*step)++, "Send De-registration Request");
    gmmbuf = testgmm_build_de_registration_request(test_ue, 1, true, false);
    ABTS_PTR_NOTNULL(tc, gmmbuf);
    sendbuf = testngap_build_initial_ue_message(test_ue, gmmbuf,
                NGAP_RRCEstablishmentCause_mo_Signalling, true, false);
    send_ngap_checked(tc, ngap, sendbuf, plmn,
            "InitialUEMessage(DeregistrationRequest)");

    recv_ngap_and_trace(tc, ngap, test_ue, plmn,
            "UEContextReleaseCommand(after dereg)");
    ABTS_INT_EQUAL(tc,
            NGAP_ProcedureCode_id_UEContextRelease,
            test_ue->ngap_procedure_code);

    sendbuf = testngap_build_ue_context_release_complete(test_ue);
    send_ngap_checked(tc, ngap, sendbuf, plmn,
            "UEContextReleaseComplete(final)");
}

static void test1_func(abts_case *tc, void *data)
{
    int step = 1;
    ogs_socknode_t *ngap_home = NULL;
    ogs_socknode_t *ngap_visited = NULL;
    ogs_socknode_t *gtpu = NULL;
    ogs_pkbuf_t *sendbuf = NULL;

    ogs_nas_5gs_mobile_identity_suci_t mobile_identity_suci;
    test_ue_t *test_ue = NULL;
    test_sess_t *sess_home = NULL;
    test_sess_t *sess_visited = NULL;
    bson_t *doc = NULL;
    bson_t *visited_doc = NULL;
    const char *v_db_uri = NULL;

    RSTEP(step++, "Scenario mode: %s", SCENARIO_NAME);

    memset(&mobile_identity_suci, 0, sizeof(mobile_identity_suci));
    mobile_identity_suci.h.supi_format = OGS_NAS_5GS_SUPI_FORMAT_IMSI;
    mobile_identity_suci.h.type = OGS_NAS_5GS_MOBILE_IDENTITY_SUCI;
    mobile_identity_suci.routing_indicator1 = 0;
    mobile_identity_suci.routing_indicator2 = 0xf;
    mobile_identity_suci.routing_indicator3 = 0xf;
    mobile_identity_suci.routing_indicator4 = 0xf;
    mobile_identity_suci.protection_scheme_id = OGS_PROTECTION_SCHEME_NULL;
    mobile_identity_suci.home_network_pki_value = 0;

    test_ue = test_ue_add_by_suci(&mobile_identity_suci, "0000203190");
    ogs_assert(test_ue);
    test_ue->nr_cgi.cell_id = 0x40001;
    test_ue->k_string = "465b5ce8b199b49faa5f0a2ee238a6bc";
    test_ue->opc_string = "e8ed289deba952e4283b54e88e6183ca";

    RSTEP(step++, "Connect gNB to HOME AMF [%s]", home_amf_addr());
    ngap_home = testsctp_client(home_amf_addr(), OGS_NGAP_SCTP_PORT);
    ABTS_PTR_NOTNULL(tc, ngap_home);

    RSTEP(step++, "Connect gNB to VISITED AMF [%s]", visited_amf_addr());
    ngap_visited = testsctp_client(visited_amf_addr(), OGS_NGAP_SCTP_PORT);
    ABTS_PTR_NOTNULL(tc, ngap_visited);

    RSTEP(step++, "Connect gNB to UPF (GTP-U)");
    gtpu = test_gtpu_server(1, AF_INET);
    ABTS_PTR_NOTNULL(tc, gtpu);

    RSTEP(step++, "Switch UE/gNB context to HOME PLMN");
    set_access_context(HOME_MCC, HOME_MNC, HOME_MNC_LEN, HOME_TAC, test_ue, "HOME");

    RSTEP(step++, "NG Setup with HOME AMF");
    sendbuf = testngap_build_ng_setup_request(0x4000, 22);
    send_ngap_checked(tc, ngap_home, sendbuf, "HOME", "NGSetupRequest");
    recv_ngap_and_trace(tc, ngap_home, test_ue, "HOME", "NGSetupResponse");

    RSTEP(step++, "Insert subscriber profile in MongoDB");
    doc = test_db_new_manual_lbo_detach_reattach(test_ue);
    ABTS_PTR_NOTNULL(tc, doc);
    visited_doc = bson_copy(doc);
    ABTS_PTR_NOTNULL(tc, visited_doc);
    ABTS_INT_EQUAL(tc, OGS_OK, test_db_insert_ue(test_ue, doc));
    doc = NULL;

    v_db_uri = visited_db_uri();
    if (v_db_uri) {
        RSTEP(step++, "Mirror subscriber profile into VISITED MongoDB");
        ABTS_INT_EQUAL(tc, OGS_OK,
                mirror_subscriber_to_uri(v_db_uri, test_ue, visited_doc));
    } else {
        abts_log_message("[MANUAL-ROAM][DB] TOPSSIM_VISITED_DB_URI not set; visited DB mirror skipped");
    }
    bson_destroy(visited_doc);
    visited_doc = NULL;

    if (topssim_sdm_cf_enabled()) {
        run_preparation_phase(&step, test_ue->supi);
    } else {
        abts_log_message("[MANUAL-ROAM][PREP][SKIP] TOPSSIM SDM-CF disabled");
    }

    RSTEP(step++, "Attach to HOME PLMN");
    run_registration_flow(tc, ngap_home, test_ue, &step, "HOME");
    if (skip_pdu_session()) {
        abts_log_message("[MANUAL-ROAM][TRACE][HOME][SKIP] PDU session");
    } else {
        sess_home = run_pdu_session_and_get_ip(
                tc, ngap_home, gtpu, test_ue, 5, &step, "HOME");
        ABTS_PTR_NOTNULL(tc, sess_home);
        log_ue_ipv4("HOME PLMN", sess_home);
    }

    wait_for_enter("Press Enter to trigger roaming to visited PLMN");

    RSTEP(step++, "Detach from HOME PLMN (no AMF context transfer)");
    run_disconnect_flow(tc, ngap_home, test_ue, &step, "HOME");
    if (sess_home) {
        test_sess_remove(sess_home);
        sess_home = NULL;
    }

    RSTEP(step++, "Switch UE/gNB context to VISITED PLMN");
    set_access_context(
            VISITED_MCC, VISITED_MNC, VISITED_MNC_LEN, VISITED_TAC, test_ue,
            "VISITED");

    RSTEP(step++, "NG Setup with VISITED AMF");
    sendbuf = testngap_build_ng_setup_request(0x4001, 22);
    send_ngap_checked(tc, ngap_visited, sendbuf, "VISITED", "NGSetupRequest");
    recv_ngap_and_trace(
            tc, ngap_visited, test_ue, "VISITED", "NGSetupResponse");

    RSTEP(step++, "Attach to VISITED PLMN");
    run_registration_flow(tc, ngap_visited, test_ue, &step, "VISITED");
    if (skip_pdu_session()) {
        abts_log_message("[MANUAL-ROAM][TRACE][VISITED][SKIP] PDU session");
    } else {
        sess_visited = run_pdu_session_and_get_ip(
                tc, ngap_visited, gtpu, test_ue, 6, &step, "VISITED");
        ABTS_PTR_NOTNULL(tc, sess_visited);
        log_ue_ipv4("VISITED PLMN", sess_visited);
    }

    if (topssim_sdm_cf_enabled()) {
        run_post_setup_phase(&step, test_ue->supi);
    } else {
        abts_log_message("[MANUAL-ROAM][POST][SKIP] TOPSSIM SDM-CF disabled");
    }

    RSTEP(step++, "Wait for user input before disconnect");
    wait_for_enter("Press Enter to start disconnect from visited PLMN");

    RSTEP(step++, "Disconnect from VISITED PLMN");
    run_disconnect_flow(tc, ngap_visited, test_ue, &step, "VISITED");

    RSTEP(step++, "Cleanup");
    if (topssim_sdm_cf_enabled())
        run_post_setup_cleanup(test_ue->supi);
    if (v_db_uri)
        remove_subscriber_from_uri(v_db_uri, test_ue);
    ABTS_INT_EQUAL(tc, OGS_OK, test_db_remove_ue(test_ue));
    if (sess_home)
        test_sess_remove(sess_home);
    if (sess_visited)
        test_sess_remove(sess_visited);
    testgnb_gtpu_close(gtpu);
    testgnb_ngap_close(ngap_visited);
    testgnb_ngap_close(ngap_home);
    test_ue_remove(test_ue);
}

abts_suite *test_manual_lbo_detach_reattach_enhanced(abts_suite *suite)
{
    suite = ADD_SUITE(suite)

    abts_run_test(suite, test1_func, NULL);

    return suite;
}
