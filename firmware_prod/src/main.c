/**
 * @file main.c
 * @brief Yieryi HH-LYAI-24 irrigation controller - MQTT firmware
 *
 * WiFi -> MQTT broker on Cubie -> subscribe valve commands -> control valves
 * Publishes: battery voltage, rain sensor, valve states
 *
 * Hardware:
 *   CD4051B mux: A=GPIO12, B=GPIO13, C=GPIO17, COM=GPIO15, INH=GND
 *   4x L9110S H-bridge -> 4 bistable latching valves
 *   Boost 5V: GPIO40
 *   LEDs: GPIO14 (red), GPIO16 (green)
 *   Rain sensor: GPIO24 (ADC2)
 *   Battery: GPIO25 (ADC1, divider R5/R6 510k)
 */

#include "tuya_cloud_types.h"
#include "mqtt_client_interface.h"
#include "tuya_config_defaults.h"
#include "core_mqtt_config.h"
#include "core_mqtt.h"
#include "tuya_transporter.h"
#include "backoff_algorithm.h"
#include "tal_api.h"
#include "tkl_output.h"
#include "tkl_gpio.h"
#include "tkl_adc.h"
#include "netmgr.h"
#include "netconn_wifi.h"

/***********************************************************
 * Configuration
 ***********************************************************/
#define WIFI_SSID          "cubie"
#define WIFI_PASS          "cubie123456789!"

#define MQTT_HOST          "192.168.50.1"
#define MQTT_PORT          1883
#define MQTT_CLIENT_ID     "irrigation"
#define MQTT_TOPIC_CMD     "irrigation/+/set"
#define MQTT_TOPIC_STATE   "irrigation/state"
#define MQTT_TOPIC_AVAIL   "irrigation/availability"

#define STATE_INTERVAL_MS  30000  /* publish state every 30s */

/***********************************************************
 * Hardware pins
 ***********************************************************/
#define MUX_A    12
#define MUX_B    13
#define MUX_C    17
#define MUX_COM  15
#define BOOST_EN 40
#define LED_RED  14
#define LED_GRN  16

#define VALVE_PULSE_MS 1500
#define VALVE_COUNT    4

#define RAIN_SENSOR 24
#define BATTERY_ADC 25
#define USB_POWER   41  /* USB power detect: LOW=USB connected, HIGH=no USB */

/***********************************************************
 * Globals
 ***********************************************************/
static netmgr_status_e net_status = NETMGR_LINK_DOWN;
static int valve_states[VALVE_COUNT] = {0};

typedef struct {
    mqtt_client_config_t config;
    MQTTContext_t mqclient;
    tuya_transporter_t network;
    uint8_t mqttbuffer[CORE_MQTT_BUFFER_SIZE];
} mqtt_ctx_t;

/***********************************************************
 * GPIO helpers
 ***********************************************************/
static void gpio_out(int pin, int level)
{
    TUYA_GPIO_BASE_CFG_T cfg = {
        .mode = TUYA_GPIO_PUSH_PULL,
        .direct = TUYA_GPIO_OUTPUT,
        .level = TUYA_GPIO_LEVEL_LOW,
    };
    tkl_gpio_init(pin, &cfg);
    tkl_gpio_write(pin, level ? TUYA_GPIO_LEVEL_HIGH : TUYA_GPIO_LEVEL_LOW);
}

static void mux_select(int channel)
{
    gpio_out(MUX_A, (channel >> 0) & 1);
    gpio_out(MUX_B, (channel >> 1) & 1);
    gpio_out(MUX_C, (channel >> 2) & 1);
}

/***********************************************************
 * Valve control
 ***********************************************************/
/* Mux channel mapping (confirmed by hardware test):
 * Valve 1: open=ch0, close=ch3
 * Valve 2: open=ch2, close=ch1
 * Valve 3: open=ch4, close=ch6
 * Valve 4: open=ch7, close=ch5
 */
static const int valve_open_ch[VALVE_COUNT]  = {0, 2, 4, 7};
static const int valve_close_ch[VALVE_COUNT] = {3, 1, 6, 5};

