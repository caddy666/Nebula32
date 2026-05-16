// =============================================================================
// webserver.c — HTTP Web Interface for Disc Selection
// =============================================================================
//
// Implements a minimal HTTP/1.1 server using lwIP TCP/IP stack (bundled with
// pico_cyw43_arch_lwip_poll) on the Pico 2 W's CYW43439 WiFi chip.
//
// Uses poll-mode lwIP (no RTOS, no threads) — webserver_poll() must be
// called regularly from the Core 0 main loop.
//
// Architecture:
//   One TCP listener on port 80.
//   Each connection is handled synchronously (accept → read → respond → close).
//   Connections are NOT kept-alive (Connection: close) for simplicity.
//   Cover art JPEGs are read from the SD card and streamed in 512-byte chunks.
//
// Memory:
//   A single 2 KB response buffer handles all non-file responses.
//   File serving uses a 512-byte SD card read buffer (FatFS cluster size).
//
// IMPORTANT: All webserver code runs on Core 0 only.
//            Core 1 is the real-time bus handler and is never involved.
// =============================================================================

#include "webserver.h"
#include "logger.h"
#include "sd_card_api.h"
#include "disc_image.h"   // MAX_PATH_LEN
#include "cd_types.h"
#include "commo_bridge.h" // commo_bridge_get_drive_state()

// Pico W WiFi + lwIP headers
#include "pico/stdlib.h"
#ifdef PICO_CYW43_SUPPORTED
#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "lwip/ip4_addr.h"
#endif
#include "ff.h"     // FatFS for cover art serving

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

#ifdef PICO_CYW43_SUPPORTED

// ---------------------------------------------------------------------------
// External state from main.c
// ---------------------------------------------------------------------------
extern char     s_image_paths[][MAX_PATH_LEN];
extern uint32_t s_image_count;

// ---------------------------------------------------------------------------
// WiFi credentials (read from cd32_ode.cfg)
// ---------------------------------------------------------------------------
static char s_ssid[WS_SSID_MAX]   = "";
static char s_pass[WS_PASS_MAX]   = "";
static char s_host[WS_HOST_MAX]   = WS_DEFAULT_HOSTNAME;
static char s_ip_str[16]          = "";
static bool s_running             = false;

// ---------------------------------------------------------------------------
// Pending load request from web interface
// ---------------------------------------------------------------------------
static volatile bool     s_load_pending = false;
static volatile uint32_t s_load_index   = 0;
static volatile uint32_t s_loaded_index = 0;   // Currently loaded disc

// ---------------------------------------------------------------------------
// TCP listener state
// ---------------------------------------------------------------------------
static struct tcp_pcb *s_listener = NULL;

// Per-connection context
typedef struct {
    char    request[512];      // Accumulate incoming HTTP request
    int     req_len;
    bool    headers_done;
} http_conn_t;

// ---------------------------------------------------------------------------
// Read WiFi credentials from cd32_ode.cfg
// ---------------------------------------------------------------------------
static void read_wifi_config(void) {
    FIL f;
    if (f_open(&f, "0:/cd32_ode.cfg", FA_READ) != FR_OK) return;

    char line[128];
    while (f_gets(line, sizeof(line), &f)) {
        // Trim
        int len = strlen(line);
        while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n' ||
                            line[len-1] == ' '))
            line[--len] = '\0';

        if (line[0] == '#' || line[0] == ';' || line[0] == '\0') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line, *val = eq + 1;
        // Trim key and val
        while (*key == ' ') key++;
        while (*val == ' ') val++;

        if (strcasecmp(key, WS_CFG_SSID_KEY) == 0) {
            strncpy(s_ssid, val, WS_SSID_MAX - 1);
        } else if (strcasecmp(key, WS_CFG_PASS_KEY) == 0) {
            strncpy(s_pass, val, WS_PASS_MAX - 1);
        } else if (strcasecmp(key, WS_CFG_HOSTNAME_KEY) == 0) {
            strncpy(s_host, val, WS_HOST_MAX - 1);
        }
    }
    f_close(&f);
}

