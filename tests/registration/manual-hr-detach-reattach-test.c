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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SCENARIO_NAME                "HPLMN attach, full detach, VPLMN roaming reattach (fresh SUCI)"

/* ========================= User Config ========================= */
/* Configured for VM3 in Alexis lab.
 * IMPORTANT: this is detach + fresh reattach, not N14 context transfer.
 * It assumes the subscriber already exists in VM1/HPLMN DB.
 */
#define HOME_AMF_ADDR                "172.16.85.100"
#define VISITED_AMF_ADDR             "172.16.85.110"
#define GNB1_N3_BIND_ADDR            "172.16.85.132"
#define GNB2_N3_BIND_ADDR            "172.16.85.133"

#define HOME_MCC                     1
#define HOME_MNC                     1
#define HOME_MNC_LEN                 2
#define HOME_TAC                     1

#define VISITED_MCC                  1
#define VISITED_MNC                  2
#define VISITED_MNC_LEN              2
#define VISITED_TAC                  1

#define HOME_PLMN_LABEL              "HOME"
#define VISITED_PLMN_LABEL           "VISITED"

#define GNB_HOME_ID                  0x4000
#define GNB_VISITED_ID               0x4001
#define GNB_NG_SETUP_TA_BITS         22
#define UE_NR_CELL_ID                0x40001

#define UE_MSIN                      "0000000010"
#define UE_K                         "465b5ce8b199b49faa5f0a2ee238a6bc"
#define UE_OPC                       "e8ed289deba952e4283b54e88e6183ca"
#define UE_SECURITY_AMF              "8000"

#define UE_DEFAULT_SST               1
#define UE_DEFAULT_DNN               "internet"
#define HOME_PDU_PSI                 5
#define VISITED_PDU_PSI              6
#define ENABLE_GTPU_PING_CHECK       1
#define HOME_PING_IPV4               "10.45.0.1"
#define VISITED_PING_IPV4            "10.46.0.1"

/* =============================================================== */

