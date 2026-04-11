#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "mqtt_client.h"

#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "ssd1306.h"

#include "odarka.h"  // WIFI_SSID, WIFI_PASS

// #define WIFI_SSID      "YOUR_WIFI_SSID"
// #define WIFI_PASS      "YOUR_WIFI_PASSWORD"

#define TAG "BME280_MQTT_OLED"

// ========================= MQTT =========================
#define MQTT_BROKER_URI         "mqtt://broker.hivemq.com"
#define MQTT_SUB_TOPIC          "EMB1/#"

#define USER_NAME               "odarka"
#define DISPLAY_SOURCE          "teacher"   // кого показувати на OLED

#define MQTT_PUB_TOPIC_TEMP     "EMB1/" USER_NAME "/bme280/temp"
#define MQTT_PUB_TOPIC_HUM      "EMB1/" USER_NAME "/bme280/hum"
#define MQTT_PUB_TOPIC_PRES     "EMB1/" USER_NAME "/bme280/pres"

// ========================= Wi-Fi =========================
#define WIFI_CONNECTED_BIT BIT0
#define MQTT_CONNECTED_BIT BIT1

static EventGroupHandle_t wifi_event_group;
static esp_mqtt_client_handle_t mqtt_client = NULL;

// ========================= OLED (I2C) =========================
#define I2C_PORT        I2C_NUM_0
#define I2C_SCL         4
#define I2C_SDA         5

static i2c_master_bus_handle_t i2c_handle = NULL;

#define OLED_SIZE I2C_SSD1306_128x32_CONFIG_DEFAULT
#define OLED_CONTRAST   255

static ssd1306_handle_t oled_hdl = NULL;

// ========================= BME280 (SPI) =========================
#define PIN_NUM_MOSI 11
#define PIN_NUM_MISO 13
#define PIN_NUM_CLK  12
#define PIN_NUM_CS   10

static spi_device_handle_t bme280_spi;

// ========================= RX data for OLED =========================
typedef struct {
    char source[20];
    float temp;
    float hum;
    float pres;
    bool has_temp;
    bool has_hum;
    bool has_pres;
} rx_telemetry_t;

static rx_telemetry_t g_rx = {0};
static SemaphoreHandle_t rx_mutex = NULL;

// ========================= BME280 calibration =========================
static uint16_t dig_T1;
static int16_t  dig_T2, dig_T3;
static uint16_t dig_P1;
static int16_t  dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;
static uint8_t  dig_H1, dig_H3;
static int16_t  dig_H2, dig_H4, dig_H5;
static int8_t   dig_H6;

static int32_t t_fine;

// ======================================================
//                        OLED
// ======================================================
static void i2c_init(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = false,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_handle));
}

static void oled_init(void)
{
    ssd1306_config_t dev_cfg = OLED_SIZE;

    ESP_ERROR_CHECK(ssd1306_init(i2c_handle, &dev_cfg, &oled_hdl));

    if (oled_hdl == NULL) {
        ESP_LOGI(TAG, "ssd1306 init failed");
        assert(oled_hdl);
    }

    ssd1306_clear_display(oled_hdl, false);
    ESP_ERROR_CHECK(ssd1306_set_contrast(oled_hdl, OLED_CONTRAST));
}

static void oled_show_mqtt_data(void)
{
    char line0[20];
    char line1[20];
    char line2[20];
    char line3[20];

    xSemaphoreTake(rx_mutex, portMAX_DELAY);

    snprintf(line0, sizeof(line0), "EMB1");
    snprintf(line1, sizeof(line1), "%.19s", g_rx.source[0] ? g_rx.source : DISPLAY_SOURCE);

    if (g_rx.has_temp && g_rx.has_pres) {
        snprintf(line2, sizeof(line2), "T%.1f P%.0f", g_rx.temp, g_rx.pres);
    } else {
        snprintf(line2, sizeof(line2), "waiting...");
    }

    if (g_rx.has_hum) {
        snprintf(line3, sizeof(line3), "H%.1f%%", g_rx.hum);
    } else {
        snprintf(line3, sizeof(line3), "H --");
    }

    xSemaphoreGive(rx_mutex);

    ssd1306_clear_display(oled_hdl, false);
    ssd1306_display_text(oled_hdl, 0, line0, false);
    ssd1306_display_text(oled_hdl, 1, line1, false);
    ssd1306_display_text(oled_hdl, 2, line2, false);
    ssd1306_display_text(oled_hdl, 3, line3, false);
}

