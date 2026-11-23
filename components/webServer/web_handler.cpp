/*
 * Copyright (c) 2023-2023, lihongquan
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2023-9-8      lihongquan   add license declaration
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "web_handler.h"
#include "cdc_uart.h"
#include "programmer.h"
#include "cJSON.h"
#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#define TAG "web_handler"
#define WEB_RESOURCE_NUM 4
#define PROGRAM_MAX_SIZE 0xA00000
#define PROGRAM_MAX_SIZE_STR "10M"
#define IS_FILE_EXT(filename, ext) (strcasecmp(&filename[strlen(filename) - sizeof(ext) + 1], ext) == 0)

/*******************************************************************************************
函数名称 : web_is_safe_filename
功 能 : 校验上传/列表文件名，禁止路径穿越与特殊字符
参 数 : name - 文件名（仅 basename）
返 回 值 : bool - true 合法，false 非法
*******************************************************************************************/
static bool web_is_safe_filename(const char *name)
{
    size_t i = 0;
    size_t len = 0;

    if ((name == NULL) || (name[0] == '\0'))
    {
        return false;
    }

    len = strlen(name);
    if ((len == 0) || (len >= 128U))
    {
        return false;
    }

    if ((strcmp(name, ".") == 0) || (strcmp(name, "..") == 0))
    {
        return false;
    }

    for (i = 0; i < len; i++)
    {
        char c = name[i];

        if ((c == '/') || (c == '\\') || (c == ':') || (c == '\0'))
        {
            return false;
        }

        if ((c == '.') && (i + 1U < len) && (name[i + 1U] == '.'))
        {
            return false;
        }

        /* 允许字母数字、点、下划线、短横线 */
        if (!(((c >= 'A') && (c <= 'Z')) ||
              ((c >= 'a') && (c <= 'z')) ||
              ((c >= '0') && (c <= '9')) ||
              (c == '.') || (c == '_') || (c == '-')))
        {
            return false;
        }
    }

    return true;
}

/*******************************************************************************************
函数名称 : web_send_html_escaped
功 能 : 向 HTTP 响应写入经 HTML 转义的字符串
参 数 : req - HTTP 请求
        text - 原始文本
返 回 值 : 无
*******************************************************************************************/
static void web_send_html_escaped(httpd_req_t *req, const char *text)
{
    char out[256];
    size_t oi = 0;
    size_t i = 0;

    if (text == NULL)
    {
        return;
    }

    for (i = 0; text[i] != '\0'; i++)
    {
        const char *rep = NULL;
        size_t rep_len = 1;
        char one[2] = {0};

        switch (text[i])
        {
        case '&':
            rep = "&amp;";
            rep_len = 5;
            break;
        case '<':
            rep = "&lt;";
            rep_len = 4;
            break;
        case '>':
            rep = "&gt;";
            rep_len = 4;
            break;
        case '"':
            rep = "&quot;";
            rep_len = 6;
            break;
        case '\'':
            rep = "&#39;";
            rep_len = 5;
            break;
        default:
            one[0] = text[i];
            rep = one;
            rep_len = 1;
            break;
        }

        if ((oi + rep_len) >= sizeof(out))
        {
            out[oi] = '\0';
            httpd_resp_sendstr_chunk(req, out);
            oi = 0;
        }

        memcpy(out + oi, rep, rep_len);
        oi += rep_len;
    }

    if (oi > 0)
    {
        out[oi] = '\0';
        httpd_resp_sendstr_chunk(req, out);
    }
}

typedef struct
{
    const char *path;
    const uint8_t *start;
    const uint8_t *end;
} web_resource_map_t;

extern const uint8_t root_html_start[] asm("_binary_root_html_start");
extern const uint8_t root_html_end[] asm("_binary_root_html_end");
extern const uint8_t program_html_start[] asm("_binary_program_html_start");
extern const uint8_t program_html_end[] asm("_binary_program_html_end");
extern const uint8_t webserial_html_start[] asm("_binary_webserial_html_start");
extern const uint8_t webserial_html_end[] asm("_binary_webserial_html_end");
extern const uint8_t favicon_ico_start[] asm("_binary_favicon_ico_start");
extern const uint8_t favicon_ico_end[] asm("_binary_favicon_ico_end");