// ---------------------------------------------------------------------------
// HTML page generation
// ---------------------------------------------------------------------------
// The HTML is generated dynamically so it always reflects the current
// disc list and loaded state.  All CSS is inline so no external files
// are needed.  The page auto-refreshes every 5 seconds.

// Filename without extension from a full path
static const char *basename_no_ext(const char *path, char *buf, int bufsz) {
    const char *slash = strrchr(path, '/');
    const char *name  = slash ? slash + 1 : path;
    strncpy(buf, name, bufsz - 1);
    buf[bufsz - 1] = '\0';
    char *dot = strrchr(buf, '.');
    if (dot) *dot = '\0';
    return buf;
}

// Check if a cover image exists for a given disc image path
static bool cover_exists(const char *image_path, char *cover_path_out, int outsz) {
    char base[128];
    basename_no_ext(image_path, base, sizeof(base));

    // Try covers/ directory first
    snprintf(cover_path_out, outsz, "%s%s%s", WS_COVERS_DIR, base, WS_COVER_EXT1);
    if (f_stat(cover_path_out, NULL) == FR_OK) return true;

    snprintf(cover_path_out, outsz, "%s%s%s", WS_COVERS_DIR, base, WS_COVER_EXT2);
    if (f_stat(cover_path_out, NULL) == FR_OK) return true;

    // Try same directory as image
    const char *slash = strrchr(image_path, '/');
    if (slash) {
        int dir_len = (int)(slash - image_path) + 1;
        snprintf(cover_path_out, outsz, "%.*s%s%s",
                 dir_len, image_path, base, WS_COVER_EXT1);
        if (f_stat(cover_path_out, NULL) == FR_OK) return true;
    }

    cover_path_out[0] = '\0';
    return false;
}

// Drive state name
static const char *state_name(int state) {
    switch (state) {
        case 0: return "RESET";   case 1: return "IDLE";
        case 2: return "SPINUP";  case 3: return "READY";
        case 4: return "SEEKING"; case 5: return "READING";
        case 6: return "PLAYING"; case 7: return "PAUSED";
        default: return "ERROR";
    }
}

// SD card space via f_getfree. Returns false if the volume is not mounted.
static bool sd_get_space(uint64_t *used_out, uint64_t *total_out) {
    FATFS *fs;
    DWORD  fre_clust;
    if (f_getfree("0:", &fre_clust, &fs) != FR_OK) return false;
    uint64_t clust_bytes = (uint64_t)fs->csize * 512;
    uint64_t total       = (uint64_t)(fs->n_fatent - 2) * clust_bytes;
    uint64_t free_bytes  = (uint64_t)fre_clust * clust_bytes;
    *total_out = total;
    *used_out  = total - free_bytes;
    return true;
}

// Human-readable byte count: "X.X GB", "X.X MB", or "XXX KB"
static void fmt_bytes(char *buf, int bufsz, uint64_t bytes) {
    if (bytes >= (uint64_t)1024 * 1024 * 1024)
        snprintf(buf, bufsz, "%.1f GB", (double)bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= (uint64_t)1024 * 1024)
        snprintf(buf, bufsz, "%.1f MB", (double)bytes / (1024.0 * 1024));
    else
        snprintf(buf, bufsz, "%lu KB", (unsigned long)(bytes / 1024));
}

