import serial
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation

PORT = "/dev/ttyUSB0"  # Linux 常见
BAUD = 115200

COLS = 32
ROWS = 24
PIXELS = COLS * ROWS

# 当前帧数据
frame_data = np.zeros((ROWS, COLS), dtype=float)

# 打开串口
ser = serial.Serial(PORT, BAUD, timeout=1)

# 创建图像
fig, ax = plt.subplots()
im = ax.imshow(
    frame_data,
    cmap="jet",
    interpolation="bicubic",
    origin="lower",
    vmin=20,
    vmax=40,
)
cbar = plt.colorbar(im, ax=ax)
cbar.set_label("Temperature (°C)")
ax.set_title("MLX90640 Thermal Image")

def read_frame():
    """从串口读取一帧 32x24 温度数据"""
    while True:
        line = ser.readline().decode("utf-8", errors="ignore").strip()

        if not line or line in ("START", "ERR"):
            continue

        parts = line.split(",")
        if len(parts) != PIXELS:
            continue

        try:
            values = np.array([float(x) for x in parts], dtype=float)
            return values.reshape((ROWS, COLS))
        except ValueError:
            continue

def update(_):
    global frame_data

    new_frame = read_frame()
    frame_data = new_frame

    # 自动缩放颜色范围
    im.set_data(frame_data)
    im.set_clim(vmin=np.min(frame_data), vmax=np.max(frame_data))

    min_temp = np.min(frame_data)
    max_temp = np.max(frame_data)
    ax.set_title(f"MLX90640 Thermal Image | Min: {min_temp:.1f} C  Max: {max_temp:.1f} C")

    return [im]

ani = FuncAnimation(fig, update, interval=50, blit=False, cache_frame_data=False)
plt.show()

ser.close()