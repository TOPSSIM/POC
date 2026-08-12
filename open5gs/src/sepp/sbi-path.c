/*
 * Copyright (C) 2023-2024 by Sukchan Lee <acetcom@gmail.com>
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

#include "sbi-path.h"

#include <ctype.h>
#include "sbi/openapi/external/cJSON.h"

#define SEPP_LOG_BODY_MAX_LEN 4096

static int request_handler(ogs_sbi_request_t *request, void *data);
static int response_handler(
        int status, ogs_sbi_response_t *response, void *data);

static void copy_request(
        ogs_sbi_request_t *target, ogs_sbi_request_t *source,
        bool do_not_remove_custom_header);

static const char *sepp_nf_type_to_string(OpenAPI_nf_type_e nf_type);
static void sepp_build_scoped_nf_name(OpenAPI_nf_type_e nf_type,
        bool is_local, char *out, size_t out_size);
static OpenAPI_nf_type_e sepp_requester_nf_type_from_user_agent(
        const char *user_agent);
static ogs_sbi_service_type_e sepp_service_type_from_uri(
        const char *uri, char *service_name, size_t service_name_size);
static void sepp_build_message_name(const char *service_name,
        const char *method, char *message_name, size_t message_name_size);
static void sepp_build_query_string(
        ogs_hash_t *params, char *out, size_t out_size);
static void sepp_build_uri_with_query(const char *uri, const char *query,
        char *out, size_t out_size);
static void sepp_build_body_preview(
        const char *content, size_t content_length,
        char *out, size_t out_size);
static bool sepp_is_likely_json_body(const char *content, size_t content_length);
static char *sepp_json_tabs_to_spaces(const char *json);
static char *sepp_body_to_log_string(
        const char *content, size_t content_length);
static void sepp_log_outbound_request(ogs_sbi_request_t *request,
        sepp_assoc_t *assoc,
        OpenAPI_nf_type_e requester_nf_type,
        ogs_sbi_service_type_e service_type,
        const char *service_name);
static void sepp_log_inbound_response(ogs_sbi_response_t *response,
        sepp_assoc_t *assoc);

static const char *sepp_nf_type_to_string(OpenAPI_nf_type_e nf_type)
{
    const char *name = NULL;

    if (nf_type != OpenAPI_nf_type_NULL)
        name = OpenAPI_nf_type_ToString(nf_type);

    if (!name)
        name = "UNKNOWN";

    return name;
}

static void sepp_build_scoped_nf_name(OpenAPI_nf_type_e nf_type,
        bool is_local, char *out, size_t out_size)
{
    const char *name = sepp_nf_type_to_string(nf_type);

    ogs_assert(out);
    ogs_assert(out_size > 0);

    ogs_snprintf(out, out_size, "%c-%s", is_local ? 'h' : 'v', name);
}

static OpenAPI_nf_type_e sepp_requester_nf_type_from_user_agent(
        const char *user_agent)
{
    OpenAPI_nf_type_e nf_type = OpenAPI_nf_type_NULL;
    char *copy = NULL, *token = NULL, *saveptr = NULL;

    if (!user_agent)
        return OpenAPI_nf_type_NULL;

    copy = ogs_strdup(user_agent);
    ogs_assert(copy);

    token = ogs_strtok_r(copy, "-", &saveptr);
    if (token)
        nf_type = OpenAPI_nf_type_FromString(token);

    ogs_free(copy);

    return nf_type;
}

static ogs_sbi_service_type_e sepp_service_type_from_uri(
        const char *uri, char *service_name, size_t service_name_size)
{
    ogs_sbi_header_t header;
    ogs_sbi_message_t message;
    ogs_sbi_service_type_e service_type = OGS_SBI_SERVICE_TYPE_NULL;

    ogs_assert(uri);

    if (service_name && service_name_size > 0)
        service_name[0] = '\0';

    memset(&header, 0, sizeof(header));
    header.uri = (char *)uri;

    if (ogs_sbi_parse_header(&message, &header) != OGS_OK) {
        ogs_sbi_header_free(&header);
        return OGS_SBI_SERVICE_TYPE_NULL;
    }

    if (message.h.service.name) {
        if (service_name && service_name_size > 0)
            ogs_cpystrn(service_name, message.h.service.name, service_name_size);
        service_type = ogs_sbi_service_type_from_name(message.h.service.name);
    }

    ogs_sbi_header_free(&header);

    return service_type;
}

static void sepp_build_message_name(const char *service_name,
        const char *method, char *message_name, size_t message_name_size)
{
    size_t pos = 0;
    int token_index = 0;
    char *copy = NULL, *token = NULL, *saveptr = NULL;

    ogs_assert(message_name);
    ogs_assert(message_name_size > 0);

    message_name[0] = '\0';

    if (!service_name || !*service_name)
        service_name = "unknown";

    copy = ogs_strdup(service_name);
    ogs_assert(copy);

    token = ogs_strtok_r(copy, "-", &saveptr);
    while (token && pos + 1 < message_name_size) {
        int i;

        if (token_index > 0 && pos + 1 < message_name_size)
            message_name[pos++] = '_';

        for (i = 0; token[i] && pos + 1 < message_name_size; i++) {
            unsigned char ch = (unsigned char)token[i];

            if (token_index == 0) {
                message_name[pos++] = (i == 0) ? toupper(ch) : tolower(ch);
            } else {
                message_name[pos++] = toupper(ch);
            }
        }

        token = ogs_strtok_r(NULL, "-", &saveptr);
        token_index++;
    }

    ogs_free(copy);

    if (method && *method && pos + 1 < message_name_size) {
        int i;

        message_name[pos++] = '_';
        for (i = 0; method[i] && pos + 1 < message_name_size; i++) {
            unsigned char ch = (unsigned char)method[i];
            message_name[pos++] = toupper(ch);
        }
    }

    message_name[pos] = '\0';
}

static void sepp_build_query_string(
        ogs_hash_t *params, char *out, size_t out_size)
{
    ogs_hash_index_t *hi = NULL;
    size_t pos = 0;
    bool has_query = false;
    bool truncated = false;

    ogs_assert(out);
    ogs_assert(out_size > 0);

    out[0] = '\0';

    if (!params) {
        ogs_cpystrn(out, "-", out_size);
        return;
    }

    for (hi = ogs_hash_first(params); hi; hi = ogs_hash_next(hi)) {
        char *key = (char *)ogs_hash_this_key(hi);
        char *val = ogs_hash_this_val(hi);
        int n = 0;

        if (!key || !val)
            continue;

        n = ogs_snprintf(out + pos, out_size - pos, "%s%s=%s",
                has_query ? "&" : "", key, val);
        if (n < 0) {
            truncated = true;
            break;
        }
        if ((size_t)n >= out_size - pos) {
            pos = out_size - 1;
            truncated = true;
            break;
        }
        pos += n;
        has_query = true;
    }

    if (!has_query) {
        ogs_cpystrn(out, "-", out_size);
        return;
    }

    if (truncated && out_size > 4) {
        out[out_size - 4] = '.';
        out[out_size - 3] = '.';
        out[out_size - 2] = '.';
        out[out_size - 1] = '\0';
    }
}

static void sepp_build_uri_with_query(const char *uri, const char *query,
        char *out, size_t out_size)
{
    ogs_assert(out);
    ogs_assert(out_size > 0);

    if (!uri || !*uri) {
        ogs_cpystrn(out, "-", out_size);
        return;
    }

    if (query && *query && strcmp(query, "-") != 0) {
        ogs_snprintf(out, out_size, "%s?%s", uri, query);
    } else {
        ogs_cpystrn(out, uri, out_size);
    }
}

static void sepp_build_body_preview(
        const char *content, size_t content_length,
        char *out, size_t out_size)
{
    size_t i = 0, pos = 0;
    size_t max_preview = SEPP_LOG_BODY_MAX_LEN;

    ogs_assert(out);
    ogs_assert(out_size > 0);

    out[0] = '\0';

    if (!content || content_length == 0) {
        ogs_cpystrn(out, "-", out_size);
        return;
    }

    if (content_length < max_preview)
        max_preview = content_length;

    for (i = 0; i < max_preview && pos + 1 < out_size; i++) {
        unsigned char ch = (unsigned char)content[i];

        if (ch == '\n' || ch == '\r' || ch == '\t')
            out[pos++] = ' ';
        else if (isprint(ch))
            out[pos++] = ch;
        else
            out[pos++] = '.';
    }

    if (content_length > max_preview && pos + 4 < out_size) {
        out[pos++] = '.';
        out[pos++] = '.';
        out[pos++] = '.';
    }

    out[pos] = '\0';
}

static bool sepp_is_likely_json_body(const char *content, size_t content_length)
{
    size_t i = 0;

    if (!content || content_length == 0)
        return false;

    while (i < content_length && isspace((unsigned char)content[i]))
        i++;

    if (i >= content_length)
        return false;

    return content[i] == '{' || content[i] == '[';
}

static char *sepp_json_tabs_to_spaces(const char *json)
{
    size_t len = 0, i = 0, j = 0, extra = 0;
    char *out = NULL;

    if (!json)
        return NULL;

    len = strlen(json);
    for (i = 0; i < len; i++) {
        if (json[i] == '\t')
            extra += 3; /* Replace '\t' (1 char) with 4 spaces */
    }

    out = ogs_malloc(len + extra + 1);
    ogs_assert(out);

    for (i = 0; i < len; i++) {
        if (json[i] == '\t') {
            out[j++] = ' ';
            out[j++] = ' ';
            out[j++] = ' ';
            out[j++] = ' ';
        } else {
            out[j++] = json[i];
        }
    }

    out[j] = '\0';
    return out;
}

