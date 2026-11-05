/* httpd [port] — веб-сервер MyOS (по умолчанию порт 80)
 *
 *   /            живая страница: процессы, память, сеть (обновляется сама)
 *   /files/...   файлы и папки с диска MyFS
 *
 * Из Windows: QEMU пробрасывает localhost:8080 -> порт 80 MyOS, так что
 * запусти «httpd &» и открой в браузере http://localhost:8080
 *
 * Каждое соединение обслуживает отдельный процесс (fork), поэтому один
 * «молчащий» клиент не задерживает остальных. */
#include "myos.h"

/* ------------------------------------------------------------ строка-буфер */

static char *sb;
static int sb_len, sb_cap;

static void sb_add(const char *s, int n) {
    if (sb_len + n + 1 > sb_cap) {
        int cap = sb_cap ? sb_cap : 4096;
        while (sb_len + n + 1 > cap) cap *= 2;
        char *p = realloc(sb, cap);
        if (!p) return;
        sb = p; sb_cap = cap;
    }
    memcpy(sb + sb_len, s, n);
    sb_len += n;
    sb[sb_len] = 0;
}

static void sb_printf(const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    sb_add(tmp, n < (int)sizeof(tmp) ? n : (int)sizeof(tmp) - 1);
}

static void sb_html(const char *s) {               /* текст с экранированием <>& */
    for (; *s; s++) {
        if (*s == '<') sb_add("&lt;", 4);
        else if (*s == '>') sb_add("&gt;", 4);
        else if (*s == '&') sb_add("&amp;", 5);
        else sb_add(s, 1);
    }
}

/* ------------------------------------------------------------ ответы */

static void send_all(int fd, const char *p, int n) {
    while (n > 0) {
        int w = write(fd, p, n);
        if (w <= 0) return;
        p += w; n -= w;
    }
}

static void respond(int fd, int code, const char *type, const char *body, int len) {
    const char *msg = code == 200 ? "OK" : code == 404 ? "Not Found" : code == 400 ? "Bad Request" : "Error";
    char h[256];
    int n = snprintf(h, sizeof(h),
        "HTTP/1.0 %d %s\r\nServer: MyOS-httpd/0.12\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
        "Connection: close\r\n\r\n", code, msg, type, len);
    send_all(fd, h, n);
    send_all(fd, body, len);
}

static const char *STYLE =
    "<style>body{font-family:Consolas,monospace;background:#101418;color:#d8dee9;margin:2em}"
    "h1{color:#88c0d0}h2{color:#a3be8c;border-bottom:1px solid #3b4252}a{color:#81a1c1}"
    "table{border-collapse:collapse}td,th{padding:2px 14px;text-align:left}"
    "th{color:#ebcb8b}tr:nth-child(even){background:#1a2028}.dim{color:#6b7689}</style>";

static void page_start(const char *title, bool refresh) {
    sb_len = 0;
    sb_printf("<!doctype html><html><head><meta charset=utf-8><title>%s</title>", title);
    if (refresh) sb_printf("<meta http-equiv=refresh content=3>");
    sb_printf("%s</head><body>", STYLE);
}

