/**
 * @file test_fields.c
 * @brief Structured-field wire format budgets (§4.11.2), round-trip
 *        (§4.11.4) and rendering (%v / json fields member, §4.11.5).
 */

#include "test_util.h"
#include "portability.h"
#include "hpulogc.h"
#include "format/format.h"

#include <stdio.h>
#include <string.h>

/** @brief Render buffer sizing helper (max_line + newline slack). */
#define HPULOGC_MAX_LOG_LINE_TEST (HPULOGC_MAX_FIELDS * 0 + 4200)

TEST(fields_serialize_roundtrip)
{
    hpulogc_field_t in[3];
    uint8_t wire[512];
    size_t len;
    uint16_t cnt;
    hpulogc_field_t out[3];
    size_t n;
    char buf[128];

    in[0].key = "i";
    in[0].value.type = HPULOGC_FIELD_I64;
    in[0].value.v.i64 = -7;
    in[1].key = "s";
    in[1].value.type = HPULOGC_FIELD_STR;
    in[1].value.v.str.s = "abc";
    in[1].value.v.str.len = 3;
    in[2].key = "b";
    in[2].value.type = HPULOGC_FIELD_BOOL;
    in[2].value.v.b = 1;

    CHECK_EQ(hpu_fields_serialize(in, 3, 4096, wire, sizeof(wire), &len,
                                  &cnt, NULL), 0);
    CHECK_EQ(cnt, 3);
    CHECK(len % 8 == 0); /* region is 8-aligned */

    n = hpu_fields_unpack(wire, len, cnt, out, 3);
    CHECK_EQ(n, 3);
    CHECK_MEMEQ(out[0].key, "i", 1);
    CHECK_EQ(out[0].value.v.i64, -7);
    CHECK_MEMEQ(out[1].value.v.str.s, "abc", 3);
    CHECK_EQ(out[1].value.v.str.len, 3);
    CHECK_EQ(out[2].value.v.b, 1);
    snprintf(buf, sizeof(buf), "%d", (int)out[2].value.v.b);
    CHECK_STREQ(buf, "1");
}

TEST(fields_budget_count_cap)
{
    hpulogc_field_t in[HPULOGC_MAX_FIELDS + 4];
    uint8_t wire[4096];
    size_t len;
    uint16_t cnt;
    unsigned long long dropped = 0;
    size_t i;

    for (i = 0; i < sizeof(in) / sizeof(in[0]); i++) {
        in[i].key = "k";
        in[i].value.type = HPULOGC_FIELD_I64;
        in[i].value.v.i64 = (int64_t)i;
    }
    CHECK_EQ(hpu_fields_serialize(in, sizeof(in) / sizeof(in[0]), 4096,
                                  wire, sizeof(wire), &len, &cnt,
                                  &dropped), 0);
    CHECK_EQ(cnt, HPULOGC_MAX_FIELDS);
    /* the tail beyond the cap is dropped, one counter each */
    CHECK_EQ(dropped, 4);
}

TEST(fields_budget_key_and_str)
{
    char longkey[HPULOGC_MAX_FIELD_KEY_LEN + 8];
    char longstr[HPULOGC_MAX_FIELD_STR_LEN + 8];
    hpulogc_field_t in[3];
    uint8_t wire[4096];
    size_t len;
    uint16_t cnt;
    unsigned long long dropped = 0;
    hpulogc_field_t out[3];
    size_t i;
    char kbuf[HPULOGC_MAX_FIELD_KEY_LEN + 8];

    memset(longkey, 'k', sizeof(longkey) - 1);
    longkey[sizeof(longkey) - 1] = '\0'; /* NUL-terminated, len > KEY_MAX */
    memset(longstr, 'v', sizeof(longstr));
    in[0].key = longkey;                    /* oversized key: dropped */
    in[0].value.type = HPULOGC_FIELD_I64;
    in[0].value.v.i64 = 1;
    in[1].key = "trunc";
    in[1].value.type = HPULOGC_FIELD_STR;
    in[1].value.v.str.s = longstr;          /* oversized string: truncated */
    in[1].value.v.str.len = sizeof(longstr);
    in[2].key = "ok";
    in[2].value.type = HPULOGC_FIELD_U64;
    in[2].value.v.u64 = 9;

    CHECK_EQ(hpu_fields_serialize(in, 3, 4096, wire, sizeof(wire), &len,
                                  &cnt, &dropped), 0);
    /* the oversized key is dropped; the string is truncated (+1) */
    CHECK_EQ(dropped, 2);
    CHECK_EQ(cnt, 2);
    CHECK_EQ(hpu_fields_unpack(wire, len, cnt, out, 3), 2);
    /* first surviving field is the truncated string */
    CHECK_MEMEQ(out[0].key, "trunc", 5);
    CHECK_EQ(out[0].value.v.str.len, HPULOGC_MAX_FIELD_STR_LEN);
    for (i = 0; i < out[0].value.v.str.len; i++) {
        CHECK_EQ(out[0].value.v.str.s[i], 'v');
    }
    /* silence unused warnings for the key buffer contents */
    kbuf[0] = 'k';
    CHECK_EQ(kbuf[0], 'k');
}

