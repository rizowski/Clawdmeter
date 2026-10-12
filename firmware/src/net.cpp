// WiFi + mDNS + MQTT transport. Usage JSON arrives on clawdmeter/<id>/usage,
// retained, so every reconnect immediately gets the latest value. The device
// finds the broker by browsing _clawdmeter._tcp for the broker id it was
// paired with, so the host's IP may change between sessions.
//
// All blocking work (WiFi join, the ~3 s mDNS browse, esp-mqtt start/stop)
// runs on its own FreeRTOS task so the LVGL loop never stalls. esp-mqtt's
// event handler runs on yet another task; payloads cross to the loop task
// through a spinlock-guarded buffer.
#include "net.h"

#ifndef BOARD_HAS_WIFI

void net_init(void) {}
uint32_t net_apply_config(void) { return 0; }
net_state_t net_get_state(void) { return NET_OFF; }
uint32_t net_connected_generation(void) { return 0; }
bool net_has_data(void) { return false; }
const char* net_get_data(void) { return ""; }

#else

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <mqtt_client.h>
#include "netcfg.h"
#include "prov.h"

#define NET_BUF_SIZE 512
static const uint32_t REDISCOVER_AFTER_MS = 20000;  // broker unreachable this long → browse again
static const uint32_t BROWSE_RETRY_MS     = 15000;  // between failed browses (laptop away)
static const uint32_t REJOIN_AFTER_MS     = 60000;  // off WiFi this long → WiFi.begin again

static TaskHandle_t  net_task_h = nullptr;
static portMUX_TYPE  mux = portMUX_INITIALIZER_UNLOCKED;
static char          rx_buf[NET_BUF_SIZE];    // written on the esp-mqtt task
static char          out_buf[NET_BUF_SIZE];   // handed to the loop task
static volatile bool data_ready = false;
static volatile bool mqtt_up = false;
static volatile bool auth_failed = false;
static volatile bool reconfig = false;
static volatile net_state_t state = NET_OFF;
static volatile uint32_t mqtt_down_since = 0;
static uint32_t          pending_gen = 0;      // bumped by every net_apply_config (loop task)
static volatile uint32_t applied_gen = 0;      // generation the net task is running
static volatile uint32_t connected_gen = 0;    // generation of the live MQTT session

static bool     mdns_up = false;   // net task only
static NetTimer browse_timer = {};
static NetTimer rejoin_timer = {};
static NetConfig pending = {};   // written by the loop task under mux
static NetConfig active  = {};   // net task's private copy
static esp_mqtt_client_handle_t client = nullptr;
static char t_usage[40], t_refresh[40], t_status[40];
static char uri[40], client_id[24], hostname[24];

static void mqtt_event(void*, esp_event_base_t, int32_t id, void* data) {
    auto* ev = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        mqtt_up = true;
        auth_failed = false;
        connected_gen = applied_gen;
        esp_mqtt_client_subscribe_single(ev->client, t_usage, 1);   // the non-_single name is a C-only _Generic macro
        esp_mqtt_client_publish(ev->client, t_status, "online", 0, 1, 1);
        esp_mqtt_client_publish(ev->client, t_refresh, "1", 0, 1, 0);  // ask the daemon to poll now
        Serial.println("NET: MQTT connected");
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (mqtt_up) mqtt_down_since = millis();
        mqtt_up = false;
        connected_gen = 0;
        break;
    case MQTT_EVENT_DATA:
        // Usage payloads are < 512 B, so they always arrive in one piece.
        if (ev->topic_len == (int)strlen(t_usage) &&
            memcmp(ev->topic, t_usage, ev->topic_len) == 0 &&
            ev->current_data_offset == 0 && ev->data_len == ev->total_data_len) {
            int len = ev->data_len < NET_BUF_SIZE - 1 ? ev->data_len : NET_BUF_SIZE - 1;
            portENTER_CRITICAL(&mux);
            memcpy(rx_buf, ev->data, len);
            rx_buf[len] = '\0';
            data_ready = true;
            portEXIT_CRITICAL(&mux);
        }
        break;
    case MQTT_EVENT_ERROR:
        if (ev->error_handle &&
            ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            esp_mqtt_connect_return_code_t rc = ev->error_handle->connect_return_code;
            if (rc == MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
                rc == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED) {
                auth_failed = true;
                Serial.println("NET: broker rejected login — re-run clawdmeter-setup");
            }
        }
        break;
    default:
        break;
    }
}

static void mqtt_stop(void) {
    if (!client) return;
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    client = nullptr;
    mqtt_up = false;
    connected_gen = 0;
}

// Browse _clawdmeter._tcp for the broker this device was paired with.
static bool discover(IPAddress* ip, uint16_t* port) {
    int n = MDNS.queryService("clawdmeter", "tcp");
    for (int i = 0; i < n; i++) {
        if (MDNS.txt(i, "id") == active.broker_id &&
            netcfg_broker_usable((uint32_t)MDNS.address(i), MDNS.port(i))) {
            *ip = MDNS.address(i);
            *port = MDNS.port(i);
            return true;
        }
    }
    return false;
}

