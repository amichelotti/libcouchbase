/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2010-2013 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

/**
 * This file contains the functions to create / destroy the libcouchbase instance
 *
 * @author Trond Norbye
 * @todo add more documentation
 */
#include "internal.h"
#ifndef _WIN32
#include <dlfcn.h>
#endif
#include "logging.h"

#include "hostlist.h"
#include "bucketconfig/clconfig.h"
#include <lcbio/iotable.h>

/**
 * Get the version of the library.
 *
 * @param version where to store the numeric representation of the
 *         version (or NULL if you don't care)
 *
 * @return the textual description of the version ('\0'
 *          terminated). Do <b>not</b> try to release this string.
 *
 */
static volatile unsigned int lcb_instance_index = 0;

LIBCOUCHBASE_API
const char *lcb_get_version(lcb_uint32_t *version)
{
    if (version != NULL) {
        *version = (lcb_uint32_t)LCB_VERSION;
    }

    return LCB_VERSION_STRING;
}

#define PARAM_CONFIG_HOST 1
#define PARAM_CONFIG_PORT 2
static const char *param_from_host(const lcb_host_t *host, int type)
{
    if (!host) {
        return NULL;
    }
    if (type == PARAM_CONFIG_HOST) {
        return *host->host ? host->host : NULL;
    } else {
        return *host->port ? host->port : NULL;
    }
}

static const char *get_rest_param(lcb_t obj, int paramtype)
{
    const char *ret = NULL;
    const lcb_host_t *host = lcb_confmon_get_rest_host(obj->confmon);
    ret = param_from_host(host, paramtype);

    if (ret) {
        return ret;
    }

    /** Don't have a REST API connection? */
    if (LCBT_VBCONFIG(obj)) {
        lcb_server_t *server = LCBT_GET_SERVER(obj, 0);
        if (paramtype == PARAM_CONFIG_HOST) {
            ret = param_from_host(&server->curhost, paramtype);
            if (ret) {
                return ret;
            }
        } else {
            char *colon = strstr(server->resthost, ":");
            if (colon) {
                if (obj->scratch) {
                    free(obj->scratch);
                }
                obj->scratch = malloc(NI_MAXSERV + 1);
                strcpy(obj->scratch, colon+1);
                if (*obj->scratch) {
                    return obj->scratch;
                }
            }
        }
    }
    return param_from_host(obj->usernodes->entries, paramtype);
}

LIBCOUCHBASE_API
const char *lcb_get_host(lcb_t instance)
{
    return get_rest_param(instance, PARAM_CONFIG_HOST);
}

LIBCOUCHBASE_API
const char *lcb_get_port(lcb_t instance)
{
    return get_rest_param(instance, PARAM_CONFIG_PORT);
}


LIBCOUCHBASE_API
lcb_int32_t lcb_get_num_replicas(lcb_t instance)
{
    if (LCBT_VBCONFIG(instance)) {
        return instance->nreplicas;
    } else {
        return -1;
    }
}

LIBCOUCHBASE_API
lcb_int32_t lcb_get_num_nodes(lcb_t instance)
{
    if (LCBT_VBCONFIG(instance)) {
        return LCBT_NSERVERS(instance);
    } else {
        return -1;
    }
}

/**
 * Return a NULL-terminated list of servers (host:port) for the entire cluster.
 */
LIBCOUCHBASE_API
const char *const *lcb_get_server_list(lcb_t instance)
{
    hostlist_ensure_strlist(instance->usernodes);
    return (const char * const * )instance->usernodes->slentries;
}



static const char *get_nonempty_string(const char *s)
{
    if (s != NULL && strlen(s) == 0) {
        return NULL;
    }
    return s;
}


/**
 * Associate a "cookie" with an instance of libcouchbase. You may only store
 * <b>one</b> cookie with each instance of libcouchbase.
 *
 * @param instance the instance to associate the cookie with
 * @param cookie the cookie to associate with this instance.
 *
 * @author Trond Norbye
 */
LIBCOUCHBASE_API
void lcb_set_cookie(lcb_t instance, const void *cookie)
{
    instance->cookie = cookie;
}

/**
 * Get the cookie associated with a given instance of libcouchbase.
 *
 * @param instance the instance to query
 * @return The cookie associated with this instance.
 *
 * @author Trond Norbye
 */
LIBCOUCHBASE_API
const void *lcb_get_cookie(lcb_t instance)
{
    return instance->cookie;
}