web_resource_map_t s_resource_map[WEB_RESOURCE_NUM] = {
    {"/data/httpd/root.html", root_html_start, root_html_end},
    {"/data/httpd/favicon.ico", favicon_ico_start, favicon_ico_end},
    {"/data/httpd/program.html", program_html_start, program_html_end},
    {"/data/httpd/webserial.html", webserial_html_start, webserial_html_end}};

void web_send_to_clients(void *context, uint8_t *data, size_t size)
{
    httpd_handle_t http_server = *((httpd_handle_t *)context);
    size_t clients = CONFIG_HTTPD_MAX_OPENED_SOCKETS;
    static int client_fds[CONFIG_HTTPD_MAX_OPENED_SOCKETS] = {0};
    /* 双缓冲：async 发送期间 payload 必须保持有效 */
    static uint8_t s_tx_bufs[2][CONFIG_HTTPD_RESP_BUF_SIZE];
    static int s_tx_idx = 0;
    size_t copy_len = 0;
    uint8_t *payload = NULL;
    httpd_ws_frame_t ws_pkt = {false, false, HTTPD_WS_TYPE_TEXT, NULL, 0};

    if (!http_server || !data || (size == 0))
    {
        return;
    }

    copy_len = (size > CONFIG_HTTPD_RESP_BUF_SIZE) ? CONFIG_HTTPD_RESP_BUF_SIZE : size;
    s_tx_idx ^= 1;
    payload = s_tx_bufs[s_tx_idx];
    memcpy(payload, data, copy_len);
    ws_pkt.payload = payload;
    ws_pkt.len = copy_len;

    if (httpd_get_client_list(http_server, &clients, client_fds) == ESP_OK)
    {
        for (size_t i = 0; i < clients; ++i)
        {
            if (httpd_ws_get_fd_info(http_server, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET)
            {
                httpd_ws_send_frame_async(http_server, client_fds[i], &ws_pkt);
            }
        }
    }
}

esp_err_t web_send_to_uart(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    web_data_t *data = (web_data_t *)req->user_ctx;
    httpd_ws_frame_t ws_pkt = {false, false, HTTPD_WS_TYPE_TEXT, NULL, 0};

    if (req->method == HTTP_GET)
    {
        ESP_LOGI(TAG, "Handshake done, the new connection was opened");
        return ESP_OK;
    }

    /* Set max_len = 0 to get the frame len */
    ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame len with %d", ret);
        goto __exit;
    }

    if ((ws_pkt.len > CONFIG_HTTPD_RESP_BUF_SIZE))
    {
        ESP_LOGE(TAG, "Frame length is over the limit(%d)", CONFIG_HTTPD_RESP_BUF_SIZE);
        ret = ESP_ERR_INVALID_SIZE;
        goto __exit;
    }

    ws_pkt.payload = data->buf;
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
        goto __exit;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT)
    {
        cdc_uart_write(data->buf, ws_pkt.len);
    }

    ret = ESP_OK;

__exit:

    return ret;
}

static esp_err_t web_set_content_type(httpd_req_t *req, const char *filename)
{
    if (IS_FILE_EXT(filename, ".pdf"))
    {
        return httpd_resp_set_type(req, "application/pdf");
    }
    else if (IS_FILE_EXT(filename, ".html"))
    {
        return httpd_resp_set_type(req, "text/html");
    }
    else if (IS_FILE_EXT(filename, ".jpeg"))
    {
        return httpd_resp_set_type(req, "image/jpeg");
    }
    else if (IS_FILE_EXT(filename, ".ico"))
    {
        return httpd_resp_set_type(req, "image/x-icon");
    }

    return httpd_resp_set_type(req, "text/plain");
}

