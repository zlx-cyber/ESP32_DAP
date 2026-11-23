#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "sdkconfig.h"

#include "wifi_config_file.h"

static const char *TAG = "wifi_cfg";

/** Flash FAT 分区标签，与 partitions.csv 中 storage 一致 */
#define WIFI_CONFIG_FAT_LABEL           "storage"

/** 默认挂载路径 */
#ifndef CONFIG_TINYUSB_MSC_MOUNT_PATH
#define WIFI_CONFIG_MOUNT_PATH          "/data"
#else
#define WIFI_CONFIG_MOUNT_PATH          CONFIG_TINYUSB_MSC_MOUNT_PATH
#endif

/** 无线模式持久挂载句柄 */
static wl_handle_t s_storage_wl_handle = WL_INVALID_HANDLE;
static bool s_storage_mounted = false;

/** 默认配置文件模板 */
static const char s_wifi_ini_template[] =
    "# WiFi configuration for wireless mode\r\n"
    "# Edit ssid/password below, then long-press button to switch wireless mode\r\n"
    "ssid=\r\n"
    "password=\r\n";

/*******************************************************************************************
函数名称 : wifi_config_trim
功 能 : 去除字符串首尾空白与回车换行
参 数 : str - 待处理字符串
返 回 值 : char* - 处理后的起始指针
*******************************************************************************************/
static char *wifi_config_trim(char *str)
{
    char *end = NULL;

    if (str == NULL)
    {
        return str;
    }

    while ((*str == ' ') || (*str == '\t') || (*str == '\r') || (*str == '\n'))
    {
        str++;
    }

    if (*str == '\0')
    {
        return str;
    }

    end = str + strlen(str) - 1U;
    while ((end > str) && ((*end == ' ') || (*end == '\t') || (*end == '\r') || (*end == '\n')))
    {
        *end = '\0';
        end--;
    }

    return str;
}

/*******************************************************************************************
函数名称 : wifi_config_parse_file
功 能 : 解析已打开的 wifi.ini，提取 ssid/password
参 数 : fp - 文件指针
        ssid - 输出缓冲区
        ssid_len - ssid 缓冲区长度
        password - 输出缓冲区
        password_len - password 缓冲区长度
返 回 值 : bool - true 表示至少读到非空 ssid
*******************************************************************************************/
static bool wifi_config_parse_file(FILE *fp, char *ssid, size_t ssid_len,
                                   char *password, size_t password_len)
{
    char line[128] = {0};
    char *p = NULL;
    char *value = NULL;
    bool has_ssid = false;

    if ((fp == NULL) || (ssid == NULL) || (password == NULL) ||
        (ssid_len == 0U) || (password_len == 0U))
    {
        return false;
    }

    ssid[0] = '\0';
    password[0] = '\0';

    while (fgets(line, (int)sizeof(line), fp) != NULL)
    {
        p = wifi_config_trim(line);

        /* 跳过空行与注释 */
        if ((p[0] == '\0') || (p[0] == '#') || (p[0] == ';'))
        {
            continue;
        }

        /* 跳过节名 [wifi] */
        if (p[0] == '[')
        {
            continue;
        }

        value = strchr(p, '=');
        if (value == NULL)
        {
            continue;
        }

        *value = '\0';
        value++;
        p = wifi_config_trim(p);
        value = wifi_config_trim(value);

        if (strcmp(p, "ssid") == 0)
        {
            strncpy(ssid, value, ssid_len - 1U);
            ssid[ssid_len - 1U] = '\0';
            if (ssid[0] != '\0')
            {
                has_ssid = true;
            }
        }
        else if ((strcmp(p, "password") == 0) || (strcmp(p, "passwd") == 0))
        {
            strncpy(password, value, password_len - 1U);
            password[password_len - 1U] = '\0';
        }
        else
        {
            /* 忽略其他键 */
        }
    }

    return has_ssid;
}

/*******************************************************************************************
函数名称 : wifi_config_build_path
功 能 : 拼接配置文件完整路径
参 数 : base_path - 根路径
        out - 输出缓冲区
        out_len - 缓冲区长度
返 回 值 : bool - true 成功
*******************************************************************************************/
static bool wifi_config_build_path(const char *base_path, char *out, size_t out_len)
{
    int ret = 0;

    if ((base_path == NULL) || (out == NULL) || (out_len < 8U))
    {
        return false;
    }

    if (base_path[strlen(base_path) - 1U] == '/')
    {
        ret = snprintf(out, out_len, "%s%s", base_path, WIFI_CONFIG_FILE_NAME);
    }
    else
    {
        ret = snprintf(out, out_len, "%s/%s", base_path, WIFI_CONFIG_FILE_NAME);
    }

    return (ret > 0) && ((size_t)ret < out_len);
}