static char *sepp_body_to_log_string(
        const char *content, size_t content_length)
{
    cJSON *item = NULL;
    char *pretty = NULL;
    char *pretty_spaces = NULL;
    char body_preview[SEPP_LOG_BODY_MAX_LEN + 32];
    char *body_log = NULL;

    if (!content || content_length == 0)
        return ogs_strdup("-");

    if (sepp_is_likely_json_body(content, content_length)) {
        item = cJSON_ParseWithLength(content, content_length);
        if (item) {
            pretty = cJSON_Print(item);
            cJSON_Delete(item);
            if (pretty) {
                pretty_spaces = sepp_json_tabs_to_spaces(pretty);
                ogs_free(pretty);
                return pretty_spaces;
            }
        }
    }

    sepp_build_body_preview(content, content_length,
            body_preview, sizeof(body_preview));

    body_log = ogs_strdup(body_preview);
    ogs_assert(body_log);

    return body_log;
}

static void sepp_log_outbound_request(ogs_sbi_request_t *request,
        sepp_assoc_t *assoc,
        OpenAPI_nf_type_e requester_nf_type,
        ogs_sbi_service_type_e service_type,
        const char *service_name)
{
    OpenAPI_nf_type_e producer_nf_type = OpenAPI_nf_type_NULL;
    char message_name[128];
    char query_string[512];
    char uri_with_query[2048];
    char requester_name[64];
    char producer_name[64];
    char *body_log = NULL;

    ogs_assert(request);
    ogs_assert(assoc);

    if (service_type != OGS_SBI_SERVICE_TYPE_NULL) {
        producer_nf_type = ogs_sbi_service_type_to_nf_type(service_type);
        service_name = ogs_sbi_service_type_to_name(service_type);
    }

    sepp_build_message_name(service_name, request->h.method,
            message_name, sizeof(message_name));
    sepp_build_query_string(
            request->http.params, query_string, sizeof(query_string));
    sepp_build_uri_with_query(
            request->h.uri, query_string,
            uri_with_query, sizeof(uri_with_query));
    sepp_build_scoped_nf_name(
            requester_nf_type, assoc->requester_is_local,
            requester_name, sizeof(requester_name));
    sepp_build_scoped_nf_name(
            producer_nf_type, assoc->producer_is_local,
            producer_name, sizeof(producer_name));
    body_log = sepp_body_to_log_string(
            request->http.content, request->http.content_length);
    ogs_assert(body_log);

    ogs_info("[SEPP][REQ][RID:%d] %s->%s\n"
            "Service: %s\n"
            "query:\n"
            "%s %s\n"
            "%s",
            assoc->stream_id,
            requester_name,
            producer_name,
            message_name,
            request->h.method ? request->h.method : "-",
            uri_with_query,
            body_log);

    ogs_free(body_log);
}

