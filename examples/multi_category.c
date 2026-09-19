/**
 * @file multi_category.c
 * @brief Multi-category example: routing rules, per-category levels.
 */

#include <stdio.h>
#include <string.h>

#include <hpulogc.h>

int main(void)
{
    hpulogc_config_t cfg;
    hpulogc_output_t outs[2];
    static const char* out_names[2][1] = { { "out0" }, { "out1" } };
    hpulogc_rule_t rules[3];

    hpulogc_config_default(&cfg);

    memset(outs, 0, sizeof(outs));
    outs[0].type = HPULOGC_OUT_FILE;
    outs[0].path = "hpulogc_categories.log";
    outs[1].type = HPULOGC_OUT_CONSOLE;
    outs[1].stream = 1;

    memset(rules, 0, sizeof(rules));
    rules[0].category = "db.*";
    rules[0].min_level = HPULOGC_LEVEL_TRACE;
    rules[0].max_level = HPULOGC_LEVEL_FATAL;
    rules[0].format = "categorized";
    rules[0].outputs = out_names[0];
    rules[0].output_count = 1;

    rules[1].category = "net.*";
    rules[1].min_level = HPULOGC_LEVEL_TRACE;
    rules[1].max_level = HPULOGC_LEVEL_FATAL;
    rules[1].format = "detailed";
    rules[1].outputs = out_names[0];
    rules[1].output_count = 1;

    rules[2].category = "*";
    rules[2].min_level = HPULOGC_LEVEL_INFO;
    rules[2].max_level = HPULOGC_LEVEL_FATAL;
    rules[2].format = "standard";
    rules[2].outputs = out_names[1];
    rules[2].output_count = 1;

    cfg.outputs = outs;
    cfg.output_count = 2;
    cfg.rules = rules;
    cfg.rule_count = 3;

    if (hpulogc_init(&cfg) != HPULOGC_OK) {
        return 1;
    }

    HPULOGC_INFO("ui", "renders normally via the fallback rule");
    HPULOGC_DEBUG("ui", "dropped: below the global INFO threshold");
    HPULOGC_ERROR("db.query", "goes to the db rule");
    HPULOGC_INFO("net.sock", "goes to the net rule");

    /* per-category override: db drops its threshold to TRACE */
    /* overrides match the exact category string (spec 4.1) */
    hpulogc_set_level_for_category("db.cache", HPULOGC_LEVEL_TRACE);
    HPULOGC_TRACE("db.cache", "now visible via the override");
    hpulogc_set_level_for_category("db.cache", (hpulogc_level_t)-1);

    hpulogc_shutdown();
    printf("done: see hpulogc_categories.log\n");
    return 0;
}
