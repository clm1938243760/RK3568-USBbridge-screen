#define _GNU_SOURCE
#include "api_config.h"
#include "usb_bridge_common.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <linux/input.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEVICE_NAME "USBKey Chip USBKey Module"
#define DEVICES_FILE "/proc/bus/input/devices"
#define TEMPLATE_FILE BASE_DIR "/config/MarkInfo_SearchTitle_Config_100.json"
#define FORM_QUEUE_DIR BASE_DIR "/form_queue"
#define LOG_FILE BASE_DIR "/logs/scan_patient_capture.log"
#define SCAN_LOG "/root/scan.log"
#define API_RAW_DIR BASE_DIR "/api_raw"
#define STATE_DIR BASE_DIR "/state"
#define CURRENT_PATIENT_FILE STATE_DIR "/current_patient.json"
#define REPORT_WAIT_DIR BASE_DIR "/report_wait_queue"

/* 鎵爜鏋€氬父浼鎴愰敭鐩樸€傛櫘閫氬洖杞﹀拰灏忛敭鐩樺洖杞﹂兘琛ㄧず鈥滀竴娆℃壂鐮佺粨鏉熲€濄€?*/
static const int enter_codes[] = { KEY_ENTER, KEY_KPENTER };
/* 璁板綍宸﹀彸 Shift锛岀敤鏉ヨ繕鍘熷ぇ鍐欏瓧姣嶅拰 !@# 杩欑被绗﹀彿銆?*/
static const int shift_codes[] = { KEY_LEFTSHIFT, KEY_RIGHTSHIFT };

/* 鎶?Linux input 鐨勬寜閿爜杞崲鎴愬疄闄呭瓧绗︺€? * 渚嬪 KEY_A -> "a"锛孠EY_1 -> "1"锛孠EY_SLASH -> "/"銆? */
static const char *key_map(int code) {
    static char one[2];
    switch (code) {
        case KEY_1: case KEY_KP1: return "1"; case KEY_2: case KEY_KP2: return "2";
        case KEY_3: case KEY_KP3: return "3"; case KEY_4: case KEY_KP4: return "4";
        case KEY_5: case KEY_KP5: return "5"; case KEY_6: case KEY_KP6: return "6";
        case KEY_7: case KEY_KP7: return "7"; case KEY_8: case KEY_KP8: return "8";
        case KEY_9: case KEY_KP9: return "9"; case KEY_0: case KEY_KP0: return "0";
        case KEY_MINUS: return "-"; case KEY_EQUAL: return "="; case KEY_LEFTBRACE: return "[";
        case KEY_RIGHTBRACE: return "]"; case KEY_SEMICOLON: return ";"; case KEY_APOSTROPHE: return "'";
        case KEY_GRAVE: return "`"; case KEY_BACKSLASH: return "\\"; case KEY_COMMA: return ",";
        case KEY_DOT: return "."; case KEY_SLASH: return "/"; case KEY_SPACE: return " ";
        default:
            if (code >= KEY_Q && code <= KEY_P) { const char *s = "qwertyuiop"; one[0] = s[code - KEY_Q]; return one; }
            if (code >= KEY_A && code <= KEY_L) { const char *s = "asdfghjkl"; one[0] = s[code - KEY_A]; return one; }
            if (code >= KEY_Z && code <= KEY_M) { const char *s = "zxcvbnm"; one[0] = s[code - KEY_Z]; return one; }
    }
    return NULL;
}