static void status_page(int fd) {
    static const char *states[] = { "ready", "running", "sleeping", "blocked", "zombie" };
    page_start("MyOS", true);
    unsigned up = ticks() / 1000;
    sb_printf("<h1>MyOS v0.12 is alive</h1><p>Uptime %u:%02u:%02u &middot; "
              "this page is served by <b>httpd</b> running inside MyOS "
              "<span class=dim>(refreshes every 3 s)</span></p>", up / 3600, up / 60 % 60, up % 60);

    struct proc_info pi[32];
    int n = ps(pi, 32);
    unsigned mem = 0;
    sb_printf("<h2>Processes</h2><table><tr><th>PID</th><th>PPID</th><th>NAME</th><th>STATE</th>"
              "<th>RING</th><th>CPU%%</th><th>MEM</th></tr>");
    for (int i = 0; i < n; i++) {
        mem += pi[i].mem_kb;
        sb_printf("<tr><td>%u</td><td>%u</td><td>", pi[i].pid, pi[i].ppid);
        sb_html(pi[i].name);
        sb_printf("</td><td>%s</td><td>%u</td><td>%u</td><td>%u KB</td></tr>",
                  pi[i].state < 5 ? states[pi[i].state] : "?", pi[i].ring, pi[i].cpu_percent, pi[i].mem_kb);
    }
    sb_printf("</table><p class=dim>%d processes, %u KB of user memory</p>", n, mem);

    struct net_info ni;
    char a[16], b[16], c[16];
    if (netinfo(&ni) == 0) {
        sb_printf("<h2>Network</h2><p>eth0 MAC %02x:%02x:%02x:%02x:%02x:%02x<br>inet %s mask %s gateway %s (%s)<br>"
                  "RX %u packets, %u KB &middot; TX %u packets, %u KB</p>",
                  ni.mac[0], ni.mac[1], ni.mac[2], ni.mac[3], ni.mac[4], ni.mac[5],
                  ip_str(ni.ip, a), ip_str(ni.mask, b), ip_str(ni.gw, c), ni.dhcp ? "DHCP" : "static",
                  ni.rx_packets, ni.rx_bytes / 1024, ni.tx_packets, ni.tx_bytes / 1024);
    }
    sb_printf("<h2>Files</h2><p><a href=\"/files/\">Browse the MyFS disk &rarr;</a></p></body></html>");
    respond(fd, 200, "text/html; charset=utf-8", sb, sb_len);
}

static const char *mime(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return "text/plain; charset=utf-8";
    if (!strcmp(dot, ".html") || !strcmp(dot, ".htm")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".css")) return "text/css";
    if (!strcmp(dot, ".js"))  return "text/javascript";
    if (!strcmp(dot, ".png")) return "image/png";
    if (!strcmp(dot, ".jpg")) return "image/jpeg";
    if (!strcmp(dot, ".gif")) return "image/gif";
    if (!strcmp(dot, ".bmp")) return "image/bmp";
    if (!strcmp(dot, ".txt") || !strcmp(dot, ".c") || !strcmp(dot, ".h") || !strcmp(dot, ".sh") ||
        !strcmp(dot, ".md")) return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

static void not_found(int fd, const char *what) {
    page_start("404", false);
    sb_printf("<h1>404</h1><p>");
    sb_html(what);
    sb_printf(" &mdash; not found</p><p><a href=\"/\">home</a></p></body></html>");
    respond(fd, 404, "text/html; charset=utf-8", sb, sb_len);
}

static void files_page(int fd, const char *path) {     /* path — путь на диске, начинается с '/' */
    struct dirent_info st;
    if (stat(path, &st) < 0) { not_found(fd, path); return; }
    if (st.type == 1) {                                  /* файл — отдаём содержимое */
        int f = open(path, O_READ);
        if (f < 0) { not_found(fd, path); return; }
        char h[256];
        int n = snprintf(h, sizeof(h), "HTTP/1.0 200 OK\r\nServer: MyOS-httpd/0.12\r\nContent-Type: %s\r\n"
                         "Content-Length: %u\r\nConnection: close\r\n\r\n", mime(path), st.size);
        send_all(fd, h, n);
        static char buf[4096];
        while ((n = read(f, buf, sizeof(buf))) > 0) send_all(fd, buf, n);
        close(f);
        return;
    }
    page_start("MyFS", false);                           /* папка — список */
    int pl = strlen(path);
    bool slash = pl && path[pl - 1] == '/';
    sb_printf("<h1>MyFS: ");
    sb_html(path);
    sb_printf("</h1><table><tr><th>NAME</th><th>SIZE</th></tr>");
    if (strcmp(path, "/")) sb_printf("<tr><td><a href=\"../\">../</a></td><td></td></tr>");
    struct dirent_info e;
    for (int i = 0; readdir(path, i, &e) == 1; i++) {
        sb_printf("<tr><td><a href=\"/files%s%s", path, slash ? "" : "/");
        sb_html(e.name);
        sb_printf("%s\">", e.type == 2 ? "/" : "");
        sb_html(e.name);
        if (e.type == 2) sb_printf("/</a></td><td class=dim>dir</td></tr>");
        else sb_printf("</a></td><td>%u</td></tr>", e.size);
    }
    sb_printf("</table><p><a href=\"/\">&larr; home</a></p></body></html>");
    respond(fd, 200, "text/html; charset=utf-8", sb, sb_len);
}