static void mqtt_start(IPAddress ip, uint16_t port) {
    snprintf(uri, sizeof uri, "mqtt://%s:%u", ip.toString().c_str(), port);
    esp_mqtt_client_config_t c = {};
    c.broker.address.uri = uri;
    c.credentials.username = prov_device_id();
    c.credentials.client_id = client_id;
    c.credentials.authentication.password = active.pass;
    c.session.keepalive = 30;
    c.session.last_will.topic = t_status;
    c.session.last_will.msg = "offline";
    c.session.last_will.qos = 1;
    c.session.last_will.retain = 1;
    c.network.reconnect_timeout_ms = 5000;
    c.buffer.size = 1024;
    client = esp_mqtt_client_init(&c);
    esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqtt_event, nullptr);
    mqtt_down_since = millis();
    esp_mqtt_client_start(client);
    Serial.printf("NET: MQTT -> %s\n", uri);
}

static void apply_pending(void) {
    portENTER_CRITICAL(&mux);
    active = pending;
    applied_gen = pending_gen;
    reconfig = false;
    portEXIT_CRITICAL(&mux);
    mqtt_stop();
    auth_failed = false;
    // WiFi.disconnect(true) tears down the STA netif, and mDNS stays bound to
    // the old one — restart it on the next connect or discovery never answers.
    if (mdns_up) {
        MDNS.end();
        mdns_up = false;
    }
    WiFi.disconnect(true);
    if (!netcfg_is_valid(&active)) {
        Serial.println("NET: no config — WiFi off");
        return;
    }
    netcfg_topic(t_usage, sizeof t_usage, prov_device_id(), "usage");
    netcfg_topic(t_refresh, sizeof t_refresh, prov_device_id(), "refresh");
    netcfg_topic(t_status, sizeof t_status, prov_device_id(), "status");
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true);           // modem sleep is mandatory while BLE shares the radio
    WiFi.setHostname(hostname);
    WiFi.setAutoReconnect(true);
    WiFi.begin(active.ssid, active.wpass);
    net_timer_arm(&rejoin_timer, millis(), REJOIN_AFTER_MS);
    net_timer_arm(&browse_timer, millis(), 0);
    Serial.printf("NET: joining \"%s\"\n", active.ssid);
}

static void net_task(void*) {
    for (;;) {
        if (reconfig) apply_pending();
        if (!netcfg_is_valid(&active)) {
            state = NET_OFF;
        } else if (WiFi.status() != WL_CONNECTED) {
            state = NET_WIFI;
            if (client) mqtt_stop();
            // The core gives up after an auth failure (not a "reconnectable"
            // reason), so a brief AP hiccup would strand us until reboot.
            if (net_timer_due(&rejoin_timer, millis())) {
                Serial.println("NET: still off WiFi — retrying join");
                WiFi.begin(active.ssid, active.wpass);
                net_timer_arm(&rejoin_timer, millis(), REJOIN_AFTER_MS);
            }
        } else {
            net_timer_arm(&rejoin_timer, millis(), REJOIN_AFTER_MS);
            if (!mdns_up) mdns_up = MDNS.begin(hostname);
            if (mqtt_up) {
                state = NET_CONNECTED;
            } else {
                state = auth_failed ? NET_AUTH_FAIL : NET_DISCOVERING;
                // No client yet, or the broker has been unreachable for a while
                // (laptop left, or came back on a new IP): browse again.
                bool stale = client && (millis() - mqtt_down_since) > REDISCOVER_AFTER_MS;
                if ((!client || stale) && net_timer_due(&browse_timer, millis())) {
                    mqtt_stop();
                    IPAddress ip;
                    uint16_t port;
                    if (discover(&ip, &port)) mqtt_start(ip, port);
                    else Serial.println("NET: broker not found on mDNS");
                    net_timer_arm(&browse_timer, millis(), BROWSE_RETRY_MS);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void net_init(void) {
    snprintf(hostname, sizeof hostname, "clawdmeter-%s", prov_device_id());
    snprintf(client_id, sizeof client_id, "clawdmeter-%s", prov_device_id());
    net_apply_config();
}

uint32_t net_apply_config(void) {
    portENTER_CRITICAL(&mux);
    pending = *prov_config();
    uint32_t gen = ++pending_gen;
    reconfig = true;
    portEXIT_CRITICAL(&mux);
    // The task (and the WiFi stack's RAM) only comes up once a config exists.
    if (!net_task_h && netcfg_is_valid(prov_config())) {
        xTaskCreatePinnedToCore(net_task, "net", 8192, nullptr, 1, &net_task_h, 0);
    }
    return gen;
}

net_state_t net_get_state(void) {
    return state;
}

uint32_t net_connected_generation(void) {
    return connected_gen;
}

bool net_has_data(void) {
    return data_ready;
}

const char* net_get_data(void) {
    portENTER_CRITICAL(&mux);
    memcpy(out_buf, rx_buf, NET_BUF_SIZE);
    data_ready = false;
    portEXIT_CRITICAL(&mux);
    return out_buf;
}

#endif  // BOARD_HAS_WIFI
