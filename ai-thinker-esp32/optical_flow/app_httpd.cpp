// ESP32-CAM：灰度帧简易稀疏 Lucas–Kanade 光流 + MJPEG 叠加显示
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "optical_flow_config.h"
#include "esp_heap_caps.h"
#include "img_converters.h"
#include "optical_flow.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_ARDUHAL_ESP_LOG)
#include "esp32-hal-log.h"
#else
#define log_i(...) \
  do { \
  } while (0)
#define log_e(...) \
  do { \
  } while (0)
#endif

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static httpd_handle_t s_httpd = NULL;
static httpd_handle_t s_streamd = NULL;

// OF_W / OF_H 见 optical_flow_config.h
// 奇数窗口，中心差分算 Ix,Iy 需距边界至少 HALF+1
static const int LK_WIN = 15;
static const int LK_HALF = LK_WIN / 2;
static const int LK_MARGIN = LK_HALF + 1;
// 2x2 系统可逆性：det(G) 过小则跳过（纹理弱/孔径问题）
static const float LK_MIN_DET = 500.0f;
// 单帧位移过大视为不可靠（可略调）
static const float LK_MAX_FLOW = 24.0f;

static uint8_t *s_prev_gray = NULL;
// 使用 RGB888 再 JPEG：避免 RGB565 小端/编码器解释不一致导致的绿、洋红伪色
static uint8_t *s_rgb888 = NULL;
static bool s_have_prev = false;

// 网页可调参数（由 /api/set 修改）
static volatile bool s_flow_overlay = true;
static volatile int s_jpeg_quality = 35;
static volatile int s_stride = 16;
static volatile int s_arrow_scale = 3;