/* 鏍规嵁 Shift 鐘舵€佹妸鏅€氬瓧绗﹁浆鎹㈡垚 Shift 鍚庣殑瀛楃銆? * 渚嬪 a -> A锛? -> !锛? -> _銆? */
static char shift_char(char c) {
    const char *normal = "1234567890-=[];'`,./\\";
    const char *shifted = "!@#$%^&*()_+{}:\"~<>?|";
    for (int i = 0; normal[i]; i++) if (normal[i] == c) return shifted[i];
    if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
    return c;
}
/* 澶у皬鍐欎笉鏁忔劅鍦板垽鏂?s 閲屾槸鍚﹀寘鍚?needle銆? * 鐢ㄤ簬璇嗗埆璁惧鍚嶉噷鐨?usbkey/scanner/barcode 绛夊叧閿瘝銆? */
static int contains_ci(const char *s, const char *needle) {
    if (!s || !needle || !*needle) return 0;
    size_t n = strlen(needle);
    for (; *s; s++) {
        size_t i = 0;
        while (i < n && s[i] &&
               tolower((unsigned char)s[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == n) return 1;
    }
    return 0;
}
/* 浠?/proc/bus/input/devices 鐨勪竴涓澶囧潡閲屾彁鍙?N: Name="..."銆? * 杩欐槸鏃х殑鈥滄寜璁惧鍚嶇寽鎵爜鏋€濋€昏緫浣跨敤鐨勮緟鍔╁嚱鏁般€? */
static void block_name(const char *block, char *out, size_t out_size) {
    out[0] = 0;
    const char *p = strstr(block, "N: Name=");
    if (!p) return;
    p += strlen("N: Name=");
    if (*p == '"') p++;
    size_t i = 0;
    while (*p && *p != '"' && *p != '\n' && i + 1 < out_size) out[i++] = *p++;
    out[i] = 0;
}

/* 浠庤澶囧潡鐨?Handlers 琛岄噷鎻愬彇 event 鍚嶇О銆? * 渚嬪 Handlers=sysrq kbd event5 leds -> event5銆? */
static int block_event(const char *block, char *out, size_t out_size) {
    char *h = strstr((char *)block, "Handlers=");
    char *e = h ? strstr(h, "event") : NULL;
    if (!e) return 0;
    int i = 0;
    while (e[i] && !isspace((unsigned char)e[i]) && i + 1 < (int)out_size) {
        out[i] = e[i];
        i++;
    }
    out[i] = 0;
    return i > 0;
}
/* 缁欒緭鍏ヨ澶囨墦鍒嗭紝鍒ゆ柇瀹冣€滃儚涓嶅儚鎵爜鏋€濄€? * 杩欎釜閫昏緫鐜板湪鍙綔涓哄厹搴曚繚鐣欙紱榛樿璺緞宸茬粡鏀逛负鐩戝惉鍏ㄩ儴 event锛? * 閬垮厤鍥犱负鎵撳垎璇€?event8 鑰岄敊杩囩湡姝ｇ殑 event5銆? */
static int scanner_score(const char *block, const char *name, const char *wanted_name) {
    int score = 0;

    if (wanted_name && *wanted_name && strstr(name, wanted_name)) score += 200;
    if (strstr(name, DEVICE_NAME)) score += 180;

    if (contains_ci(name, "usbkey")) score += 100;
    if (contains_ci(name, "scanner")) score += 100;
    if (contains_ci(name, "barcode")) score += 100;
    if (contains_ci(name, "scan")) score += 70;
    if (contains_ci(name, "usb")) score += 45;
    if (contains_ci(name, "hid")) score += 30;
    if (contains_ci(name, "keyboard")) score += 25;
    if (contains_ci(block, "Handlers=") && contains_ci(block, "kbd")) score += 15;

    if (contains_ci(name, "mouse")) score -= 80;
    if (contains_ci(name, "touch")) score -= 80;
    if (contains_ci(name, "gpio")) score -= 80;
    if (contains_ci(name, "pwrkey")) score -= 80;
    if (contains_ci(name, "rk805")) score -= 80;
    if (contains_ci(name, "adc")) score -= 80;
    if (contains_ci(name, "ir")) score -= 80;
    if (contains_ci(name, "rockchip")) score -= 60;

    return score;
}

/* 鍒涘缓鏈▼搴忚繍琛岄渶瑕佺殑鐩綍銆? * 杩欎簺鐩綍鍒嗗埆鐢ㄤ簬濉〃闃熷垪銆佹棩蹇椼€佹帴鍙ｅ師濮嬭繑鍥炪€佺姸鎬佹枃浠跺拰绛夊緟鎵撳嵃闃熷垪銆? */
static void ensure_dirs(void) {
    ensure_dir(FORM_QUEUE_DIR);
    ensure_parent_dir(LOG_FILE);
    ensure_dir(API_RAW_DIR);
    ensure_dir(STATE_DIR);
    ensure_dir(REPORT_WAIT_DIR);
}//纭繚鐩綍瀛樺湪

/* 淇濆瓨鍘熷鎵爜鍐呭鍒?/root/scan.log銆? * 杩欎釜鏂囦欢鐢ㄤ簬鎺掓煡鈥滄壂鐮佹灙鏄惁鐪熺殑杈撳叆浜嗗唴瀹广€佸唴瀹规槸浠€涔堚€濄€? */
static void save_scan_log(const char *text) {
    ensure_parent_dir(SCAN_LOG);
    FILE *f = fopen(SCAN_LOG, "a");
    if (f) {
        fprintf(f, "[%s] %s\n", now_text(), text);
        fclose(f);
    }
}       //淇濆瓨鎵弿鏃ュ織

/* 鏃х殑鍗曡澶囨煡鎵鹃€昏緫銆? *
 * 濡傛灉璁剧疆 SCANNER_DEV=/dev/input/event5锛屽垯鐩存帴浣跨敤鎸囧畾璁惧銆? * 鍚﹀垯璇诲彇 /proc/bus/input/devices锛屽苟鏍规嵁 scanner_score() 閫夋嫨鏈€鍍忔壂鐮佹灙鐨勮澶囥€? *
 * 娉ㄦ剰锛氶粯璁ゆ儏鍐典笅 main() 涓嶈蛋杩欓噷锛岃€屾槸 read_all_input_events()
 * 鍚屾椂鐩戝惉鎵€鏈?/dev/input/event*銆? */
static int find_event_dev(char *out, size_t out_size) {
    const char *forced_dev = getenv("SCANNER_DEV");
    if (forced_dev && *forced_dev) {
        snprintf(out, out_size, "%s", forced_dev);
        return path_exists(out);
    }

    const char *scanner_name = getenv("SCANNER_NAME");

    char *content = read_file_all(DEVICES_FILE, NULL);
    if (!content) return 0;

    char *block = content;
    char best_event[32] = "";
    char best_name[128] = "";
    int best_score = -100000;

    while (block && *block) {
        char *next = strstr(block, "\n\n");
        if (next) {
            *next = 0;
            next += 2;
        }

        char ev[32], name[128];
        if (block_event(block, ev, sizeof(ev))) {
            block_name(block, name, sizeof(name));
            int score = scanner_score(block, name, scanner_name);
            if (score > best_score) {
                best_score = score;
                snprintf(best_event, sizeof(best_event), "%s", ev);
                snprintf(best_name, sizeof(best_name), "%s", name);
            }
        }

        block = next;
    }

    if (best_event[0] && best_score >= 20) {
        snprintf(out, out_size, "/dev/input/%s", best_event);
        log_line(LOG_FILE, "scanner auto selected: %s name=\"%s\" score=%d", out, best_name, best_score);
        free(content);
        return 1;
    }

    free(content);
    return 0;
}

/* 鏃х殑绛夊緟閫昏緫锛氬惊鐜瓑寰?find_event_dev() 鎵惧埌鍙敤璁惧銆? * 鍙湁鎵嬪姩璁剧疆 SCANNER_DEV 鏃舵墠浼氫娇鐢ㄨ繖鏉¤矾寰勩€? */
static void wait_scanner(char *dev, size_t size) {
    int dump_count = 0;
    for (;;) {
        if (find_event_dev(dev, size) && path_exists(dev)) {
            log_line(LOG_FILE, "scanner found: %s", dev);
            return;
        }
        const char *scanner_name = getenv("SCANNER_NAME");
        const char *scanner_dev = getenv("SCANNER_DEV");
        log_line(LOG_FILE, "scanner not found name=%s dev=%s, retry in 2s",
            scanner_name && *scanner_name ? scanner_name : "<auto>",
            scanner_dev && *scanner_dev ? scanner_dev : "<auto>");
        if ((dump_count++ % 5) == 0) {
            char *content = read_file_all(DEVICES_FILE, NULL);
            if (content) {
                char *line = strtok(content, "\n");
                while (line) {
                    if (!strncmp(line, "N: Name=", 8) || strstr(line, "Handlers="))
                        log_line(LOG_FILE, "input: %s", line);
                    line = strtok(NULL, "\n");
                }
                free(content);
            }
        }
        sleep(2);
    }
}

/* 鏍规嵁鎵爜鍐呭鐢熸垚鏌ヨ鎮ｈ€呬俊鎭殑 SQL銆? * 浼氭妸鎵爜鍐呭杞垚澶у啓锛屽苟鎶婂崟寮曞彿鏇挎崲涓轰袱涓崟寮曞彿锛岄伩鍏嶇牬鍧?SQL 瀛楃涓层€? */
static char *build_sql(const char *scan) {
    strbuf_t kw;
    sb_init(&kw);
    for (const char *p = scan; p && *p; p++) {
        if (*p == '\'') sb_append(&kw, "''");
        else {
            char c = *p;
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            sb_appendn(&kw, &c, 1);
        }
    }
    strbuf_t sql;
    sb_init(&sql);
    sb_printf(&sql,
        "select\n"
        "  t.exam_item,\n  t.his_exam_no,\n  z.report_no,\n  t.patient_id,\n"
        "  t.patient_name,\n  q.name_phonetic,\n  substr(t.patient_name, 0, 2) as xing,\n"
        "  substr(t.patient_name, 2, 8) as ming,\n  t.sex,\n  t.age,\n"
        "  to_char(t.birthday,'yyyy') as nian,\n  to_char(t.birthday,'mm') as yue,\n"
        "  to_char(t.birthday,'dd') as ri,\n  t.birthday\n"
        "from exam_master t\n"
        "left join exam_item z on t.his_exam_no=z.his_exam_no\n"
        "left join patient_info q on t.patient_id=q.patient_id\n"
        "where\n  (\n"
        "    upper(z.report_no) like '%%%s%%'\n"
        "    or upper(t.patient_id) like '%%%s%%'\n"
        "    or upper(t.patient_name) like '%%%s%%'\n"
        "    or upper(t.his_exam_no) like '%%%s%%'\n"
        "  )\norder by t.req_date desc\nlimit 5",
        kw.data ? kw.data : "", kw.data ? kw.data : "", kw.data ? kw.data : "", kw.data ? kw.data : "");
    sb_free(&kw);
    return sql.data;
}

/* 璇锋眰鎮ｈ€呬俊鎭帴鍙ｃ€? *
 * 涓昏姝ラ锛? * 1. 鏍规嵁鎵爜鍐呭鐢熸垚 SQL銆? * 2. 鎶?SQL 鍋?base64锛屾斁杩?{"sqlStr":"..."}銆? * 3. 璋冪敤 curl POST 鍒?API_URL銆? * 4. 鎶婃帴鍙ｅ師濮嬭繑鍥炰繚瀛樺埌 api_raw锛屾柟渚跨幇鍦烘帓鏌ャ€? * 5. 瑙ｆ瀽 JSON 骞惰繑鍥炵粰涓婂眰銆? */
static json_value_t *http_get_patient(const char *scan) {
    api_config_t cfg;
    api_config_load(&cfg);
    char *sql = build_sql(scan);
    char *b64 = base64_encode((const unsigned char *)sql, strlen(sql));
    long long ts = now_ms();
    char payload[1024], raw[1024], cmd[4096];
    snprintf(payload, sizeof(payload), "%s/payload_%lld.json", API_RAW_DIR, ts);
    snprintf(raw, sizeof(raw), "%s/api_%lld.json", API_RAW_DIR, ts);
    strbuf_t body;
    sb_init(&body);
    sb_append(&body, "{\"sqlStr\":");
    json_escape_append(&body, b64 ? b64 : "");
    sb_append(&body, "}\n");
    atomic_write_file(payload, body.data, body.len);
    char *qpayload = shell_quote(payload);
    char *qraw = shell_quote(raw);
    char *qurl = shell_quote(cfg.patient_query_url);
    snprintf(cmd, sizeof(cmd),
        "curl -fsS --max-time %d -H 'Content-Type: application/json;charset=UTF-8' "
        "-H 'Accept: application/json' -A RK3568-USB-Bridge --data-binary @%s -o %s %s",
        cfg.patient_api_timeout, qpayload, qraw, qurl);
    log_line(LOG_FILE, "request api POST json: %s scan=%s", cfg.patient_query_url, scan);
    log_line(LOG_FILE, "sql raw first chars: %.80s", sql);
    log_line(LOG_FILE, "sql base64 first chars: %.80s", b64 ? b64 : "");
    int rc = system(cmd);
    free(qpayload); free(qraw); free(qurl); free(sql); free(b64); sb_free(&body);
    unlink(payload);
    if (rc != 0) return NULL;
    log_line(LOG_FILE, "api raw saved: %s", raw);
    return json_parse_file(raw);
}

/* 浠庢帴鍙ｈ繑鍥為噷鎸戠涓€鏉℃偅鑰呰褰曘€? * 鍏煎鐩存帴鏁扮粍銆乨ata/result/rows/list 鍖呰９銆佺洿鎺ュ璞″嚑绉嶆牸寮忋€? */
static json_value_t *pick_first_record(json_value_t *api) {
    if (json_value_is_array(api)) return json_arr_get(api, 0);
    if (!json_value_is_object(api)) return NULL;
    const char *keys[] = {"data", "result", "rows", "list"};
    for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); i++) {
        json_value_t *v = json_obj_get(api, keys[i]);
        if (json_value_is_array(v)) return json_arr_get(v, 0);
        if (json_value_is_object(v)) return v;
    }
    if (json_obj_get(api, "patient_id") || json_obj_get(api, "patient_name")) return api;
    return NULL;
}

