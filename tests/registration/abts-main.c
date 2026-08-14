/*
 * Copyright (C) 2019,2020 by Sukchan Lee <acetcom@gmail.com>
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

#include "test-app.h"

#include <stdlib.h>

#ifndef TOPSSIM_BUILD_DIR
#define TOPSSIM_BUILD_DIR "build/topssim"
#endif

abts_suite *test_simple(abts_suite *suite);
abts_suite *test_manual_lbo_detach_reattach(abts_suite *suite);
abts_suite *test_manual_lbo_detach_reattach_enhanced(abts_suite *suite);
abts_suite *test_manual_hr_detach_reattach(abts_suite *suite);
abts_suite *test_guti(abts_suite *suite);
abts_suite *test_auth(abts_suite *suite);
abts_suite *test_ecc(abts_suite *suite);
abts_suite *test_idle(abts_suite *suite);
abts_suite *test_dereg(abts_suite *suite);
abts_suite *test_identity(abts_suite *suite);
abts_suite *test_gmm_status(abts_suite *suite);
abts_suite *test_ue_context(abts_suite *suite);
abts_suite *test_reset(abts_suite *suite);
abts_suite *test_multi_ue(abts_suite *suite);
abts_suite *test_crash(abts_suite *suite);

const struct testlist {
    abts_suite *(*func)(abts_suite *suite);
} alltests[] = {
    {test_simple},
    {test_guti},
    {test_auth},
    {test_ecc},
    {test_idle},
    {test_dereg},
    {test_identity},
    {test_gmm_status},
    {test_ue_context},
    {test_reset},
    {test_multi_ue},
#if 0 /* Since there is error LOG, we disabled the following test */
    {test_crash},
#endif
    {NULL},
};

#define MANUAL_LBO_DETACH_REATTACH_TEST_NAME "manual-lbo-detach-reattach-test"
#define MANUAL_LBO_DETACH_REATTACH_ENHANCED_TEST_NAME \
    "manual-lbo-detach-reattach-enhanced-test"
#define MANUAL_HR_DETACH_REATTACH_TEST_NAME "manual-hr-detach-reattach-test"

static int is_manual_test_name(const char *arg)
{
    if (!arg)
        return 0;

    if (!strcmp(arg, MANUAL_LBO_DETACH_REATTACH_TEST_NAME))
        return 1;
    if (!strcmp(arg, MANUAL_LBO_DETACH_REATTACH_ENHANCED_TEST_NAME))
        return 1;
    if (!strcmp(arg, MANUAL_HR_DETACH_REATTACH_TEST_NAME))
        return 1;

    return 0;
}

static int run_manual_test_requested(
        int argc, const char *const argv[], const char *test_name)
{
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], test_name))
            return 1;
    }

    return 0;
}

static void terminate(void)
{
    ogs_msleep(50);

    test_child_terminate();
    app_terminate();

    test_5gc_final();

    ogs_app_terminate();
}

static int test_udm_context_parse_config(void)
{
    int rv;
    yaml_document_t *document = NULL;
    ogs_yaml_iter_t root_iter;

    document = ogs_app()->document;
    ogs_assert(document);

    ogs_yaml_iter_init(&root_iter, document);
    while (ogs_yaml_iter_next(&root_iter)) {
        const char *root_key = ogs_yaml_iter_key(&root_iter);
        ogs_assert(root_key);
        if (!strcmp(root_key, "udm")) {
            ogs_yaml_iter_t udm_iter;
            ogs_yaml_iter_recurse(&root_iter, &udm_iter);
            while (ogs_yaml_iter_next(&udm_iter)) {
                const char *udm_key = ogs_yaml_iter_key(&udm_iter);
                ogs_assert(udm_key);
                if (!strcmp(udm_key, "sbi")) {
                    /* handle config in sbi library */
                } else if (!strcmp(udm_key, "service_name")) {
                    /* handle config in sbi library */
                } else if (!strcmp(udm_key, "discovery")) {
                    /* handle config in sbi library */
                } else if (!strcmp(udm_key, "hnet")) {
                    rv = ogs_sbi_context_parse_hnet_config(&udm_iter);
                    if (rv != OGS_OK) return rv;
                } else
                    ogs_warn("unknown key `%s`", udm_key);
            }
        }
    }

    return OGS_OK;
}

static void initialize(const char *const argv[])
{
    int rv;

    rv = ogs_app_initialize(NULL, NULL, argv);
    ogs_assert(rv == OGS_OK);

    test_5gc_init();

    ogs_assert(OGS_OK == test_udm_context_parse_config());

    rv = app_initialize(argv);
    ogs_assert(rv == OGS_OK);
}

int main(int argc, const char *const argv[])
{
    int i;
    int filtered_argc = 0;
    const char *filtered_argv[256];
    abts_suite *suite = NULL;
    int run_manual_lbo_detach_reattach = run_manual_test_requested(
            argc, argv, MANUAL_LBO_DETACH_REATTACH_TEST_NAME);
    int run_manual_lbo_detach_reattach_enhanced = run_manual_test_requested(
            argc, argv, MANUAL_LBO_DETACH_REATTACH_ENHANCED_TEST_NAME);
    int run_manual_hr_detach_reattach = run_manual_test_requested(
            argc, argv, MANUAL_HR_DETACH_REATTACH_TEST_NAME);
    int run_any_manual = run_manual_lbo_detach_reattach ||
            run_manual_lbo_detach_reattach_enhanced ||
            run_manual_hr_detach_reattach;

    /*
     * Remove manual scenario names from argv before abts_main().
     * Otherwise APR/abts may treat them as a test-name filter and skip suites.
     */
    for (i = 0; i < argc && filtered_argc < (int)(sizeof(filtered_argv)/sizeof(filtered_argv[0])) - 1; i++) {
        if (i > 0 && is_manual_test_name(argv[i]))
            continue;
        filtered_argv[filtered_argc++] = argv[i];
    }
    filtered_argv[filtered_argc] = NULL;

    atexit(terminate);
    if (run_manual_lbo_detach_reattach_enhanced) {
        setenv("TOPSSIM_SDM_CF", "1", 1);
        setenv("TOPSSIM_SDM_CACHE_DIR", TOPSSIM_BUILD_DIR "/sdm-cache", 1);
        setenv("TOPSSIM_SDM_CF_PLMN", "00102", 1);
    }
    test_app_run(filtered_argc, filtered_argv, "sample.yaml", initialize);

    if (!run_any_manual) {
        for (i = 0; alltests[i].func; i++)
            suite = alltests[i].func(suite);
    }

    if (run_manual_lbo_detach_reattach)
        suite = test_manual_lbo_detach_reattach(suite);
    if (run_manual_lbo_detach_reattach_enhanced)
        suite = test_manual_lbo_detach_reattach_enhanced(suite);
    if (run_manual_hr_detach_reattach)
        suite = test_manual_hr_detach_reattach(suite);

    return abts_report(suite);
}