static void sepp_log_inbound_response(ogs_sbi_response_t *response,
        sepp_assoc_t *assoc)
{
    OpenAPI_nf_type_e producer_nf_type = OpenAPI_nf_type_NULL;
    const char *service_name = NULL;
    char message_name[128];
    char requester_name[64];
    char producer_name[64];
    char *body_log = NULL;

    ogs_assert(response);
    ogs_assert(assoc);

    if (assoc->service_type != OGS_SBI_SERVICE_TYPE_NULL) {
        producer_nf_type = ogs_sbi_service_type_to_nf_type(assoc->service_type);
        service_name = ogs_sbi_service_type_to_name(assoc->service_type);
    }

    sepp_build_message_name(service_name, response->h.method,
            message_name, sizeof(message_name));
    sepp_build_scoped_nf_name(
            producer_nf_type, assoc->producer_is_local,
            producer_name, sizeof(producer_name));
    sepp_build_scoped_nf_name(
            assoc->requester_nf_type, assoc->requester_is_local,
            requester_name, sizeof(requester_name));
    body_log = sepp_body_to_log_string(
            response->http.content, response->http.content_length);
    ogs_assert(body_log);

    ogs_info("[SEPP][RSP][RID:%d] %s->%s\n"
            "Service: %s\n"
            "HTTP Status: %d\n"
            "reply:\n"
            "%s",
            assoc->stream_id,
            producer_name,
            requester_name,
            message_name, response->status, body_log);

    ogs_free(body_log);
}