static json_value_t *records_array(json_value_t *api) {
    if (json_value_is_array(api)) return api;
    if (!json_value_is_object(api)) return NULL;
    const char *keys[] = {"data", "result", "rows", "list"};
    for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); i++) {
        json_value_t *v = json_obj_get(api, keys[i]);
        if (json_value_is_array(v)) return v;
    }
    return NULL;
}

static int scan_debounce_allows(const char *scan) {
    static char last_scan[512];
    static long long last_ms;
    long long now = now_ms();
    if (scan && *scan && !strcmp(scan, last_scan) && now - last_ms < 3000) {
        log_line(LOG_FILE, "ignore duplicate scan within debounce window: %s", scan);
        return 0;
    }
    snprintf(last_scan, sizeof(last_scan), "%s", scan ? scan : "");
    last_ms = now;
    return 1;
}

static int open_input_fds(int *fds, int max_fds) {
    DIR *d = opendir("/dev/input");
    if (!d) return 0;

    int count = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        if (count >= max_fds) break;

        char path[128];
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            fds[count++] = fd;
            log_line(LOG_FILE, "project select listens: %s", path);
        }
    }

    closedir(d);
    return count;
}

static int open_project_input_fds(const api_config_t *cfg, int *fds, int max_fds) {
    int count = 0;
    if (cfg && cfg->project_input_dev[0]) {
        int fd = open(cfg->project_input_dev, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            log_line(LOG_FILE, "ERROR open project input %s failed: %s",
                cfg->project_input_dev, strerror(errno));
        } else {
            fds[count++] = fd;
            log_line(LOG_FILE, "project select listens configured device: %s", cfg->project_input_dev);
        }
    }

    DIR *d = opendir("/dev/input");
    if (!d) return count;

    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        if (count >= max_fds) break;

        char path[128];
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        if (cfg && cfg->project_input_dev[0] && !strcmp(path, cfg->project_input_dev)) continue;

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            fds[count++] = fd;
            log_line(LOG_FILE, "project select also listens: %s", path);
        }
    }

    closedir(d);
    return count;
}

