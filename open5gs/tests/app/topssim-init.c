/*
 * TOPSSIM proof-of-concept test application initializer.
 *
 * This starts the normal Open5GS 5GC test application plus a Python TOPSSIM
 * sidecar used for manual prediction triggers and preparation artifacts.
 */

#include "test-app.h"

#include <stdlib.h>

#ifndef TOPSSIM_PYTHON
#define TOPSSIM_PYTHON "python3"
#endif

#ifndef TOPSSIM_SOURCE_DIR
#define TOPSSIM_SOURCE_DIR "."
#endif

#ifndef TOPSSIM_BUILD_DIR
#define TOPSSIM_BUILD_DIR "build/topssim"
#endif

/* If want to increase this number, check and modify run_threads() function
 * for static integer to string conversion */
#define OGS_MAX_NF_INSTANCES        4

static ogs_thread_t *topssim_thread = NULL;
static ogs_proc_t topssim_proc;
static bool topssim_proc_started = false;

static ogs_thread_t *nrf_thread = NULL;
static ogs_thread_t *scp_thread = NULL;
static ogs_thread_t *sepp_thread = NULL;
static ogs_thread_t *upf_threads[OGS_MAX_NF_INSTANCES] = { NULL };
static ogs_thread_t *smf_threads[OGS_MAX_NF_INSTANCES] = { NULL };
static ogs_thread_t *amf_threads[OGS_MAX_NF_INSTANCES] = { NULL };
static ogs_thread_t *ausf_threads[OGS_MAX_NF_INSTANCES] = { NULL };
static ogs_thread_t *udm_threads[OGS_MAX_NF_INSTANCES] = { NULL };
static ogs_thread_t *pcf_threads[OGS_MAX_NF_INSTANCES] = { NULL };
static ogs_thread_t *nssf_threads[OGS_MAX_NF_INSTANCES] = { NULL };
static ogs_thread_t *bsf_threads[OGS_MAX_NF_INSTANCES] = { NULL };
static ogs_thread_t *udr_threads[OGS_MAX_NF_INSTANCES] = { NULL };

static void topssim_sidecar_main(void *data)
{
    const char *commandLine[8];
    FILE *out = NULL;
    char buf[OGS_HUGE_LEN];
    char script[OGS_MAX_FILEPATH_LEN];
    int ret = 0, out_return_code = 0;

    ogs_assert(data == NULL);

    ogs_snprintf(script, sizeof script, "%s%s%s%s%s",
            TOPSSIM_SOURCE_DIR, OGS_DIR_SEPARATOR_S,
            "topssim", OGS_DIR_SEPARATOR_S, "poc/sidecar.py");

    commandLine[0] = TOPSSIM_PYTHON;
    commandLine[1] = script;
    commandLine[2] = "--state-dir";
    commandLine[3] = TOPSSIM_BUILD_DIR;
    commandLine[4] = NULL;

    memset(&topssim_proc, 0, sizeof topssim_proc);
    ret = ogs_proc_create(commandLine,
            ogs_proc_option_combined_stdout_stderr|
            ogs_proc_option_inherit_environment,
            &topssim_proc);
    if (ret != 0) {
        ogs_error("TOPSSIM sidecar failed to start");
        return;
    }

    topssim_proc_started = true;
    out = ogs_proc_stdout(&topssim_proc);
    ogs_assert(out);

    while(fgets(buf, OGS_HUGE_LEN, out)) {
        printf("%s", buf);
    }

    ret = ogs_proc_join(&topssim_proc, &out_return_code);
    if (ret != 0)
        ogs_error("TOPSSIM sidecar join failed");
    else if (out_return_code != 0)
        ogs_warn("TOPSSIM sidecar exited with code [%d]", out_return_code);

    ret = ogs_proc_destroy(&topssim_proc);
    if (ret != 0)
        ogs_error("TOPSSIM sidecar destroy failed");

    topssim_proc_started = false;
}

static void run_threads(const char *nf_name, int count,
        const char *argv_out[], int argv_out_idx, ogs_thread_t *threads[])
{
    int i;

    threads[0] = test_child_create(nf_name, 0, argv_out);

    for (i = 1; i < count; i++) {
        const char *idx_string = NULL;;

        switch (i) {
            case 1: idx_string = "1"; break;
            case 2: idx_string = "2"; break;
            case 3: idx_string = "3"; break;
            default:
                idx_string = ogs_msprintf("%d", i);
                ogs_warn("Missing static conversion of integer to string");
                break;
        }
        ogs_assert(idx_string);

        argv_out[argv_out_idx + 0] = "-k";
        argv_out[argv_out_idx + 1] = idx_string;
        argv_out[argv_out_idx + 2] = NULL;

        threads[i] = test_child_create(nf_name, i, argv_out);
    }

    argv_out[argv_out_idx] = NULL;
}