int sepp_sbi_open(void)
{
    ogs_sbi_nf_instance_t *nf_instance = NULL;

    /* Initialize SELF NF instance */
    nf_instance = ogs_sbi_self()->nf_instance;
    ogs_assert(nf_instance);
    ogs_sbi_nf_fsm_init(nf_instance);

    /* Build NF instance information. It will be transmitted to NRF. */
    ogs_sbi_nf_instance_build_default(nf_instance);

    /* Initialize NRF NF Instance */
    nf_instance = ogs_sbi_self()->nrf_instance;
    if (nf_instance)
        ogs_sbi_nf_fsm_init(nf_instance);

    if (ogs_sbi_server_start_all(request_handler) != OGS_OK)
        return OGS_ERROR;

    return OGS_OK;
}

void sepp_sbi_close(void)
{
    ogs_sbi_client_stop_all();
    ogs_sbi_server_stop_all();
}

bool sepp_n32c_handshake_send_security_capability_request(
        sepp_node_t *sepp_node, bool none)
{
    bool rc;
    ogs_sbi_request_t *request = NULL;
    ogs_sbi_client_t *client = NULL;

    ogs_assert(sepp_node);
    client = sepp_node->client;
    if (!client) {
        ogs_error("No Client");
        return false;
    }

    request = sepp_n32c_handshake_build_security_capability_request(
            sepp_node, none);
    if (!request) {
        ogs_error("sepp_n32c_handshake_build_exchange_capability() failed");
        return false;
    }

    rc = ogs_sbi_client_send_request(
            client, ogs_sbi_client_handler, request, sepp_node);
    ogs_expect(rc == true);

    ogs_sbi_request_free(request);

    return rc;
}

