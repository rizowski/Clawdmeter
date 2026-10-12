// Host unit test for netcfg — the pure provisioning logic (config JSON, setup
// code session, topic names). No Arduino/LVGL deps. From firmware/:
//
//   g++ -std=c++17 -I src -I .pio/libdeps/sim/ArduinoJson/src \
//       test/test_netcfg/test_main.cpp src/netcfg.cpp -o /tmp/t_netcfg && /tmp/t_netcfg

#include "netcfg.h"
#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);             \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static const char* GOOD =
    "{\"op\":\"config\",\"code\":\"123456\",\"ssid\":\"Home \\\"5G\\\": up\","
    "\"wpass\":\"p@ss:word\",\"broker_id\":\"00aa11bb22cc33dd\",\"pass\":\"devpass\"}";

static void should_parse_config_with_json_hostile_chars() {
    NetConfig c = {};
    CHECK(netcfg_parse(GOOD, &c));
    CHECK(strcmp(c.ssid, "Home \"5G\": up") == 0);
    CHECK(strcmp(c.wpass, "p@ss:word") == 0);
    CHECK(strcmp(c.broker_id, "00aa11bb22cc33dd") == 0);
    CHECK(strcmp(c.pass, "devpass") == 0);
    CHECK(netcfg_is_valid(&c));
}

static void should_accept_max_length_ssid_and_pass_and_reject_longer() {
    std::string ssid32(32, 's'), pass63(63, 'p');
    std::string ok = "{\"ssid\":\"" + ssid32 + "\",\"wpass\":\"" + pass63 +
                     "\",\"broker_id\":\"00aa11bb22cc33dd\",\"pass\":\"x\"}";
    NetConfig c = {};
    CHECK(netcfg_parse(ok.c_str(), &c));
    CHECK(strlen(c.ssid) == 32 && strlen(c.wpass) == 63);

    std::string too_long = "{\"ssid\":\"" + ssid32 + "s\",\"wpass\":\"x\","
                           "\"broker_id\":\"00aa11bb22cc33dd\",\"pass\":\"x\"}";
    CHECK(!netcfg_parse(too_long.c_str(), &c));   // never silently truncate
}

static void should_reject_missing_fields_and_bad_json() {
    NetConfig c = {};
    CHECK(!netcfg_parse("{\"ssid\":\"a\"}", &c));
    CHECK(!netcfg_parse("not json", &c));
    CHECK(!netcfg_parse("{\"ssid\":\"a\",\"wpass\":\"b\",\"broker_id\":\"short\",\"pass\":\"x\"}", &c));
}

static void should_allow_open_network_with_empty_wifi_password() {
    NetConfig c = {};
    CHECK(netcfg_parse("{\"ssid\":\"cafe\",\"wpass\":\"\",\"broker_id\":\"00aa11bb22cc33dd\",\"pass\":\"x\"}", &c));
    CHECK(netcfg_is_valid(&c));
}

static void should_generate_six_digit_code_and_expire() {
    ProvSession s = {};
    prov_session_begin(&s, 42u, 1000);
    CHECK(strlen(s.code) == 6);
    CHECK(strcmp(s.code, "000042") == 0);
    CHECK(prov_session_visible(&s, 1000));
    CHECK(prov_session_visible(&s, 1000 + PROV_CODE_TTL_MS - 1));
    CHECK(!prov_session_visible(&s, 1000 + PROV_CODE_TTL_MS));
}

static void should_handle_millis_wraparound() {
    ProvSession s = {};
    prov_session_begin(&s, 1u, 0xFFFFFF00u);
    CHECK(prov_session_visible(&s, 0x00000100u));   // 512 ms later, after wrap
}

static void should_apply_with_right_code_and_end_session() {
    ProvSession s = {};
    prov_session_begin(&s, 123456u, 0);
    NetConfig c = {};
    CHECK(prov_session_apply(&s, GOOD, 10, &c) == PROV_OK);
    CHECK(strcmp(c.pass, "devpass") == 0);
    CHECK(!s.active);
    CHECK(prov_session_apply(&s, GOOD, 20, &c) == PROV_NOT_ACTIVE);
}

static void should_count_wrong_codes_and_lock_after_max() {
    ProvSession s = {};
    prov_session_begin(&s, 999999u, 0);
    NetConfig c = {};
    for (int i = 1; i < PROV_MAX_FAILS; i++) CHECK(prov_session_apply(&s, GOOD, 10, &c) == PROV_BAD_CODE);
    CHECK(prov_session_apply(&s, GOOD, 10, &c) == PROV_LOCKED);
    CHECK(!s.active);
}

static void should_not_count_bad_json_as_a_wrong_code() {
    ProvSession s = {};
    prov_session_begin(&s, 123456u, 0);
    NetConfig c = {};
    CHECK(prov_session_apply(&s, "{\"op\":\"config\",\"code\":\"123456\"}", 10, &c) == PROV_BAD_JSON);
    CHECK(s.active && s.fails == 0);
}

static void should_reject_after_expiry() {
    ProvSession s = {};
    prov_session_begin(&s, 123456u, 0);
    NetConfig c = {};
    CHECK(prov_session_apply(&s, GOOD, PROV_CODE_TTL_MS + 1, &c) == PROV_NOT_ACTIVE);
}

static void should_build_topics() {
    char t[48];
    netcfg_topic(t, sizeof t, "a1b2c3", "usage");
    CHECK(strcmp(t, "clawdmeter/a1b2c3/usage") == 0);
}

