#include <WiFi.h>
#include <WebServer.h>
#include "Bitcraze_PMW3901.h"

// =========================
// WiFi 配置
// =========================
const char* ssid     = "DEEP-RD";
const char* password = "07310731";

// =========================
// PMW3901 配置
// =========================
Bitcraze_PMW3901 flow(5);

// 35 x 35 = 1225
uint8_t frame[35 * 35];

// =========================
// Web 服务器
// =========================
WebServer server(80);

// =========================
// 主页 HTML
// =========================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width,initial-scale=1.0">
  <title>PMW3901 35x35 Viewer</title>
  <style>
    body {
      margin: 0;
      padding: 20px;
      background: #0b0b0b;
      color: #f0f0f0;
      font-family: Arial, sans-serif;
      box-sizing: border-box;
    }

    h1 {
      margin: 0 0 8px 0;
      font-size: 22px;
    }

    .sub {
      color: #b8b8b8;
      font-size: 14px;
      margin-bottom: 18px;
    }

    .wrap {
      display: flex;
      gap: 18px;
      align-items: flex-start;
      flex-wrap: wrap;
    }

    .panel {
      background: #151515;
      border: 1px solid #333;
      border-radius: 12px;
      padding: 14px;
      box-sizing: border-box;
    }

    .panel h2 {
      margin: 0 0 10px 0;
      font-size: 18px;
      color: #ffffff;
    }

    canvas {
      width: 420px;
      height: 420px;
      background: #000;
      border: 1px solid #555;
      image-rendering: pixelated;
      image-rendering: crisp-edges;
      display: block;
    }

    .leftPanel {
      width: 440px;
    }

    .rightPanel {
      min-width: 560px;
      max-width: 900px;
      max-height: 700px;
      overflow: auto;
    }

    .status {
      margin-top: 12px;
      color: #8df58d;
      font-size: 14px;
      word-break: break-all;
    }

    .stats {
      margin-top: 8px;
      color: #d0d0d0;
      font-size: 13px;
    }

    button {
      margin-top: 12px;
      padding: 8px 14px;
      border: none;
      border-radius: 8px;
      cursor: pointer;
      font-size: 14px;
    }

    pre {
      margin: 0;
      color: #8df58d;
      font-size: 12px;
      line-height: 1.35;
      white-space: pre;
      font-family: Consolas, Menlo, Monaco, monospace;
    }
  </style>