static void valve_set(int valve, int open)
{
    if (valve < 0 || valve >= VALVE_COUNT) return;

    int channel = open ? valve_open_ch[valve] : valve_close_ch[valve];

    PR_NOTICE("Valve %d %s (mux ch%d)", valve + 1, open ? "OPEN" : "CLOSE", channel);

    gpio_out(LED_GRN, open ? 1 : 0);
    gpio_out(LED_RED, open ? 0 : 1);

    gpio_out(BOOST_EN, 1);
    tal_system_sleep(50);

    mux_select(channel);
    tal_system_sleep(10);

    gpio_out(MUX_COM, 1);
    tal_system_sleep(VALVE_PULSE_MS);
    gpio_out(MUX_COM, 0);

    mux_select(0);
    gpio_out(BOOST_EN, 0);

    gpio_out(LED_GRN, 0);
    gpio_out(LED_RED, 0);

    valve_states[valve] = open;
}

/***********************************************************
 * ADC / Sensors
 ***********************************************************/
static int32_t read_adc_channel(uint8_t ch)
{
    TUYA_ADC_BASE_CFG_T cfg = {
        .ch_list.data = (1 << ch),
        .ch_nums = 1,
        .width = 12,
        .mode = TUYA_ADC_CONTINUOUS,
        .type = TUYA_ADC_INNER_SAMPLE_VOL,
        .conv_cnt = 1,
    };

    tkl_adc_init(TUYA_ADC_NUM_0, &cfg);

    int32_t val = 0;
    tkl_adc_read_voltage(TUYA_ADC_NUM_0, &val, 1);

    tkl_adc_deinit(TUYA_ADC_NUM_0);
    return val;
}

static int32_t read_battery_mv(void)
{
    /* ADC channel 1 = GPIO 25 = battery */
    int32_t mv = read_adc_channel(1);
    /* Voltage divider R5/R6 = 510k/510k, factor = 2 */
    return mv * 2;
}

static int32_t read_rain_sensor(void)
{
    /* ADC channel 2 = GPIO 24 = rain sensor */
    return read_adc_channel(2);
}

static int read_usb_power(void)
{
    TUYA_GPIO_BASE_CFG_T cfg = {
        .mode = TUYA_GPIO_PULLUP,
        .direct = TUYA_GPIO_INPUT,
    };
    tkl_gpio_init(USB_POWER, &cfg);
    TUYA_GPIO_LEVEL_E level = 0;
    tkl_gpio_read(USB_POWER, &level);
    return !level;  /* LOW=USB connected (true), HIGH=no USB (false) */
}

static int battery_mv_to_percent(int32_t mv)
{
    /* Li-Ion: 4200mV=100%, 3000mV=0% */
    if (mv >= 4200) return 100;
    if (mv <= 3000) return 0;
    return (mv - 3000) * 100 / 1200;
}

/***********************************************************
 * MQTT state publishing
 ***********************************************************/
static void publish_state(void *client)
{
    int32_t bat_mv = read_battery_mv();
    int bat_pct = battery_mv_to_percent(bat_mv);
    int32_t rain = read_rain_sensor();
    int usb_power = read_usb_power();

    char payload[384];
    int len = snprintf(payload, sizeof(payload),
        "{\"valve_1\":%s,\"valve_2\":%s,\"valve_3\":%s,\"valve_4\":%s,"
        "\"battery_mv\":%d,\"battery_percent\":%d,"
        "\"rain_sensor\":%d,\"usb_power\":%s}",
        valve_states[0] ? "true" : "false",
        valve_states[1] ? "true" : "false",
        valve_states[2] ? "true" : "false",
        valve_states[3] ? "true" : "false",
        (int)bat_mv, bat_pct,
        (int)rain,
        usb_power ? "true" : "false");

    mqtt_client_publish(client, MQTT_TOPIC_STATE, (uint8_t *)payload, len, MQTT_QOS_0);
}

/***********************************************************
 * MQTT callbacks
 ***********************************************************/
static void *g_mqtt_client = NULL;