// ======================================================
//                     BME280 SPI
// ======================================================
static esp_err_t bme280_write(uint8_t reg, uint8_t value)
{
    uint8_t tx[2] = { reg & 0x7F, value };

    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx
    };

    return spi_device_transmit(bme280_spi, &t);
}

static esp_err_t bme280_read(uint8_t reg, uint8_t *data, size_t len)
{
    uint8_t tx[len + 1];
    uint8_t rx[len + 1];

    tx[0] = reg | 0x80;
    memset(&tx[1], 0xFF, len);

    spi_transaction_t t = {
        .length = (len + 1) * 8,
        .tx_buffer = tx,
        .rx_buffer = rx
    };

    esp_err_t ret = spi_device_transmit(bme280_spi, &t);
    if (ret == ESP_OK) {
        memcpy(data, &rx[1], len);
    }
    return ret;
}

static void bme280_read_calibration(void)
{
    uint8_t buf1[26];
    uint8_t buf2[7];

    ESP_ERROR_CHECK(bme280_read(0x88, buf1, 26));
    ESP_ERROR_CHECK(bme280_read(0xE1, buf2, 7));

    dig_T1 = (uint16_t)((buf1[1] << 8) | buf1[0]);
    dig_T2 = (int16_t)((buf1[3] << 8) | buf1[2]);
    dig_T3 = (int16_t)((buf1[5] << 8) | buf1[4]);

    dig_P1 = (uint16_t)((buf1[7] << 8) | buf1[6]);
    dig_P2 = (int16_t)((buf1[9] << 8) | buf1[8]);
    dig_P3 = (int16_t)((buf1[11] << 8) | buf1[10]);
    dig_P4 = (int16_t)((buf1[13] << 8) | buf1[12]);
    dig_P5 = (int16_t)((buf1[15] << 8) | buf1[14]);
    dig_P6 = (int16_t)((buf1[17] << 8) | buf1[16]);
    dig_P7 = (int16_t)((buf1[19] << 8) | buf1[18]);
    dig_P8 = (int16_t)((buf1[21] << 8) | buf1[20]);
    dig_P9 = (int16_t)((buf1[23] << 8) | buf1[22]);

    dig_H1 = buf1[25];
    dig_H2 = (int16_t)((buf2[1] << 8) | buf2[0]);
    dig_H3 = buf2[2];
    dig_H4 = (int16_t)((buf2[3] << 4) | (buf2[4] & 0x0F));
    dig_H5 = (int16_t)((buf2[5] << 4) | (buf2[4] >> 4));
    dig_H6 = (int8_t)buf2[6];
}

static float compensate_temperature(int32_t adc_T)
{
    float var1 = (((float)adc_T) / 16384.0f - ((float)dig_T1) / 1024.0f) * ((float)dig_T2);
    float var2 = ((((float)adc_T) / 131072.0f - ((float)dig_T1) / 8192.0f) *
                  (((float)adc_T) / 131072.0f - ((float)dig_T1) / 8192.0f)) *
                 ((float)dig_T3);

    t_fine = (int32_t)(var1 + var2);
    return (var1 + var2) / 5120.0f;
}