static bool web_resp_file(httpd_req_t *req, const char *filename, char *buf, size_t size)
{
    int i = 0;
    uint32_t offset = 0;
    uint32_t chunksize = 0;
    uint32_t file_size = 0;

    for (i = 0; i < WEB_RESOURCE_NUM; i++)
    {
        if (!strcmp(filename, s_resource_map[i].path))
        {
            break;
        }
    }

    if (i == WEB_RESOURCE_NUM)
    {
        ESP_LOGE(TAG, "File %s not exist", filename);
        httpd_resp_send_404(req);
        return false;
    }

    offset = 0;
    file_size = s_resource_map[i].end - s_resource_map[i].start;
    web_set_content_type(req, filename);

    while (offset < file_size)
    {
        chunksize = file_size - offset;
        chunksize = (chunksize < size) ? (chunksize) : (size);

        if (ESP_OK != httpd_resp_send_chunk(req, (const char *)s_resource_map[i].start + offset, chunksize))
        {
            ESP_LOGE(TAG, "httpd_resp_send_chunk failed");
            httpd_resp_send_chunk(req, NULL, 0);
            httpd_resp_send_500(req);
            return false;
        }

        offset += chunksize;
    }

    return true;
}

esp_err_t web_serial_handler(httpd_req_t *req)
{
    web_data_t *data = (web_data_t *)req->user_ctx;

    if ((req->method == HTTP_GET) && web_resp_file(req, "/data/httpd/webserial.html", (char *)data->buf, CONFIG_HTTPD_RESP_BUF_SIZE))
    {
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }

    return ESP_FAIL;
}

esp_err_t web_index_handler(httpd_req_t *req)
{
    web_data_t *data = (web_data_t *)req->user_ctx;
    ESP_LOGI(TAG, "%d connected", httpd_req_to_sockfd(req));

    if (req->method == HTTP_GET && web_resp_file(req, "/data/httpd/root.html", (char *)data->buf, CONFIG_HTTPD_RESP_BUF_SIZE))
    {
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }

    return ESP_FAIL;
}

esp_err_t web_favicon_handler(httpd_req_t *req)
{
    web_data_t *data = (web_data_t *)req->user_ctx;

    if (req->method == HTTP_GET && web_resp_file(req, "/data/httpd/favicon.ico", (char *)data->buf, CONFIG_HTTPD_RESP_BUF_SIZE))
    {
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_OK;
    }

    return ESP_FAIL;
}

bool web_list_files(const char *root, const char *path, void (*cb)(httpd_req_t *, char *), httpd_req_t *req)
{
#define PATH_MAX_SIZE 260

    DIR *dir = NULL;
    bool ret = false;
    struct dirent *entry = NULL;
    char *file_path = (char *)malloc(PATH_MAX_SIZE);

    if (!file_path)
    {
        ESP_LOGE(TAG, "Memory not enough");
        goto __exit;
    }

    dir = opendir(path);
    if (dir == NULL)
    {
        ESP_LOGE(TAG, "Open directory %s failed", path);
        goto __exit;
    }

    while ((entry = readdir(dir)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }

        snprintf(file_path, PATH_MAX_SIZE, "%s/%s", path, entry->d_name);

        if (entry->d_type == DT_DIR)
        {
            web_list_files(root, file_path, cb, req);
        }
        else if (cb)
        {
            cb(req, file_path + strlen(root) + 1);
        }
    }

    ret = true;

__exit:

    if (dir)
        closedir(dir);

    if (file_path)
        free(file_path);

    return ret;
}

void web_add_option(httpd_req_t *req, char *path)
{
    if ((path == NULL) || !web_is_safe_filename(path))
    {
        return;
    }

    httpd_resp_sendstr_chunk(req, "<option value=\"");
    web_send_html_escaped(req, path);
    httpd_resp_sendstr_chunk(req, "\">");
    web_send_html_escaped(req, path);
    httpd_resp_sendstr_chunk(req, "</option>");
}

