/*
 * Copyright (c) 2023-2023, lihongquan
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2023-9-8      lihongquan   add license declaration
 */
#include "swd_host.h"
#include "target_swd.h"
#include "DAP.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "target_swd"

void TargetSWD::msleep(uint32_t ms)
{
    vTaskDelay(ms / portTICK_PERIOD_MS);
}

bool TargetSWD::init(void)
{
    /* 离线烧录强制 GPIO 位带；PORT_SWD_SETUP 在 swd_init() 内完成引脚切换 */
    SWD_TransferSpeed = kTransfer_GPIO_normal;
    DAP_Data.debug_port = DAP_PORT_SWD;
    DAP_Data.fast_clock = 0U;
    /* 更慢更稳，利于 PY32 等小芯片 */
    DAP_Data.clock_delay = 40U;

    if (!swd_init())
    {
        ESP_LOGE(TAG, "swd_init failed");
        return false;
    }

    /* 释放 nRESET，保证目标可响应 SWD */
    swd_set_target_reset(0);
    msleep(20);

    SWD_TransferSpeed = kTransfer_GPIO_normal;
    DAP_Data.fast_clock = 0U;
    DAP_Data.clock_delay = 40U;

    return true;
}

bool TargetSWD::off(void)
{
    return swd_off();
}

SWDIface::transfer_err_def TargetSWD::transer(uint32_t request, uint32_t *data)
{
    return static_cast<SWDIface::transfer_err_def>(SWD_Transfer(request, data));
}

void TargetSWD::swj_sequence(uint32_t count, const uint8_t *data)
{
    SWJ_Sequence(count, data);
}

void TargetSWD::set_target_reset(uint8_t asserted)
{
    /* 离线/Web 烧录只用硬件 nRESET，避免走 PIN_nRESET_OUT 里的软复位 */
    swd_set_target_reset_hw(asserted);
}

TargetSWD &TargetSWD::get_instance()
{
    static TargetSWD instance;
    return instance;
}