// Generate the main HTML page into a dynamically allocated string.
// Caller must free() the returned pointer.
static char *build_html_page(void) {
    // We build into a large static buffer (stack is too small for this)
    static char html[32768];
    int pos = 0;

#define HCAT(fmt, ...) \
    pos += snprintf(html + pos, sizeof(html) - pos, fmt, ##__VA_ARGS__)

    HCAT("HTTP/1.1 200 OK\r\n"
         "Content-Type: text/html; charset=utf-8\r\n"
         "Connection: close\r\n"
         "Cache-Control: no-cache\r\n\r\n");

    HCAT("<!DOCTYPE html><html lang='en'><head>"
         "<meta charset='UTF-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<meta http-equiv='refresh' content='5'>"
         "<title>Nebula32 — Disc Selector</title>"
         "<style>"
         "body{font-family:system-ui,sans-serif;background:#1a1a2e;color:#e0e0e0;"
              "margin:0;padding:16px}"
         "h1{color:#c8a000;font-size:1.4rem;margin:0 0 4px 0}"
         ".subtitle{color:#888;font-size:.85rem;margin-bottom:20px}"
         ".status{background:#16213e;border-radius:8px;padding:12px 16px;"
                 "margin-bottom:20px;display:flex;gap:20px;flex-wrap:wrap;"
                 "border:1px solid #0f3460}"
         ".status-item{display:flex;flex-direction:column}"
         ".status-label{font-size:.72rem;color:#888;text-transform:uppercase;"
                        "letter-spacing:.05em}"
         ".status-value{font-size:1rem;font-weight:600;color:#c8a000}"
         ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));"
               "gap:16px}"
         ".disc{background:#16213e;border-radius:10px;padding:12px;cursor:pointer;"
               "border:2px solid transparent;transition:.15s;text-align:center;"
               "position:relative}"
         ".disc:hover{border-color:#c8a000;background:#1e2a4a}"
         ".disc.loaded{border-color:#2ecc71;background:#0d2b1e}"
         ".disc-cover{width:160px;height:160px;object-fit:cover;border-radius:6px;"
                     "background:#0f3460;display:block;margin:0 auto 10px}"
         ".disc-cover-svg{width:160px;height:160px;border-radius:6px;"
                         "background:#0f3460;display:flex;align-items:center;"
                         "justify-content:center;margin:0 auto 10px;"
                         "font-size:3rem}"
         ".disc-name{font-size:.8rem;color:#c8c8c8;word-break:break-all;"
                    "line-height:1.3}"
         ".badge{position:absolute;top:8px;right:8px;background:#2ecc71;"
                "color:#000;font-size:.65rem;font-weight:700;padding:2px 6px;"
                "border-radius:10px;text-transform:uppercase}"
         ".disc-num{position:absolute;top:8px;left:8px;background:#0f3460;"
                   "color:#888;font-size:.65rem;padding:2px 6px;border-radius:10px}"
         ".load-btn{display:block;width:100%%;margin-top:8px;padding:6px;"
                   "background:#0f3460;color:#c8a000;border:1px solid #c8a000;"
                   "border-radius:6px;cursor:pointer;font-size:.8rem;"
                   "transition:.15s}"
         ".load-btn:hover{background:#c8a000;color:#000}"
         ".load-btn.active{background:#2ecc71;color:#000;border-color:#2ecc71}"
         "@media(max-width:400px){.grid{grid-template-columns:repeat(2,1fr)}"
                                 ".disc-cover,.disc-cover-svg{width:120px;height:120px}}"
         "</style></head><body>");

    HCAT("<h1>💿 Nebula32, a CD32 Optical Drive Emulator</h1>"
         "<p class='subtitle'>%s &nbsp;·&nbsp; IP: %s &nbsp;·&nbsp; "
         "Use rotary encoder or click below to load a disc</p>",
         s_host, s_ip_str);

    // Status bar
    HCAT("<div class='status'>"
         "<div class='status-item'>"
         "<span class='status-label'>Drive State</span>"
         "<span class='status-value'>%s</span></div>",
         state_name(commo_bridge_get_drive_state()));

    if (s_loaded_index < s_image_count) {
        char base[128];
        const char *loaded_name = s_image_paths[s_loaded_index];
        const char *sl = strrchr(loaded_name, '/');
        strncpy(base, sl ? sl + 1 : loaded_name, sizeof(base) - 1);
        HCAT("<div class='status-item'>"
             "<span class='status-label'>Loaded Disc</span>"
             "<span class='status-value'>%s</span></div>", base);
    }

    HCAT("<div class='status-item'>"
         "<span class='status-label'>Images Found</span>"
         "<span class='status-value'>%lu</span></div>",
         (unsigned long)s_image_count);

    uint64_t sd_used = 0, sd_total = 0;
    if (sd_get_space(&sd_used, &sd_total)) {
        char used_str[16], total_str[16];
        fmt_bytes(used_str, sizeof(used_str), sd_used);
        fmt_bytes(total_str, sizeof(total_str), sd_total);
        HCAT("<div class='status-item'>"
             "<span class='status-label'>SD Card</span>"
             "<span class='status-value'>%s used / %s</span></div>",
             used_str, total_str);
    }

    HCAT("</div>");

    // Disc grid
    HCAT("<div class='grid'>");

    for (uint32_t i = 0; i < s_image_count && i < 99; i++) {
        char base[128];
        basename_no_ext(s_image_paths[i], base, sizeof(base));
        bool is_loaded = (i == s_loaded_index);

        char cover_path[256] = "";
        bool has_cover = cover_exists(s_image_paths[i], cover_path, sizeof(cover_path));

        HCAT("<div class='disc%s' onclick='loadDisc(%lu)'>",
             is_loaded ? " loaded" : "", (unsigned long)i);
        HCAT("<span class='disc-num'>%lu</span>", (unsigned long)(i + 1));
        if (is_loaded) HCAT("<span class='badge'>▶ Playing</span>");

        if (has_cover) {
            // Serve cover via /covers/ API endpoint
            HCAT("<img class='disc-cover' src='/covers/%s' "
                 "onerror=\"this.style.display='none'\" "
                 "alt='%s cover' loading='lazy'>", base, base);
        } else {
            HCAT("<div class='disc-cover-svg'>📀</div>");
        }

        HCAT("<div class='disc-name'>%s</div>", base);
        HCAT("<button class='load-btn%s' onclick='event.stopPropagation();loadDisc(%lu)'>"
             "%s</button>",
             is_loaded ? " active" : "",
             (unsigned long)i,
             is_loaded ? "✓ Loaded" : "Load");
        HCAT("</div>");  // .disc
    }

    HCAT("</div>");  // .grid

    // JavaScript
    HCAT("<script>"
         "function loadDisc(idx){"
         "  fetch('/api/load/'+idx,{method:'POST'})"
         "  .then(r=>r.json())"
         "  .then(d=>{if(d.ok){"
         "    document.querySelectorAll('.disc').forEach((el,i)=>{"
         "      el.classList.toggle('loaded',i===idx);"
         "      const btn=el.querySelector('.load-btn');"
         "      btn.textContent=i===idx?'✓ Loaded':'Load';"
         "      btn.classList.toggle('active',i===idx);"
         "      const badge=el.querySelector('.badge');"
         "      if(badge){badge.remove();}"
         "      if(i===idx){"
         "        const b=document.createElement('span');"
         "        b.className='badge';b.textContent='▶ Playing';el.appendChild(b);"
         "      }"
         "    });"
         "  }})"
         "  .catch(e=>console.error(e));"
         "}"
         "</script>");

    // Perspective starfield — 150 stars in normalised 3D space.
    // Each frame z decreases (star approaches); projected with x/z * W + cx.
    // Stars that reach z<=0 or fly off-screen are recycled at z=1 (far away).
    HCAT("<script>"
         "(function(){"
         "var c=document.createElement('canvas');"
         "c.style.cssText='position:fixed;top:0;left:0;width:100%%;height:100%%;z-index:-1;pointer-events:none';"
         "document.body.appendChild(c);"
         "var ctx=c.getContext('2d'),W,H,N=150,stars=[];"
         "function resize(){"
         "W=c.width=innerWidth;H=c.height=innerHeight;"
         "ctx.fillStyle='#1a1a2e';ctx.fillRect(0,0,W,H);}"
         "window.addEventListener('resize',resize);resize();"
         "function mk(){return{x:Math.random()-.5,y:Math.random()-.5,z:Math.random()*.9+.1};}"
         "for(var i=0;i<N;i++)stars.push(mk());"
         "function frame(){"
         "ctx.fillStyle='rgba(26,26,46,.2)';ctx.fillRect(0,0,W,H);"
         "var cx=W/2,cy=H/2;"
         "for(var i=0;i<N;i++){"
         "var s=stars[i];"
         "s.z-=.004;"
         "if(s.z<=0){stars[i]=mk();continue;}"
         "var px=s.x/s.z*W+cx,py=s.y/s.z*H+cy;"
         "if(px<0||px>W||py<0||py>H){stars[i]=mk();continue;}"
         "ctx.fillStyle='rgba(255,255,255,'+(1-s.z*.5)+')';"
         "ctx.beginPath();ctx.arc(px,py,Math.max(.2,(1-s.z)*2.5),0,6.28);ctx.fill();}"
         "requestAnimationFrame(frame);}"
         "frame();"
         "})();"
         "</script>");

    HCAT("</body></html>");
    return html;