static void close_input_fds(int *fds, int count) {
    for (int i = 0; i < count; i++) {
        if (fds[i] >= 0) close(fds[i]);
        fds[i] = -1;
    }
}

static int is_project_key(int code, const api_config_t *cfg) {
    return code == cfg->project_key_prev ||
           code == cfg->project_key_next ||
           code == cfg->project_key_confirm ||
           code == KEY_UP || code == KEY_DOWN ||
           code == KEY_LEFT || code == KEY_RIGHT ||
           code == KEY_ENTER || code == KEY_KPENTER;
}

static int is_navigation_project_key(int code, const api_config_t *cfg) {
    return code == cfg->project_key_prev ||
           code == cfg->project_key_next ||
           code == KEY_UP || code == KEY_DOWN ||
           code == KEY_LEFT || code == KEY_RIGHT;
}

/* 瀹夊叏鍙栧瓧娈碉細瀛楁涓嶅瓨鍦ㄦ椂杩斿洖绌哄瓧绗︿覆锛岃€屼笉鏄?NULL銆?*/
static const char *norm_field(json_value_t *row, const char *key) {
    return json_str(row, key, "");
}

static const char *project_name(json_value_t *row) {
    const char *name = norm_field(row, "exam_item");
    return name && *name ? name : "(no exam item)";
}

static const char *skip_project_space(const char *s) {
    while (s && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) s++;
    return s ? s : "";
}

static size_t project_trimmed_len(const char *s) {
    const char *p = skip_project_space(s);
    size_t n = strlen(p);
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\r' || p[n - 1] == '\n')) n--;
    return n;
}

static int same_project_name(const char *a, const char *b) {
    const char *aa = skip_project_space(a);
    const char *bb = skip_project_space(b);
    size_t an = project_trimmed_len(aa);
    size_t bn = project_trimmed_len(bb);
    return an == bn && strncmp(aa, bb, an) == 0;
}

static void publish_project_selection(const char *scan, json_value_t *row,
                                      int selected, const char **projects, int count) {
    ui_status_update_projects(scan,
        norm_field(row, "patient_id"),
        norm_field(row, "report_no"),
        selected, count, projects);
}