esp_err_t web_program_handler(httpd_req_t *req)
{
    web_data_t *data = (web_data_t *)req->user_ctx;

    if (req->method != HTTP_GET || !web_resp_file(req, "/data/httpd/program.html", (char *)data->buf, CONFIG_HTTPD_RESP_BUF_SIZE))
    {
        return ESP_FAIL;
    }

    httpd_resp_sendstr_chunk(req,
        "<body><div class=\"page\">"
        "<a class=\"back-link\" href=\"/\">← 返回首页</a>"
        "<div class=\"accent-bar\"></div>"
        "<h1 class=\"page-title\">烧录目标芯片</h1>"
        "<p class=\"page-sub\">通过 SWD 将固件写入目标 MCU。请先接好 SWDIO / SWCLK / GND（建议 nRESET）。</p>"
        "<div class=\"tip-box\">"
        "<strong>怎么选：</strong><br>"
        "① 先准备算法(.FLM)和程序(.hex/.bin)到设备盘；"
        "② 填地址后点「离线烧录」；"
        "或选好算法后，用「在线烧录」直接传固件边传边烧。<br>"
        "<strong>文件类型：</strong> 算法 = Keil FLM；程序 = hex/bin。"
        "</div>"

        "<section class=\"section\">"
        "<h2>1. 公共参数（烧录前必看）</h2>"
        "<p class=\"section-desc\">离线烧录、在线烧录都使用下面这些参数。</p>"

        "<div class=\"form-group\">"
        "<div class=\"label-row\"><label for=\"algorithm\">算法</label>"
        "<span class=\"tag tag-req\">必选</span></div>"
        "<p class=\"hint\">Flash 擦写算法，扩展名 .FLM，须与目标芯片匹配（如 PY32F030xx_32.FLM）。</p>"
        "<select id=\"algorithm\"><option value=\"\">请选择算法 (.FLM)</option>");
    web_list_files(CONFIG_PROGRAMMER_ALGORITHM_ROOT, CONFIG_PROGRAMMER_ALGORITHM_ROOT, web_add_option, req);
    httpd_resp_sendstr_chunk(req,
        "</select></div>"

        "<div class=\"row-2\">"
        "<div class=\"form-group\">"
        "<div class=\"label-row\"><label for=\"flash-address\">Flash 地址</label>"
        "<span class=\"tag tag-opt\">.hex可空</span>"
        "<span class=\"tag tag-req\">.bin必填</span></div>"
        "<p class=\"hint\">目标 Flash 起始地址，十六进制，不要写 0x。例：08000000。hex 文件自带地址，可留空。</p>"
        "<input type=\"text\" id=\"flash-address\" placeholder=\"08000000\">"
        "</div>"
        "<div class=\"form-group\">"
        "<div class=\"label-row\"><label for=\"ram-address\">RAM 地址</label>"
        "<span class=\"tag tag-req\">建议填写</span></div>"
        "<p class=\"hint\">算法下载到目标 RAM 的基址。Cortex-M 常见 20000000（PY32/STM32）。</p>"
        "<input type=\"text\" id=\"ram-address\" value=\"20000000\" placeholder=\"20000000\">"
        "</div></div>"

        "<div class=\"form-group\">"
        "<div class=\"label-row\"><label for=\"erase-mode\">擦除方式</label>"
        "<span class=\"tag tag-req\">必选</span></div>"
        "<p class=\"hint\">"
        "扇区擦除=写哪个扇区擦哪个（默认）；"
        "整片擦除=先 EraseChip 再写；"
        "不擦除=跳过擦除（排查擦除失败或 Flash 已擦空时用，未擦空可能写失败）。"
        "</p>"
        "<select id=\"erase-mode\">"
        "<option value=\"sector\" selected>扇区擦除（默认）</option>"
        "<option value=\"chip\">整片擦除</option>"
        "<option value=\"none\">不擦除</option>"
        "</select></div></section>"

        "<section class=\"section\">"
        "<h2>2. 离线烧录（推荐）</h2>"
        "<p class=\"section-desc\">使用设备盘里已有的算法/程序烧录。不经过电脑边传边烧。</p>"
        "<div class=\"form-group\">"
        "<div class=\"label-row\"><label for=\"offline-program\">程序</label>"
        "<span class=\"tag tag-req\">必选</span></div>"
        "<p class=\"hint\">固件文件 .hex 或 .bin，来自下方「上传程序」或有线 U 盘 program/ 目录。</p>"
        "<select id=\"offline-program\"><option value=\"\">请选择程序 (.hex / .bin)</option>");
    web_list_files(CONFIG_PROGRAMMER_PROGRAM_ROOT, CONFIG_PROGRAMMER_PROGRAM_ROOT, web_add_option, req);
    httpd_resp_sendstr_chunk(req,
        "</select></div>"
        "<div class=\"form-group\">"
        "<button id=\"offline-program-btn\">开始离线烧录</button>"
        "</div></section>"

        "<section class=\"section\">"
        "<h2>3. 在线烧录</h2>"
        "<p class=\"section-desc\">从电脑选择固件，直接烧到目标芯片（不保存到设备盘）。算法仍须在上方选好。</p>"
        "<div class=\"form-group\">"
        "<div class=\"label-row\"><label for=\"online-program\">固件文件</label>"
        "<span class=\"tag tag-req\">必选</span></div>"
        "<p class=\"hint\">只选程序 .hex / .bin，不要选 .FLM。</p>"
        "<div class=\"btn-row\">"
        "<input type=\"file\" id=\"online-program\" accept=\".hex,.bin,.HEX,.BIN\">"
        "<button id=\"online-program-btn\">在线烧录</button>"
        "</div></div></section>"

        "<section class=\"section\">"
        "<h2>4. 上传到设备盘（只存文件，不烧录）</h2>"
        "<p class=\"section-desc\">把文件存进 ESP32 的 /data，供以后离线烧录使用。</p>"
        "<div class=\"form-group\">"
        "<div class=\"label-row\"><label for=\"upload-algorithm\">上传算法</label>"
        "<span class=\"tag tag-opt\">可选</span></div>"
        "<p class=\"hint\">上传 .FLM → 存到 algorithm/，并加入上方算法下拉框。</p>"
        "<div class=\"btn-row\">"
        "<input type=\"file\" id=\"upload-algorithm\" accept=\".flm,.FLM\">"
        "<button class=\"secondary\" id=\"upload-algorithm-btn\">上传算法</button>"
        "</div></div>"
        "<div class=\"form-group\">"
        "<div class=\"label-row\"><label for=\"upload-program\">上传程序</label>"
        "<span class=\"tag tag-opt\">可选</span></div>"
        "<p class=\"hint\">上传 .hex / .bin → 存到 program/，并加入离线烧录程序下拉框。</p>"
        "<div class=\"btn-row\">"
        "<input type=\"file\" id=\"upload-program\" accept=\".hex,.bin,.HEX,.BIN\">"
        "<button class=\"secondary\" id=\"upload-program-btn\">上传程序</button>"
        "</div></div></section>"

        "<section class=\"section progress-wrap\">"
        "<h2>进度</h2>"
        "<progress id=\"progress\" value=\"0\" max=\"100\"></progress>"
        "</section>"
        "</div></body></html>");
    httpd_resp_send_chunk(req, NULL, 0);

    return ESP_OK;
}