static float compensate_pressure(int32_t adc_P)
{
    float var1 = ((float)t_fine / 2.0f) - 64000.0f;
    float var2 = var1 * var1 * ((float)dig_P6) / 32768.0f;
    var2 = var2 + var1 * ((float)dig_P5) * 2.0f;
    var2 = (var2 / 4.0f) + (((float)dig_P4) * 65536.0f);
    var1 = (((float)dig_P3) * var1 * var1 / 524288.0f +
            ((float)dig_P2) * var1) / 524288.0f;
    var1 = (1.0f + var1 / 32768.0f) * ((float)dig_P1);

    if (var1 == 0.0f) {
        return 0.0f;
    }

    float p = 1048576.0f - (float)adc_P;
    p = (p - (var2 / 4096.0f)) * 6250.0f / var1;
    var1 = ((float)dig_P9) * p * p / 2147483648.0f;
    var2 = p * ((float)dig_P8) / 32768.0f;

    return (p + (var1 + var2 + ((float)dig_P7)) / 16.0f) / 100.0f; // hPa
}

static float compensate_humidity(int32_t adc_H)
{
    float h = ((float)t_fine) - 76800.0f;
    h = (adc_H - (((float)dig_H4) * 64.0f + ((float)dig_H5) / 16384.0f * h)) *
        (((float)dig_H2) / 65536.0f *
         (1.0f + ((float)dig_H6) / 67108864.0f * h *
          (1.0f + ((float)dig_H3) / 67108864.0f * h)));

    h = h * (1.0f - ((float)dig_H1) * h / 524288.0f);
    if (h > 100.0f) h = 100.0f;
    if (h < 0.0f) h = 0.0f;

    return h;
}

static void bme280_init_sensor(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32
    };

    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 1 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = PIN_NUM_CS,
        .queue_size = 1,
    };

    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &bme280_spi));

    uint8_t chip_id = 0;
    ESP_ERROR_CHECK(bme280_read(0xD0, &chip_id, 1));
    ESP_LOGI(TAG, "Chip ID: 0x%02X", chip_id);

    if (chip_id != 0x60) {
        ESP_LOGE(TAG, "BME280 not detected");
        abort();
    }

    ESP_ERROR_CHECK(bme280_write(0xE0, 0xB6));
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_ERROR_CHECK(bme280_write(0xF2, 0x01));
    ESP_ERROR_CHECK(bme280_write(0xF4, 0x27));
    ESP_ERROR_CHECK(bme280_write(0xF5, 0xA0));

    bme280_read_calibration();
    ESP_LOGI(TAG, "Calibration data loaded");
}

static void bme280_read_values(float *temp, float *press, float *hum)
{
    uint8_t raw[8];
    ESP_ERROR_CHECK(bme280_read(0xF7, raw, 8));

    int32_t adc_P = (raw[0] << 12) | (raw[1] << 4) | (raw[2] >> 4);
    int32_t adc_T = (raw[3] << 12) | (raw[4] << 4) | (raw[5] >> 4);
    int32_t adc_H = (raw[6] << 8)  | raw[7];

    *temp = compensate_temperature(adc_T);
    *press = compensate_pressure(adc_P);
    *hum = compensate_humidity(adc_H);
}

// ======================================================
//                        Wi-Fi
// ======================================================
static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | MQTT_CONNECTED_BIT);
        esp_wifi_connect();
        ESP_LOGI(TAG, "Retrying Wi-Fi connection...");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi connected");
    }
}

static void wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
}

