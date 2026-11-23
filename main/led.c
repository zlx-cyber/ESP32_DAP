#include "stdio.h"
#include "led_strip.h"
#include "led.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_err.h"
#include "esp_log.h"

#define TAG "led"

extern TaskHandle_t ws2812_task_handle;
extern bool tcpserver_run;

/*
 * 板载 WS2812 为 GRB；蓝色通道用满亮度，略加红避免部分灯珠偏青/发暗。
 */
const rgb_color_t led_colors[] = {
    [LED_COLOR_ORANGE] = {.r = 255, .g = 128, .b = 0},
    [LED_COLOR_PURPLE] = {.r = 128, .g = 0, .b = 255},
    [LED_COLOR_RED]    = {.r = 255, .g = 0, .b = 0},
    [LED_COLOR_GREEN]  = {.r = 0, .g = 255, .b = 0},
    [LED_COLOR_BLUE]   = {.r = 32, .g = 0, .b = 255},
    [LED_COLOR_WHITE]  = {.r = 255, .g = 255, .b = 255},
    [LED_COLOR_BLACK]  = {.r = 0, .g = 0, .b = 0}
};

// LED strip common configuration
led_strip_config_t strip_config = {
    .strip_gpio_num = GPIO_LED_WIFI_STATUS,  // The GPIO that connected to the LED strip's data line
    .max_leds = 1,                 // The number of LEDs in the strip,
    .led_model = LED_MODEL_WS2812, // LED strip model, it determines the bit timing
    .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB, // The color component format is G-R-B
    .flags = {
        .invert_out = false, // don't invert the output signal
    }
};

/// RMT backend specific configuration
led_strip_rmt_config_t rmt_config = {
    .clk_src = RMT_CLK_SRC_DEFAULT,    // different clock source can lead to different power consumption
    .resolution_hz = 10 * 1000 * 1000, // RMT counter clock frequency: 10MHz
    .mem_block_symbols = 64,           // the memory size of each RMT channel, in words (4 bytes)
    .flags = {
        .with_dma = false, // DMA feature is available on chips like ESP32-S3/P4
    }
};

/// Create the LED strip object
led_strip_handle_t led_strip;

void ws2812_init(void){
    // 初始化信号灯
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
}

/**
 * @brief LED设置颜色函数
 * @param r: 红色分量(0-255)
 * @param g: 绿色分量(0-255)
 * @param b: 蓝色分量(0-255)
 */
void ws2812_set_color(led_color_type_t color)
{
    const rgb_color_t *rgb = &led_colors[color];
    esp_err_t err = ESP_OK;

    if (led_strip == NULL)
    {
        ESP_LOGE(TAG, "led_strip not initialized");
        return;
    }

    err = led_strip_set_pixel(led_strip, 0, rgb->r, rgb->g, rgb->b);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "set_pixel failed: %s", esp_err_to_name(err));
        return;
    }

    err = led_strip_refresh(led_strip);
    if (err != ESP_OK)
    {
        /* USB 初始化后偶发 RMT 忙，重试一次 */
        vTaskDelay(pdMS_TO_TICKS(10));
        err = led_strip_refresh(led_strip);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "refresh failed: %s", esp_err_to_name(err));
        }
    }
}

/**
 * @brief LED闪烁控制函数
 * @param delay_ms: 闪烁延时(ms)
 * @param blink_count: 闪烁次数
 * @param r: 红色分量(0-255)
 * @param g: 绿色分量(0-255)
 * @param b: 蓝色分量(0-255)
 */
void ws2812_blink(uint32_t delay_ms, uint8_t blink_count, led_color_type_t color)
{
    const rgb_color_t *rgb = &led_colors[color];
    
    for (uint8_t i = 0; i < blink_count; i++) {
        led_strip_set_pixel(led_strip, 0, rgb->r, rgb->g, rgb->b);
        led_strip_refresh(led_strip);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));

        led_strip_set_pixel(led_strip, 0, 0, 0, 0);
        led_strip_refresh(led_strip);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

/**
 * @brief LED呼吸灯控制函数
 * @param r: 红色最大亮度(0-255)
 * @param g: 绿色最大亮度(0-255)
 * @param b: 蓝色最大亮度(0-255)
 * @param breath_period_ms: 呼吸周期(ms)
 */
void ws2812_breath(led_color_type_t color, uint32_t breath_period_ms)
{
    const rgb_color_t *rgb = &led_colors[color];
    const int STEPS = 100;
    float step_time = (float)breath_period_ms / (STEPS * 2);
    
    // 渐亮
    for(int i = 0; i <= STEPS; i++) {
        float ratio = (float)i / STEPS;
        led_strip_set_pixel(led_strip, 0, 
                          (uint8_t)(rgb->r * ratio),
                          (uint8_t)(rgb->g * ratio),
                          (uint8_t)(rgb->b * ratio));
        led_strip_refresh(led_strip);
        vTaskDelay(pdMS_TO_TICKS(step_time));
    }
    
    // 渐暗
    for(int i = STEPS; i >= 0; i--) {
        float ratio = (float)i / STEPS;
        led_strip_set_pixel(led_strip, 0,
                          (uint8_t)(rgb->r * ratio),
                          (uint8_t)(rgb->g * ratio),
                          (uint8_t)(rgb->b * ratio));
        led_strip_refresh(led_strip);
        vTaskDelay(pdMS_TO_TICKS(step_time));
    }
}

void ws2812_task(void *pvParameters) {
    while (1) {

        if (ws2812_task_handle == NULL) {
            // 清理并退出
            ws2812_set_color(LED_COLOR_BLACK);
            vTaskDelete(NULL);
            return;
        }

        // 等待通知
        ws2812_set_color(LED_COLOR_RED);
        
        while (tcpserver_run) {
            // 检查WiFi连接
            wifi_ap_record_t ap_info;
            esp_err_t status = esp_wifi_sta_get_ap_info(&ap_info);
            if (status != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            } 

            if (status == ESP_ERR_WIFI_NOT_CONNECT) {
                // WiFi未连接，无需打印警告
                ws2812_set_color(LED_COLOR_RED);
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }

            if (status == ESP_OK) {
                int8_t rssi = ap_info.rssi;

                if (rssi > -70) {
                    /* 信号好：绿色常亮 */
                    ws2812_set_color(LED_COLOR_GREEN);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    continue;
                }
                else if (rssi > -80) {
                    /* RSSI -70 ~ -80：绿色慢闪 */
                    ws2812_blink(500, 1, LED_COLOR_GREEN);
                }
                else {
                    /* RSSI ≤ -80：绿色呼吸 */
                    ws2812_breath(LED_COLOR_GREEN, 2000);
                }
            }
            else {
                // 未连接WiFi，LED常亮红色
                ws2812_set_color(LED_COLOR_RED);
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}