static int select_record_index(const char *scan, json_value_t *records, const api_config_t *cfg) {
    int record_count = json_arr_size(records);
    if (record_count <= 0) return -1;

    const char **projects = calloc((size_t)record_count, sizeof(projects[0]));
    int *record_indexes = calloc((size_t)record_count, sizeof(record_indexes[0]));
    if (!projects || !record_indexes) {
        free(projects);
        free(record_indexes);
        return -1;
    }

    int count = 0;
    for (int i = 0; i < record_count; i++) {
        const char *name = project_name(json_arr_get(records, i));
        int duplicate = 0;
        for (int j = 0; j < count; j++) {
            if (same_project_name(name, projects[j])) {
                duplicate = 1;
                log_line(LOG_FILE, "project duplicate skipped record=%d same_as_option=%d item=%s", i, j, name);
                break;
            }
        }
        if (duplicate) continue;
        projects[count] = name;
        record_indexes[count] = i;
        log_line(LOG_FILE, "project option %d record=%d/%d: %s", count + 1, i + 1, record_count, projects[count]);
        count++;
    }
    if (count <= 0) {
        free(projects);
        free(record_indexes);
        return -1;
    }

    int selected = 0;
    publish_project_selection(scan, json_arr_get(records, record_indexes[selected]), selected, projects, count);

    int fds[64];
    for (int i = 0; i < 64; i++) fds[i] = -1;
    int fd_count = open_project_input_fds(cfg, fds, 64);
    if (fd_count <= 0) {
        log_line(LOG_FILE, "ERROR project select found no /dev/input/event* devices");
        free(projects);
        free(record_indexes);
        return -1;
    }

    log_line(LOG_FILE, "project select waiting: prev=%d next=%d confirm=%d",
        cfg->project_key_prev, cfg->project_key_next, cfg->project_key_confirm);

    for (;;) {
        struct pollfd pfds[64];
        for (int i = 0; i < fd_count; i++) {
            pfds[i].fd = fds[i];
            pfds[i].events = POLLIN;
            pfds[i].revents = 0;
        }

        int rc = poll(pfds, fd_count, 1000);
        if (rc < 0) {
            if (errno != EINTR) log_line(LOG_FILE, "project select poll failed: %s", strerror(errno));
            continue;
        }
        if (rc == 0) continue;

        for (int i = 0; i < fd_count; i++) {
            if (!(pfds[i].revents & POLLIN)) continue;

            for (;;) {
                struct input_event ev;
                ssize_t n = read(fds[i], &ev, sizeof(ev));
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                if (n != sizeof(ev)) break;
                if (ev.type != EV_KEY || ev.value != 1) continue;
                if (!is_project_key(ev.code, cfg)) continue;

                if (ev.code == cfg->project_key_confirm || ev.code == KEY_ENTER || ev.code == KEY_KPENTER) {
                    int record_index = record_indexes[selected];
                    log_line(LOG_FILE, "project confirmed option=%d record=%d item=%s",
                        selected, record_index, projects[selected]);
                    close_input_fds(fds, fd_count);
                    free(projects);
                    free(record_indexes);
                    return record_index;
                }

                if (ev.code == cfg->project_key_prev || ev.code == KEY_UP || ev.code == KEY_LEFT) {
                    selected = (selected + count - 1) % count;
                } else if (ev.code == cfg->project_key_next || ev.code == KEY_DOWN || ev.code == KEY_RIGHT) {
                    selected = (selected + 1) % count;
                }

                log_line(LOG_FILE, "project selected index=%d item=%s key=%d",
                    selected, projects[selected], ev.code);
                publish_project_selection(scan, json_arr_get(records, record_indexes[selected]), selected, projects, count);
            }
        }
    }
}

/* 瑙勮寖鍖栨€у埆瀛楁銆? * 鎺ュ彛鍙兘杩斿洖 1/2銆丮/F銆乵ale/female锛岃繖閲岃浆鎹㈡垚妯℃澘鑳藉尮閰嶇殑鍊笺€? */
static const char *norm_sex(json_value_t *row) {
    const char *s = norm_field(row, "sex");
    if (!strcmp(s, "1") || !strcmp(s, "M") || !strcmp(s, "m") || !strcmp(s, "male") || !strcmp(s, "Male")) return "\xe7\x94\xb7";
    if (!strcmp(s, "2") || !strcmp(s, "F") || !strcmp(s, "f") || !strcmp(s, "female") || !strcmp(s, "Female")) return "\xe5\xa5\xb3";
    return s;
}

/* 鎶婃偅鑰呬俊鎭嫾杩涘姩鎬佸瓧绗︿覆 b锛屽舰鎴愪竴涓?JSON object銆? * 杩欓噷涓嶇洿鎺ュ啓鏂囦欢锛屾槸涓轰簡缁欏～琛ㄤ换鍔″拰绛夊緟鎵撳嵃浠诲姟澶嶇敤銆? */
static void append_patient_json(strbuf_t *b, json_value_t *row) {
    const char *keys[] = {"exam_item","his_exam_no","report_no","patient_id","patient_name","name_phonetic","xing","ming","age","nian","yue","ri","birthday"};
    sb_append(b, "{\n");
    for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); i++) {
        sb_printf(b, "    \"%s\": ", keys[i]);
        json_escape_append(b, norm_field(row, keys[i]));
        sb_append(b, ",\n");
    }
    sb_append(b, "    \"sex\": ");
    json_escape_append(b, norm_sex(row));
    sb_append(b, "\n  }");
}

/* 鏍规嵁妯℃澘涓殑涓€涓簨浠剁敓鎴愭渶缁堝～琛ㄤ簨浠躲€? *
 * clickType == 1锛氳緭鍏ユ浜嬩欢锛屾妸妯℃澘涓殑瀛楁鍚嶆浛鎹负鎮ｈ€呯湡瀹炲€笺€? * clickType == 7锛氭潯浠剁偣鍑讳簨浠讹紝渚嬪鏍规嵁鎮ｈ€?sex 鐐瑰嚮鐢?濂冲崟閫夋銆? */
