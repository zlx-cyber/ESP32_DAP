/*
 * Copyright (c) 2023-2023, lihongquan
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2023-9-8      lihongquan   add license declaration
 */

#include "cdc_uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

typedef struct
{
    uart_port_t uart;
    cdc_uart_cb_t cb[CDC_UART_HANDLER_NUM];
} cdc_uart_t;

static cdc_uart_t s_cdc_uart = {0};
static const char *TAG = "cdc_uart";
static TaskHandle_t s_rx_task = NULL;
static void cdc_uart_rx_task(void *param);

bool cdc_uart_init(uart_port_t uart, gpio_num_t tx_pin, gpio_num_t rx_pin, int baudrate, bool create_task)
{
    bool ret = false;
    uart_config_t uart_config = {
        .baud_rate = baudrate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    s_cdc_uart.uart = uart;
    ret = (ESP_OK == uart_driver_install(s_cdc_uart.uart, 2 * 1024, 2 * 1024, 0, NULL, 0));
    ret = ret && (ESP_OK == uart_param_config(s_cdc_uart.uart, &uart_config));
    ret = ret && (ESP_OK == uart_set_pin(s_cdc_uart.uart, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    if (ret && create_task && (s_rx_task == NULL))
    {
        if (xTaskCreate(cdc_uart_rx_task, "cdc_uart_rx_task", 4096, (void *)&s_cdc_uart, 10, &s_rx_task) != pdPASS)
        {
            ESP_LOGE(TAG, "Create RX task failed");
            s_rx_task = NULL;
            ret = false;
        }
        else
        {
            ESP_LOGI(TAG, "UART RX task started");
        }
    }

    return ret;
}

bool cdc_uart_set_baudrate(uint32_t baudrate)
{
    return (ESP_OK == uart_set_baudrate(s_cdc_uart.uart, baudrate));
}

bool cdc_uart_get_baudrate(uint32_t *baudrate)
{
    return (ESP_OK == uart_get_baudrate(s_cdc_uart.uart, baudrate));
}

bool cdc_uart_write(const void *src, size_t size)
{
    return (0 <= uart_write_bytes(s_cdc_uart.uart, src, size));
}

void cdc_uart_register_rx_handler(cdc_uart_handler_def handler, cdc_uart_rx_callback_t func, void *context)
{
    if ((handler >= CDC_UART_HANDLER_NUM) || !func)
    {
        return;
    }

    s_cdc_uart.cb[handler].func = func;
    s_cdc_uart.cb[handler].usr_data = context;
    ESP_LOGI(TAG, "RX handler %d registered", (int)handler);
}

static void cdc_uart_rx_task(void *param)
{
#define RX_BUF_SIZE 256
    uint8_t data[RX_BUF_SIZE] = {0};
    cdc_uart_t *cdc_uart = (cdc_uart_t *)param;
    int read = 0;
    int i = 0;

    while (1)
    {
        read = uart_read_bytes(cdc_uart->uart, data, RX_BUF_SIZE, pdMS_TO_TICKS(50));

        if (read > 0)
        {
            for (i = 0; i < CDC_UART_HANDLER_NUM; i++)
            {
                if (cdc_uart->cb[i].func)
                {
                    cdc_uart->cb[i].func(cdc_uart->cb[i].usr_data, data, (size_t)read);
                }
            }
        }
    }
}