#define RSTEP(_step, _fmt, ...) \
    abts_log_message("[MANUAL-ROAM][STEP %02d] " _fmt, (_step), ##__VA_ARGS__)

typedef struct manual_metrics_s {
    ogs_time_t t_start;
    ogs_time_t t_end;

    ogs_time_t t_home_reg_start;
    ogs_time_t t_home_reg_end;
    ogs_time_t t_home_pdu_start;
    ogs_time_t t_home_pdu_end;

    ogs_time_t t_cp_gap_start;

    ogs_time_t t_visited_ng_setup_start;
    ogs_time_t t_visited_ng_setup_end;
    ogs_time_t t_visited_reg_start;
    ogs_time_t t_visited_reg_end;
    ogs_time_t t_visited_pdu_start;
    ogs_time_t t_visited_pdu_end;

    ogs_time_t t_home_last_ping_rx;
    ogs_time_t t_visited_first_ping_rx;

    int n_retry_sctp;
    int n_reset_events;
} manual_metrics_t;

static manual_metrics_t g_metrics;

static ogs_time_t metric_now(void)
{
    return ogs_time_now();
}

static long long metric_delta_ms(ogs_time_t start, ogs_time_t end)
{
    if (!start || !end || end < start)
        return -1;
    return (long long)((end - start) / 1000);
}

static void metrics_reset(void)
{
    memset(&g_metrics, 0, sizeof(g_metrics));
}

static void metrics_log_summary(void)
{
    long long total_ms = metric_delta_ms(g_metrics.t_start, g_metrics.t_end);
    long long home_reg_ms = metric_delta_ms(
            g_metrics.t_home_reg_start, g_metrics.t_home_reg_end);
    long long home_pdu_ms = metric_delta_ms(
            g_metrics.t_home_pdu_start, g_metrics.t_home_pdu_end);
    long long visited_ng_setup_ms = metric_delta_ms(
            g_metrics.t_visited_ng_setup_start, g_metrics.t_visited_ng_setup_end);
    long long visited_reg_ms = metric_delta_ms(
            g_metrics.t_visited_reg_start, g_metrics.t_visited_reg_end);
    long long visited_pdu_ms = metric_delta_ms(
            g_metrics.t_visited_pdu_start, g_metrics.t_visited_pdu_end);
    long long cp_gap_ms = metric_delta_ms(
            g_metrics.t_cp_gap_start, g_metrics.t_visited_reg_end);
    long long up_gap_ms = metric_delta_ms(
            g_metrics.t_home_last_ping_rx, g_metrics.t_visited_first_ping_rx);

    abts_log_message("[MANUAL-ROAM][METRICS] ===== Summary =====");
    abts_log_message("[MANUAL-ROAM][METRICS] scenario            : %s",
            SCENARIO_NAME);
    abts_log_message("[MANUAL-ROAM][METRICS] total_ms            : %lld",
            total_ms);
    abts_log_message("[MANUAL-ROAM][METRICS] home_reg_ms         : %lld",
            home_reg_ms);
    abts_log_message("[MANUAL-ROAM][METRICS] home_pdu_ms         : %lld",
            home_pdu_ms);
    abts_log_message("[MANUAL-ROAM][METRICS] visited_ng_setup_ms : %lld",
            visited_ng_setup_ms);
    abts_log_message("[MANUAL-ROAM][METRICS] visited_reg_ms      : %lld",
            visited_reg_ms);
    abts_log_message("[MANUAL-ROAM][METRICS] visited_pdu_ms      : %lld",
            visited_pdu_ms);
    abts_log_message("[MANUAL-ROAM][METRICS] cp_gap_ms           : %lld",
            cp_gap_ms);
    abts_log_message("[MANUAL-ROAM][METRICS] up_gap_ms           : %lld",
            up_gap_ms);
    abts_log_message("[MANUAL-ROAM][METRICS] retry_sctp          : %d",
            g_metrics.n_retry_sctp);
    abts_log_message("[MANUAL-ROAM][METRICS] reset_events        : %d",
            g_metrics.n_reset_events);
    abts_log_message("[MANUAL-ROAM][METRICS] ====================");
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
    ctx->plmn_support[0].s_nssai[0].sst = UE_DEFAULT_SST;
    ctx->plmn_support[0].s_nssai[0].sd.v = OGS_S_NSSAI_NO_SD_VALUE;

    ctx->num_of_nr_served_tai = 1;
    memset(&ctx->nr_served_tai[0], 0, sizeof(ctx->nr_served_tai[0]));
    ctx->nr_served_tai[0].list2.num = 1;
    memcpy(&ctx->nr_served_tai[0].list2.tai[0].plmn_id, &plmn_id, OGS_PLMN_ID_LEN);
    ctx->nr_served_tai[0].list2.tai[0].tac.v = tac;

    memcpy(&ctx->nr_tai, &ctx->nr_served_tai[0].list2.tai[0], sizeof(ctx->nr_tai));
    memcpy(&ctx->nr_cgi.plmn_id, &plmn_id, OGS_PLMN_ID_LEN);
    ctx->nr_cgi.cell_id = UE_NR_CELL_ID;

    if (test_ue) {
        memcpy(&test_ue->nr_tai, &ctx->nr_tai, sizeof(test_ue->nr_tai));
        memcpy(&test_ue->nr_cgi.plmn_id, &ctx->nr_cgi.plmn_id, OGS_PLMN_ID_LEN);
        test_ue->nr_cgi.cell_id = ctx->nr_cgi.cell_id;
    }

    abts_log_message("[MANUAL-ROAM] Set access context to %s PLMN[%03u/%03u] TAC[%u]",
            label, mcc, mnc, tac);
}

static void set_gtpu_bind_context(void)
{
    int rv;

    if (test_self()->gnb1_addr)
        ogs_freeaddrinfo(test_self()->gnb1_addr);
    if (test_self()->gnb2_addr)
        ogs_freeaddrinfo(test_self()->gnb2_addr);

    rv = ogs_getaddrinfo(&test_self()->gnb1_addr, AF_UNSPEC,
            GNB1_N3_BIND_ADDR, OGS_GTPV1_U_UDP_PORT, 0);
    ogs_assert(rv == OGS_OK);
    rv = ogs_getaddrinfo(&test_self()->gnb2_addr, AF_UNSPEC,
            GNB2_N3_BIND_ADDR, OGS_GTPV1_U_UDP_PORT, 0);
    ogs_assert(rv == OGS_OK);
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

static bool recv_ngap_and_trace_optional(ogs_socknode_t *ngap,
        test_ue_t *test_ue, const char *plmn, const char *label)
{
    ogs_pkbuf_t *recvbuf = NULL;

    ogs_assert(ngap);
    ogs_assert(test_ue);

    recvbuf = testgnb_ngap_read(ngap);
    if (!recvbuf) {
        g_metrics.n_reset_events++;
        abts_log_message("[MANUAL-ROAM][TRACE][%s][RX] %s (no message)",
                plmn ? plmn : "N/A", label ? label : "(null)");
        return false;
    }

    testngap_recv(test_ue, recvbuf);
    abts_log_message("[MANUAL-ROAM][TRACE][%s][RX] %s",
            plmn ? plmn : "N/A", label ? label : "(null)");
    return true;
}

static bool send_ngap_optional(ogs_socknode_t *ngap, ogs_pkbuf_t *sendbuf,
        const char *plmn, const char *label)
{
    int rv;

    ogs_assert(ngap);
    ogs_assert(sendbuf);

    abts_log_message("[MANUAL-ROAM][TRACE][%s][TX] %s",
            plmn ? plmn : "N/A", label ? label : "(null)");
    rv = testgnb_ngap_send(ngap, sendbuf);
    if (rv != OGS_OK) {
        g_metrics.n_reset_events++;
        abts_log_message("[MANUAL-ROAM][TRACE][%s][TX] %s (send failed:%d)",
                plmn ? plmn : "N/A", label ? label : "(null)", rv);
        return false;
    }
    return true;
}

static bool run_registration_flow(
        abts_case *tc, ogs_socknode_t *ngap, test_ue_t *test_ue, int *step,
        const char *plmn)
{
    ogs_pkbuf_t *gmmbuf = NULL;
    ogs_pkbuf_t *nasbuf = NULL;
    ogs_pkbuf_t *sendbuf = NULL;

    ogs_assert(tc);
    ogs_assert(ngap);
    ogs_assert(test_ue);
    ogs_assert(step);

    if (plmn && !strcmp(plmn, HOME_PLMN_LABEL))
        g_metrics.t_home_reg_start = metric_now();
    else if (plmn && !strcmp(plmn, VISITED_PLMN_LABEL))
        g_metrics.t_visited_reg_start = metric_now();

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
    if (!send_ngap_optional(ngap, sendbuf, plmn,
                "InitialUEMessage(RegistrationRequest)"))
        return false;

    /*
     * 2) Authentication pre-phase:
     *    AMF may ask for Identity first, or directly send AuthenticationRequest.
     */
    RSTEP((*step)++, "Wait Identity Request or Authentication Request");
    if (!recv_ngap_and_trace_optional(
                ngap, test_ue, plmn, "Downlink NAS after RegistrationRequest"))
        return false;
    if (test_ue->gmm_message_type == OGS_NAS_5GS_IDENTITY_REQUEST) {
        RSTEP((*step)++, "Send Identity Response");
        gmmbuf = testgmm_build_identity_response(test_ue);
        ABTS_PTR_NOTNULL(tc, gmmbuf);
        sendbuf = testngap_build_uplink_nas_transport(test_ue, gmmbuf);
        if (!send_ngap_optional(
                    ngap, sendbuf, plmn, "UplinkNASTransport(IdentityResponse)"))
            return false;

        RSTEP((*step)++, "Wait Authentication Request");
        if (!recv_ngap_and_trace_optional(
                    ngap, test_ue, plmn, "DownlinkNAS(AuthenticationRequest)"))
            return false;
    } else {
        RSTEP((*step)++, "Identity step skipped (AMF already has SUCI)");
    }

    /* 3) Authentication challenge/response (UE proves credentials). */
    if (test_ue->gmm_message_type != OGS_NAS_5GS_AUTHENTICATION_REQUEST) {
        abts_log_message("[MANUAL-ROAM][TRACE][%s] Expected AuthenticationRequest, got GMM message [%d]",
                plmn ? plmn : "N/A", test_ue->gmm_message_type);
        return false;
    }
    RSTEP((*step)++, "Send Authentication Response");

    gmmbuf = testgmm_build_authentication_response(test_ue);
    ABTS_PTR_NOTNULL(tc, gmmbuf);
    sendbuf = testngap_build_uplink_nas_transport(test_ue, gmmbuf);
    if (!send_ngap_optional(
                ngap, sendbuf, plmn, "UplinkNASTransport(AuthenticationResponse)"))
        return false;

    /*
     * 4) Security phase:
     *    AMF sends SecurityModeCommand, UE confirms with SecurityModeComplete.
     */
    RSTEP((*step)++, "Wait Security Mode Command / Send Security Mode Complete");
    if (!recv_ngap_and_trace_optional(
                ngap, test_ue, plmn, "DownlinkNAS(SecurityModeCommand)"))
        return false;
    if (test_ue->gmm_message_type != OGS_NAS_5GS_SECURITY_MODE_COMMAND) {
        abts_log_message("[MANUAL-ROAM][TRACE][%s] Expected SecurityModeCommand, got GMM message [%d] (NGAP proc:%ld)",
                plmn ? plmn : "N/A",
                test_ue->gmm_message_type, test_ue->ngap_procedure_code);
        return false;
    }

    gmmbuf = testgmm_build_security_mode_complete(test_ue, nasbuf);
    ABTS_PTR_NOTNULL(tc, gmmbuf);
    sendbuf = testngap_build_uplink_nas_transport(test_ue, gmmbuf);
    if (!send_ngap_optional(
                ngap, sendbuf, plmn, "UplinkNASTransport(SecurityModeComplete)"))
        return false;

    /*
     * 5) Access setup + registration acceptance:
     *    AMF sends InitialContextSetup carrying RegistrationAccept.
     */
    RSTEP((*step)++, "Wait InitialContextSetup + Registration Accept");
    if (!recv_ngap_and_trace_optional(
                ngap, test_ue, plmn, "InitialContextSetup(+RegistrationAccept)"))
        return false;
    if (test_ue->ngap_procedure_code != NGAP_ProcedureCode_id_InitialContextSetup) {
        abts_log_message("[MANUAL-ROAM][TRACE][%s] Expected InitialContextSetup, got NGAP procedure [%ld]",
                plmn ? plmn : "N/A", test_ue->ngap_procedure_code);
        return false;
    }

    sendbuf = testngap_build_ue_radio_capability_info_indication(test_ue);
    if (!send_ngap_optional(
                ngap, sendbuf, plmn, "UERadioCapabilityInfoIndication"))
        return false;

    sendbuf = testngap_build_initial_context_setup_response(test_ue, false);
    if (!send_ngap_optional(
                ngap, sendbuf, plmn, "InitialContextSetupResponse"))
        return false;

    /* 6) UE finalizes registration and handles Configuration Update Command. */
    RSTEP((*step)++, "Send Registration Complete / Wait Configuration Update Command");
    gmmbuf = testgmm_build_registration_complete(test_ue);
    ABTS_PTR_NOTNULL(tc, gmmbuf);
    sendbuf = testngap_build_uplink_nas_transport(test_ue, gmmbuf);
    if (!send_ngap_optional(
                ngap, sendbuf, plmn, "UplinkNASTransport(RegistrationComplete)"))
        return false;

    if (!recv_ngap_and_trace_optional(
                ngap, test_ue, plmn, "DownlinkNAS(ConfigurationUpdateCommand)")) {
        abts_log_message("[MANUAL-ROAM][TRACE][%s] Registration complete without ConfigurationUpdateCommand",
                plmn ? plmn : "N/A");
    }

    if (plmn && !strcmp(plmn, HOME_PLMN_LABEL))
        g_metrics.t_home_reg_end = metric_now();
    else if (plmn && !strcmp(plmn, VISITED_PLMN_LABEL))
        g_metrics.t_visited_reg_end = metric_now();

    return true;
}

static void set_ue_security_vectors(test_ue_t *test_ue)
{
    ogs_assert(test_ue);
    ogs_assert(test_ue->k_string);
    ogs_assert(test_ue->opc_string);

    ogs_hex_from_string(test_ue->k_string, test_ue->k, sizeof(test_ue->k));
    ogs_hex_from_string(test_ue->opc_string, test_ue->opc, sizeof(test_ue->opc));
}

static const char *select_ping_dst_ipv4(test_sess_t *sess)
{
    uint32_t ipv4;

    ogs_assert(sess);

    if (!sess->ue_ip.ipv4)
        return TEST_PING_IPV4;

    ipv4 = be32toh(sess->ue_ip.addr);
    if ((ipv4 & 0xFFFF0000) == ((10u << 24) | (45u << 16)))
        return HOME_PING_IPV4;
    if ((ipv4 & 0xFFFF0000) == ((10u << 24) | (46u << 16)))
        return VISITED_PING_IPV4;

    return TEST_PING_IPV4;
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

    if (plmn && !strcmp(plmn, HOME_PLMN_LABEL))
        g_metrics.t_home_pdu_start = metric_now();
    else if (plmn && !strcmp(plmn, VISITED_PLMN_LABEL))
        g_metrics.t_visited_pdu_start = metric_now();

    RSTEP((*step)++, "Send PDU Session Establishment Request [PSI:%d]", psi);
    sess = test_sess_add_by_dnn_and_psi(test_ue, UE_DEFAULT_DNN, psi);
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

    if (plmn && !strcmp(plmn, HOME_PLMN_LABEL))
        g_metrics.t_home_pdu_end = metric_now();
    else if (plmn && !strcmp(plmn, VISITED_PLMN_LABEL))
        g_metrics.t_visited_pdu_end = metric_now();

    qos_flow = test_qos_flow_find_by_qfi(sess, 1);
    ogs_assert(qos_flow);

    sendbuf = testngap_sess_build_pdu_session_resource_setup_response(sess);
    send_ngap_checked(tc, ngap, sendbuf, plmn,
            "PDUSessionResourceSetupResponse");

#if ENABLE_GTPU_PING_CHECK
    const char *ping_dst = select_ping_dst_ipv4(sess);
    abts_log_message("[MANUAL-ROAM][TRACE][%s][TX] GTP-U IPv4 ping",
            plmn ? plmn : "N/A");
    abts_log_message("[MANUAL-ROAM][TRACE][%s] Ping destination: %s",
            plmn ? plmn : "N/A", ping_dst);
    rv = test_gtpu_send_ping(gtpu, qos_flow, ping_dst);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);

    recvbuf = testgnb_gtpu_read(gtpu);
    ABTS_PTR_NOTNULL(tc, recvbuf);
    abts_log_message("[MANUAL-ROAM][TRACE][%s][RX] GTP-U IPv4 ping reply", plmn);
    if (plmn && !strcmp(plmn, HOME_PLMN_LABEL))
        g_metrics.t_home_last_ping_rx = metric_now();
    else if (plmn && !strcmp(plmn, VISITED_PLMN_LABEL) &&
             !g_metrics.t_visited_first_ping_rx)
        g_metrics.t_visited_first_ping_rx = metric_now();
    ogs_pkbuf_free(recvbuf);
#else
    abts_log_message("[MANUAL-ROAM][TRACE][%s] Skip local GTP-U ping validation",
            plmn ? plmn : "N/A");
#endif

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

    RSTEP((*step)++, "Send UEContextReleaseRequest (quiet mode)");
    sendbuf = testngap_build_ue_context_release_request(test_ue,
            NGAP_Cause_PR_radioNetwork, NGAP_CauseRadioNetwork_user_inactivity,
            true);
    if (!send_ngap_optional(ngap, sendbuf, plmn, "UEContextReleaseRequest"))
        return;

    RSTEP((*step)++, "Skip waiting UEContextReleaseCommand (quiet mode)");

    RSTEP((*step)++, "Send De-registration Request (quiet mode)");
    gmmbuf = testgmm_build_de_registration_request(test_ue, 1, true, false);
    ABTS_PTR_NOTNULL(tc, gmmbuf);
    sendbuf = testngap_build_initial_ue_message(test_ue, gmmbuf,
                NGAP_RRCEstablishmentCause_mo_Signalling, true, false);
    if (!send_ngap_optional(
                ngap, sendbuf, plmn, "InitialUEMessage(DeregistrationRequest)"))
        return;
}

static void test1_func(abts_case *tc, void *data)
{
    int step = 1;
    ogs_socknode_t *ngap_home = NULL;
    ogs_socknode_t *ngap_visited = NULL;
    ogs_socknode_t *gtpu = NULL;
    ogs_pkbuf_t *sendbuf = NULL;

    ogs_nas_5gs_mobile_identity_suci_t mobile_identity_suci;
    ogs_plmn_id_t home_plmn_id;
    test_ue_t *test_ue = NULL;
    test_sess_t *sess_home = NULL;
    test_sess_t *sess_visited = NULL;

    metrics_reset();
    g_metrics.t_start = metric_now();

    RSTEP(step++, "Scenario mode: %s", SCENARIO_NAME);

    memset(&mobile_identity_suci, 0, sizeof(mobile_identity_suci));
    mobile_identity_suci.h.supi_format = OGS_NAS_5GS_SUPI_FORMAT_IMSI;
    mobile_identity_suci.h.type = OGS_NAS_5GS_MOBILE_IDENTITY_SUCI;
    mobile_identity_suci.routing_indicator1 = 0;
    mobile_identity_suci.routing_indicator2 = 0;
    mobile_identity_suci.routing_indicator3 = 0;
    mobile_identity_suci.routing_indicator4 = 0;
    mobile_identity_suci.protection_scheme_id = OGS_PROTECTION_SCHEME_NULL;
    mobile_identity_suci.home_network_pki_value = 0;
    ogs_plmn_id_build(&home_plmn_id, HOME_MCC, HOME_MNC, HOME_MNC_LEN);
    ogs_nas_from_plmn_id(&mobile_identity_suci.nas_plmn_id, &home_plmn_id);

    test_ue = test_ue_add_by_suci(&mobile_identity_suci, UE_MSIN);
    ogs_assert(test_ue);
    test_ue->nr_cgi.cell_id = UE_NR_CELL_ID;
    test_ue->k_string = UE_K;
    test_ue->opc_string = UE_OPC;
    set_ue_security_vectors(test_ue);

    RSTEP(step++, "Connect gNB to HOME AMF [%s]", HOME_AMF_ADDR);
    ngap_home = testsctp_client(HOME_AMF_ADDR, OGS_NGAP_SCTP_PORT);
    ABTS_PTR_NOTNULL(tc, ngap_home);

    RSTEP(step++, "Set gNB N3 bind context [%s/%s]",
            GNB1_N3_BIND_ADDR, GNB2_N3_BIND_ADDR);
    set_gtpu_bind_context();

    RSTEP(step++, "Connect gNB to UPF (GTP-U)");
    gtpu = test_gtpu_server(1, AF_INET);
    ABTS_PTR_NOTNULL(tc, gtpu);

    RSTEP(step++, "Switch UE/gNB context to HOME PLMN");
    set_access_context(HOME_MCC, HOME_MNC, HOME_MNC_LEN, HOME_TAC, test_ue,
            HOME_PLMN_LABEL);

    RSTEP(step++, "NG Setup with HOME AMF");
    sendbuf = testngap_build_ng_setup_request(
            GNB_HOME_ID, GNB_NG_SETUP_TA_BITS);
    send_ngap_checked(tc, ngap_home, sendbuf, HOME_PLMN_LABEL,
            "NGSetupRequest");
    recv_ngap_and_trace(tc, ngap_home, test_ue, HOME_PLMN_LABEL,
            "NGSetupResponse");

    RSTEP(step++, "UE-only mode: skip local MongoDB insert");

    RSTEP(step++, "Attach to HOME PLMN");
    ABTS_TRUE(tc, run_registration_flow(tc, ngap_home, test_ue, &step, HOME_PLMN_LABEL));
    sess_home = run_pdu_session_and_get_ip(
            tc, ngap_home, gtpu, test_ue, HOME_PDU_PSI, &step,
            HOME_PLMN_LABEL);
    ABTS_PTR_NOTNULL(tc, sess_home);
    log_ue_ipv4("HOME PLMN", sess_home);

    RSTEP(step++, "Detach from HOME PLMN (no AMF context transfer)");
    g_metrics.t_cp_gap_start = metric_now();
    run_disconnect_flow(tc, ngap_home, test_ue, &step, HOME_PLMN_LABEL);
    test_sess_remove(sess_home);
    sess_home = NULL;

    RSTEP(step++, "Switch UE/gNB context to VISITED PLMN");
    set_access_context(
            VISITED_MCC, VISITED_MNC, VISITED_MNC_LEN, VISITED_TAC, test_ue,
            VISITED_PLMN_LABEL);

    RSTEP(step++, "Connect gNB to VISITED AMF [%s]", VISITED_AMF_ADDR);
    ngap_visited = testsctp_client(VISITED_AMF_ADDR, OGS_NGAP_SCTP_PORT);
    ABTS_PTR_NOTNULL(tc, ngap_visited);

    RSTEP(step++, "NG Setup with VISITED AMF");
    g_metrics.t_visited_ng_setup_start = metric_now();
    sendbuf = testngap_build_ng_setup_request(
            GNB_VISITED_ID, GNB_NG_SETUP_TA_BITS);
    send_ngap_checked(tc, ngap_visited, sendbuf, VISITED_PLMN_LABEL,
            "NGSetupRequest");
    recv_ngap_and_trace(
            tc, ngap_visited, test_ue, VISITED_PLMN_LABEL, "NGSetupResponse");
    g_metrics.t_visited_ng_setup_end = metric_now();

    RSTEP(step++, "Attach to VISITED PLMN");
    if (!run_registration_flow(tc, ngap_visited, test_ue, &step, VISITED_PLMN_LABEL)) {
        RSTEP(step++, "Retry VISITED registration with fresh SCTP");
        g_metrics.n_retry_sctp++;
        testgnb_ngap_close(ngap_visited);
        ngap_visited = testsctp_client(VISITED_AMF_ADDR, OGS_NGAP_SCTP_PORT);
        ABTS_PTR_NOTNULL(tc, ngap_visited);
        g_metrics.t_visited_ng_setup_start = metric_now();
        sendbuf = testngap_build_ng_setup_request(
                GNB_VISITED_ID, GNB_NG_SETUP_TA_BITS);
        send_ngap_checked(tc, ngap_visited, sendbuf, VISITED_PLMN_LABEL,
                "NGSetupRequest(retry)");
        recv_ngap_and_trace(
                tc, ngap_visited, test_ue, VISITED_PLMN_LABEL, "NGSetupResponse(retry)");
        g_metrics.t_visited_ng_setup_end = metric_now();
        ABTS_TRUE(tc, run_registration_flow(tc, ngap_visited, test_ue, &step, VISITED_PLMN_LABEL));
    }
    sess_visited = run_pdu_session_and_get_ip(
            tc, ngap_visited, gtpu, test_ue, VISITED_PDU_PSI, &step,
            VISITED_PLMN_LABEL);
    ABTS_PTR_NOTNULL(tc, sess_visited);
    log_ue_ipv4("VISITED PLMN", sess_visited);

    RSTEP(step++, "Disconnect from VISITED PLMN");
    run_disconnect_flow(tc, ngap_visited, test_ue, &step, VISITED_PLMN_LABEL);

    g_metrics.t_end = metric_now();
    metrics_log_summary();

    RSTEP(step++, "Cleanup");
    if (sess_home)
        test_sess_remove(sess_home);
    if (sess_visited)
        test_sess_remove(sess_visited);
    testgnb_gtpu_close(gtpu);
    testgnb_ngap_close(ngap_visited);
    testgnb_ngap_close(ngap_home);
    test_ue_remove(test_ue);
}

abts_suite *test_manual_hr_detach_reattach(abts_suite *suite)
{
    suite = ADD_SUITE(suite)

    abts_run_test(suite, test1_func, NULL);

    return suite;
}