static void append_event_json(strbuf_t *b, json_value_t *ev, json_value_t *row) {
    int ct = json_int(ev, "clickType", -1);
    const char *text = json_str(ev, "text", "");
    sb_append(b, "    {\n");
    sb_printf(b, "      \"index\": %d,\n      \"clickType\": %d,\n      \"x\": %d,\n      \"y\": %d",
        json_int(ev, "index", 0), ct, json_int(ev, "x", 0), json_int(ev, "y", 0));
    if (ct == 1 && *text) {
        sb_append(b, ",\n      \"field\": ");
        json_escape_append(b, text);
        sb_append(b, ",\n      \"text\": ");
        json_escape_append(b, norm_field(row, text));
    } else if (ct == 7 && *text) {
        const char *prefix = "鎬у埆:";
        const char *eq = strstr(text, prefix) == text ? text + strlen(prefix) : text;
        sb_append(b, ",\n      \"condition\": {\"field\": \"sex\", \"equals\": ");
        json_escape_append(b, eq);
        sb_append(b, "}");
    } else if (*text) {
        sb_append(b, ",\n      \"text\": ");
        json_escape_append(b, text);
    }
    sb_append(b, "\n    }");
}

/* 鐢熸垚濉〃闃熷垪鏂囦欢 form_xxx.json銆? * hid_executor 浼氫粠 form_queue 璇诲彇杩欎釜鏂囦欢锛岀劧鍚庢ā鎷熼紶鏍囬敭鐩樺～琛ㄣ€? */
static void write_form_task(const char *scan, json_value_t *row) {
    json_value_t *tmpl = json_parse_file(TEMPLATE_FILE);
    if (!tmpl) {
        log_line(LOG_FILE, "template not found or invalid: %s", TEMPLATE_FILE);
        return;
    }
    json_value_t *events = json_obj_get(tmpl, "eventClassList");
    long long ts = now_ms();
    strbuf_t b;
    sb_init(&b);
    sb_printf(&b, "{\n  \"id\": \"form_%lld\",\n  \"source\": \"scanner_api\",\n  \"action\": \"form_fill\",\n  \"scan_text\": ", ts);
    json_escape_append(&b, scan);
    sb_append(&b, ",\n  \"time\": ");
    json_escape_append(&b, now_text());
    sb_append(&b, ",\n  \"patient\": ");
    append_patient_json(&b, row);
    sb_append(&b, ",\n  \"title\": ");
    json_escape_append(&b, json_str(tmpl, "title", ""));
    sb_append(&b, ",\n  \"windowTitleLocation\": ");
    json_escape_append(&b, json_str(tmpl, "windowTitleLocation", ""));
    sb_append(&b, ",\n  \"eventClassList\": [\n");
    for (int i = 0; i < json_arr_size(events); i++) {
        if (i) sb_append(&b, ",\n");
        append_event_json(&b, json_arr_get(events, i), row);
    }
    sb_append(&b, "\n  ]\n}\n");
    char path[1024];
    snprintf(path, sizeof(path), "%s/form_%lld.json", FORM_QUEUE_DIR, ts);
    atomic_write_file(path, b.data, b.len);
    log_line(LOG_FILE, "form task saved: %s", path);
    sb_free(&b);
    json_free(tmpl);
}

/* 鍐欏綋鍓嶆偅鑰呯姸鎬佸拰绛夊緟鎵撳嵃浠诲姟銆? *
 * current_patient.json锛氭柟渚挎煡鐪嬪綋鍓嶆壂鐮佸搴旂殑鎮ｈ€呫€? * report_wait_queue/report_xxx.json锛氱粰 printer_capture 鍖归厤鍚庣画鎵撳嵃鎶ュ憡銆? */
static void write_state_files(const char *scan, json_value_t *row) {
    long long ts = now_ms();
    strbuf_t b;
    sb_init(&b);
    sb_append(&b, "{\n  \"scan_text\": ");
    json_escape_append(&b, scan);
    sb_append(&b, ",\n  \"time\": ");
    json_escape_append(&b, now_text());
    sb_append(&b, ",\n  \"patient\": ");
    append_patient_json(&b, row);
    sb_append(&b, ",\n  \"patient_id\": ");
    json_escape_append(&b, norm_field(row, "patient_id"));
    sb_append(&b, ",\n  \"report_no\": ");
    json_escape_append(&b, norm_field(row, "report_no"));
    sb_append(&b, ",\n  \"his_exam_no\": ");
    json_escape_append(&b, norm_field(row, "his_exam_no"));
    sb_append(&b, "\n}\n");
    atomic_write_file(CURRENT_PATIENT_FILE, b.data, b.len);
    log_line(LOG_FILE, "current patient state saved: %s", CURRENT_PATIENT_FILE);
    sb_free(&b);

    sb_init(&b);
    sb_printf(&b, "{\n  \"id\": \"report_%lld\",\n  \"source\": \"scanner_api\",\n  \"status\": \"waiting_print\",\n  \"scan_text\": ", ts);
    json_escape_append(&b, scan);
    sb_append(&b, ",\n  \"time\": ");
    json_escape_append(&b, now_text());
    sb_append(&b, ",\n  \"patient\": ");
    append_patient_json(&b, row);
    sb_append(&b, ",\n  \"patient_id\": ");
    json_escape_append(&b, norm_field(row, "patient_id"));
    sb_append(&b, ",\n  \"report_no\": ");
    json_escape_append(&b, norm_field(row, "report_no"));
    sb_append(&b, ",\n  \"his_exam_no\": ");
    json_escape_append(&b, norm_field(row, "his_exam_no"));
    sb_append(&b, "\n}\n");
    char report[1024];
    snprintf(report, sizeof(report), "%s/report_%lld.json", REPORT_WAIT_DIR, ts);
    atomic_write_file(report, b.data, b.len);
    log_line(LOG_FILE, "report wait task saved: %s", report);
    sb_free(&b);
}

