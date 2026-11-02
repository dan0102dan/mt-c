/* Host transport/pipeline comparison. Links unmodified production objects;
 * only main() and the netfilter sink are replaced. This does NOT measure
 * daemon API/journal work, ipset syscalls or router forwarding performance. */
#include "magitrickle/dnspipeline.h"
#include "magitrickle/dnsproxy.h"
#include "magitrickle/log.h"
#include "magitrickle/yamlio.h"

#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>

static uint64_t actions;
static uint64_t responses;

static void sink(const mt_match_action_t *action, void *ud)
{
    (void)action;
    (void)ud;
    actions++;
}

static void response(const mt_dns_msg_t *msg, const char *client,
                     const char *network, void *ud)
{
    (void)client;
    (void)network;
    responses++;
    mt_dns_pipeline_handle_message(ud, msg, (int64_t)time(NULL));
}

static void stopped(mt_loop_t *loop, int signo, void *ud)
{
    (void)signo;
    (void)ud;
    mt_loop_stop(loop);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s config.yaml\n", argv[0]);
        return 2;
    }
    mt_log_set_level(MT_LOG_ERROR);
    mt_config_t cfg;
    mt_config_init_defaults(&cfg);
    if (mt_config_load_file(&cfg, argv[1]) != MT_OK) return 3;
    mt_loop_t *loop = NULL;
    if (mt_loop_create(&loop) != MT_OK) return 3;
    mt_cache_t *cache = mt_cache_create(0);
    mt_dns_pipeline_t *pipeline = mt_dns_pipeline_create(cache, 3600, sink, NULL);
    mt_ruleset_snapshot_t *snap = mt_ruleset_snapshot_build(&cfg);
    if (!cache || !pipeline || !snap || snap->n_groups == 0) return 3;
    mt_dns_pipeline_set_snapshot(pipeline, snap);
    mt_dnsproxy_config_t options = {
        .listen_addr = cfg.app.dns_proxy.host.address,
        .listen_port = cfg.app.dns_proxy.host.port,
        .upstream_addr = cfg.app.dns_proxy.upstream.address,
        .upstream_port = cfg.app.dns_proxy.upstream.port,
        .timeout_ms = 1000, .max_concurrent = 100, .max_idle_conns = 10,
        .disable_fake_ptr = cfg.app.dns_proxy.disable_fake_ptr,
        .disable_drop_aaaa = cfg.app.dns_proxy.disable_drop_aaaa,
    };
    mt_dnsproxy_t *proxy = NULL;
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    if (mt_loop_add_signals(loop, &signals, stopped, NULL) != MT_OK ||
        mt_dnsproxy_create(&options, loop, response, pipeline, &proxy) != MT_OK ||
        mt_dnsproxy_start(proxy) != MT_OK) return 3;
    mt_err_t status = mt_loop_run(loop);
    printf("{\"responses\":%" PRIu64 ",\"sink_calls\":%" PRIu64
           ",\"dropped\":%" PRIu64 "}\n", responses, actions,
           mt_dnsproxy_dropped(proxy));
    mt_dnsproxy_destroy(proxy);
    mt_dns_pipeline_destroy(pipeline);
    mt_cache_destroy(cache);
    mt_loop_destroy(loop);
    mt_config_clear(&cfg);
    return status == MT_OK ? 0 : 1;
}