void sepp_n32c_handshake_send_security_capability_response(
        sepp_node_t *sepp_node, ogs_sbi_stream_t *stream)
{
    ogs_sbi_message_t sendmsg;
    ogs_sbi_response_t *response = NULL;

    OpenAPI_sec_negotiate_rsp_data_t SecNegotiateRspData;

    OpenAPI_list_t *PlmnIdList = NULL;
    OpenAPI_plmn_id_t *PlmnId = NULL;

    int i;
    OpenAPI_lnode_t *node = NULL;

    ogs_assert(sepp_self()->sender);
    ogs_assert(sepp_node);
    ogs_assert(stream);

    memset(&SecNegotiateRspData, 0, sizeof(SecNegotiateRspData));
    SecNegotiateRspData.sender = sepp_self()->sender;
    SecNegotiateRspData.selected_sec_capability =
        sepp_node->negotiated_security_scheme;

    if (SecNegotiateRspData.selected_sec_capability !=
            OpenAPI_security_capability_NONE) {
        if (sepp_node->target_apiroot_supported == true) {
            SecNegotiateRspData.is__3_gpp_sbi_target_api_root_supported = true;
            SecNegotiateRspData._3_gpp_sbi_target_api_root_supported = 1;
        }
    }

    PlmnIdList = OpenAPI_list_create();
    ogs_assert(PlmnIdList);

    for (i = 0; i < ogs_local_conf()->num_of_serving_plmn_id; i++) {
        PlmnId = ogs_sbi_build_plmn_id(&ogs_local_conf()->serving_plmn_id[i]);
        ogs_assert(PlmnId);
        OpenAPI_list_add(PlmnIdList, PlmnId);
    }

    if (PlmnIdList->count)
        SecNegotiateRspData.plmn_id_list = PlmnIdList;
    else
        OpenAPI_list_free(PlmnIdList);

    SecNegotiateRspData.supported_features =
        ogs_uint64_to_string(sepp_node->supported_features);
    ogs_assert(SecNegotiateRspData.supported_features);

    memset(&sendmsg, 0, sizeof(sendmsg));
    sendmsg.SecNegotiateRspData = &SecNegotiateRspData;

    response = ogs_sbi_build_response(&sendmsg, OGS_SBI_HTTP_STATUS_OK);
    ogs_assert(response);
    ogs_assert(true == ogs_sbi_server_send_response(stream, response));

    OpenAPI_list_for_each(SecNegotiateRspData.plmn_id_list, node) {
        PlmnId = node->data;
        if (PlmnId)
            ogs_sbi_free_plmn_id(PlmnId);
    }
    OpenAPI_list_free(SecNegotiateRspData.plmn_id_list);
    if (SecNegotiateRspData.supported_features)
        ogs_free(SecNegotiateRspData.supported_features);
}