lcb_error_t lcb_init_providers(lcb_t obj,
                               const struct lcb_create_st2 *e_options)
{
    hostlist_t mc_nodes;
    lcb_error_t err;
    const char *hosts;
    int http_enabled = 1;
    int cccp_enabled = 1;

    clconfig_provider *http =
            lcb_confmon_get_provider(obj->confmon, LCB_CLCONFIG_HTTP);

    clconfig_provider *cccp =
            lcb_confmon_get_provider(obj->confmon, LCB_CLCONFIG_CCCP);


    if (e_options->transports) {
        int cccp_found = 0;
        int http_found = 0;
        const lcb_config_transport_t *cur;

        for (cur = e_options->transports;
                *cur != LCB_CONFIG_TRANSPORT_LIST_END; cur++) {
            if (*cur == LCB_CONFIG_TRANSPORT_CCCP) {
                cccp_found = 1;
            } else if (*cur == LCB_CONFIG_TRANSPORT_HTTP) {
                http_found = 1;
            } else {
                return LCB_EINVAL;
            }
        }

        if (http_found || cccp_found) {
            cccp_enabled = cccp_found;
            http_enabled = http_found;
        }
    }

    if (lcb_getenv_boolean("LCB_NO_CCCP")) {
        cccp_enabled = 0;
    }

    if (lcb_getenv_boolean("LCB_NO_HTTP")) {
        http_enabled = 0;
    }

    /** The only way we can get to here is if one of the vars are set */
    if (cccp_enabled == 0 && http_enabled == 0) {
        return LCB_BAD_ENVIRONMENT;
    }

    if (http_enabled) {
        lcb_clconfig_http_enable(http);
        lcb_clconfig_http_set_nodes(http, obj->usernodes);
    } else {
        lcb_confmon_set_provider_active(obj->confmon, LCB_CLCONFIG_HTTP, 0);
    }

    if (!cccp_enabled) {
        lcb_confmon_set_provider_active(obj->confmon, LCB_CLCONFIG_CCCP, 0);
        return LCB_SUCCESS;
    }

    hosts = get_nonempty_string(e_options->mchosts);
    mc_nodes = hostlist_create();

    if (!mc_nodes) {
        return LCB_CLIENT_ENOMEM;
    }

    if (hosts) {
        err = hostlist_add_stringz(mc_nodes, hosts, LCB_CONFIG_MCD_PORT);
        if (err != LCB_SUCCESS) {
            hostlist_destroy(mc_nodes);
            return err;
        }

    } else {
        lcb_size_t ii;
        for (ii = 0; ii < obj->usernodes->nentries; ii++) {
            lcb_host_t *cur = obj->usernodes->entries + ii;
            hostlist_add_stringz(mc_nodes, cur->host, LCB_CONFIG_MCD_PORT);
        }
    }

    lcb_clconfig_cccp_enable(cccp, obj);
    lcb_clconfig_cccp_set_nodes(cccp, mc_nodes);
    hostlist_destroy(mc_nodes);
    return LCB_SUCCESS;
}

static lcb_error_t normalize_options(struct lcb_create_st *myopts,
                                     const struct lcb_create_st *useropts)
{
    lcb_size_t to_copy;
    memset(myopts, 0, sizeof(*myopts));

    if (useropts == NULL) {
        return LCB_SUCCESS;
    }

    if (useropts->version < 0 || useropts->version > 2) {
        return LCB_EINVAL;
    }

    if (useropts->version == 0) {
        to_copy = sizeof(struct lcb_create_st0);
    } else if (useropts->version == 1) {
        to_copy = sizeof(struct lcb_create_st1);
    } else if (useropts->version == 2) {
        to_copy = sizeof(struct lcb_create_st2);
    } else {
        return LCB_EINVAL;
    }

    memcpy(&myopts->v, &useropts->v, to_copy);
    return LCB_SUCCESS;
}