static void on_connected(void *client, void *userdata)
{
    PR_NOTICE("MQTT connected!");
    g_mqtt_client = client;
}

static void on_disconnected(void *client, void *userdata)
{
    PR_NOTICE("MQTT disconnected!");
    g_mqtt_client = NULL;
}

static void on_subscribed(void *client, uint16_t msgid, void *userdata)
{
    PR_NOTICE("Subscribed OK (id=%d)", msgid);
    mqtt_client_publish(client, "irrigation/availability", (uint8_t *)"online", 6, MQTT_QOS_0);
}

static void on_message(void *client, uint16_t msgid, const mqtt_client_message_t *msg, void *userdata)
{
    PR_NOTICE("MQTT msg: %s = %.*s", msg->topic, msg->length, (char *)msg->payload);

    /* Parse topic: irrigation/{valve_num}/set
     * Payload: "true" or "false" or "1" or "0"
     */
    const char *topic = msg->topic;
    const char *payload = (const char *)msg->payload;
    int valve = -1;

    /* Find valve number from topic */
    const char *p = topic;
    while (*p && *p != '/') p++;  /* skip "irrigation" */
    if (*p == '/') p++;
    if (*p >= '1' && *p <= '4') {
        valve = *p - '1';  /* 0-3 */
    }

    if (valve < 0 || valve >= VALVE_COUNT) {
        PR_ERR("Invalid valve in topic: %s", topic);
        return;
    }

    int open = 0;
    if (msg->length >= 4 && memcmp(payload, "true", 4) == 0) open = 1;
    else if (msg->length >= 1 && payload[0] == '1') open = 1;
    else if (msg->length >= 5 && memcmp(payload, "false", 5) == 0) open = 0;
    else if (msg->length >= 1 && payload[0] == '0') open = 0;
    else {
        PR_ERR("Invalid payload: %.*s", msg->length, payload);
        return;
    }

    valve_set(valve, open);
    publish_state(client);
}

static void on_published(void *client, uint16_t msgid, void *userdata)
{
    PR_DEBUG("Published (id=%d)", msgid);
}

/***********************************************************
 * Network callback
 ***********************************************************/
static OPERATE_RET on_link_status(void *data)
{
    netmgr_status_e new_status = *((netmgr_status_e *)data);
    PR_NOTICE("Network status: %d -> %d", net_status, new_status);
    net_status = new_status;
    return OPRT_OK;
}

/***********************************************************
 * Main task
 ***********************************************************/