#undef HCAT
}

#ifdef WEBSERVER_TEST_BUILD
// Expose build_html_page() to the host test suite.
// Only compiled when building with -DWEBSERVER_TEST_BUILD.
const char *webserver_get_page_for_test(void) { return build_html_page(); }
#endif

// ---------------------------------------------------------------------------
// Build JSON status response
// ---------------------------------------------------------------------------
static void build_json_status(char *buf, int bufsz) {
    char loaded_name[128] = "none";
    if (s_loaded_index < s_image_count) {
        const char *p = s_image_paths[s_loaded_index];
        const char *sl = strrchr(p, '/');
        strncpy(loaded_name, sl ? sl + 1 : p, sizeof(loaded_name) - 1);
    }

    int pos = snprintf(buf, bufsz,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n\r\n"
        "{\"ok\":true,\"state\":\"%s\",\"state_id\":%d,"
        "\"loaded_index\":%lu,\"loaded_name\":\"%s\","
        "\"image_count\":%lu,\"ip\":\"%s\"",
        state_name(commo_bridge_get_drive_state()), commo_bridge_get_drive_state(),
        (unsigned long)s_loaded_index, loaded_name,
        (unsigned long)s_image_count, s_ip_str);

    uint64_t sd_used = 0, sd_total = 0;
    if (sd_get_space(&sd_used, &sd_total)) {
        snprintf(buf + pos, bufsz - pos,
            ",\"sd_used_mb\":%lu,\"sd_total_mb\":%lu}",
            (unsigned long)(sd_used  / (1024 * 1024)),
            (unsigned long)(sd_total / (1024 * 1024)));
    } else {
        snprintf(buf + pos, bufsz - pos, "}");
    }
}