// ======================================================
//                    MQTT parsing
// ======================================================
static void parse_and_store_message(const char *topic, const char *payload)
{
    char topic_copy[128];
    char source[20];
    char metric[16];
    float value;

    memset(source, 0, sizeof(source));
    memset(metric, 0, sizeof(metric));

    snprintf(topic_copy, sizeof(topic_copy), "%s", topic);
    value = strtof(payload, NULL);

    char *saveptr = NULL;
    char *p1 = strtok_r(topic_copy, "/", &saveptr); // EMB1
    char *p2 = strtok_r(NULL, "/", &saveptr);       // source
    char *p3 = strtok_r(NULL, "/", &saveptr);       // bme280
    char *p4 = strtok_r(NULL, "/", &saveptr);       // temp/hum/pres

    if (!p1 || !p2 || !p3 || !p4) return;
    if (strcmp(p1, "EMB1") != 0) return;
    if (strcmp(p3, "bme280") != 0) return;

    snprintf(source, sizeof(source), "%.19s", p2);
    snprintf(metric, sizeof(metric), "%s", p4);

    if (strcmp(source, DISPLAY_SOURCE) != 0) {
        return;
    }

    xSemaphoreTake(rx_mutex, portMAX_DELAY);

    snprintf(g_rx.source, sizeof(g_rx.source), "%.19s", source);

    if (strcmp(metric, "temp") == 0) {
        g_rx.temp = value;
        g_rx.has_temp = true;
    } else if (strcmp(metric, "hum") == 0) {
        g_rx.hum = value;
        g_rx.has_hum = true;
    } else if (strcmp(metric, "pres") == 0) {
        g_rx.pres = value;
        g_rx.has_pres = true;
    }

    xSemaphoreGive(rx_mutex);
}

// ======================================================
//                        MQTT
// ======================================================
static void mqtt_event_handler(void *handler_args,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        xEventGroupSetBits(wifi_event_group, MQTT_CONNECTED_BIT);

        esp_mqtt_client_subscribe(mqtt_client, MQTT_SUB_TOPIC, 0);
        ESP_LOGI(TAG, "Subscribed to: %s", MQTT_SUB_TOPIC);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT disconnected");
        xEventGroupClearBits(wifi_event_group, MQTT_CONNECTED_BIT);
        break;

    case MQTT_EVENT_DATA: {
        char topic[128];
        char payload[64];

        int topic_len = event->topic_len < (int)(sizeof(topic) - 1) ? event->topic_len : (int)(sizeof(topic) - 1);
        int data_len  = event->data_len  < (int)(sizeof(payload) - 1) ? event->data_len : (int)(sizeof(payload) - 1);

        memcpy(topic, event->topic, topic_len);
        topic[topic_len] = '\0';

        memcpy(payload, event->data, data_len);
        payload[data_len] = '\0';

        ESP_LOGI(TAG, "Received topic: %s", topic);
        ESP_LOGI(TAG, "Received data : %s", payload);

        parse_and_store_message(topic, payload);
        break;
    }

    default:
        break;
    }
}

static void mqtt_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt_client));
}

// ======================================================
//                        APP MAIN
// ======================================================
void app_main(void)
{
    esp_err_t ret;

    rx_mutex = xSemaphoreCreateMutex();

    i2c_init();
    oled_init();

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    bme280_init_sensor();
    wifi_init();
    mqtt_start();

    while (1) {
        float temp = 0.0f;
        float press = 0.0f;
        float hum = 0.0f;

        bme280_read_values(&temp, &press, &hum);

        ESP_LOGI(TAG, "Local BME280: T=%.2f °C  P=%.2f hPa  H=%.2f %%", temp, press, hum);

        if ((xEventGroupGetBits(wifi_event_group) & MQTT_CONNECTED_BIT) && mqtt_client != NULL) {
            char temp_str[16];
            char hum_str[16];
            char pres_str[16];

            snprintf(temp_str, sizeof(temp_str), "%.2f", temp);
            snprintf(hum_str, sizeof(hum_str), "%.2f", hum);
            snprintf(pres_str, sizeof(pres_str), "%.2f", press);

            esp_mqtt_client_publish(mqtt_client, MQTT_PUB_TOPIC_TEMP, temp_str, 0, 0, 0);
            esp_mqtt_client_publish(mqtt_client, MQTT_PUB_TOPIC_HUM, hum_str, 0, 0, 0);
            esp_mqtt_client_publish(mqtt_client, MQTT_PUB_TOPIC_PRES, pres_str, 0, 0, 0);

            ESP_LOGI(TAG, "Published: T=%s  P=%s  H=%s", temp_str, pres_str, hum_str);
        }

        oled_show_mqtt_data();

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}