static int clamp_i(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

// 亮度恒定：Ix*u + Iy*v = -It，窗口内最小二乘 -> 2x2 方程组
static bool lk_flow_at_point(const uint8_t *prev, const uint8_t *curr, int w, int h, int cx, int cy, float *out_u,
                             float *out_v) {
  float gxx = 0, gxy = 0, gyy = 0, gxt = 0, gyt = 0;
  for (int dy = -LK_HALF; dy <= LK_HALF; dy++) {
    int py = cy + dy;
    for (int dx = -LK_HALF; dx <= LK_HALF; dx++) {
      int px = cx + dx;
      float ix = 0.5f * ((float)prev[py * w + (px + 1)] - (float)prev[py * w + (px - 1)]);
      float iy = 0.5f * ((float)prev[(py + 1) * w + px] - (float)prev[(py - 1) * w + px]);
      float it = (float)curr[py * w + px] - (float)prev[py * w + px];
      gxx += ix * ix;
      gxy += ix * iy;
      gyy += iy * iy;
      gxt += ix * it;
      gyt += iy * it;
    }
  }
  float det = gxx * gyy - gxy * gxy;
  if (det < LK_MIN_DET) return false;
  float u = (-gxt * gyy + gyt * gxy) / det;
  float v = (gxy * gxt - gxx * gyt) / det;
  if (fabsf(u) > LK_MAX_FLOW || fabsf(v) > LK_MAX_FLOW) return false;
  *out_u = u;
  *out_v = v;
  return true;
}

static void rgb888_set(uint8_t *buf, int w, int h, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  if (x < 0 || y < 0 || x >= w || y >= h) return;
  size_t i = ((size_t)y * (size_t)w + (size_t)x) * 3;
  buf[i] = r;
  buf[i + 1] = g;
  buf[i + 2] = b;
}

static void draw_line_rgb888(uint8_t *buf, int w, int h, int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    rgb888_set(buf, w, h, x0, y0, r, g, b);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

static void gray_to_rgb888(const uint8_t *g, uint8_t *out, int n) {
  for (int i = 0; i < n; i++) {
    uint8_t v = g[i];
    out[i * 3 + 0] = v;
    out[i * 3 + 1] = v;
    out[i * 3 + 2] = v;
  }
}

static void compute_and_draw_flow(const uint8_t *prev, const uint8_t *curr, uint8_t *rgb888) {
  gray_to_rgb888(curr, rgb888, OF_W * OF_H);
  if (!s_flow_overlay) return;

  const float mag_min = 0.35f;
  const int stride = clamp_i((int)s_stride, 8, 32);
  const int ascale = clamp_i((int)s_arrow_scale, 1, 8);

  for (int cy = LK_MARGIN; cy < OF_H - LK_MARGIN; cy += stride) {
    for (int cx = LK_MARGIN; cx < OF_W - LK_MARGIN; cx += stride) {
      float u = 0, v = 0;
      if (!lk_flow_at_point(prev, curr, OF_W, OF_H, cx, cy, &u, &v)) continue;
      if (sqrtf(u * u + v * v) < mag_min) continue;
      int ex = (int)lrintf((float)cx + u * (float)ascale);
      int ey = (int)lrintf((float)cy + v * (float)ascale);
      if (ex < 0) ex = 0;
      if (ey < 0) ey = 0;
      if (ex >= OF_W) ex = OF_W - 1;
      if (ey >= OF_H) ey = OF_H - 1;
      draw_line_rgb888(rgb888, OF_W, OF_H, cx, cy, ex, ey, 0, 255, 0);
    }
  }
}

static esp_err_t api_status_handler(httpd_req_t *req) {
  char json[160];
  int n = snprintf(json, sizeof(json), "{\"flow\":%d,\"quality\":%d,\"stride\":%d,\"scale\":%d}", s_flow_overlay ? 1 : 0,
                   (int)s_jpeg_quality, (int)s_stride, (int)s_arrow_scale);
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, json, (size_t)n);
}

static esp_err_t api_set_handler(httpd_req_t *req) {
  size_t qlen = httpd_req_get_url_query_len(req);
  if (qlen == 0) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  char *buf = (char *)malloc(qlen + 1);
  if (!buf) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  if (httpd_req_get_url_query_str(req, buf, qlen + 1) != ESP_OK) {
    free(buf);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  char val[16];
  if (httpd_query_key_value(buf, "flow", val, sizeof(val)) == ESP_OK) {
    s_flow_overlay = (atoi(val) != 0);
  }
  if (httpd_query_key_value(buf, "quality", val, sizeof(val)) == ESP_OK) {
    s_jpeg_quality = clamp_i(atoi(val), 5, 90);
  }
  if (httpd_query_key_value(buf, "stride", val, sizeof(val)) == ESP_OK) {
    s_stride = clamp_i(atoi(val), 8, 32);
  }
  if (httpd_query_key_value(buf, "scale", val, sizeof(val)) == ESP_OK) {
    s_arrow_scale = clamp_i(atoi(val), 1, 8);
  }
  free(buf);

  char json[160];
  int n = snprintf(json, sizeof(json), "{\"ok\":1,\"flow\":%d,\"quality\":%d,\"stride\":%d,\"scale\":%d}",
                   s_flow_overlay ? 1 : 0, (int)s_jpeg_quality, (int)s_stride, (int)s_arrow_scale);
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, json, (size_t)n);
}

static esp_err_t index_handler(httpd_req_t *req) {
  static const char html[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,"
    "initial-scale=1\"><title>ESP32 光流</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;background:#121212;color:#e0e0e0;margin:0;padding:16px;max-width:720px;}"
    "h1{font-size:1.25rem;margin:0 0 8px;}"
    "p{color:#9e9e9e;font-size:.9rem;margin:0 0 16px;line-height:1.45;}"
    ".panel{background:#1e1e1e;border:1px solid #333;border-radius:10px;padding:14px 16px;margin-bottom:16px;}"
    ".row{display:flex;align-items:center;justify-content:space-between;gap:12px;margin:10px 0;flex-wrap:wrap;}"
    "label{font-size:.9rem;}"
    "input[type=range]{width:160px;vertical-align:middle;}"
    ".val{color:#8bc34a;min-width:2.5rem;display:inline-block;font-variant-numeric:tabular-nums;}"
    "button{background:#2d5a27;color:#fff;border:none;padding:8px 14px;border-radius:8px;cursor:pointer;font-size:0.9rem;}"
    "button:hover{filter:brightness(1.08);}"
    "button.secondary{background:#333;color:#ccc;}"
    "#stream{width:100%;max-height:70vh;border:1px solid #444;border-radius:8px;background:#000;}"
    ".hint{font-size:.75rem;color:#666;margin-top:8px;}"
    "</style></head><body>"
    "<h1>光流可视化</h1>"
    "<p>灰度 QVGA 320×240，稀疏 Lucas–Kanade（单层）。绿线为位移；卡顿可调大「网格步长」或关光流叠加。</p>"
    "<div class=\"panel\">"
    "<div class=\"row\"><label for=\"sw\">叠加光流矢量</label>"
    "<input type=\"checkbox\" id=\"sw\" checked /></div>"
    "<div class=\"row\"><label>JPEG 质量 <span class=\"val\" id=\"qv\"></span></label>"
    "<input type=\"range\" id=\"q\" min=\"5\" max=\"90\" value=\"35\" /></div>"
    "<div class=\"row\"><label>网格步长 <span class=\"val\" id=\"sv\"></span></label>"
    "<input type=\"range\" id=\"s\" min=\"8\" max=\"32\" step=\"4\" value=\"16\" /></div>"
    "<div class=\"row\"><label>箭头长度倍率 <span class=\"val\" id=\"av\"></span></label>"
    "<input type=\"range\" id=\"a\" min=\"1\" max=\"8\" value=\"3\" /></div>"
    "<div class=\"row\"><button type=\"button\" class=\"secondary\" id=\"reload\">刷新视频流</button>"
    "<span class=\"hint\">设置即时生效；视频在端口 81</span></div></div>"
    "<img id=\"stream\" alt=\"flow\"/>"
    "<script>"
    "(function(){"
    "var host=location.hostname;"
    "var streamUrl='http://'+host+':81/flow_stream';"
    "var img=document.getElementById('stream');"
    "function bust(){img.src=streamUrl+'?t='+Date.now();}"
    "function api(p){return fetch('/api/set?'+p).then(function(r){return r.json();});}"
    "function sync(){return fetch('/api/status').then(function(r){return r.json();}).then(function(j){"
    "document.getElementById('sw').checked=!!j.flow;"
    "document.getElementById('q').value=j.quality;document.getElementById('qv').textContent=j.quality;"
    "document.getElementById('s').value=j.stride;document.getElementById('sv').textContent=j.stride;"
    "document.getElementById('a').value=j.scale;document.getElementById('av').textContent=j.scale;"
    "});}"
    "document.getElementById('sw').onchange=function(){"
    "api('flow='+(this.checked?1:0));bust();};"
    "document.getElementById('q').oninput=function(){document.getElementById('qv').textContent=this.value;};"
    "document.getElementById('q').onchange=function(){api('quality='+this.value);bust();};"
    "document.getElementById('s').oninput=function(){document.getElementById('sv').textContent=this.value;};"
    "document.getElementById('s').onchange=function(){api('stride='+this.value);bust();};"
    "document.getElementById('a').oninput=function(){document.getElementById('av').textContent=this.value;};"
    "document.getElementById('a').onchange=function(){api('scale='+this.value);bust();};"
    "document.getElementById('reload').onclick=function(){bust();};"
    "sync().then(bust).catch(function(){bust();});"
    "})();"
    "</script></body></html>";
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, html, strlen(html));
}

static esp_err_t flow_stream_handler(httpd_req_t *req) {
  if (!s_prev_gray) {
    s_prev_gray = (uint8_t *)heap_caps_malloc(OF_W * OF_H, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_prev_gray) s_prev_gray = (uint8_t *)malloc(OF_W * OF_H);
  }
  if (!s_rgb888) {
    s_rgb888 = (uint8_t *)heap_caps_malloc((size_t)OF_W * OF_H * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rgb888) s_rgb888 = (uint8_t *)malloc((size_t)OF_W * OF_H * 3);
  }
  if (!s_prev_gray || !s_rgb888) {
    log_e("alloc failed");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  esp_err_t res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  uint8_t *jpg = NULL;
  size_t jpg_len = 0;

  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      log_e("capture failed");
      res = ESP_FAIL;
      break;
    }
    if (fb->format != PIXFORMAT_GRAYSCALE || fb->width != OF_W || fb->height != OF_H) {
      esp_camera_fb_return(fb);
      res = ESP_FAIL;
      break;
    }

    const uint8_t *curr = fb->buf;
    if (s_have_prev) {
      compute_and_draw_flow(s_prev_gray, curr, s_rgb888);
    } else {
      gray_to_rgb888(curr, s_rgb888, OF_W * OF_H);
    }

    memcpy(s_prev_gray, curr, (size_t)OF_W * OF_H);
    s_have_prev = true;
    esp_camera_fb_return(fb);

    if (jpg) {
      free(jpg);
      jpg = NULL;
    }

    camera_fb_t enc = {};
    enc.width = OF_W;
    enc.height = OF_H;
    enc.format = PIXFORMAT_RGB888;
    enc.buf = s_rgb888;
    enc.len = (size_t)OF_W * OF_H * 3;
    int jq = clamp_i((int)s_jpeg_quality, 5, 90);
    if (!frame2jpg(&enc, (uint8_t)jq, &jpg, &jpg_len)) {
      log_e("frame2jpg failed");
      res = ESP_FAIL;
      break;
    }

    res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
    if (res != ESP_OK) break;
    char hdr[96];
    size_t hlen = snprintf(hdr, sizeof(hdr), _STREAM_PART, (unsigned)jpg_len);
    res = httpd_resp_send_chunk(req, hdr, hlen);
    if (res != ESP_OK) break;
    res = httpd_resp_send_chunk(req, (const char *)jpg, jpg_len);
    if (res != ESP_OK) break;
  }
  if (jpg) free(jpg);
  return res;
}

void startFlowServer() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.max_uri_handlers = 12;

  if (httpd_start(&s_httpd, &cfg) == ESP_OK) {
    httpd_uri_t u = {.uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL};
    httpd_register_uri_handler(s_httpd, &u);
    httpd_uri_t u_st = {.uri = "/api/status", .method = HTTP_GET, .handler = api_status_handler, .user_ctx = NULL};
    httpd_register_uri_handler(s_httpd, &u_st);
    httpd_uri_t u_set = {.uri = "/api/set", .method = HTTP_GET, .handler = api_set_handler, .user_ctx = NULL};
    httpd_register_uri_handler(s_httpd, &u_set);
  }

  cfg.server_port += 1;
  cfg.ctrl_port += 1;
  if (httpd_start(&s_streamd, &cfg) == ESP_OK) {
    httpd_uri_t u = {.uri = "/flow_stream", .method = HTTP_GET, .handler = flow_stream_handler, .user_ctx = NULL};
    httpd_register_uri_handler(s_streamd, &u);
  }
}