/* %20 -> пробел и т. п. */
static void url_decode(char *s) {
    char *w = s;
    for (; *s; s++) {
        if (*s == '%' && s[1] && s[2]) {
            int v = 0;
            for (int i = 1; i <= 2; i++) {
                char c = s[i];
                v = v * 16 + (isdigit(c) ? c - '0' : (toupper(c) - 'A' + 10) & 15);
            }
            *w++ = v; s += 2;
        } else *w++ = *s;
    }
    *w = 0;
}

static bool has_dotdot(const char *p) {
    for (; *p; p++) if (p[0] == '.' && p[1] == '.') return true;
    return false;
}

static void handle(int fd, const char *who) {
    char req[2048];
    int len = 0;
    while (len < (int)sizeof(req) - 1) {                 /* до пустой строки */
        int n = read(fd, req + len, sizeof(req) - 1 - len);
        if (n <= 0) break;
        len += n;
        req[len] = 0;
        if (strchr(req, '\n') && (len >= 4 && !strncmp(req + len - 4, "\r\n\r\n", 4))) break;
    }
    req[len] = 0;
    if (!len) return;
    char *eol = strchr(req, '\r');
    if (eol) *eol = 0;
    char method[8] = "", path[256] = "";
    char *sp = strchr(req, ' ');
    if (sp && sp - req < 8) {
        memcpy(method, req, sp - req); method[sp - req] = 0;
        char *p2 = strchr(sp + 1, ' ');
        int pl = p2 ? p2 - sp - 1 : (int)strlen(sp + 1);
        if (pl > 255) pl = 255;
        memcpy(path, sp + 1, pl); path[pl] = 0;
    }
    char *q = strchr(path, '?');
    if (q) *q = 0;
    url_decode(path);
    if (strcmp(method, "GET") || path[0] != '/' || has_dotdot(path)) {
        respond(fd, 400, "text/plain", "bad request\n", 12);
        printf("httpd: %s \"%s\" 400\n", who, req);
        return;
    }
    if (!strcmp(path, "/")) status_page(fd);
    else if (!strcmp(path, "/files")) files_page(fd, "/");
    else if (!strncmp(path, "/files/", 7)) files_page(fd, path + 6);
    else not_found(fd, path);
    printf("httpd: %s GET %s\n", who, path);
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 80;
    int ls = socket(SOCK_TCP);
    if (ls < 0) { dprintf(STDERR, "httpd: socket: %s\n", strerror(-ls)); return 1; }
    int r = bind(ls, port);
    if (r == 0) r = listen(ls, 8);
    if (r < 0) { dprintf(STDERR, "httpd: port %d: %s\n", port, strerror(-r)); return 1; }
    printf("httpd: listening on port %d", port);
    if (port == 80) printf(" - open http://localhost:8080 in your browser");
    printf("\n");

    for (;;) {
        struct sockaddr from;
        int c = accept(ls, &from);
        while (waitpid(-1, NULL, WNOHANG) > 0) {}       /* прибрать закончивших помощников */
        if (c < 0) { dprintf(STDERR, "httpd: accept: %s\n", strerror(-c)); sleep_ms(100); continue; }
        char who[24], ips[16];
        snprintf(who, sizeof(who), "%s:%u", ip_str(from.ip, ips), from.port);
        int pid = fork();
        if (pid == 0) {                                   /* помощник: обслужить и выйти */
            close(ls);
            handle(c, who);
            close(c);
            exit(0);
        }
        if (pid < 0) { handle(c, who); }                  /* процессов не хватило — сами */
        close(c);
    }
}