</head>
<body>
  <h1>PMW3901 实时图像</h1>
  <div class="sub">左侧显示 35×35 灰度图，右侧显示最新一帧 35×35 数值矩阵</div>

  <div class="wrap">
    <div class="panel leftPanel">
      <h2>图像</h2>
      <canvas id="canvas" width="35" height="35"></canvas>
      <div class="status" id="status">连接中...</div>
      <div class="stats" id="stats">尚未收到有效帧</div>
      <button id="toggleBtn">暂停</button>
    </div>

    <div class="panel rightPanel">
      <h2>最新矩阵数值</h2>
      <pre id="matrixText">尚未收到有效 1225 字节帧</pre>
    </div>
  </div>

  <script>
    const W = 35;
    const H = 35;
    const FRAME_LEN = W * H;

    const canvas = document.getElementById("canvas");
    const ctx = canvas.getContext("2d");
    const statusEl = document.getElementById("status");
    const statsEl = document.getElementById("stats");
    const matrixText = document.getElementById("matrixText");
    const toggleBtn = document.getElementById("toggleBtn");

    let running = true;
    let lastTime = performance.now();

    function pad3(n) {
      return String(n).padStart(3, ' ');
    }

    function drawFrame(bytes) {
      const img = ctx.createImageData(W, H);
      for (let i = 0; i < bytes.length; i++) {
        const v = bytes[i];
        const p = i * 4;
        img.data[p]     = v;
        img.data[p + 1] = v;
        img.data[p + 2] = v;
        img.data[p + 3] = 255;
      }
      ctx.putImageData(img, 0, 0);
    }

    function renderMatrix(bytes) {
      let lines = [];
      for (let y = 0; y < H; y++) {
        let row = [];
        for (let x = 0; x < W; x++) {
          row.push(pad3(bytes[y * W + x]));
        }
        lines.push(row.join(' '));
      }
      matrixText.textContent = lines.join('\n');
    }

    function calcStats(bytes) {
      let min = 255;
      let max = 0;
      let sum = 0;

      for (let i = 0; i < bytes.length; i++) {
        const v = bytes[i];
        if (v < min) min = v;
        if (v > max) max = v;
        sum += v;
      }

      const avg = sum / bytes.length;
      return { min, max, avg };
    }

    async function updateFrame() {
      if (!running) return;

      try {
        const res = await fetch("/frame?t=" + Date.now(), {
          cache: "no-store"
        });

        if (!res.ok) {
          statusEl.textContent = "HTTP 错误: " + res.status;
          setTimeout(updateFrame, 500);
          return;
        }

        const buf = await res.arrayBuffer();
        const bytes = new Uint8Array(buf);

        if (bytes.length !== FRAME_LEN) {
          statusEl.textContent = "收到长度错误: " + bytes.length + "，应为 " + FRAME_LEN;
          statsEl.textContent = "当前帧无效";
          matrixText.textContent = "收到长度错误: " + bytes.length + "，应为 " + FRAME_LEN;
          setTimeout(updateFrame, 1000);
          return;
        }

        drawFrame(bytes);
        renderMatrix(bytes);

        const st = calcStats(bytes);
        const now = performance.now();
        const fps = 1000 / Math.max(1, now - lastTime);
        lastTime = now;

        statusEl.textContent = "运行中，约 " + fps.toFixed(1) + " FPS，长度 " + bytes.length;
        statsEl.textContent =
          "min=" + st.min +
          "  max=" + st.max +
          "  avg=" + st.avg.toFixed(2);

        setTimeout(updateFrame, 80);
      } catch (e) {
        statusEl.textContent = "连接失败: " + e;
        statsEl.textContent = "等待重连";
        setTimeout(updateFrame, 1000);
      }
    }

    toggleBtn.addEventListener("click", () => {
      running = !running;
      toggleBtn.textContent = running ? "暂停" : "继续";
      statusEl.textContent = running ? "恢复中..." : "已暂停";
      if (running) {
        lastTime = performance.now();
        updateFrame();
      }
    });

    updateFrame();
  </script>
</body>
</html>
)rawliteral";

// =========================
// 采集一帧
// =========================
void captureFrame() {
  flow.readFrameBuffer((char*)frame);
}

// =========================
// HTTP 路由
// =========================
void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

void handleFrame() {
  captureFrame();

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
  server.setContentLength(sizeof(frame));
  server.send(200, "application/octet-stream", "");

  WiFiClient client = server.client();
  client.write((const uint8_t*)frame, sizeof(frame));
}

void handleDebug() {
  captureFrame();

  uint8_t minV = 255;
  uint8_t maxV = 0;
  uint32_t sum = 0;

  for (int i = 0; i < 35 * 35; i++) {
    uint8_t v = frame[i];
    if (v < minV) minV = v;
    if (v > maxV) maxV = v;
    sum += v;
  }

  float avg = sum / 1225.0f;

  String s;
  s.reserve(1024);
  s += "len=1225\n";
  s += "min=" + String(minV) + "\n";
  s += "max=" + String(maxV) + "\n";
  s += "avg=" + String(avg, 2) + "\n";
  s += "first20=";
  for (int i = 0; i < 20; i++) {
    s += String(frame[i]);
    if (i != 19) s += ",";
  }
  s += "\n";

  server.send(200, "text/plain; charset=utf-8", s);
}

void handleNotFound() {
  server.send(404, "text/plain; charset=utf-8", "404 Not Found");
}

// =========================
// setup
// =========================
void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("Booting...");

  if (!flow.begin()) {
    Serial.println("Initialization of the flow sensor failed");
    while (1) {
      delay(1000);
    }
  }

  flow.enableFrameBuffer();
  Serial.println("PMW3901 framebuffer init OK");

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("WiFi connected, IP = ");
  Serial.println(WiFi.localIP());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/frame", HTTP_GET, handleFrame);
  server.on("/debug", HTTP_GET, handleDebug);
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("HTTP server started");
  Serial.println("Open browser:");
  Serial.print("http://");
  Serial.println(WiFi.localIP());
  Serial.println("Debug URL:");
  Serial.print("http://");
  Serial.print(WiFi.localIP());
  Serial.println("/debug");
}

// =========================
// loop
// =========================
void loop() {
  server.handleClient();
}
