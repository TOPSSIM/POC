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
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "test-common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>

#define HOME_AMF_ADDR                "10.145.0.5"
#define VISITED_AMF_ADDR             "10.146.0.5"
#define HOME_MCC                     999
#define HOME_MNC                     70
#define HOME_MNC_LEN                 2
#define HOME_TAC                     1
#define VISITED_MCC                  1
#define VISITED_MNC                  1
#define VISITED_MNC_LEN              2
#define VISITED_TAC                  1

#define HSTEP(_step, _fmt, ...) \
    abts_log_message("[MANUAL-ROAM][STEP %02d] " _fmt, (_step), ##__VA_ARGS__)

static bson_t *test_db_new_hr_mobility_profile(test_ue_t *test_ue)
{
    bson_t *doc = NULL;

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
                    "lbo_roaming_allowed", BCON_BOOL(false),
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
            "access_restriction_data", BCON_INT32(32));
    ogs_assert(doc);

    return doc;
}

static void wait_for_enter(const char *prompt)
{
    abts_log_message("[MANUAL-ROAM] %s", prompt);
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

static void run_initial_registration_flow(
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

    test_ue->nas.registration.tsc = 0;
    test_ue->nas.registration.ksi = OGS_NAS_KSI_NO_KEY_IS_AVAILABLE;
    test_ue->nas.registration.follow_on_request = 1;
    test_ue->nas.registration.value = OGS_NAS_5GS_REGISTRATION_TYPE_INITIAL;

    memset(&test_ue->registration_request_param, 0,
            sizeof(test_ue->registration_request_param));

    HSTEP((*step)++, "Send Registration Request (SUCI)");
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

    HSTEP((*step)++, "Wait Identity Request or Authentication Request");
    recv_ngap_and_trace(tc, ngap, test_ue, plmn,
            "Downlink NAS after RegistrationRequest");
    if (test_ue->gmm_message_type == OGS_NAS_5GS_IDENTITY_REQUEST) {
        HSTEP((*step)++, "Send Identity Response");
        gmmbuf = testgmm_build_identity_response(test_ue);
        ABTS_PTR_NOTNULL(tc, gmmbuf);
        sendbuf = testngap_build_uplink_nas_transport(test_ue, gmmbuf);
        send_ngap_checked(tc, ngap, sendbuf, plmn,
                "UplinkNASTransport(IdentityResponse)");

        HSTEP((*step)++, "Wait Authentication Request");
        recv_ngap_and_trace(tc, ngap, test_ue, plmn,
                "DownlinkNAS(AuthenticationRequest)");
    }

    ABTS_INT_EQUAL(tc, OGS_NAS_5GS_AUTHENTICATION_REQUEST, test_ue->gmm_message_type);
    HSTEP((*step)++, "Send Authentication Response");
    gmmbuf = testgmm_build_authentication_response(test_ue);
    ABTS_PTR_NOTNULL(tc, gmmbuf);
    sendbuf = testngap_build_uplink_nas_transport(test_ue, gmmbuf);
    send_ngap_checked(tc, ngap, sendbuf, plmn,
            "UplinkNASTransport(AuthenticationResponse)");

    HSTEP((*step)++, "Wait Security Mode Command / Send Security Mode Complete");
    recv_ngap_and_trace(tc, ngap, test_ue, plmn,
            "DownlinkNAS(SecurityModeCommand)");

    gmmbuf = testgmm_build_security_mode_complete(test_ue, nasbuf);
    ABTS_PTR_NOTNULL(tc, gmmbuf);
    sendbuf = testngap_build_uplink_nas_transport(test_ue, gmmbuf);
    send_ngap_checked(tc, ngap, sendbuf, plmn,
            "UplinkNASTransport(SecurityModeComplete)");

    HSTEP((*step)++, "Wait InitialContextSetup + Registration Accept");
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

    HSTEP((*step)++, "Send Registration Complete / Wait Configuration Update Command");
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

    HSTEP((*step)++, "Send PDU Session Establishment Request [PSI:%d]", psi);
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

    HSTEP((*step)++, "Wait PDU Session Resource Setup + Establishment Accept");
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

    abts_log_message("[MANUAL-ROAM][TRACE][%s][TX] GTP-U IPv4 ping",
            plmn ? plmn : "N/A");
    rv = test_gtpu_send_ping(gtpu, qos_flow, TEST_PING_IPV4);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);

    recvbuf = testgnb_gtpu_read(gtpu);
    ABTS_PTR_NOTNULL(tc, recvbuf);
    abts_log_message("[MANUAL-ROAM][TRACE][%s][RX] GTP-U IPv4 ping reply", plmn);
    ogs_pkbuf_free(recvbuf);

    return sess;
}