static void should_format_device_id_from_last_three_base_mac_bytes() {
    // Base MAC of the 2.16 C6 on the bench (its eFuse EUI-64 is
    // 20:6e:f1:ff:fe:16:e0:4c — the id must skip the ff:fe filler).
    const uint8_t mac[6] = {0x20, 0x6e, 0xf1, 0x16, 0xe0, 0x4c};
    char id[7];
    netcfg_device_id(mac, id);
    CHECK(strcmp(id, "16e04c") == 0);
}

static void should_not_reset_wrong_code_count_on_a_new_begin() {
    // Re-requesting a code must not refill the guess budget, or a client
    // could guess 4 times, re-begin, and brute-force the 6 digits.
    ProvSession s = {};
    NetConfig c = {};
    prov_session_begin(&s, 999999u, 0);
    for (int i = 1; i < PROV_MAX_FAILS; i++) CHECK(prov_session_apply(&s, GOOD, 10, &c) == PROV_BAD_CODE);
    CHECK(prov_session_begin(&s, 999998u, 20));
    CHECK(prov_session_apply(&s, GOOD, 30, &c) == PROV_LOCKED);
}

static void should_refuse_begin_during_lockout_then_allow_after() {
    ProvSession s = {};
    NetConfig c = {};
    prov_session_begin(&s, 999999u, 0);
    for (int i = 0; i < PROV_MAX_FAILS; i++) prov_session_apply(&s, GOOD, 1000, &c);
    CHECK(!prov_session_begin(&s, 123456u, 1000 + PROV_LOCKOUT_MS - 1));
    CHECK(!s.active);
    CHECK(prov_session_begin(&s, 123456u, 1000 + PROV_LOCKOUT_MS));
    CHECK(prov_session_apply(&s, GOOD, 1000 + PROV_LOCKOUT_MS + 1, &c) == PROV_OK);
}

static void should_reset_wrong_code_count_after_success() {
    ProvSession s = {};
    NetConfig c = {};
    prov_session_begin(&s, 123456u, 0);
    CHECK(prov_session_apply(&s, "{\"code\":\"000000\"}", 1, &c) == PROV_BAD_CODE);
    CHECK(prov_session_apply(&s, GOOD, 2, &c) == PROV_OK);
    CHECK(s.fails == 0);
}

static void should_report_net_up_only_for_the_config_just_applied() {
    // Re-provisioning while connected must not report the OLD session as
    // success for the NEW config (a wrong new password would look fine).
    NetUpGate g = {};
    net_up_arm(&g, 2);
    CHECK(!net_up_take(&g, 0));   // not connected
    CHECK(!net_up_take(&g, 1));   // still the previous config's session
    CHECK(net_up_take(&g, 2));    // the new config reached the broker
    CHECK(!net_up_take(&g, 2));   // reported once
    // Re-provision while that session is still up: in the same loop pass the
    // live session is still generation 2, which must not count for config 3.
    net_up_arm(&g, 3);
    CHECK(!net_up_take(&g, 2));
    CHECK(net_up_take(&g, 3));
}

static void should_fire_an_armed_timer_past_the_int32_millis_boundary() {
    // Uptime 24.8-49.7 days: (int32_t)millis() is negative, so a deadline left
    // at 0 never comes due there. Timers must be armed from "now".
    const uint32_t now = 0x90000000u;
    NetTimer t = {};
    net_timer_arm(&t, now, 0);
    CHECK(net_timer_due(&t, now));
    net_timer_arm(&t, now, 15000);
    CHECK(!net_timer_due(&t, now + 14999));
    CHECK(net_timer_due(&t, now + 15000));
}

static void should_fire_a_timer_across_the_millis_wrap() {
    NetTimer t = {};
    net_timer_arm(&t, 0xFFFFF000u, 0x2000);
    CHECK(!net_timer_due(&t, 0xFFFFFFFFu));
    CHECK(net_timer_due(&t, 0x00001000u));
}

static void should_skip_an_mdns_answer_whose_address_is_unresolved() {
    // ESPmDNS can return the service before its A record: address 0.0.0.0.
    CHECK(!netcfg_broker_usable(0, 18830));
    CHECK(!netcfg_broker_usable(0xF802A8C0u, 0));      // 192.168.2.248, no port
    CHECK(netcfg_broker_usable(0xF802A8C0u, 18830));
}

int main() {
    should_parse_config_with_json_hostile_chars();
    should_accept_max_length_ssid_and_pass_and_reject_longer();
    should_reject_missing_fields_and_bad_json();
    should_allow_open_network_with_empty_wifi_password();
    should_generate_six_digit_code_and_expire();
    should_handle_millis_wraparound();
    should_apply_with_right_code_and_end_session();
    should_count_wrong_codes_and_lock_after_max();
    should_not_count_bad_json_as_a_wrong_code();
    should_reject_after_expiry();
    should_build_topics();
    should_format_device_id_from_last_three_base_mac_bytes();
    should_not_reset_wrong_code_count_on_a_new_begin();
    should_refuse_begin_during_lockout_then_allow_after();
    should_reset_wrong_code_count_after_success();
    should_report_net_up_only_for_the_config_just_applied();
    should_fire_an_armed_timer_past_the_int32_millis_boundary();
    should_fire_a_timer_across_the_millis_wrap();
    should_skip_an_mdns_answer_whose_address_is_unresolved();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all netcfg tests passed\n");
    return 0;
}