LIBCOUCHBASE_API
lcb_error_t lcb_create(lcb_t *instance,
                       const struct lcb_create_st *options)
{
    const char *host = NULL;
    const char *user = NULL;
    const char *passwd = NULL;
    const char *bucket = NULL;

    struct lcb_io_opt_st *io_priv = NULL;
    struct lcb_create_st options_container;
    struct lcb_create_st2 *e_options = &options_container.v.v2;

    lcb_type_t type = LCB_TYPE_BUCKET;
    lcb_t obj;
    lcb_error_t err;
    lcb_settings *settings;

    err = normalize_options(&options_container, options);

    if (err != LCB_SUCCESS) {
        return err;
    }

    host = get_nonempty_string(e_options->host);
    user = get_nonempty_string(e_options->user);
    passwd = get_nonempty_string(e_options->passwd);
    bucket = get_nonempty_string(e_options->bucket);
    io_priv = e_options->io;
    type = e_options->type;

    if (type == LCB_TYPE_CLUSTER && user == NULL && passwd == NULL) {
        return LCB_EINVAL;
    }

    if (host == NULL) {
        host = "localhost";
    }

    if (bucket == NULL) {
        bucket = "default";
    }

    /* Do not allow people use Administrator account for data access */
    if (type == LCB_TYPE_BUCKET && user && strcmp(user, bucket) != 0) {
        return LCB_INVALID_USERNAME;
    }

    if ((obj = calloc(1, sizeof(*obj))) == NULL) {
        return LCB_CLIENT_ENOMEM;
    }

    obj->type = type;
    obj->compat.type = (lcb_compat_t)0xdead;

    if (io_priv == NULL) {
        lcb_io_opt_t ops;
        if ((err = lcb_create_io_ops(&ops, NULL)) != LCB_SUCCESS) {
            /* You can't initialize the library without a io-handler! */
            free(obj);
            return err;
        }
        io_priv = ops;
        io_priv->v.v0.need_cleanup = 1;
    }

    obj->iotable = lcbio_table_new(io_priv);
    obj->settings = settings = lcb_settings_new();
    settings->bucket = strdup(bucket);
    settings->logger = lcb_init_console_logger();
    settings->iid = lcb_instance_index++;


    if (user) {
        settings->username = strdup(user);
    } else {
        settings->username = strdup(settings->bucket);
    }

    if (passwd) {
        settings->password = strdup(passwd);
    }

    lcb_initialize_packet_handlers(obj);
    obj->memd_sockpool = lcbio_mgr_create(settings, obj->iotable);
    obj->memd_sockpool->maxidle = 1;
    obj->memd_sockpool->tmoidle = 10000000;

    obj->confmon = lcb_confmon_create(settings, obj->iotable);
    obj->usernodes = hostlist_create();
    obj->retryq = lcb_retryq_new(&obj->cmdq, obj->iotable, obj->settings);

    /** We might want to sanitize this a bit more later on.. */
    if (strstr(host, "://") != NULL && strstr(host, "http://") == NULL) {
        lcb_destroy(obj);
        return LCB_INVALID_HOST_FORMAT;
    }


    err = hostlist_add_string(obj->usernodes, host, -1, LCB_CONFIG_HTTP_PORT);
    if (err != LCB_SUCCESS) {
        lcb_destroy(obj);
        return err;
    }

    err = lcb_init_providers(obj, e_options);
    if (err != LCB_SUCCESS) {
        lcb_destroy(obj);
        return err;
    }

    lcb_initialize_packet_handlers(obj);
    lcb_aspend_init(&obj->pendops);
    /* No error has occurred yet. */
    obj->last_error = LCB_SUCCESS;
    *instance = obj;
    return LCB_SUCCESS;
}

LIBCOUCHBASE_API
lcb_error_t lcb_create_compat(
        lcb_cluster_t type, const void *specific, lcb_t *instance,
        struct lcb_io_opt_st *io)
{
    struct lcb_create_st cst = { 0 };
    const struct lcb_cached_config_st *cfg = specific;
    const struct lcb_create_st *crp = &cfg->createopt;
    lcb_error_t err;
    lcb_size_t to_copy = 0;

    if (type != LCB_CACHED_CONFIG) {
        return LCB_NOT_SUPPORTED;
    }

    if (crp->version == 0) {
        to_copy = sizeof(cst.v.v0);
    } else if (crp->version == 1) {
        to_copy = sizeof(cst.v.v1);
    } else if (crp->version >= 2) {
        to_copy = sizeof(cst.v.v2);
    }
    memcpy(&cst, crp, to_copy);

    if (io) {
        cst.v.v0.io = io;
    }
    err = lcb_create(instance, &cst);
    if (err != LCB_SUCCESS) {
        return err;
    }
    err = lcb_cntl(*instance, LCB_CNTL_SET, LCB_CNTL_CONFIGCACHE,
                   (void *)cfg->cachefile);
    if (err != LCB_SUCCESS) {
        lcb_destroy(*instance);
    }
    return err;
}

