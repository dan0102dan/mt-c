#include "magitrickle/models.h"

#include <ctype.h>
#include <net/if.h>
#include <stdlib.h>
#include <string.h>

mt_err_t mt_adaptive_config_defaults(mt_adaptive_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->connect_timeout_ms = 2000;
    config->tls_timeout_ms = 3000;
    config->idle_timeout_ms = 300000;
    config->ttl_seconds = 600;
    config->max_connections = 128;
    config->max_entries = 2048;
    return mt_strset(&config->interface, "");
}

void mt_adaptive_config_clear(mt_adaptive_config_t *config)
{
    if (!config) { return; }
    free(config->interface);
    if (config->ingress_interfaces) {
        for (size_t i = 0; i < config->n_ingress; i++) { free(config->ingress_interfaces[i]); }
    }
    free(config->ingress_interfaces);
    memset(config, 0, sizeof(*config));
}

mt_err_t mt_adaptive_config_clone(mt_adaptive_config_t *out, const mt_adaptive_config_t *source)
{
    *out = *source;
    out->interface = NULL;
    out->ingress_interfaces = NULL;
    out->n_ingress = 0;
    mt_err_t err = mt_strset(&out->interface, source->interface);
    if (err != MT_OK) { goto fail; }
    if (source->n_ingress) {
        out->ingress_interfaces = calloc(source->n_ingress, sizeof(*out->ingress_interfaces));
        if (!out->ingress_interfaces) { err = MT_ERR_NOMEM; goto fail; }
    }
    for (size_t i = 0; i < source->n_ingress; i++) {
        err = mt_strset(&out->ingress_interfaces[i], source->ingress_interfaces[i]);
        if (err != MT_OK) { goto fail; }
        out->n_ingress++;
    }
    return MT_OK;
fail:
    mt_adaptive_config_clear(out);
    return err;
}

bool mt_adaptive_interface_valid(const char *name)
{
    if (!name || !*name || strlen(name) >= IF_NAMESIZE ||
        !strcmp(name, "lo") || !strcmp(name, "blackhole")) { return false; }
    /* No iptables '+' prefix wildcard, whitespace, switches or shell syntax. */
    if (name[0] == '-') { return false; }
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (!(isalnum(*p) || *p == '_' || *p == '.' || *p == ':' || *p == '-')) { return false; }
    }
    return true;
}

bool mt_adaptive_config_valid(const mt_adaptive_config_t *config)
{
    if (!config || !config->interface || config->n_ingress > 16 ||
        (config->n_ingress && !config->ingress_interfaces) ||
        (config->enabled && (!*config->interface || !config->n_ingress)) ||
        (*config->interface && !mt_adaptive_interface_valid(config->interface)) ||
        config->connect_timeout_ms < 100 || config->connect_timeout_ms > 60000 ||
        config->tls_timeout_ms < 100 || config->tls_timeout_ms > 60000 ||
        config->idle_timeout_ms < 1000 || config->idle_timeout_ms > 3600000 ||
        config->ttl_seconds < 30 || config->ttl_seconds > 86400 ||
        !config->max_connections || config->max_connections > 1024 ||
        !config->max_entries || config->max_entries > 65536) { return false; }
    for (size_t i = 0; i < config->n_ingress; i++) {
        if (!mt_adaptive_interface_valid(config->ingress_interfaces[i]) ||
            !strcmp(config->ingress_interfaces[i], config->interface)) { return false; }
        for (size_t j = 0; j < i; j++) {
            if (!strcmp(config->ingress_interfaces[i], config->ingress_interfaces[j])) { return false; }
        }
    }
    return true;
}

bool mt_adaptive_config_equal(const mt_adaptive_config_t *a, const mt_adaptive_config_t *b)
{
    if (a->enabled != b->enabled || a->tcp_all_ports != b->tcp_all_ports ||
        a->quic_enabled != b->quic_enabled || a->n_ingress != b->n_ingress ||
        strcmp(a->interface ? a->interface : "", b->interface ? b->interface : "") != 0 ||
        a->connect_timeout_ms != b->connect_timeout_ms || a->tls_timeout_ms != b->tls_timeout_ms ||
        a->idle_timeout_ms != b->idle_timeout_ms || a->ttl_seconds != b->ttl_seconds ||
        a->max_connections != b->max_connections || a->max_entries != b->max_entries) { return false; }
    for (size_t i = 0; i < a->n_ingress; i++) {
        if (strcmp(a->ingress_interfaces[i], b->ingress_interfaces[i]) != 0) { return false; }
    }
    return true;
}