static void run_context_release_only(
        abts_case *tc, ogs_socknode_t *ngap, test_ue_t *test_ue, int *step,
        const char *plmn)
{
    ogs_pkbuf_t *sendbuf = NULL;

    ogs_assert(tc);
    ogs_assert(ngap);
    ogs_assert(test_ue);
    ogs_assert(step);

    HSTEP((*step)++, "Send UEContextReleaseRequest (keep PDU session)");
    sendbuf = testngap_build_ue_context_release_request(test_ue,
            NGAP_Cause_PR_radioNetwork, NGAP_CauseRadioNetwork_user_inactivity,
            true);
    send_ngap_checked(tc, ngap, sendbuf, plmn, "UEContextReleaseRequest");

    HSTEP((*step)++, "Wait UEContextReleaseCommand / Send UEContextReleaseComplete");
    recv_ngap_and_trace(tc, ngap, test_ue, plmn, "UEContextReleaseCommand");
    ABTS_INT_EQUAL(tc,
            NGAP_ProcedureCode_id_UEContextRelease,
            test_ue->ngap_procedure_code);

    sendbuf = testngap_build_ue_context_release_complete(test_ue);
    send_ngap_checked(tc, ngap, sendbuf, plmn, "UEContextReleaseComplete");
}

static void run_mobility_update_flow(
        abts_case *tc, ogs_socknode_t *ngap_visited, test_ue_t *test_ue,
        test_sess_t *sess, int *step)
{
    /* Buffers used across NAS->GMM->NGAP message construction and transmission. */
    ogs_pkbuf_t *gmmbuf = NULL;
    ogs_pkbuf_t *nasbuf = NULL;
    ogs_pkbuf_t *sendbuf = NULL;
    ogs_pkbuf_t *recvbuf = NULL;
    int got_ics = 0;
    int got_ps_setup = 0;
    int fdmax;
    int i;
    int rv;

    /* Hard-stop early if required test/context pointers are missing. */
    ogs_assert(tc);
    ogs_assert(ngap_visited);
    ogs_assert(test_ue);
    ogs_assert(sess);
    ogs_assert(step);

    /*
     * Inter-AMF mobility path:
     * - use registration type "mobility updating"
     * - identify UE by 5G-GUTI (not SUCI)
     * - include Uplink Data Status for existing PDU session.
     */
    HSTEP((*step)++, "Send Mobility Registration Update (5G-GUTI) [PSI:%d]", sess->psi);
    
    /* Mark this Registration Request as Mobility Updating so the new AMF treats it as inter-AMF mobility, not a fresh initial registration. */
    test_ue->nas.registration.value = OGS_NAS_5GS_REGISTRATION_TYPE_MOBILITY_UPDATING;
    
    /*
    ----------------------------------
    Build the NAS Registration Request
    ----------------------------------
    */

    /* Clear all optional Registration Request parameters so we start from a clean state. */
    memset(&test_ue->registration_request_param, 0, sizeof(test_ue->registration_request_param));

    /* Enable the "Uplink Data Status" IE in the Registration Request. */
    test_ue->registration_request_param.uplink_data_status = 1;

    /* Set Uplink Data Status bit for this PSI, i.e., indicate which existing PDU Session ID should be considered for reactivation during mobility update. */
    test_ue->registration_request_param.psimask.uplink_data_status = 1 << sess->psi;

    /* Build inner NAS Registration Request carrying Uplink Data Status. */
    nasbuf = testgmm_build_registration_request(test_ue, NULL, false, false);
    ABTS_PTR_NOTNULL(tc, nasbuf);

    /* Clear temporary registration flags before building the outer Registration Request wrapper. */
    memset(&test_ue->registration_request_param, 0, sizeof(test_ue->registration_request_param));

    /* Use 5G-GUTI as UE identity in this Registration Request (mobility path, old context lookup). */
    test_ue->registration_request_param.guti = 1;

    /* Build final GMM message using the previously built NAS payload (nasbuf) as container content. */
    gmmbuf = testgmm_build_registration_request(test_ue, nasbuf, true, false);
    ABTS_PTR_NOTNULL(tc, gmmbuf);

    /* Wrap GMM/NAS into NGAP InitialUEMessage and send it to visited AMF. */
    sendbuf = testngap_build_initial_ue_message(test_ue, gmmbuf,
                NGAP_RRCEstablishmentCause_mo_Signalling, true, true);
    send_ngap_checked(tc, ngap_visited, sendbuf, "VISITED",
            "InitialUEMessage(MobilityRegistrationUpdate)");

    /* Read first mobility-registration downlink.
     * If ICS is present, complete ICS.
     * If only DownlinkNASTransport is present, continue with existing context.
     */
    HSTEP((*step)++, "Wait InitialContextSetup + Mobility Registration Accept");
    recv_ngap_and_trace(tc, ngap_visited, test_ue, "VISITED",
            "InitialContextSetup(+MobilityRegistrationAccept)");
    if (test_ue->ngap_procedure_code == NGAP_ProcedureCode_id_InitialContextSetup) {
        got_ics = 1;
    } else if (test_ue->ngap_procedure_code == NGAP_ProcedureCode_id_DownlinkNASTransport) {
        abts_log_message("[MANUAL-ROAM][TRACE][VISITED][RX] DownlinkNASTransport before InitialContextSetup");
    }

    if (got_ics) {
        ABTS_INT_EQUAL(tc,
                NGAP_ProcedureCode_id_InitialContextSetup,
                test_ue->ngap_procedure_code);

        /* Verify AMF reports successful reactivation result for requested PSI(s). */
        ABTS_INT_EQUAL(tc, 0x0000, test_ue->pdu_session_reactivation_result);

        /* Send UE radio capability indication as part of normal ICS completion path. */
        sendbuf = testngap_build_ue_radio_capability_info_indication(test_ue);
        send_ngap_checked(tc, ngap_visited, sendbuf, "VISITED",
                "UERadioCapabilityInfoIndication");

        /* Complete NGAP Initial Context Setup from UE side. */
        sendbuf = testngap_build_initial_context_setup_response(test_ue, true);
        send_ngap_checked(tc, ngap_visited, sendbuf, "VISITED",
                "InitialContextSetupResponse");
    } else {
        ABTS_INT_EQUAL(tc,
                NGAP_ProcedureCode_id_DownlinkNASTransport,
                test_ue->ngap_procedure_code);
        abts_log_message("[MANUAL-ROAM][TRACE][VISITED] No InitialContextSetup received; continuing with existing context");
    }

    /* Complete mobility registration on UE side. */
    HSTEP((*step)++, "Send Registration Complete (mobility)");
    gmmbuf = testgmm_build_registration_complete(test_ue);
    ABTS_PTR_NOTNULL(tc, gmmbuf);
    sendbuf = testngap_build_uplink_nas_transport(test_ue, gmmbuf);
    send_ngap_checked(tc, ngap_visited, sendbuf, "VISITED",
            "UplinkNASTransport(RegistrationComplete)");

    /*
     * Drain optional post-registration reactivation signaling.
     * Depending on timing, AMF may still send ICS/PS setup/config update after
     * mobility update completion.
     */
    HSTEP((*step)++, "Handle optional post-registration reactivation signaling");
    fdmax = ngap_visited->sock->fd + 1;
    for (i = 0; i < 50; i++) { /* up to ~5 seconds */
        fd_set rfds;
        struct timeval tv;

        FD_ZERO(&rfds);
        FD_SET(ngap_visited->sock->fd, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 100 * 1000; /* 100ms */

        rv = select(fdmax, &rfds, NULL, NULL, &tv);
        if (rv == 0)
            continue;
        if (rv < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (!FD_ISSET(ngap_visited->sock->fd, &rfds))
            continue;

        recvbuf = testgnb_ngap_read(ngap_visited);
        ABTS_PTR_NOTNULL(tc, recvbuf);
        testngap_recv(test_ue, recvbuf);

        if (test_ue->ngap_procedure_code == NGAP_ProcedureCode_id_InitialContextSetup) {
            abts_log_message("[MANUAL-ROAM][TRACE][VISITED][RX] InitialContextSetup(post-registration)");
            if (!got_ics) {
                got_ics = 1;
                sendbuf = testngap_build_ue_radio_capability_info_indication(test_ue);
                send_ngap_checked(tc, ngap_visited, sendbuf, "VISITED",
                        "UERadioCapabilityInfoIndication(post-registration)");

                sendbuf = testngap_build_initial_context_setup_response(test_ue, true);
                send_ngap_checked(tc, ngap_visited, sendbuf, "VISITED",
                        "InitialContextSetupResponse(post-registration)");
            }
        } else if (test_ue->ngap_procedure_code == NGAP_ProcedureCode_id_PDUSessionResourceSetup) {
            abts_log_message("[MANUAL-ROAM][TRACE][VISITED][RX] PDUSessionResourceSetup(reactivation)");
            sendbuf = testngap_sess_build_pdu_session_resource_setup_response(sess);
            send_ngap_checked(tc, ngap_visited, sendbuf, "VISITED",
                    "PDUSessionResourceSetupResponse(reactivation)");
            got_ps_setup = 1;
        } else if (test_ue->ngap_procedure_code == NGAP_ProcedureCode_id_DownlinkNASTransport) {
            abts_log_message("[MANUAL-ROAM][TRACE][VISITED][RX] DownlinkNASTransport(post-registration)");
        } else {
            abts_log_message("[MANUAL-ROAM][TRACE][VISITED][RX] NGAP procedure[%d](post-registration)",
                    test_ue->ngap_procedure_code);
        }
    }

    if (!got_ps_setup) {
        abts_log_message("[MANUAL-ROAM][TRACE][VISITED] No PDUSessionResourceSetup observed before data-path check");
    }

    /* Guard delay to let async SMF/UPF reactivation settle before ping. */
    ogs_msleep(200);
}


static void run_existing_pdu_ping(
        abts_case *tc, ogs_socknode_t *gtpu, test_sess_t *sess,
        int *step, const char *plmn)
{
    int rv;
    int i;
    int fdmax;
    int attempt;
    int grace;
    ogs_pkbuf_t *recvbuf = NULL;
    test_bearer_t *qos_flow = NULL;

    ogs_assert(tc);
    ogs_assert(gtpu);
    ogs_assert(sess);
    ogs_assert(step);

    HSTEP((*step)++, "Verify existing PDU session is still active");
    qos_flow = test_qos_flow_find_by_qfi(sess, 1);
    ogs_assert(qos_flow);

    fdmax = gtpu->sock->fd + 1;
    for (attempt = 1; attempt <= 5 && !recvbuf; attempt++) {
        abts_log_message("[MANUAL-ROAM][TRACE][%s][TX] GTP-U IPv4 ping (existing session) [attempt:%d]",
                plmn ? plmn : "N/A", attempt);
        rv = test_gtpu_send_ping(gtpu, qos_flow, TEST_PING_IPV4);
        ABTS_INT_EQUAL(tc, OGS_OK, rv);

        for (i = 0; i < 20; i++) {
            fd_set rfds;
            struct timeval tv;

            FD_ZERO(&rfds);
            FD_SET(gtpu->sock->fd, &rfds);
            tv.tv_sec = 0;
            tv.tv_usec = 100 * 1000; /* 100ms */

            rv = select(fdmax, &rfds, NULL, NULL, &tv);
            if (rv > 0 && FD_ISSET(gtpu->sock->fd, &rfds)) {
                recvbuf = testgnb_gtpu_read(gtpu);
                break;
            }
            if (rv < 0 && errno != EINTR)
                break;
        }
        if (!recvbuf) {
            abts_log_message("[MANUAL-ROAM][TRACE][%s][RX] No GTP-U ping reply within 2s [attempt:%d]",
                    plmn ? plmn : "N/A", attempt);
        }
    }

    /* Grace window: reply can arrive right after the last timed attempt. */
    if (!recvbuf) {
        for (grace = 0; grace < 100 && !recvbuf; grace++) {
            fd_set rfds;
            struct timeval tv;

            FD_ZERO(&rfds);
            FD_SET(gtpu->sock->fd, &rfds);
            tv.tv_sec = 0;
            tv.tv_usec = 100 * 1000; /* 100ms */

            rv = select(fdmax, &rfds, NULL, NULL, &tv);
            if (rv > 0 && FD_ISSET(gtpu->sock->fd, &rfds))
                recvbuf = testgnb_gtpu_read(gtpu);
            else if (rv < 0 && errno != EINTR)
                break;
        }
    }

    ABTS_PTR_NOTNULL(tc, recvbuf);
    if (recvbuf) {
        abts_log_message("[MANUAL-ROAM][TRACE][%s][RX] GTP-U IPv4 ping reply (existing session)",
                plmn ? plmn : "N/A");
        ogs_pkbuf_free(recvbuf);
    }
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

    HSTEP((*step)++, "Send UEContextReleaseRequest");
    sendbuf = testngap_build_ue_context_release_request(test_ue,
            NGAP_Cause_PR_radioNetwork, NGAP_CauseRadioNetwork_user_inactivity,
            true);
    send_ngap_checked(tc, ngap, sendbuf, plmn, "UEContextReleaseRequest");

    HSTEP((*step)++, "Wait UEContextReleaseCommand / Send UEContextReleaseComplete");
    recv_ngap_and_trace(tc, ngap, test_ue, plmn, "UEContextReleaseCommand");
    ABTS_INT_EQUAL(tc,
            NGAP_ProcedureCode_id_UEContextRelease,
            test_ue->ngap_procedure_code);

    sendbuf = testngap_build_ue_context_release_complete(test_ue);
    send_ngap_checked(tc, ngap, sendbuf, plmn, "UEContextReleaseComplete");

    HSTEP((*step)++, "Send De-registration Request");
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
    bson_t *doc = NULL;
    uint32_t home_ipv4 = 0;

    memset(&mobile_identity_suci, 0, sizeof(mobile_identity_suci));
    mobile_identity_suci.h.supi_format = OGS_NAS_5GS_SUPI_FORMAT_IMSI;
    mobile_identity_suci.h.type = OGS_NAS_5GS_MOBILE_IDENTITY_SUCI;
    mobile_identity_suci.routing_indicator1 = 0;
    mobile_identity_suci.routing_indicator2 = 0xf;
    mobile_identity_suci.routing_indicator3 = 0xf;
    mobile_identity_suci.routing_indicator4 = 0xf;
    mobile_identity_suci.protection_scheme_id = OGS_PROTECTION_SCHEME_NULL;
    mobile_identity_suci.home_network_pki_value = 0;

    test_ue = test_ue_add_by_suci(&mobile_identity_suci, "000021309");
    ABTS_PTR_NOTNULL(tc, test_ue);

    test_ue->nr_cgi.cell_id = 0x40001;
    test_ue->k_string = "465b5ce8b199b49faa5f0a2ee238a6bc";
    test_ue->opc_string = "e8ed289deba952e4283b54e88e6183ca";

    HSTEP(step++, "Scenario mode: HOME-ROUTED");

    HSTEP(step++, "Connect gNB to HOME AMF [%s]", HOME_AMF_ADDR);
    ngap_home = testsctp_client(HOME_AMF_ADDR, OGS_NGAP_SCTP_PORT);
    ABTS_PTR_NOTNULL(tc, ngap_home);

    HSTEP(step++, "Connect gNB to VISITED AMF [%s]", VISITED_AMF_ADDR);
    ngap_visited = testsctp_client(VISITED_AMF_ADDR, OGS_NGAP_SCTP_PORT);
    ABTS_PTR_NOTNULL(tc, ngap_visited);

    HSTEP(step++, "Connect gNB to UPF (GTP-U)");
    gtpu = test_gtpu_server(1, AF_INET);
    ABTS_PTR_NOTNULL(tc, gtpu);

    HSTEP(step++, "Switch UE/gNB context to HOME PLMN");
    set_access_context(HOME_MCC, HOME_MNC, HOME_MNC_LEN, HOME_TAC, test_ue, "HOME");

    HSTEP(step++, "NG Setup with HOME AMF");
    sendbuf = testngap_build_ng_setup_request(0x4000, 22);
    send_ngap_checked(tc, ngap_home, sendbuf, "HOME", "NGSetupRequest");
    recv_ngap_and_trace(tc, ngap_home, test_ue, "HOME", "NGSetupResponse");

    HSTEP(step++, "Insert subscriber profile in MongoDB");
    doc = test_db_new_hr_mobility_profile(test_ue);
    ABTS_PTR_NOTNULL(tc, doc);
    ABTS_INT_EQUAL(tc, OGS_OK, test_db_insert_ue(test_ue, doc));

    HSTEP(step++, "Attach to HOME PLMN and create first PDU session");
    run_initial_registration_flow(tc, ngap_home, test_ue, &step, "HOME");
    sess_home = run_pdu_session_and_get_ip(
            tc, ngap_home, gtpu, test_ue, 5, &step, "HOME");
    ABTS_PTR_NOTNULL(tc, sess_home);
    ABTS_INT_EQUAL(tc, true, sess_home->ue_ip.ipv4);
    home_ipv4 = sess_home->ue_ip.addr;
    log_ue_ipv4("HOME PLMN", sess_home);

    wait_for_enter("Press Enter to trigger AMF mobility to visited PLMN");

    HSTEP(step++, "Leave HOME PLMN without de-registration (keep PDU session)");
    run_context_release_only(tc, ngap_home, test_ue, &step, "HOME");

    HSTEP(step++, "Switch UE/gNB context to VISITED PLMN");
    set_access_context(
            VISITED_MCC, VISITED_MNC, VISITED_MNC_LEN, VISITED_TAC, test_ue,
            "VISITED");

    HSTEP(step++, "NG Setup with VISITED AMF");
    sendbuf = testngap_build_ng_setup_request(0x4001, 22);
    send_ngap_checked(tc, ngap_visited, sendbuf, "VISITED", "NGSetupRequest");
    recv_ngap_and_trace(tc, ngap_visited, test_ue, "VISITED", "NGSetupResponse");

    HSTEP(step++, "Trigger mobility registration update in VISITED AMF");
    run_mobility_update_flow(tc, ngap_visited, test_ue, sess_home, &step);

    run_existing_pdu_ping(tc, gtpu, sess_home, &step, "VISITED");
    ABTS_INT_EQUAL(tc, true, sess_home->ue_ip.ipv4);
    ABTS_INT_EQUAL(tc, home_ipv4, sess_home->ue_ip.addr);
    log_ue_ipv4("VISITED PLMN", sess_home);

    HSTEP(step++, "Verified same UE IPv4 is kept after mobility");

    wait_for_enter("Press Enter to start disconnect from visited PLMN");

    HSTEP(step++, "Disconnect from VISITED PLMN");
    run_disconnect_flow(tc, ngap_visited, test_ue, &step, "VISITED");

    HSTEP(step++, "Cleanup");
    ABTS_INT_EQUAL(tc, OGS_OK, test_db_remove_ue(test_ue));
    if (sess_home)
        test_sess_remove(sess_home);
    testgnb_gtpu_close(gtpu);
    testgnb_ngap_close(ngap_visited);
    testgnb_ngap_close(ngap_home);
    test_ue_remove(test_ue);
}

abts_suite *test_manual_hr_mobility_keep_pdu(abts_suite *suite)
{
    suite = ADD_SUITE(suite)

    abts_run_test(suite, test1_func, NULL);

    return suite;
}