TEST(fields_budget_byte_budget)
{
    hpulogc_field_t in[3];
    uint8_t wire[4096];
    size_t len;
    uint16_t cnt;
    unsigned long long dropped = 0;

    in[0].key = "a";
    in[0].value.type = HPULOGC_FIELD_STR;
    in[0].value.v.str.s = "xxxxxxxxxx";
    in[0].value.v.str.len = 10;
    in[1].key = "b";
    in[1].value.type = HPULOGC_FIELD_STR;
    in[1].value.v.str.s = "yyyyyyyyyy";
    in[1].value.v.str.len = 10;
    in[2].key = "c";
    in[2].value.type = HPULOGC_FIELD_I64;
    in[2].value.v.i64 = 5;

    /* budget fits a=10 and b=10 but not c: the tail is dropped */
    CHECK_EQ(hpu_fields_serialize(in, 3, 6 + 1 + 10 + 6 + 1 + 10,
                                  wire, sizeof(wire), &len, &cnt,
                                  &dropped), 0);
    CHECK_EQ(cnt, 2);
    CHECK_EQ(dropped, 1);
}

TEST(fields_render_v_placeholder)
{
    hpu_format_t fmt;
    hpu_log_record_t rec;
    hpu_fmt_env_t env;
    hpu_fmt_cache_t cache;
    uint8_t wire[256];
    size_t wire_len;
    uint16_t wire_cnt;
    hpulogc_field_t in[2];
    char out[HPULOGC_MAX_LOG_LINE_TEST];
    size_t out_len;

    in[0].key = "k1";
    in[0].value.type = HPULOGC_FIELD_I64;
    in[0].value.v.i64 = 42;
    in[1].key = "s1";
    in[1].value.type = HPULOGC_FIELD_STR;
    in[1].value.v.str.s = "vv";
    in[1].value.v.str.len = 2;
    CHECK_EQ(hpu_fields_serialize(in, 2, 4096, wire, sizeof(wire),
                                  &wire_len, &wire_cnt, NULL), 0);

    CHECK_EQ(hpu_format_compile(&fmt, "standard",
                                "%time [%level] %msg %v%n"), 0);
    memset(&rec, 0, sizeof(rec));
    rec.level = HPULOGC_LEVEL_INFO;
    rec.msg = "hello";
    rec.msg_len = 5;
    rec.fields_wire = wire;
    rec.fields_len = (uint32_t)wire_len;
    rec.field_count = wire_cnt;
    rec.realtime_ns = 0;
    memset(&env, 0, sizeof(env));
    env.newline_style = HPULOGC_NEWLINE_LF; /* AUTO is CRLF on Windows */
    env.timestamp_source = HPULOGC_TS_MONOTONIC; /* deterministic %time */
    env.mono_base_us = 0;
    hpu_fmt_cache_init(&cache);

    CHECK_EQ(hpu_format_render(&fmt, &rec, &env, &cache, 1, 4096, "...",
                               out, sizeof(out), &out_len), 0);
    out[out_len] = '\0';
    CHECK(strstr(out, "hello k1=42 s1=vv") != NULL);

    /* without fields the placeholder renders empty */
    rec.field_count = 0;
    rec.fields_wire = NULL;
    CHECK_EQ(hpu_format_render(&fmt, &rec, &env, &cache, 1, 4096, "...",
                               out, sizeof(out), &out_len), 0);
    out[out_len] = '\0';
    CHECK(strstr(out, "hello \n") != NULL); /* %v renders empty */
    hpu_format_free(&fmt);
}

TEST(fields_render_json_member)
{
    hpu_format_t fmt;
    hpu_log_record_t rec;
    hpu_fmt_env_t env;
    hpu_fmt_cache_t cache;
    uint8_t wire[256];
    size_t wire_len;
    uint16_t wire_cnt;
    hpulogc_field_t in[2];
    char out[HPULOGC_MAX_LOG_LINE_TEST];
    size_t out_len;

    in[0].key = "num";
    in[0].value.type = HPULOGC_FIELD_U64;
    in[0].value.v.u64 = 5;
    in[1].key = "s";
    in[1].value.type = HPULOGC_FIELD_STR;
    in[1].value.v.str.s = "a\"b";
    in[1].value.v.str.len = 3;
    CHECK_EQ(hpu_fields_serialize(in, 2, 4096, wire, sizeof(wire),
                                  &wire_len, &wire_cnt, NULL), 0);

    CHECK_EQ(hpu_format_compile(&fmt, "json",
                                "{\"msg\":\"%msg\"}%n"), 0);
    memset(&rec, 0, sizeof(rec));
    rec.level = HPULOGC_LEVEL_INFO;
    rec.msg = "hello";
    rec.msg_len = 5;
    rec.fields_wire = wire;
    rec.fields_len = (uint32_t)wire_len;
    rec.field_count = wire_cnt;
    memset(&env, 0, sizeof(env));
    env.newline_style = HPULOGC_NEWLINE_LF;
    hpu_fmt_cache_init(&cache);

    CHECK_EQ(hpu_format_render(&fmt, &rec, &env, &cache, 1, 4096, "...",
                               out, sizeof(out), &out_len), 0);
    out[out_len] = '\0';
    /* fields member lands inside the object, before the closing brace */
    CHECK(strstr(out, "\"fields\":{\"num\":5,\"s\":\"a\\\"b\"}}") != NULL);

    /* field-less records: byte-identical to the plain template */
    rec.field_count = 0;
    rec.fields_wire = NULL;
    CHECK_EQ(hpu_format_render(&fmt, &rec, &env, &cache, 1, 4096, "...",
                               out, sizeof(out), &out_len), 0);
    out[out_len] = '\0';
    CHECK_STREQ(out, "{\"msg\":\"hello\"}\n");
    hpu_format_free(&fmt);
}