static void irrigation_task(void *arg)
{
    PR_NOTICE("============================================");
    PR_NOTICE("  Yieryi HH-LYAI-24 MQTT Irrigation");
    PR_NOTICE("  WiFi: %s", WIFI_SSID);
    PR_NOTICE("  MQTT: %s:%d", MQTT_HOST, MQTT_PORT);
    PR_NOTICE("============================================");

    /* Init hardware */
    gpio_out(BOOST_EN, 0);
    gpio_out(MUX_A, 0);
    gpio_out(MUX_B, 0);
    gpio_out(MUX_C, 0);
    gpio_out(MUX_COM, 0);
    gpio_out(LED_RED, 0);
    gpio_out(LED_GRN, 0);

    /* Close all valves on boot */
    for (int v = 0; v < VALVE_COUNT; v++) {
        valve_set(v, 0);
        tal_system_sleep(200);
    }

    /* Wait for network */
    PR_NOTICE("Waiting for WiFi...");
    gpio_out(LED_RED, 1);
    while (net_status != NETMGR_LINK_UP) {
        tal_system_sleep(100);
    }
    gpio_out(LED_RED, 0);
    gpio_out(LED_GRN, 1);
    PR_NOTICE("WiFi connected!");

    /* MQTT connect */
    static mqtt_ctx_t mqtt = {0};
    static const mqtt_client_config_t mqtt_cfg = {
        .cacert = NULL,
        .cacert_len = 0,
        .host = MQTT_HOST,
        .port = MQTT_PORT,
        .keepalive = 60,
        .timeout_ms = 5000,
        .clientid = MQTT_CLIENT_ID,
        .username = "",
        .password = "",
        .on_connected = on_connected,
        .on_disconnected = on_disconnected,
        .on_message = on_message,
        .on_subscribed = on_subscribed,
        .on_published = on_published,
        .userdata = NULL,
    };

    mqtt_client_status_t status;

mqtt_retry:
    PR_NOTICE("MQTT init...");
    status = mqtt_client_init(&mqtt, &mqtt_cfg);
    if (status != MQTT_STATUS_SUCCESS) {
        PR_ERR("MQTT init failed: %d, retrying in 30s", status);
        tal_system_sleep(30000);
        goto mqtt_retry;
    }

    PR_NOTICE("MQTT connecting to %s:%d...", MQTT_HOST, MQTT_PORT);
    status = mqtt_client_connect(&mqtt);
    if (status != MQTT_STATUS_SUCCESS) {
        PR_ERR("MQTT connect failed: %d, retrying in 30s", status);
        tal_system_sleep(30000);
        goto mqtt_retry;
    }

    gpio_out(LED_GRN, 0);
    PR_NOTICE("MQTT connected, subscribing...");

    tal_system_sleep(1000);
    mqtt_client_subscribe(&mqtt, "irrigation/+/set", MQTT_QOS_0);
    PR_NOTICE("Subscribed, entering main loop");

    /* Main loop */
    uint32_t last_state_publish = 0;
    while (1) {
        mqtt_client_yield(&mqtt);

        uint32_t now = tal_system_get_millisecond();
        if (g_mqtt_client && (now - last_state_publish > STATE_INTERVAL_MS)) {
            publish_state(g_mqtt_client);
            last_state_publish = now;
        }

        tal_system_sleep(100);
    }
}

/***********************************************************
 * Entry point
 ***********************************************************/
void user_main(void)
{
    OPERATE_RET rt = OPRT_OK;

    tal_log_init(TAL_LOG_LEVEL_DEBUG, 1024, (TAL_LOG_OUTPUT_CB)tkl_log_output);

    PR_NOTICE("Irrigation MQTT Firmware v1.0");
    PR_NOTICE("Chip: %s, Board: %s", PLATFORM_CHIP, PLATFORM_BOARD);

    tal_kv_init(&(tal_kv_cfg_t){
        .seed = "vmlkasdh93dlvlcy",
        .key = "dflfuap134ddlduq",
    });
    tal_sw_timer_init();
    tal_workq_init();
    tal_event_subscribe(EVENT_LINK_STATUS_CHG, "irrigation", on_link_status, SUBSCRIBE_TYPE_NORMAL);

    /* Network init */
    netmgr_init(NETCONN_WIFI);

    netconn_wifi_info_t wifi_info = {0};
    strcpy(wifi_info.ssid, WIFI_SSID);
    strcpy(wifi_info.pswd, WIFI_PASS);
    netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_SSID_PSWD, &wifi_info);

    /* Start main task */
    static THREAD_HANDLE task = NULL;
    THREAD_CFG_T thrd = {
        .stackDepth = 1024 * 8,
        .priority = THREAD_PRIO_2,
        .thrdname = "irrigation",
    };
    tal_thread_create_and_start(&task, NULL, NULL, irrigation_task, NULL, &thrd);
}

#if OPERATING_SYSTEM == SYSTEM_LINUX
void main(int argc, char *argv[]) { user_main(); while (1) { tal_system_sleep(500); } }
#else
static THREAD_HANDLE ty_app_thread = NULL;
static void tuya_app_thread(void *arg) { user_main(); tal_thread_delete(ty_app_thread); ty_app_thread = NULL; }
void tuya_app_main(void)
{
    THREAD_CFG_T thrd = {0};
    thrd.stackDepth = 1024 * 4;
    thrd.priority = THREAD_PRIO_1;
    thrd.thrdname = "tuya_app_main";
    tal_thread_create_and_start(&ty_app_thread, NULL, NULL, tuya_app_thread, NULL, &thrd);
}
#endif