static int request_handler(ogs_sbi_request_t *request, void *data)
{
    int rv;
    ogs_hash_index_t *hi;
    ogs_sbi_client_t *client = NULL, *scp_client = NULL;
    ogs_sbi_stream_t *stream = data;
    ogs_pool_id_t stream_id = OGS_INVALID_POOL_ID;
    ogs_sbi_server_t *server = NULL;

    ogs_sbi_request_t sepp_request;
    char *apiroot = NULL, *newuri = NULL;

    sepp_assoc_t *assoc = NULL;

    struct {
        char *target_apiroot;
        char *user_agent;
    } headers = {
        NULL, NULL
    };

    sepp_event_t *e = NULL;

    ogs_assert(request);
    ogs_assert(request->h.uri);

    stream_id = OGS_POINTER_TO_UINT(data);
    ogs_assert(stream_id >= OGS_MIN_POOL_ID &&
            stream_id <= OGS_MAX_POOL_ID);

    stream = ogs_sbi_stream_find_by_id(stream_id);
    if (!stream) {
        ogs_error("STREAM has already been removed [%d]", stream_id);
        return OGS_ERROR;
    }

    server = ogs_sbi_server_from_stream(stream);
    ogs_assert(server);

    /* Extract HTTP Header */
    for (hi = ogs_hash_first(request->http.headers);
            hi; hi = ogs_hash_next(hi)) {
        char *key = (char *)ogs_hash_this_key(hi);
        char *val = ogs_hash_this_val(hi);

        if (!key || !val) {
            ogs_error("No Key[%s] Value[%s]", key, val);
            continue;
        }

        /*
         * <RFC 2616>
         *  Each header field consists of a name followed by a colon (":")
         *  and the field value. Field names are case-insensitive.
         */
        if (!strcasecmp(key, OGS_SBI_CUSTOM_TARGET_APIROOT)) {
            headers.target_apiroot = val;
        } else if (!strcasecmp(key, OGS_SBI_USER_AGENT)) {
            headers.user_agent = val;
        }
    }

    if (headers.target_apiroot) {
        bool rc;
        bool target_is_remote = false;
        OpenAPI_nf_type_e requester_nf_type = OpenAPI_nf_type_NULL;
        ogs_sbi_service_type_e service_type = OGS_SBI_SERVICE_TYPE_NULL;
        char service_name[128];
        sepp_node_t *sepp_node = NULL;
        bool do_not_remove_custom_header;

        requester_nf_type =
            sepp_requester_nf_type_from_user_agent(headers.user_agent);
        service_type = sepp_service_type_from_uri(
                request->h.uri, service_name, sizeof(service_name));

        assoc = sepp_assoc_add(stream_id);
        if (!assoc) {
            ogs_error("sepp_assoc_add() failed");
            return OGS_ERROR;
        }
        assoc->requester_nf_type = requester_nf_type;
        assoc->service_type = service_type;

        do_not_remove_custom_header = true;

        target_is_remote = ogs_sbi_fqdn_in_vplmn(headers.target_apiroot);
        if (target_is_remote == true) {
            uint16_t mcc = 0, mnc = 0;

            if (server->interface) {
                ogs_error("[DROP] Peer SEPP is using "
                        "the wrong interface[%s]", server->interface);
                sepp_assoc_remove(assoc);
                return OGS_ERROR;
            }

            mcc = ogs_plmn_id_mcc_from_fqdn(headers.target_apiroot);
            ogs_assert(mcc);
            mnc = ogs_plmn_id_mnc_from_fqdn(headers.target_apiroot);
            ogs_assert(mnc);

            /*
             * Different PLMN : FROM c-SEPP TO p-SEPP
             */
            sepp_node = sepp_node_find_by_plmn_id(mcc, mnc);
            if (!sepp_node) {
                ogs_error("Cannot find SEPP Peer Node [%s:%d:%d]",
                        headers.target_apiroot, mcc, mnc);
                sepp_assoc_remove(assoc);
                return OGS_ERROR;
            }

            client = NF_INSTANCE_CLIENT(&sepp_node->n32f);
            if (!client) {
                client = NF_INSTANCE_CLIENT(sepp_node);
                if (!client) {
                    ogs_error("No Client in SEPP Peer Node [%s:%d:%d]",
                            headers.target_apiroot, mcc, mnc);
                    sepp_assoc_remove(assoc);
                    return OGS_ERROR;
                }
            }

            /* Client ApiRoot */
            apiroot = ogs_sbi_client_apiroot(client);
            ogs_assert(apiroot);

        } else {
            /*
             * Same PLMN : From p-SEPP to NF via SCP
             */
            OpenAPI_uri_scheme_e scheme = OpenAPI_uri_scheme_NULL;
            char *fqdn = NULL;
            uint16_t fqdn_port = 0;
            ogs_sockaddr_t *addr = NULL, *addr6 = NULL;

            if (server->interface == NULL) {
                if (ogs_sbi_server_first_by_interface(
                            OGS_SBI_INTERFACE_NAME_SEPP) ||
                    ogs_sbi_server_first_by_interface(
                            OGS_SBI_INTERFACE_NAME_N32F)) {
                    ogs_error("[DROP] Peer SEPP is using "
                            "the wrong interface[%s]", server->interface);
                    sepp_assoc_remove(assoc);
                    return OGS_ERROR;
                }
            } else {
                if (strcmp(server->interface,
                            OGS_SBI_INTERFACE_NAME_SEPP) == 0) {
                    if (ogs_sbi_server_first_by_interface(
                                OGS_SBI_INTERFACE_NAME_N32F)) {
                        ogs_error("[DROP] Peer SEPP is using "
                                "the wrong interface[%s]", server->interface);
                        sepp_assoc_remove(assoc);
                        return OGS_ERROR;
                    }
                }
            }

            /* Find or Add Client Instance */
            rc = ogs_sbi_getaddr_from_uri(
                    &scheme, &fqdn, &fqdn_port, &addr, &addr6,
                    headers.target_apiroot);
            if (rc == false || scheme == OpenAPI_uri_scheme_NULL) {
                ogs_error("Invalid Target-apiRoot [%s]",
                        headers.target_apiroot);

                sepp_assoc_remove(assoc);
                return OGS_ERROR;
            }

            client = ogs_sbi_client_find(
                    scheme, fqdn, fqdn_port, addr, addr6);
            if (!client) {
                client = ogs_sbi_client_add(
                        scheme, fqdn, fqdn_port, addr, addr6);
                ogs_assert(client);
            }
            OGS_SBI_SETUP_CLIENT(assoc, client);

            ogs_free(fqdn);
            ogs_freeaddrinfo(addr);
            ogs_freeaddrinfo(addr6);

            /* Get SCP client */
            scp_client = NF_INSTANCE_CLIENT(ogs_sbi_self()->scp_instance);

            /* Client ApiRoot */
            if (scp_client) {
                apiroot = ogs_sbi_client_apiroot(scp_client);
                ogs_assert(apiroot);

                /* Switch to the SCP's client */
                client = scp_client;
            } else {
                apiroot = ogs_sbi_client_apiroot(client);
                ogs_assert(apiroot);

                /* Remove Target-apiRoot */
                do_not_remove_custom_header = false;
            }
        }

        /* For h-/v- prefixing:
         * target_is_remote: local requester -> remote producer
         * otherwise: remote requester -> local producer
         */
        assoc->requester_is_local = target_is_remote;
        assoc->producer_is_local = !target_is_remote;

        /* Setup New URI */
        newuri = ogs_msprintf("%s%s", apiroot, request->h.uri);
        ogs_assert(newuri);

        ogs_free(apiroot);

        /* Copy Request for SEPP request */
        copy_request(&sepp_request, request, do_not_remove_custom_header);
        ogs_assert(sepp_request.http.headers);

        /* Set New URI to SEPP request */
        sepp_request.h.uri = newuri;
        ogs_assert(sepp_request.h.uri);

        sepp_log_outbound_request(
                request, assoc, requester_nf_type, service_type, service_name);

        /* Send the HTTP Request with New URI and HTTP Headers */
        if (scp_client) {
            rc = ogs_sbi_client_send_via_scp_or_sepp(
                    scp_client, response_handler, &sepp_request, assoc);
            ogs_expect(rc == true);
        } else {
            rc = ogs_sbi_client_send_request(
                    client, response_handler, &sepp_request, assoc);
            ogs_expect(rc == true);
        }

        if (rc == false) {
            ogs_error("ogs_sbi_send_request_to_client() failed");

            ogs_sbi_http_hash_free(sepp_request.http.headers);
            ogs_free(sepp_request.h.uri);
            sepp_assoc_remove(assoc);

            return OGS_ERROR;
        }

        ogs_sbi_http_hash_free(sepp_request.http.headers);
        ogs_free(sepp_request.h.uri);

        return OGS_OK;
    }

    /***************************************
     * Receive NOTIFICATION message from NRF
     ***************************************/
    ogs_assert(request);
    ogs_assert(data);

    if (server->interface &&
        strcmp(server->interface, OGS_SBI_INTERFACE_NAME_N32F) == 0) {
        ogs_error("[DROP] Peer SEPP is using the wrong interface[%s]",
                server->interface);
        return OGS_ERROR;
    }

    e = sepp_event_new(OGS_EVENT_SBI_SERVER);
    ogs_assert(e);

    e->h.sbi.request = request;
    e->h.sbi.data = data;

    rv = ogs_queue_push(ogs_app()->queue, e);
    if (rv != OGS_OK) {
        ogs_error("ogs_queue_push() failed:%d", (int)rv);

        ogs_event_free(e);
        return OGS_ERROR;
    }

    return OGS_OK;
}

