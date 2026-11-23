#include "tinyusb.h"
#include "tusb_msc_storage.h"
#include <dirent.h>
#include <errno.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "diskio_impl.h"
#include "esp_check.h"
#include "diskio_sdmmc.h"
#include "wifi_config_file.h"

static const char *TAG = "msc_disk";

static wl_handle_t wl_handle;

static esp_err_t storage_init_spiflash(wl_handle_t *wl_handle)
{
    // ESP_LOGI(TAG, "Initializing wear levelling");

    const esp_partition_t *data_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, NULL);
    if (data_partition == NULL) {
        ESP_LOGE(TAG, "Failed to find FATFS partition. Check the partition table.");
        return ESP_ERR_NOT_FOUND;
    }

    return wl_mount(data_partition, wl_handle);
}

/*******************************************************************************************
函数名称 : msc_disk_prepare_wifi_ini
功 能 : 本地挂载 FAT，确保存在 wifi.ini 后卸载，供 PC 识别盘符
参 数 : path - VFS 挂载路径
返 回 值 : 无
*******************************************************************************************/
static void msc_disk_prepare_wifi_ini(const char *path)
{
    esp_err_t err = ESP_OK;

    err = tinyusb_msc_storage_mount(path);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Local mount for wifi.ini failed: %s", esp_err_to_name(err));
        return;
    }

    (void)wifi_config_file_ensure_default(path);

    /* 卸载后交给 USB Host 访问盘符 */
    err = tinyusb_msc_storage_unmount();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Unmount after wifi.ini prepare failed: %s", esp_err_to_name(err));
    }
}

bool msc_disk_mount(const char *path) {

    wl_handle = WL_INVALID_HANDLE;
    ESP_ERROR_CHECK(storage_init_spiflash(&wl_handle));

    const tinyusb_msc_spiflash_config_t config_spi = {
        .wl_handle = wl_handle,
        // .callback_mount_changed = storage_mount_changed_cb,  /* First way to register the callback. This is while initializing the storage. */
        // .mount_config.max_files = 5,
    };
    ESP_ERROR_CHECK(tinyusb_msc_storage_init_spiflash(&config_spi));
    // ESP_ERROR_CHECK(tinyusb_msc_register_callback(TINYUSB_MSC_EVENT_MOUNT_CHANGED, storage_mount_changed_cb));

    /* 有线模式：在盘中准备 wifi.ini 供用户编辑 */
    msc_disk_prepare_wifi_ini(path);

    return true;
}