int app_initialize(const char *const argv[])
{
    const char *argv_out[OGS_ARG_MAX];
    bool user_config = false;
    int i = 0;

    for (i = 0; argv[i] && i < OGS_ARG_MAX-3; i++) {
        if (strcmp("-c", argv[i]) == 0) {
            user_config = true;
        }
        argv_out[i] = argv[i];
    }
    argv_out[i] = NULL;

    if (!user_config) {
        argv_out[i++] = "-c";
        argv_out[i++] = DEFAULT_CONFIG_FILENAME;
        argv_out[i] = NULL;
    }

    setenv("TOPSSIM_SDM_CF", "1", 1);
    setenv("TOPSSIM_SDM_CACHE_DIR", TOPSSIM_BUILD_DIR "/sdm-cache", 1);
    setenv("TOPSSIM_SDM_CF_PLMN", "00102", 1);

    topssim_thread = ogs_thread_create(topssim_sidecar_main, NULL);
    ogs_msleep(500);

    if (ogs_global_conf()->parameter.no_nrf == 0)
        nrf_thread = test_child_create("nrf", 0, argv_out);
    if (ogs_global_conf()->parameter.no_scp == 0)
        scp_thread = test_child_create("scp", 0, argv_out);
    if (ogs_global_conf()->parameter.no_sepp == 0)
        sepp_thread = test_child_create("sepp", 0, argv_out);

    if (ogs_global_conf()->parameter.no_upf == 0)
        run_threads("upf", ogs_global_conf()->parameter.upf_count,
                argv_out, i, upf_threads);
    if (ogs_global_conf()->parameter.no_smf == 0)
        run_threads("smf", ogs_global_conf()->parameter.smf_count,
                argv_out, i, smf_threads);
    if (ogs_global_conf()->parameter.no_amf == 0)
        run_threads("amf", ogs_global_conf()->parameter.amf_count,
                argv_out, i, amf_threads);
    if (ogs_global_conf()->parameter.no_ausf == 0)
        run_threads("ausf", ogs_global_conf()->parameter.ausf_count,
                argv_out, i, ausf_threads);
    if (ogs_global_conf()->parameter.no_udm == 0)
        run_threads("udm", ogs_global_conf()->parameter.udm_count,
                argv_out, i, udm_threads);
    if (ogs_global_conf()->parameter.no_pcf == 0)
        run_threads("pcf", ogs_global_conf()->parameter.pcf_count,
                argv_out, i, pcf_threads);
    if (ogs_global_conf()->parameter.no_nssf == 0)
        run_threads("nssf", ogs_global_conf()->parameter.nssf_count,
                argv_out, i, nssf_threads);
    if (ogs_global_conf()->parameter.no_bsf == 0)
        run_threads("bsf", ogs_global_conf()->parameter.bsf_count,
                argv_out, i, bsf_threads);
    if (ogs_global_conf()->parameter.no_udr == 0)
        run_threads("udr", ogs_global_conf()->parameter.udr_count,
                argv_out, i, udr_threads);

    ogs_msleep(3000);

    return OGS_OK;
}

void app_terminate(void)
{
    int i;

    for (i = 0; i < OGS_MAX_NF_INSTANCES; i++) {
        if (amf_threads[i]) {
            ogs_thread_destroy(amf_threads[i]);
            amf_threads[i] = NULL;
        }
        if (smf_threads[i]) {
            ogs_thread_destroy(smf_threads[i]);
            smf_threads[i] = NULL;
        }
        if (upf_threads[i]) {
            ogs_thread_destroy(upf_threads[i]);
            upf_threads[i] = NULL;
        }
        if (udr_threads[i]) {
            ogs_thread_destroy(udr_threads[i]);
            udr_threads[i] = NULL;
        }
        if (nssf_threads[i]) {
            ogs_thread_destroy(nssf_threads[i]);
            nssf_threads[i] = NULL;
        }
        if (bsf_threads[i]) {
            ogs_thread_destroy(bsf_threads[i]);
            bsf_threads[i] = NULL;
        }
        if (pcf_threads[i]) {
            ogs_thread_destroy(pcf_threads[i]);
            pcf_threads[i] = NULL;
        }
        if (udm_threads[i]) {
            ogs_thread_destroy(udm_threads[i]);
            udm_threads[i] = NULL;
        }
        if (ausf_threads[i]) {
            ogs_thread_destroy(ausf_threads[i]);
            ausf_threads[i] = NULL;
        }
    }
    if (sepp_thread) {
        ogs_thread_destroy(sepp_thread);
        sepp_thread = NULL;
    }
    if (scp_thread) {
        ogs_thread_destroy(scp_thread);
        scp_thread = NULL;
    }
    if (nrf_thread) {
        ogs_thread_destroy(nrf_thread);
        nrf_thread = NULL;
    }
    if (topssim_proc_started)
        ogs_proc_terminate(&topssim_proc);
    if (topssim_thread) {
        ogs_thread_destroy(topssim_thread);
        topssim_thread = NULL;
    }
}

void test_topssim_init(void)
{
    ogs_log_install_domain(&__ogs_sctp_domain, "sctp", OGS_LOG_ERROR);
    ogs_log_install_domain(&__ogs_ngap_domain, "ngap", OGS_LOG_ERROR);
    ogs_log_install_domain(&__ogs_dbi_domain, "dbi", OGS_LOG_ERROR);
    ogs_log_install_domain(&__ogs_nas_domain, "nas", OGS_LOG_ERROR);
    ogs_log_install_domain(&__ogs_gtp_domain, "gtp", OGS_LOG_ERROR);
    ogs_log_install_domain(&__ogs_sbi_domain, "sbi", OGS_LOG_ERROR);

    ogs_sctp_init(ogs_app()->usrsctp.udp_port);
    if (!test_app_no_dbi())
        ogs_assert(ogs_dbi_init(ogs_app()->db_uri) == OGS_OK);
}

void test_topssim_final(void)
{
    if (!test_app_no_dbi())
        ogs_dbi_final();
    ogs_sctp_final();

    test_context_final();
}