// ---------------------------------------------------------------------------
// Handle HTTP request line
// ---------------------------------------------------------------------------
// Returns the response as a static buffer or NULL to stream a file.
// 'conn' holds the accumulated request.

static char s_resp_buf[4096];

static void handle_request(struct tcp_pcb *pcb, http_conn_t *conn) {
    char *req   = conn->request;
    bool  is_get  = (strncmp(req, "GET ",  4) == 0);
    bool  is_post = (strncmp(req, "POST ", 5) == 0);
    char *path_start = req + (is_get ? 4 : 5);
    char  path[256]  = "";

    // Extract URL path (up to space or \r\n)
    int i = 0;
    while (path_start[i] && path_start[i] != ' ' && path_start[i] != '\r'
           && i < 255) {
        path[i] = path_start[i];
        i++;
    }
    path[i] = '\0';

    LOG_INFO_MSG("HTTP", "%s %s", is_get ? "GET" : "POST", path);

    // ── GET / ── Main HTML page
    if (is_get && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
        char *html = build_html_page();
        tcp_write(pcb, html, strlen(html), TCP_WRITE_FLAG_COPY);
        tcp_output(pcb);
        return;
    }

    // ── GET /api/status ── JSON status
    if (is_get && strcmp(path, "/api/status") == 0) {
        build_json_status(s_resp_buf, sizeof(s_resp_buf));
        tcp_write(pcb, s_resp_buf, strlen(s_resp_buf), TCP_WRITE_FLAG_COPY);
        tcp_output(pcb);
        return;
    }

    // ── GET /api/images ── JSON image list
    if (is_get && strcmp(path, "/api/images") == 0) {
        int pos = snprintf(s_resp_buf, sizeof(s_resp_buf),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
            "Connection: close\r\n\r\n[");
        for (uint32_t j = 0; j < s_image_count && j < 99; j++) {
            char base[128], cp[256] = "";
            basename_no_ext(s_image_paths[j], base, sizeof(base));
            bool hc = cover_exists(s_image_paths[j], cp, sizeof(cp));
            pos += snprintf(s_resp_buf + pos, sizeof(s_resp_buf) - pos,
                "%s{\"index\":%lu,\"name\":\"%s\",\"has_cover\":%s,\"loaded\":%s}",
                j > 0 ? "," : "",
                (unsigned long)j, base,
                hc ? "true" : "false",
                (j == s_loaded_index) ? "true" : "false");
        }
        pos += snprintf(s_resp_buf + pos, sizeof(s_resp_buf) - pos, "]");
        tcp_write(pcb, s_resp_buf, pos, TCP_WRITE_FLAG_COPY);
        tcp_output(pcb);
        return;
    }

    // ── POST /api/load/{index} ── Load a disc
    if (is_post && strncmp(path, "/api/load/", 10) == 0) {
        uint32_t idx = (uint32_t)atoi(path + 10);
        if (idx < s_image_count) {
            s_load_index   = idx;
            s_load_pending = true;
            LOG_INFO_MSG("HTTP", "web load request: image %lu", (unsigned long)idx);
            snprintf(s_resp_buf, sizeof(s_resp_buf),
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                "Connection: close\r\n\r\n"
                "{\"ok\":true,\"index\":%lu}", (unsigned long)idx);
        } else {
            snprintf(s_resp_buf, sizeof(s_resp_buf),
                "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
                "Connection: close\r\n\r\n"
                "{\"ok\":false,\"error\":\"index out of range\"}");
        }
        tcp_write(pcb, s_resp_buf, strlen(s_resp_buf), TCP_WRITE_FLAG_COPY);
        tcp_output(pcb);
        return;
    }

    // ── POST /api/eject ── Eject disc
    if (is_post && strcmp(path, "/api/eject") == 0) {
        s_load_index   = UINT32_MAX;
        s_load_pending = true;
        snprintf(s_resp_buf, sizeof(s_resp_buf),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
            "Connection: close\r\n\r\n{\"ok\":true}");
        tcp_write(pcb, s_resp_buf, strlen(s_resp_buf), TCP_WRITE_FLAG_COPY);
        tcp_output(pcb);
        return;
    }

    // ── GET /covers/{name}.jpg ── Serve cover art from SD card
    if (is_get && strncmp(path, "/covers/", 8) == 0) {
        // Sanitise: reject any ".." path traversal attempts
        if (strstr(path, "..")) {
            static const char *forbidden =
                "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n";
            tcp_write(pcb, forbidden, strlen(forbidden), TCP_WRITE_FLAG_COPY);
            tcp_output(pcb);
            return;
        }

        char sd_path[264];
        snprintf(sd_path, sizeof(sd_path), "0:/covers/%s", path + 8);

        FIL cover_file;
        FRESULT fr = f_open(&cover_file, sd_path, FA_READ);
        if (fr == FR_OK) {
            FSIZE_t fsize = f_size(&cover_file);
            // Send HTTP header
            int hlen = snprintf(s_resp_buf, sizeof(s_resp_buf),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: image/jpeg\r\n"
                "Content-Length: %lu\r\n"
                "Cache-Control: max-age=3600\r\n"
                "Connection: close\r\n\r\n",
                (unsigned long)fsize);
            tcp_write(pcb, s_resp_buf, hlen, TCP_WRITE_FLAG_COPY);

            // Stream file in 512-byte chunks
            static uint8_t chunk[512];
            UINT br;
            while (f_read(&cover_file, chunk, sizeof(chunk), &br) == FR_OK && br > 0) {
                // Wait if TCP send buffer is full
                err_t e = tcp_write(pcb, chunk, br, TCP_WRITE_FLAG_COPY);
                if (e != ERR_OK) break;
            }
            f_close(&cover_file);
            tcp_output(pcb);
            return;
        }

        // Cover not found — return 404 with generic CD SVG
        static const char *not_found =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: image/svg+xml\r\n"
            "Cache-Control: max-age=60\r\n"
            "Connection: close\r\n\r\n"
            "<svg xmlns='http://www.w3.org/2000/svg' width='200' height='200'>"
            "<rect width='200' height='200' fill='#0f3460' rx='8'/>"
            "<circle cx='100' cy='100' r='75' fill='#1a1a4e' stroke='#c8a000' stroke-width='2'/>"
            "<circle cx='100' cy='100' r='15' fill='#0f3460' stroke='#888' stroke-width='1'/>"
            "<text x='100' y='170' text-anchor='middle' fill='#888' "
            "font-family='system-ui' font-size='12'>No Cover Art</text>"
            "</svg>";
        tcp_write(pcb, not_found, strlen(not_found), TCP_WRITE_FLAG_COPY);
        tcp_output(pcb);
        return;
    }

    // ── 404 fallback ──
    snprintf(s_resp_buf, sizeof(s_resp_buf),
        "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n"
        "Connection: close\r\n\r\nNot found: %s", path);
    tcp_write(pcb, s_resp_buf, strlen(s_resp_buf), TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
}

// ---------------------------------------------------------------------------
// lwIP TCP callbacks
// ---------------------------------------------------------------------------
static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb,
                          struct pbuf *p, err_t err) {
    http_conn_t *conn = (http_conn_t *)arg;

    if (!p || err != ERR_OK) {
        tcp_close(pcb);
        if (conn) free(conn);
        return ERR_OK;
    }

    // Accumulate request data
    struct pbuf *q = p;
    while (q && conn->req_len < (int)sizeof(conn->request) - 1) {
        int copy = q->len;
        if (conn->req_len + copy >= (int)sizeof(conn->request) - 1)
            copy = sizeof(conn->request) - 1 - conn->req_len;
        memcpy(conn->request + conn->req_len, q->payload, copy);
        conn->req_len += copy;
        q = q->next;
    }
    conn->request[conn->req_len] = '\0';
    pbuf_free(p);

    // Check if we have a complete HTTP header (ends with \r\n\r\n)
    if (strstr(conn->request, "\r\n\r\n")) {
        handle_request(pcb, conn);
        tcp_close(pcb);
        free(conn);
        tcp_arg(pcb, NULL);
    }

    tcp_recved(pcb, p ? p->tot_len : 0);
    return ERR_OK;
}

