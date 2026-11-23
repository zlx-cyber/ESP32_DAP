#ifndef __WIFI_CONFIG_FILE_H
#define __WIFI_CONFIG_FILE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** WiFi 配置文件名（位于 MSC 根目录） */
#define WIFI_CONFIG_FILE_NAME       "wifi.ini"

/** SSID 最大长度（不含结束符） */
#define WIFI_CONFIG_SSID_MAX_LEN    32U

/** 密码最大长度（不含结束符） */
#define WIFI_CONFIG_PASS_MAX_LEN    64U

/*******************************************************************************************
函数名称 : wifi_config_file_ensure_default
功 能 : 有线模式下在已挂载的 MSC 路径创建默认 wifi.ini（若不存在）
参 数 : base_path - MSC/VFS 挂载根路径，如 "/data"
返 回 值 : bool - true 表示文件已存在或创建成功；false 表示失败
*******************************************************************************************/
bool wifi_config_file_ensure_default(const char *base_path);

/*******************************************************************************************
函数名称 : wifi_config_storage_mount
功 能 : 无线模式持久挂载 Flash FAT 到 /data，供 Web 上传/烧录与读取 wifi.ini
参 数 : 无
返 回 值 : bool - true 挂载成功或已挂载；false 失败
*******************************************************************************************/
bool wifi_config_storage_mount(void);

/*******************************************************************************************
函数名称 : wifi_config_storage_is_mounted
功 能 : 查询 /data FAT 是否已持久挂载
参 数 : 无
返 回 值 : bool - true 已挂载
*******************************************************************************************/
bool wifi_config_storage_is_mounted(void);

/*******************************************************************************************
函数名称 : wifi_config_file_load
功 能 : 读取 wifi.ini 中的 ssid/password；若已持久挂载则直接读，否则临时挂载后卸载
参 数 : ssid - 输出缓冲区
        ssid_len - ssid 缓冲区长度
        password - 输出缓冲区
        password_len - password 缓冲区长度
返 回 值 : bool - true 表示读取到有效配置；false 表示无文件或无效，应回退 wifi_list
*******************************************************************************************/
bool wifi_config_file_load(char *ssid, size_t ssid_len, char *password, size_t password_len);

#ifdef __cplusplus
}
#endif

#endif /* __WIFI_CONFIG_FILE_H */