static int response_handler(
        int status, ogs_sbi_response_t *response, void *data)
{
    sepp_assoc_t *assoc = data;
    ogs_sbi_stream_t *stream = NULL;
    ogs_pool_id_t stream_id = OGS_INVALID_POOL_ID;

    ogs_assert(assoc);

    stream_id = assoc->stream_id;
    ogs_assert(stream_id >= OGS_MIN_POOL_ID && stream_id <= OGS_MAX_POOL_ID);
    stream = ogs_sbi_stream_find_by_id(stream_id);

    if (status != OGS_OK) {

        ogs_log_message(
                status == OGS_DONE ? OGS_LOG_DEBUG : OGS_LOG_WARN, 0,
                "response_handler() failed [%d]", status);

        sepp_assoc_remove(assoc);

        if (stream) {
            ogs_assert(true ==
                ogs_sbi_server_send_error(stream,
                    OGS_SBI_HTTP_STATUS_INTERNAL_SERVER_ERROR, NULL,
                    "response_handler() failed", NULL, NULL));
        } else
            ogs_error("STREAM has already been removed [%d]", stream_id);

        return OGS_ERROR;
    }

    ogs_assert(response);

    sepp_log_inbound_response(response, assoc);

    sepp_assoc_remove(assoc);

    if (!stream) {
        ogs_error("STREAM has already been removed [%d]", stream_id);
        ogs_sbi_response_free(response);
        return OGS_ERROR;
    }
    ogs_expect(true == ogs_sbi_server_send_response(stream, response));

    return OGS_OK;
}

