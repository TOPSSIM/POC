/*
 * Copyright (C) 2019-2023 by Sukchan Lee <acetcom@gmail.com>
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

#include "context.h"

static nrf_context_t self;

int __nrf_log_domain;

static OGS_POOL(nrf_assoc_pool, nrf_assoc_t);
static OGS_POOL(nrf_disc_cache_pool, nrf_disc_cache_entry_t);

static int context_initialized = 0;

static int max_num_of_nrf_assoc = 0;
static int max_num_of_nrf_disc_cache = 0;

static void nrf_disc_cache_entry_clear(nrf_disc_cache_entry_t *entry)
{
    int i;

    ogs_assert(entry);

    if (entry->key) {
        ogs_free(entry->key);
        entry->key = NULL;
    }

    if (entry->nf_instance_id) {
        for (i = 0; i < entry->num_nf_instance_id; i++) {
            if (entry->nf_instance_id[i])
                ogs_free(entry->nf_instance_id[i]);
        }
        ogs_free(entry->nf_instance_id);
        entry->nf_instance_id = NULL;
    }

    entry->num_nf_instance_id = 0;
    entry->expires_at = 0;
}

static void nrf_disc_cache_remove(nrf_disc_cache_entry_t *entry)
{
    ogs_assert(entry);

    ogs_list_remove(&self.disc_cache_list, entry);
    nrf_disc_cache_entry_clear(entry);
    ogs_pool_free(&nrf_disc_cache_pool, entry);
}

static void nrf_disc_cache_remove_expired(void)
{
    ogs_time_t now = ogs_get_monotonic_time();
    nrf_disc_cache_entry_t *entry = NULL, *next_entry = NULL;

    ogs_list_for_each_safe(&self.disc_cache_list, next_entry, entry) {
        if (entry->expires_at <= now)
            nrf_disc_cache_remove(entry);
    }
}

void nrf_context_init(void)
{
    ogs_assert(context_initialized == 0);

    /* Initialize NRF context */
    memset(&self, 0, sizeof(nrf_context_t));

    ogs_log_install_domain(&__nrf_log_domain, "nrf", ogs_core()->log.level);

#define MAX_NUM_OF_NRF_ASSOC 8
    max_num_of_nrf_assoc = ogs_global_conf()->max.ue * MAX_NUM_OF_NRF_ASSOC;
    ogs_pool_init(&nrf_assoc_pool, max_num_of_nrf_assoc);

#define MAX_NUM_OF_NRF_DISC_CACHE 32
    max_num_of_nrf_disc_cache =
        ogs_global_conf()->max.ue * MAX_NUM_OF_NRF_DISC_CACHE;
    ogs_pool_init(&nrf_disc_cache_pool, max_num_of_nrf_disc_cache);

    context_initialized = 1;
}

void nrf_context_final(void)
{
    ogs_sbi_nf_instance_t *nf_instance = NULL, *next_nf_instance = NULL;

    ogs_assert(context_initialized == 1);

    ogs_list_for_each_safe(
            &ogs_sbi_self()->nf_instance_list, next_nf_instance, nf_instance) {
        if (NF_INSTANCE_TYPE_IS_NRF(nf_instance))
            continue;
        if (OGS_FSM_STATE(&nf_instance->sm))
            nrf_nf_fsm_fini(nf_instance);
    }

    nrf_assoc_remove_all();
    nrf_disc_cache_remove_all();

    ogs_pool_final(&nrf_assoc_pool);
    ogs_pool_final(&nrf_disc_cache_pool);

    context_initialized = 0;
}

nrf_context_t *nrf_self(void)
{
    return &self;
}

static int nrf_context_prepare(void)
{
    /* NF Instance Heartbeat
     * Default value is 10 seconds if it is not configured in nrf.yaml */
    if (!ogs_local_conf()->time.nf_instance.heartbeat_interval)
        ogs_local_conf()->time.nf_instance.heartbeat_interval = 10;

    return OGS_OK;
}

static int nrf_context_validation(void)
{
    return OGS_OK;
}

int nrf_context_parse_config(void)
{
    int rv;
    yaml_document_t *document = NULL;
    ogs_yaml_iter_t root_iter;

    document = ogs_app()->document;
    ogs_assert(document);

    rv = nrf_context_prepare();
    if (rv != OGS_OK) return rv;

    ogs_yaml_iter_init(&root_iter, document);
    while (ogs_yaml_iter_next(&root_iter)) {
        const char *root_key = ogs_yaml_iter_key(&root_iter);
        ogs_assert(root_key);
        if (!strcmp(root_key, "time")) {
            ogs_yaml_iter_t time_iter;
            ogs_yaml_iter_recurse(&root_iter, &time_iter);
            while (ogs_yaml_iter_next(&time_iter)) {
                const char *time_key = ogs_yaml_iter_key(&time_iter);
                ogs_assert(time_key);
                if (!strcmp(time_key, "nf_instance")) {
                    ogs_yaml_iter_t sbi_iter;
                    ogs_yaml_iter_recurse(&time_iter, &sbi_iter);

                    while (ogs_yaml_iter_next(&sbi_iter)) {
                        const char *sbi_key =
                            ogs_yaml_iter_key(&sbi_iter);
                        ogs_assert(sbi_key);

                        if (!strcmp(sbi_key, "heartbeat")) {
                            const char *v = ogs_yaml_iter_value(&sbi_iter);
                            if (v) ogs_local_conf()->time.nf_instance.
                                    heartbeat_interval = atoi(v);
                        }
                    }
                }
            }
        }
    }

    rv = nrf_context_validation();
    if (rv != OGS_OK) return rv;

    return OGS_OK;
}

