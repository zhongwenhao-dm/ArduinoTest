#pragma once
#include "esp_camera.h"

/**
 * 分辨率只在这里改一处：
 * - OPTICAL_FLOW_FRAMESIZE：传给 camera_config_t.frame_size（见 esp_camera.h 里 framesize_t）
 * - OF_W / OF_H：须与该档实际输出一致（QVGA 一般为 320×240，以你传感器为准）
 *
 * 参考：QQVGA≈160×120 | QVGA≈320×240 | HQVGA≈240×176
 */
#define OPTICAL_FLOW_FRAMESIZE FRAMESIZE_QVGA
#define OF_W 320
#define OF_H 240