LIBCOUCHBASE_API
void lcb_destroy(lcb_t instance)
{
    lcb_size_t ii;
    lcb_settings *settings = instance->settings;
    hashset_t hs;
    lcb_ASPEND *po = &instance->pendops;

    if (instance->cur_configinfo) {
        lcb_clconfig_decref(instance->cur_configinfo);
        instance->cur_configinfo = NULL;
    }
    instance->cmdq.config = NULL;

    lcb_bootstrap_destroy(instance);
    hostlist_destroy(instance->usernodes);
    if ((hs = lcb_aspend_get(po, LCB_PENDTYPE_TIMER))) {
        for (ii = 0; ii < hs->capacity; ++ii) {
            if (hs->items[ii] > 1) {
                lcb_timer_destroy(instance, (lcb_timer_t)hs->items[ii]);
            }
        }
    }

    if ((hs = lcb_aspend_get(po, LCB_PENDTYPE_DURABILITY))) {
        struct lcb_durability_set_st **dset_list;
        lcb_size_t nitems = hashset_num_items(hs);
        dset_list = (struct lcb_durability_set_st **)hashset_get_items(hs, NULL);
        if (dset_list) {
            for (ii = 0; ii < nitems; ii++) {
                lcb_durability_dset_destroy(dset_list[ii]);
            }
            free(dset_list);
        }
    }

    for (ii = 0; ii < LCBT_NSERVERS(instance); ++ii) {
        lcb_server_t *server = LCBT_GET_SERVER(instance, ii);
        mcserver_close(server);
    }

    if ((hs = lcb_aspend_get(po, LCB_PENDTYPE_HTTP))) {
        for (ii = 0; ii < hs->capacity; ++ii) {
            if (hs->items[ii] > 1) {
                lcb_http_request_t htreq = (lcb_http_request_t)hs->items[ii];

                /**
                 * We don't want to invoke callbacks *or* remove it from our
                 * hash table
                 */
                htreq->status |= LCB_HTREQ_S_CBINVOKED | LCB_HTREQ_S_HTREMOVED;

                /* we should figure out a better error code for this.. */
                lcb_http_request_finish(instance, htreq, LCB_ERROR);
            }
        }
    }

    lcb_retryq_destroy(instance->retryq);
    lcb_confmon_destroy(instance->confmon);
    lcbio_mgr_destroy(instance->memd_sockpool);
    lcbio_table_unref(instance->iotable);
    mcreq_queue_cleanup(&instance->cmdq);
    lcb_settings_unref(settings);
    lcb_aspend_cleanup(po);
    free(instance->histogram);
    free(instance->scratch);
    memset(instance, 0xff, sizeof(*instance));
    free(instance);
}

static void
destroy_cb(void *arg)
{
    lcb_t instance = arg;
    lcbio_timer_destroy(instance->dtor_timer);
    lcb_destroy(instance);
}

LIBCOUCHBASE_API
void
lcb_destroy_async(lcb_t instance, const void *arg)
{
    instance->dtor_timer = lcbio_timer_new(instance->iotable, instance, destroy_cb);
    instance->settings->dtorarg = (void *)arg;
    lcbio_async_signal(instance->dtor_timer);
}

LIBCOUCHBASE_API
lcb_error_t lcb_connect(lcb_t instance)
{
    return lcb_bootstrap_initial(instance);
}

LIBCOUCHBASE_API
void *lcb_mem_alloc(lcb_size_t size)
{
    return malloc(size);
}

LIBCOUCHBASE_API
void lcb_mem_free(void *ptr)
{
    free(ptr);
}

LCB_INTERNAL_API
void lcb_run_loop(lcb_t instance)
{
    IOT_START(instance->iotable);
}

LCB_INTERNAL_API
void lcb_stop_loop(lcb_t instance)
{
    IOT_STOP(instance->iotable);
}

void
lcb_aspend_init(lcb_ASPEND *ops)
{
    unsigned ii;
    for (ii = 0; ii < LCB_PENDTYPE_MAX; ++ii) {
        ops->items[ii] = hashset_create();
    }
    ops->count = 0;
}

void
lcb_aspend_add(lcb_ASPEND *ops, lcb_ASPENDTYPE type, const void *item)
{
    ops->count++;
    if (type == LCB_PENDTYPE_COUNTER) {
        return;
    }
    hashset_add(ops->items[type], (void *)item);
}

void
lcb_aspend_del(lcb_ASPEND *ops, lcb_ASPENDTYPE type, const void *item)
{
    if (type == LCB_PENDTYPE_COUNTER) {
        ops->count--;
        return;
    }
    if (hashset_remove(ops->items[type], (void *)item)) {
        ops->count--;
    }
}

void
lcb_aspend_cleanup(lcb_ASPEND *ops)
{
    unsigned ii;
    for (ii = 0; ii < LCB_PENDTYPE_MAX; ii++) {
        hashset_destroy(ops->items[ii]);
    }
}
