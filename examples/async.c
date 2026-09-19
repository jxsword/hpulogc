/**
 * @file async.c
 * @brief Async-mode example: code configuration, ring buffer, shutdown
 *        drain.
 */

#include <stdio.h>
#include <string.h>

#include <hpulogc.h>

int main(void)
{
    hpulogc_config_t cfg;
    hpulogc_output_t out;
    hpulogc_rule_t rule;
    static const char* names[] = { "out0" };
    int i;

    hpulogc_config_default(&cfg);

    memset(&out, 0, sizeof(out));
    out.type = HPULOGC_OUT_FILE;
    out.path = "hpulogc_async_example.log";
    cfg.outputs = &out;
    cfg.output_count = 1;

    memset(&rule, 0, sizeof(rule));
    rule.category = "app";
    rule.min_level = HPULOGC_LEVEL_TRACE;
    rule.max_level = HPULOGC_LEVEL_FATAL;
    rule.format = "standard";
    rule.outputs = names;
    rule.output_count = 1;
    cfg.rules = &rule;
    cfg.rule_count = 1;

    cfg.buffer_size = 256 * 1024;
    cfg.batch_size = 32;
    cfg.flush_interval_ms = 50;

    if (hpulogc_init(&cfg) != HPULOGC_OK) {
        return 1;
    }

    for (i = 0; i < 1000; i++) {
        HPULOGC_INFO("app", "async log %d", i);
    }

    /* shutdown drains the queue (at most shutdown_timeout_ms) */
    hpulogc_shutdown();
    printf("done: see hpulogc_async_example.log\n");
    return 0;
}