static err_t tcp_accept_cb(void *arg, struct tcp_pcb *new_pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || !new_pcb) return ERR_VAL;

    http_conn_t *conn = (http_conn_t *)malloc(sizeof(http_conn_t));
    if (!conn) {
        tcp_close(new_pcb);
        return ERR_MEM;
    }
    memset(conn, 0, sizeof(*conn));

    tcp_arg(new_pcb, conn);
    tcp_recv(new_pcb, tcp_recv_cb);
    tcp_setprio(new_pcb, TCP_PRIO_MIN);  // Low priority vs bus handler

    return ERR_OK;
}

// ---------------------------------------------------------------------------
// webserver_init
// ---------------------------------------------------------------------------
bool webserver_init(void) {
    read_wifi_config();

    if (s_ssid[0] == '\0') {
        printf("[WEB] No wifi_ssid in cd32_ode.cfg — WiFi disabled\n"
               "[WEB] Add these lines to enable:\n"
               "      wifi_ssid     = YourNetworkName\n"
               "      wifi_password = YourPassword\n");
        return false;
    }

    printf("[WEB] Connecting to WiFi SSID: %s ...\n", s_ssid);
    LOG_INFO_MSG("WEB ", "connecting to SSID: %s", s_ssid);

    // Initialise WiFi in poll mode (no RTOS required)
    if (cyw43_arch_init()) {
        printf("[WEB] CYW43 init failed\n");
        LOG_ERROR_MSG("WiFi CYW43 init failed");
        return false;
    }

    cyw43_arch_enable_sta_mode();

    // Connect (timeout: 30 seconds)
    if (cyw43_arch_wifi_connect_timeout_ms(s_ssid, s_pass,
                                            CYW43_AUTH_WPA2_AES_PSK,
                                            30000) != 0) {
        printf("[WEB] WiFi connection failed (wrong password or SSID?)\n");
        LOG_ERROR_MSG("WiFi connection failed (SSID=%s)", s_ssid);
        return false;
    }

    // Get IP address
    ip4_addr_t *ip = &cyw43_state.netif[0].ip_addr;
    snprintf(s_ip_str, sizeof(s_ip_str), "%s", ip4addr_ntoa(ip));
    printf("[WEB] Connected! IP: %s\n", s_ip_str);
    LOG_INFO_MSG("WEB ", "WiFi connected, IP=%s", s_ip_str);

    // Start TCP listener on port 80
    s_listener = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!s_listener) return false;

    tcp_bind(s_listener, IP_ANY_TYPE, WS_HTTP_PORT);
    s_listener = tcp_listen_with_backlog(s_listener, 4);
    tcp_accept(s_listener, tcp_accept_cb);

    s_running = true;
    printf("[WEB] HTTP server running at http://%s/\n", s_ip_str);
    printf("[WEB] Also try: http://%s.local/\n", s_host);
    LOG_INFO_MSG("WEB ", "HTTP server listening on port %d", WS_HTTP_PORT);

    return true;
}