esp_err_t web_flash_handler(httpd_req_t *req)
{
    size_t offset = 0;
    esp_err_t ret = ESP_FAIL;
    web_data_t *data = (web_data_t *)req->user_ctx;
    char *buf = (char *)data->buf;

    if (req->method != HTTP_POST)
    {
        return ESP_FAIL;
    }

    if (req->content_len >= CONFIG_HTTPD_RESP_BUF_SIZE)
    {
        ESP_LOGE(TAG, "Failed to allocate memory of %d bytes!", req->content_len + 1);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    while (offset < req->content_len)
    {
        ret = httpd_req_recv(req, buf + offset, req->content_len - offset);
        if (ret <= 0)
        {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT)
            {
                httpd_resp_send_408(req);
            }

            return ESP_FAIL;
        }

        offset += ret;
    }

    buf[offset] = '\0';

    if (programmer_request_handle(buf, offset) == PROG_ERR_NONE)
    {
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_sendstr(req, "Start to program");
        return ESP_OK;
    }
    else
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Program failed");
        return ESP_FAIL;
    }
}

static esp_err_t web_upload_file(httpd_req_t *req, char *path, bool overwrite)
{
    FILE *fd = NULL;
    int received = 0;
    struct stat file_stat;
    int remaining = req->content_len;
    web_data_t *data = (web_data_t *)req->user_ctx;

    ESP_LOGI(TAG, "File name : %s", path);

    if (stat(path, &file_stat) == 0)
    {
        if (!overwrite)
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File already exist!");
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "%s will be overwritten", path);
        unlink(path);
    }

    if (req->content_len > PROGRAM_MAX_SIZE)
    {
        ESP_LOGE(TAG, "File too large : %d bytes", req->content_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File size must be less than " PROGRAM_MAX_SIZE_STR "!");
        return ESP_FAIL;
    }

    /* 算法 .FLM 为二进制，必须用二进制写模式 */
    fd = fopen(path, "wb");
    if (!fd)
    {
        ESP_LOGE(TAG, "Failed to create file : %s", path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create file (is /data mounted?)");
        return ESP_FAIL;
    }

    while (remaining > 0)
    {
        ESP_LOGD(TAG, "Remaining size : %d", remaining);
        received = httpd_req_recv(req, (char *)data->buf, (remaining <= CONFIG_HTTPD_RESP_BUF_SIZE) ? (remaining) : (CONFIG_HTTPD_RESP_BUF_SIZE));

        if (received <= 0)
        {
            if (received == HTTPD_SOCK_ERR_TIMEOUT)
            {
                continue;
            }

            fclose(fd);
            unlink(path);
            ESP_LOGE(TAG, "File reception failed!");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive file");
            return ESP_FAIL;
        }

        if (received && (received != fwrite((char *)data->buf, 1, received, fd)))
        {
            /* Couldn't write everything to file! Storage may be full? */
            fclose(fd);
            unlink(path);
            ESP_LOGE(TAG, "File write failed!");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to write file to storage");
            return ESP_FAIL;
        }

        remaining -= received;
    }

    fclose(fd);
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "File uploaded successfully");
    ESP_LOGI(TAG, "File reception complete");

    return ESP_OK;
}