nrf_assoc_t *nrf_assoc_add(ogs_sbi_stream_t *stream)
{
    nrf_assoc_t *assoc = NULL;

    ogs_assert(stream);

    ogs_pool_alloc(&nrf_assoc_pool, &assoc);
    if (!assoc) {
        ogs_error("Maximum number of association[%d] reached",
                    max_num_of_nrf_assoc);
        return NULL;
    }
    memset(assoc, 0, sizeof *assoc);

    assoc->stream = stream;

    ogs_list_add(&self.assoc_list, assoc);

    return assoc;
}

void nrf_assoc_remove(nrf_assoc_t *assoc)
{
    ogs_assert(assoc);

    ogs_list_remove(&self.assoc_list, assoc);

    if (assoc->disc_cache_key)
        ogs_free(assoc->disc_cache_key);

    ogs_pool_free(&nrf_assoc_pool, assoc);
}

void nrf_assoc_remove_all(void)
{
    nrf_assoc_t *assoc = NULL, *next_assoc = NULL;

    ogs_list_for_each_safe(&self.assoc_list, next_assoc, assoc)
        nrf_assoc_remove(assoc);
}

nrf_disc_cache_entry_t *nrf_disc_cache_find(const char *key)
{
    nrf_disc_cache_entry_t *entry = NULL;

    if (!key)
        return NULL;

    nrf_disc_cache_remove_expired();

    ogs_list_for_each(&self.disc_cache_list, entry) {
        if (entry->key && !strcmp(entry->key, key))
            return entry;
    }

    return NULL;
}

void nrf_disc_cache_update(
        const char *key, OpenAPI_search_result_t *SearchResult, int max_age)
{
    nrf_disc_cache_entry_t *entry = NULL;
    OpenAPI_lnode_t *node = NULL;
    int count = 0;
    int i = 0;

    if (!key)
        return;

    if (max_age <= 0)
        max_age = ogs_local_conf()->time.nf_instance.validity_duration;
    if (max_age <= 0)
        max_age = 1;

    entry = nrf_disc_cache_find(key);
    if (!entry) {
        ogs_pool_alloc(&nrf_disc_cache_pool, &entry);
        if (!entry) {
            ogs_error("Maximum number of discovery cache[%d] reached",
                    max_num_of_nrf_disc_cache);
            return;
        }
        memset(entry, 0, sizeof(*entry));
        ogs_list_add(&self.disc_cache_list, entry);
    } else {
        nrf_disc_cache_entry_clear(entry);
    }

    entry->key = ogs_strdup(key);
    ogs_assert(entry->key);
    entry->expires_at = ogs_get_monotonic_time() + ogs_time_from_sec(max_age);

    if (!SearchResult || !SearchResult->nf_instances)
        return;

    count = SearchResult->nf_instances->count;
    if (count <= 0)
        return;

    entry->nf_instance_id = ogs_calloc(count, sizeof(char *));
    ogs_assert(entry->nf_instance_id);

    OpenAPI_list_for_each(SearchResult->nf_instances, node) {
        OpenAPI_nf_profile_t *NFProfile = node->data;

        if (!NFProfile || !NFProfile->nf_instance_id)
            continue;

        entry->nf_instance_id[i] = ogs_strdup(NFProfile->nf_instance_id);
        ogs_assert(entry->nf_instance_id[i]);
        i++;
    }

    if (i == 0) {
        ogs_free(entry->nf_instance_id);
        entry->nf_instance_id = NULL;
        return;
    }

    if (i != count) {
        char **nf_instance_id = ogs_realloc(entry->nf_instance_id,
                i * sizeof(char *));
        ogs_assert(nf_instance_id);
        entry->nf_instance_id = nf_instance_id;
    }

    entry->num_nf_instance_id = i;
}

void nrf_disc_cache_remove_all(void)
{
    nrf_disc_cache_entry_t *entry = NULL, *next_entry = NULL;

    ogs_list_for_each_safe(&self.disc_cache_list, next_entry, entry)
        nrf_disc_cache_remove(entry);
}