/* 涓€娆℃壂鐮佸畬鎴愬悗鐨勪富涓氬姟娴佺▼銆? * 鎵爜鍐呭 -> 璇锋眰鎺ュ彛 -> 鍙栨偅鑰?-> 鐢熸垚濉〃浠诲姟 -> 鍐欑姸鎬?绛夊緟鎵撳嵃浠诲姟銆? */
static void handle_scan(const char *scan) {
    api_config_t cfg;
    api_config_load(&cfg);

    if (!scan_debounce_allows(scan)) {
        return;
    }

    log_line(LOG_FILE, "SCAN: %s", scan);
    save_scan_log(scan);
    ui_status_update("scan", "Scan received", "Querying patient API", scan, "", "");

    json_value_t *api = http_get_patient(scan);
    json_value_t *records = records_array(api);
    json_value_t *row = NULL;

    if (records && json_arr_size(records) > 0) {
        int selected = select_record_index(scan, records, &cfg);
        if (selected >= 0) row = json_arr_get(records, selected);
    } else {
        row = pick_first_record(api);
    }

    if (!row) {
        log_line(LOG_FILE, "ERROR handle scan failed: api returned no patient record");
        ui_status_update("error", "Query failed", "API returned no patient record", scan, "", "");
        sleep(3);
        ui_status_update("idle", "Waiting for scan", "Use barcode scanner to input patient ID", "", "", "");
        json_free(api);
        return;
    }

    log_line(LOG_FILE, "patient_id=%s patient_name=%s exam_item=%s sex=%s age=%s",
        norm_field(row, "patient_id"), norm_field(row, "patient_name"),
        norm_field(row, "exam_item"), norm_sex(row), norm_field(row, "age"));

    ui_status_update("ready_input", "Patient matched", "Starting HID auto input, do not use mouse or keyboard",
        scan, norm_field(row, "patient_id"), norm_field(row, "report_no"));
    write_form_task(scan, row);
    write_state_files(scan, row);
    json_free(api);
}

static int is_in(int code, const int *arr, size_t n) {
    for (size_t i = 0; i < n; i++) if (arr[i] == code) return 1;
    return 0;
}

/* 鏃х殑鍗曡澶囩洃鍚嚱鏁般€? * 褰撹缃?SCANNER_DEV=/dev/input/eventX 鏃讹紝鍙鍙栬繖涓澶囥€? */
static void read_scanner(const char *dev) {
    log_line(LOG_FILE, "listening on %s", dev);
    int fd = open(dev, O_RDONLY);
    if (fd < 0) return;
    strbuf_t buf;
    sb_init(&buf);
    int shift = 0;
    for (;;) {
        struct input_event ev;
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n == 0) break;
        if (n != sizeof(ev)) continue;
        if (ev.type != EV_KEY) continue;
        if (ev.code == KEY_CAPSLOCK) continue;
        if (is_in(ev.code, shift_codes, sizeof(shift_codes)/sizeof(shift_codes[0]))) {
            shift = ev.value != 0;
            continue;
        }
        if (ev.value != 1) continue;
        if (is_in(ev.code, enter_codes, sizeof(enter_codes)/sizeof(enter_codes[0]))) {
            if (buf.len) {
                handle_scan(buf.data);
                sb_free(&buf); sb_init(&buf);
            }
            continue;
        }
        const char *s = key_map(ev.code);
        if (!s) {
            log_line(LOG_FILE, "unknown key code: %d", ev.code);
            continue;
        }
        char c = s[0];
        if (shift) c = shift_char(c);
        sb_appendn(&buf, &c, 1);
    }
    sb_free(&buf);
    close(fd);
}

/* 涓€涓?input_reader_t 瀵瑰簲涓€涓?/dev/input/eventX銆? * 姣忎釜 event 閮芥湁鐙珛 fd銆佽矾寰勩€丼hift 鐘舵€佸拰杈撳叆缂撳啿鍖恒€? */
typedef struct {
    int fd;
    char path[128];
    int shift;
    strbuf_t buf;
} input_reader_t;

/* 鍒ゆ柇鏌愪釜 event 璺緞鏄惁宸茬粡琚姞鍏ョ洃鍚垪琛紝閬垮厤閲嶅 open銆?*/
static int reader_exists(input_reader_t *readers, int count, const char *path) {
    for (int i = 0; i < count; i++)
        if (!strcmp(readers[i].path, path)) return 1;
    return 0;
}

/* 鍏抽棴涓€涓?event 璁惧锛屽苟閲婃斁瀹冭嚜宸辩殑杈撳叆缂撳啿鍖恒€?*/
static void close_reader(input_reader_t *r) {
    if (r->fd >= 0) close(r->fd);
    r->fd = -1;
    sb_free(&r->buf);
}

/* 鎵弿 /dev/input锛屾妸鎵€鏈?event* 璁惧鍔犲叆鐩戝惉銆? * 杩欐牱鎵爜鏋彉鎴?event5銆乪vent8 鎴栭噸鎻掑悗鍙樻垚鍒殑缂栧彿锛岄兘鍙互鑷姩鎹曡幏銆? */
static void add_input_readers(input_reader_t *readers, int *count, int max_count) {
    DIR *d = opendir("/dev/input");
    if (!d) {
        log_line(LOG_FILE, "open /dev/input failed: %s", strerror(errno));
        return;
    }

    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        if (*count >= max_count) break;

        char path[128];
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        if (reader_exists(readers, *count, path)) continue;

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        input_reader_t *r = &readers[*count];
        r->fd = fd;
        snprintf(r->path, sizeof(r->path), "%s", path);
        r->shift = 0;
        sb_init(&r->buf);
        (*count)++;
        log_line(LOG_FILE, "input auto listen: %s", path);
    }

    closedir(d);
}