static void copy_request(
        ogs_sbi_request_t *target, ogs_sbi_request_t *source,
        bool do_not_remove_custom_header)
{
    ogs_hash_index_t *hi;

    ogs_assert(source);
    ogs_assert(target);

    memset(target, 0, sizeof(*target));

    /* HTTP method/params/content */
    target->h.method = source->h.method;
    target->http.params = source->http.params;
    target->http.content = source->http.content;
    target->http.content_length = source->http.content_length;

    /* HTTP Headers
     *
     * To remove the followings,
     *   Scheme - https
     *   Authority - sepp.open5gs.org
     */
    target->http.headers = ogs_hash_make();
    ogs_assert(target->http.headers);

    /* Extract HTTP Header */
    for (hi = ogs_hash_first(source->http.headers);
            hi; hi = ogs_hash_next(hi)) {
        char *key = (char *)ogs_hash_this_key(hi);
        char *val = ogs_hash_this_val(hi);

        if (!key || !val) {
            ogs_error("No Key[%s] Value[%s]", key, val);
            continue;
        }

        /*
         * <RFC 2616>
         *  Each header field consists of a name followed by a colon (":")
         *  and the field value. Field names are case-insensitive.
         */
        if (do_not_remove_custom_header == false &&
            !strcasecmp(key, OGS_SBI_CUSTOM_TARGET_APIROOT)) {
        } else if (do_not_remove_custom_header == false &&
            !strncasecmp(key, OGS_SBI_CUSTOM_DISCOVERY_COMMON,
                strlen(OGS_SBI_CUSTOM_DISCOVERY_COMMON))) {
        } else if (!strcasecmp(key, OGS_SBI_SCHEME)) {
        } else if (!strcasecmp(key, OGS_SBI_AUTHORITY)) {
        } else {
            ogs_sbi_header_set(target->http.headers, key, val);
        }
    }
}