esp_err_t web_upload_file_handler(httpd_req_t *req)
{
    char *buf = NULL;
    size_t buf_size = 0;
    char overwrite[5] = {0};
    size_t location_offset = 0;
    static char location[CONFIG_PROGRAMMER_FILE_MAX_LEN] = {0};

    buf_size = httpd_req_get_url_query_len(req) + 1;
    if (buf_size == 1)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
        return ESP_FAIL;
    }

    buf = (char *)malloc(buf_size);
    if (!buf)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Not enough ram to store query string");
        return ESP_FAIL;
    }

    if (httpd_req_get_url_query_str(req, buf, buf_size) != ESP_OK)
    {
        free(buf);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Query string read failed");
        return ESP_FAIL;
    }

    /* Set disk mount path */
    memcpy(location, "/data/", sizeof("/data/"));
    location_offset = strlen(location);

    if (httpd_query_key_value(buf, "location", location + location_offset, sizeof(location) - location_offset) != ESP_OK)
    {
        free(buf);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Location is unknown");
        return ESP_FAIL;
    }

    /* Check the upload position */
    if (strcmp(location + location_offset, "algorithm") && strcmp(location + location_offset, "program"))
    {
        free(buf);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Upload position is invalid");
        return ESP_FAIL;
    }

    /* Add Slash */
    location_offset = strlen(location);
    if ((location[location_offset - 1] != '/') && ((location_offset + 2) < sizeof(location)))
    {
        location[location_offset] = '/';
        location_offset++;
        location[location_offset] = '\0';
    }

    if (httpd_query_key_value(buf, "name", location + location_offset, sizeof(location) - location_offset) != ESP_OK)
    {
        free(buf);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Name is unknown");
        return ESP_FAIL;
    }

    if (!web_is_safe_filename(location + location_offset))
    {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid file name");
        return ESP_FAIL;
    }

    httpd_query_key_value(buf, "overwrite", overwrite, sizeof(overwrite));
    free(buf);

    /* overwrite=true 时允许覆盖已有算法/程序 */
    return web_upload_file(req, location, (strncmp(overwrite, "true", 4) == 0));
}