/* 澶勭悊鏌愪釜 event 璁惧璇诲埌鐨勪竴娆?input_event銆? *
 * 鍙鐞嗛敭鐩樹簨浠讹細
 * - Shift锛氭洿鏂板綋鍓嶈澶囩殑 Shift 鐘舵€併€? * - 鏅€氬瓧绗︼細杩藉姞鍒板綋鍓嶈澶囪嚜宸辩殑缂撳啿鍖恒€? * - Enter锛氳涓轰竴娆℃壂鐮佺粨鏉燂紝璋冪敤 handle_scan()銆? */
static void process_input_event(input_reader_t *r, struct input_event *ev, const api_config_t *cfg) {
    if (ev->type != EV_KEY) return;
    if (ev->code == KEY_CAPSLOCK) return;

    if (is_in(ev->code, shift_codes, sizeof(shift_codes)/sizeof(shift_codes[0]))) {
        r->shift = ev->value != 0;
        return;
    }

    if (ev->value != 1) return;

    if (cfg && is_navigation_project_key(ev->code, cfg)) {
        log_line(LOG_FILE, "ignore project navigation key in scan mode: code=%d dev=%s", ev->code, r->path);
        return;
    }

    if (is_in(ev->code, enter_codes, sizeof(enter_codes)/sizeof(enter_codes[0]))) {
        if (r->buf.len) {
            log_line(LOG_FILE, "scanner auto input from %s: %s", r->path, r->buf.data);
            handle_scan(r->buf.data);
            sb_free(&r->buf);
            sb_init(&r->buf);
        }
        return;
    }

    const char *s = key_map(ev->code);
    if (!s) return;

    char c = s[0];
    if (r->shift) c = shift_char(c);
    sb_appendn(&r->buf, &c, 1);

    if (r->buf.len > 256) {
        log_line(LOG_FILE, "drop overlong input buffer from %s", r->path);
        sb_free(&r->buf);
        sb_init(&r->buf);
    }
}

/* 榛樿鏍稿績鏂规锛氬悓鏃剁洃鍚墍鏈?/dev/input/event*銆? *
 * 杩欐瘮鈥滅寽鍝釜 event 鏄壂鐮佹灙鈥濇洿鍙潬锛? * 鐪熸鏈変竴涓插瓧绗﹀苟浠?Enter 缁撴潫鐨勮澶囷紝鎵嶄細瑙﹀彂鎵爜澶勭悊銆? */
static void read_all_input_events(void) {
    api_config_t cfg;
    api_config_load(&cfg);
    input_reader_t readers[64];
    int count = 0;
    memset(readers, 0, sizeof(readers));
    for (int i = 0; i < 64; i++) readers[i].fd = -1;

    log_line(LOG_FILE, "scanner auto mode: listening all /dev/input/event*");

    long long next_scan = 0;
    for (;;) {
        if (now_ms() >= next_scan) {
            add_input_readers(readers, &count, 64);
            next_scan = now_ms() + 2000;
        }

        if (count == 0) {
            log_line(LOG_FILE, "no /dev/input/event* found, retry in 2s");
            sleep(2);
            continue;
        }

        struct pollfd pfds[64];
        int pcount = 0;
        for (int i = 0; i < count; i++) {
            if (readers[i].fd < 0) continue;
            pfds[pcount].fd = readers[i].fd;
            pfds[pcount].events = POLLIN;
            pfds[pcount].revents = 0;
            pcount++;
        }

        int rc = poll(pfds, pcount, 1000);
        if (rc < 0) {
            if (errno != EINTR) log_line(LOG_FILE, "poll input failed: %s", strerror(errno));
            continue;
        }
        if (rc == 0) continue;

        int pi = 0;
        for (int i = 0; i < count; i++) {
            if (readers[i].fd < 0) continue;
            short revents = pfds[pi++].revents;
            if (!revents) continue;

            if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
                log_line(LOG_FILE, "input disconnected: %s", readers[i].path);
                close_reader(&readers[i]);
                continue;
            }

            if (revents & POLLIN) {
                for (;;) {
                    struct input_event ev;
                    ssize_t n = read(readers[i].fd, &ev, sizeof(ev));
                    if (n == sizeof(ev)) {
                        process_input_event(&readers[i], &ev, &cfg);
                    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        break;
                    } else if (n == 0) {
                        log_line(LOG_FILE, "input eof: %s", readers[i].path);
                        close_reader(&readers[i]);
                        break;
                    } else {
                        break;
                    }
                }
            }
        }
    }
}

/* 绋嬪簭鍏ュ彛銆? *
 * 榛樿涓嶈缃?SCANNER_DEV锛氱洃鍚叏閮?event銆? * 璁剧疆 SCANNER_DEV锛氬彧鐩戝惉鎸囧畾 event锛屼究浜庝复鏃惰皟璇曘€? */
int main(void) {
    ensure_dirs();
    api_config_t cfg;
    api_config_load(&cfg);
    log_line(LOG_FILE, "scan_patient_capture start");
    api_config_log(&cfg, LOG_FILE);
    ui_status_update("idle", "绛夊緟鎵爜", "璇蜂娇鐢ㄦ壂鐮佹灙鎵弿鎮ｈ€匢D", "", "", "");

    const char *forced_dev = getenv("SCANNER_DEV");
    if (!forced_dev || !*forced_dev) {
        read_all_input_events();
        return 0;
    }

    for (;;) {
        char dev[256];
        wait_scanner(dev, sizeof(dev));
        read_scanner(dev);
        log_line(LOG_FILE, "FATAL scanner error, exit for supervisor restart");
        return 1;
    }
}
