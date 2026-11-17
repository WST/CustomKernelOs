#ifndef WIFI_UI_H
#define WIFI_UI_H
#include "wifi_shm.h"
/* Клиент читает номера страниц из файла, чтобы не забирать чужие сообщения у стола. */
static struct wifi_pub *wifi_view;
static struct wifi_req *wifi_cmd;
static unsigned wifi_tag;
static int wifi_link(void) {
    if (wifi_view && wifi_view->magic == WIFI_MAGIC) return 1;
    int fd = open("/tmp/wifi.ids", O_RDONLY);
    if (fd < 0) return 0;
    char buf[32];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    int a = 0, b = 0, i = 0;
    while (buf[i] >= '0' && buf[i] <= '9') a = a * 10 + buf[i++] - '0';
    if (buf[i] == ' ') i++;
    while (buf[i] >= '0' && buf[i] <= '9') b = b * 10 + buf[i++] - '0';
    if (!a || !b) return 0;
    wifi_view = shm_map(a);
    wifi_cmd = shm_map(b);
    return wifi_view && wifi_cmd && wifi_view->magic == WIFI_MAGIC;
}
static void wifi_order(unsigned op, const char *ssid, const char *pass, unsigned extra) {
    if (!wifi_link() || !wifi_cmd) return;
    memset(wifi_cmd, 0, sizeof(*wifi_cmd));
    wifi_cmd->op = op;
    wifi_cmd->priority = extra;
    if (ssid) snprintf(wifi_cmd->ssid, sizeof(wifi_cmd->ssid), "%s", ssid);
    if (pass) snprintf(wifi_cmd->pass, sizeof(wifi_cmd->pass), "%s", pass);
    wifi_cmd->seq = ++wifi_tag;
}
#endif