/*******************************************************************************************
函数名称 : wifi_config_file_ensure_default
功 能 : 有线模式下在已挂载的 MSC 路径创建默认 wifi.ini（若不存在）
参 数 : base_path - MSC/VFS 挂载根路径，如 "/data"
返 回 值 : bool - true 表示文件已存在或创建成功；false 表示失败
*******************************************************************************************/
bool wifi_config_file_ensure_default(const char *base_path)
{
    char path[64] = {0};
    FILE *fp = NULL;
    struct stat st = {0};
    size_t wrote = 0U;

    if (base_path == NULL)
    {
        base_path = WIFI_CONFIG_MOUNT_PATH;
    }

    if (wifi_config_build_path(base_path, path, sizeof(path)) != true)
    {
        ESP_LOGE(TAG, "Build wifi.ini path failed");
        return false;
    }

    /* 已存在则不覆盖用户配置 */
    if (stat(path, &st) == 0)
    {
        ESP_LOGI(TAG, "%s already exists", path);
        return true;
    }

    fp = fopen(path, "w");
    if (fp == NULL)
    {
        ESP_LOGE(TAG, "Create %s failed", path);
        return false;
    }

    wrote = fwrite(s_wifi_ini_template, 1U, sizeof(s_wifi_ini_template) - 1U, fp);
    fclose(fp);

    if (wrote != (sizeof(s_wifi_ini_template) - 1U))
    {
        ESP_LOGE(TAG, "Write %s failed", path);
        return false;
    }

    ESP_LOGI(TAG, "Created default %s", path);
    return true;
}

/*******************************************************************************************
函数名称 : wifi_config_storage_mount
功 能 : 无线模式持久挂载 Flash FAT 到 /data，供 Web 上传/烧录与读取 wifi.ini
参 数 : 无
返 回 值 : bool - true 挂载成功或已挂载；false 失败
*******************************************************************************************/
bool wifi_config_storage_mount(void)
{
    esp_err_t err = ESP_OK;
    const esp_vfs_fat_mount_config_t mount_config =
    {
        .max_files = 8,
        .format_if_mount_failed = false,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };

    if (s_storage_mounted == true)
    {
        return true;
    }

    err = esp_vfs_fat_spiflash_mount_rw_wl(WIFI_CONFIG_MOUNT_PATH,
                                           WIFI_CONFIG_FAT_LABEL,
                                           &mount_config,
                                           &s_storage_wl_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Persist mount FAT failed: %s", esp_err_to_name(err));
        s_storage_wl_handle = WL_INVALID_HANDLE;
        return false;
    }

    s_storage_mounted = true;
    ESP_LOGI(TAG, "FAT mounted at %s for wireless storage", WIFI_CONFIG_MOUNT_PATH);
    return true;
}

/*******************************************************************************************
函数名称 : wifi_config_storage_is_mounted
功 能 : 查询 /data FAT 是否已持久挂载
参 数 : 无
返 回 值 : bool - true 已挂载
*******************************************************************************************/
bool wifi_config_storage_is_mounted(void)
{
    return s_storage_mounted;
}

/*******************************************************************************************
函数名称 : wifi_config_file_load
功 能 : 读取 wifi.ini 中的 ssid/password；若已持久挂载则直接读，否则临时挂载后卸载
参 数 : ssid - 输出缓冲区
        ssid_len - ssid 缓冲区长度
        password - 输出缓冲区
        password_len - password 缓冲区长度
返 回 值 : bool - true 表示读取到有效配置；false 表示无文件或无效，应回退 wifi_list
*******************************************************************************************/
bool wifi_config_file_load(char *ssid, size_t ssid_len, char *password, size_t password_len)
{
    esp_err_t err = ESP_OK;
    wl_handle_t wl_handle = WL_INVALID_HANDLE;
    FILE *fp = NULL;
    char path[64] = {0};
    bool ok = false;
    bool temp_mount = false;
    const esp_vfs_fat_mount_config_t mount_config =
    {
        .max_files = 4,
        .format_if_mount_failed = false,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };

    if ((ssid == NULL) || (password == NULL) || (ssid_len == 0U) || (password_len == 0U))
    {
        return false;
    }

    ssid[0] = '\0';
    password[0] = '\0';

    /* 已持久挂载则直接读文件，避免反复挂载/卸载导致 Web 无法写盘 */
    if (s_storage_mounted != true)
    {
        err = esp_vfs_fat_spiflash_mount_rw_wl(WIFI_CONFIG_MOUNT_PATH,
                                               WIFI_CONFIG_FAT_LABEL,
                                               &mount_config,
                                               &wl_handle);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "Mount FAT failed: %s, fallback wifi_list", esp_err_to_name(err));
            return false;
        }

        temp_mount = true;
    }

    if (wifi_config_build_path(WIFI_CONFIG_MOUNT_PATH, path, sizeof(path)) != true)
    {
        if (temp_mount == true)
        {
            esp_vfs_fat_spiflash_unmount_rw_wl(WIFI_CONFIG_MOUNT_PATH, wl_handle);
        }

        return false;
    }

    fp = fopen(path, "r");
    if (fp == NULL)
    {
        ESP_LOGW(TAG, "%s not found, fallback wifi_list", path);
        if (temp_mount == true)
        {
            esp_vfs_fat_spiflash_unmount_rw_wl(WIFI_CONFIG_MOUNT_PATH, wl_handle);
        }

        return false;
    }

    ok = wifi_config_parse_file(fp, ssid, ssid_len, password, password_len);
    fclose(fp);

    if (temp_mount == true)
    {
        esp_vfs_fat_spiflash_unmount_rw_wl(WIFI_CONFIG_MOUNT_PATH, wl_handle);
    }

    if (ok != true)
    {
        ESP_LOGW(TAG, "Invalid ssid in wifi.ini, fallback wifi_list");
        return false;
    }

    ESP_LOGI(TAG, "Loaded WiFi from wifi.ini, ssid=%s", ssid);
    return true;
}