// ---------------------------------------------------------------------------
// webserver_poll — call from Core 0 main loop
// ---------------------------------------------------------------------------
void webserver_poll(void) {
    if (!s_running) return;
    cyw43_arch_poll();
}

void webserver_notify_state_change(void) {
    // No persistent state to invalidate in our simple implementation —
    // the HTML page is always rebuilt fresh from current globals.
}

bool webserver_is_running(void) { return s_running; }
const char *webserver_get_ip(void) { return s_ip_str; }

bool webserver_has_load_request(void) {
    if (s_load_pending) {
        s_load_pending = false;
        return true;
    }
    return false;
}

uint32_t webserver_get_load_index(void) { return s_load_index; }

// Called by main.c after a disc is loaded to update the "loaded" badge
void webserver_set_loaded_index(uint32_t index) {
    s_loaded_index = index;
}

#else // !PICO_CYW43_SUPPORTED — stub out the entire webserver for non-W builds

bool webserver_init(void) { return false; }
void webserver_poll(void) {}
void webserver_notify_state_change(void) {}
bool webserver_is_running(void) { return false; }
const char *webserver_get_ip(void) { return ""; }
bool webserver_has_load_request(void) { return false; }
uint32_t webserver_get_load_index(void) { return 0; }
void webserver_set_loaded_index(uint32_t index) { (void)index; }

#endif // PICO_CYW43_SUPPORTED