esp_err_t web_query_handler(httpd_req_t *req)
{
    char *buf = NULL;
    size_t buf_size = 0;
    char type[32] = {0};
    int encode_len = 0;
    web_data_t *data = (web_data_t *)req->user_ctx;

    buf_size = httpd_req_get_url_query_len(req) + 1;
    if (buf_size == 1)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
        return ESP_FAIL;
    }

    buf = (char *)malloc(buf_size);
    if (!buf)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Not enough ram to store query string");
        return ESP_FAIL;
    }

    if (httpd_req_get_url_query_str(req, buf, buf_size) != ESP_OK)
    {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Query string read failed");
        return ESP_FAIL;
    }

    if (httpd_query_key_value(buf, "type", type, sizeof(type)) != ESP_OK)
    {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Type is unknown");
        return ESP_FAIL;
    }

    if (!strcmp("program-status", type))
    {
        programmer_get_status((char *)data->buf, CONFIG_HTTPD_RESP_BUF_SIZE, &encode_len);
        httpd_resp_send_chunk(req, (char *)data->buf, encode_len);
        httpd_resp_send_chunk(req, NULL, 0);
    }
    else
    {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unsupported type");
        return ESP_FAIL;
    }

    free(buf);
    return ESP_OK;
}

esp_err_t web_online_program_handler(httpd_req_t *req)
{
    int received = 0;
    int remaining = req->content_len;
    web_data_t *data = (web_data_t *)req->user_ctx;

    if (req->content_len > PROGRAM_MAX_SIZE)
    {
        ESP_LOGE(TAG, "Online program too large : %d bytes", req->content_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File size must be less than " PROGRAM_MAX_SIZE_STR "!");
        return ESP_FAIL;
    }

    while (remaining > 0)
    {
        ESP_LOGD(TAG, "Remaining size : %d", remaining);
        received = httpd_req_recv(req, (char *)data->buf, (remaining <= CONFIG_HTTPD_RESP_BUF_SIZE) ? (remaining) : (CONFIG_HTTPD_RESP_BUF_SIZE));

        if (received <= 0)
        {
            if (received == HTTPD_SOCK_ERR_TIMEOUT)
            {
                continue;
            }

            ESP_LOGE(TAG, "File reception failed!");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive program");
            return ESP_FAIL;
        }

        if (received)
        {
            prog_err_def ret = programmer_write_data(data->buf, received);

            if (PROG_ERR_NONE != ret)
            {
                ESP_LOGE(TAG, "File flash failed! ret: %d", ret);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to write file to target");
                return ESP_FAIL;
            }
        }

        remaining -= received;
    }

    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "Target program successfully");

    return ESP_OK;
}

/*******************************************************************************************
函数名称 : web_set_baudrate_handler
功 能 : Web 串口设置目标 UART 波特率
参 数 : req - HTTP 请求（JSON: {"baudrate":115200}）
返 回 值 : esp_err_t - ESP_OK 成功
*******************************************************************************************/
esp_err_t web_set_baudrate_handler(httpd_req_t *req)
{
    int received = 0;
    int remaining = 0;
    cJSON *root = NULL;
    cJSON *baud_item = NULL;
    uint32_t baudrate = 0;
    web_data_t *data = (web_data_t *)req->user_ctx;

    if ((req->content_len <= 0) || (req->content_len >= (int)CONFIG_HTTPD_RESP_BUF_SIZE))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    remaining = req->content_len;
    while (remaining > 0)
    {
        int n = httpd_req_recv(req, (char *)data->buf + received,
                               (remaining <= (CONFIG_HTTPD_RESP_BUF_SIZE - 1 - received))
                                   ? remaining
                                   : (CONFIG_HTTPD_RESP_BUF_SIZE - 1 - received));
        if (n <= 0)
        {
            if (n == HTTPD_SOCK_ERR_TIMEOUT)
            {
                continue;
            }
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv failed");
            return ESP_FAIL;
        }
        received += n;
        remaining -= n;
    }
    data->buf[received] = 0;

    root = cJSON_Parse((char *)data->buf);
    if (root == NULL)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    baud_item = cJSON_GetObjectItem(root, "baudrate");
    if ((baud_item == NULL) ||
        ((baud_item->type != cJSON_Number) && (baud_item->type != cJSON_String)))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "baudrate required");
        return ESP_FAIL;
    }

    if (baud_item->type == cJSON_Number)
    {
        baudrate = (uint32_t)baud_item->valueint;
    }
    else
    {
        baudrate = (uint32_t)atoi(baud_item->valuestring);
    }
    cJSON_Delete(root);

    if ((baudrate < 1200U) || (baudrate > 2000000U))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "baudrate out of range");
        return ESP_FAIL;
    }

    if (!cdc_uart_set_baudrate(baudrate))
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set baudrate failed");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}