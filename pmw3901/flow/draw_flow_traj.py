#!/usr/bin/env python3
"""
PMW3901 光流轨迹：两种模式（由参数自动选择）
  - 指定串口 --port → 模式 1：每行 t,dx,dy（t 毫秒），窗口内实时累加轨迹；关闭窗口或 Ctrl+C 后保存 CSV
  - 未指定 --port → 模式 2：从 CSV 文件读数据并画轨迹（位置参数 FILE 或 --csv）

累加：X = sum(dx), Y = sum(dy)（与传感器计数一致，无物理标定）
运行示例：

- 采集串口数据并实时显示轨迹：
  
      python3 draw_flow_traj.py --port /dev/ttyUSB0

- 从 CSV 文件回放轨迹：

      python3 draw_flow_traj.py recording.csv

- 或使用 --csv 参数指定 CSV 文件：

      python3 draw_flow_traj.py --csv recording.csv

- 指定波特率和输出文件：

      python3 draw_flow_traj.py --port /dev/ttyUSB0 --baud 115200 --output flow_20230517.csv --png traj.png

"""
from __future__ import annotations

import argparse
import csv
import sys
from datetime import datetime
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

try:
    import serial
except ImportError:
    serial = None


def parse_line(line: str):
    line = line.strip()
    if not line:
        return None
    parts = line.split(",")
    if len(parts) != 3:
        return None
    try:
        t, dx, dy = float(parts[0]), float(parts[1]), float(parts[2])
        return (t, dx, dy)
    except ValueError:
        return None


def rows_to_cumulative(rows: list[tuple[float, float, float]]):
    """累加位移 -> 轨迹坐标 (px, py)，与 t 无关，仅按采样顺序累加。"""
    if not rows:
        return np.array([]), np.array([])
    dx = np.array([r[1] for r in rows], dtype=float)
    dy = np.array([r[2] for r in rows], dtype=float)
    x = np.cumsum(dx)
    y = np.cumsum(dy)
    return x, y


def save_csv(path: Path, rows: list[tuple[float, float, float]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["t_ms", "dx", "dy"])
        for t, dx, dy in rows:
            w.writerow([t, dx, dy])
    print(f"已保存: {path}（{len(rows)} 行）", file=sys.stderr)


def load_csv(path: Path) -> list[tuple[float, float, float]]:
    rows: list[tuple[float, float, float]] = []
    with path.open(newline="", encoding="utf-8") as f:
        for row in csv.reader(f):
            if len(row) < 3:
                continue
            try:
                t, dx, dy = float(row[0]), float(row[1]), float(row[2])
            except ValueError:
                continue
            rows.append((t, dx, dy))
    return rows


def _autoscale_equal_aspect(ax, x: np.ndarray, y: np.ndarray) -> None:
    """Fit square viewport around trajectory while keeping equal aspect."""
    if len(x) == 0:
        return
    xmin, xmax = float(np.min(x)), float(np.max(x))
    ymin, ymax = float(np.min(y)), float(np.max(y))
    xc = 0.5 * (xmin + xmax)
    yc = 0.5 * (ymin + ymax)
    half = max(xmax - xmin, ymax - ymin, 1e-9) * 0.5
    half *= 1.1
    ax.set_xlim(xc - half, xc + half)
    ax.set_ylim(yc - half, yc + half)


def plot_trajectory(
    x: np.ndarray,
    y: np.ndarray,
    title: str = "PMW3901 cumulative trajectory",
    save_fig: Path | None = None,
) -> None:
    fig, ax = plt.subplots(figsize=(7, 7))
    ax.plot(x, y, "-", lw=1.2, color="C0")
    if len(x) > 0:
        ax.scatter([x[0]], [y[0]], c="green", s=40, zorder=5, label="Start")
        ax.scatter([x[-1]], [y[-1]], c="red", s=40, zorder=5, label="End")
    ax.set_aspect("equal", adjustable="box")
    ax.set_xlabel("Cumulative X (counts)")
    ax.set_ylabel("Cumulative Y (counts)")
    ax.set_title(title)
    ax.grid(True, alpha=0.3)
    ax.legend(loc="best")
    plt.tight_layout()
    if save_fig:
        fig.savefig(save_fig, dpi=150)
        print(f"图像已保存: {save_fig}", file=sys.stderr)
    plt.show()


def run_serial(port: str, baud: int, out_csv: Path | None, out_png: Path | None) -> int:
    if serial is None:
        print("请安装: pip install pyserial", file=sys.stderr)
        return 1

    if out_csv is None:
        out_csv = Path(f"flow_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv")

    rows: list[tuple[float, float, float]] = []
    ser = serial.Serial(port, baud, timeout=0.3)
    print(
        f"串口 {port} @ {baud}，实时轨迹窗口已打开；关闭窗口或 Ctrl+C 停止并保存 CSV",
        file=sys.stderr,
    )

    plt.ion()
    fig, ax = plt.subplots(figsize=(7, 7))
    (line,) = ax.plot([], [], "-", lw=1.2, color="C0")
    sc_start = ax.scatter([np.nan], [np.nan], c="green", s=40, zorder=5, label="Start")
    sc_end = ax.scatter([np.nan], [np.nan], c="red", s=40, zorder=5, label="End")
    ax.set_aspect("equal", adjustable="box")
    ax.set_xlabel("Cumulative X (counts)")
    ax.set_ylabel("Cumulative Y (counts)")
    ax.set_title("Serial capture | live")
    ax.grid(True, alpha=0.3)
    ax.legend(loc="best")
    plt.tight_layout()

    cx, cy = 0.0, 0.0
    xs: list[float] = []
    ys: list[float] = []
    running = True

    def on_close(_):
        nonlocal running
        running = False

    fig.canvas.mpl_connect("close_event", on_close)

    try:
        ser.reset_input_buffer()
        while running:
            raw = ser.readline()
            if not raw:
                fig.canvas.flush_events()
                plt.pause(0.01)
                continue
            line_s = raw.decode("utf-8", errors="ignore")
            p = parse_line(line_s)
            if p is None:
                continue
            rows.append(p)
            _t, dx, dy = p
            cx += dx
            cy += dy
            xs.append(cx)
            ys.append(cy)
            xa = np.asarray(xs, dtype=float)
            ya = np.asarray(ys, dtype=float)
            line.set_data(xa, ya)
            sc_start.set_offsets([[xa[0], ya[0]]])
            sc_end.set_offsets([[xa[-1], ya[-1]]])
            ax.set_title(f"Serial capture | N={len(rows)} (live)")
            _autoscale_equal_aspect(ax, xa, ya)
            fig.canvas.draw_idle()
            fig.canvas.flush_events()
            plt.pause(0.001)
    except KeyboardInterrupt:
        print("\n结束采集", file=sys.stderr)
    finally:
        ser.close()

    if not rows:
        plt.close(fig)
        print("未采集到有效数据（需 t,dx,dy 格式）", file=sys.stderr)
        return 1

    save_csv(out_csv, rows)
    xa = np.asarray(xs, dtype=float)
    ya = np.asarray(ys, dtype=float)
    ax.set_title(f"Serial capture | N={len(rows)}")
    fig.canvas.draw_idle()
    if out_png:
        fig.savefig(out_png, dpi=150)
        print(f"图像已保存: {out_png}", file=sys.stderr)
    plt.ioff()
    plt.show(block=True)
    return 0


def run_csv(csv_path: Path, out_png: Path | None) -> int:
    if not csv_path.is_file():
        print(f"文件不存在: {csv_path}", file=sys.stderr)
        return 1
    rows = load_csv(csv_path)
    if not rows:
        print("CSV 无有效数据行", file=sys.stderr)
        return 1
    x, y = rows_to_cumulative(rows)
    plot_trajectory(x, y, title=f"{csv_path.name} | N={len(rows)}", save_fig=out_png)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="PMW3901 光流：有 --port 则串口采集，否则从 CSV 回放轨迹",
        epilog=(
            "示例：  %(prog)s --port /dev/ttyUSB0\n"
            "        %(prog)s recording.csv\n"
            "        %(prog)s --csv recording.csv"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--port",
        metavar="DEVICE",
        default=None,
        help="串口设备（如 /dev/ttyUSB0）；指定则采集模式",
    )
    parser.add_argument(
        "csv",
        nargs="?",
        type=Path,
        metavar="FILE",
        help="回放模式：CSV 路径（无 --port 时必填，与 --csv 二选一即可）",
    )
    parser.add_argument(
        "--csv",
        dest="csv_opt",
        type=Path,
        metavar="FILE",
        help="同位置参数 FILE",
    )
    parser.add_argument("--baud", type=int, default=115200, help="波特率（仅串口）")
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        help="串口模式：保存 CSV 的路径（默认 flow_时间戳.csv）",
    )
    parser.add_argument(
        "--png",
        type=Path,
        metavar="FILE",
        help="同时将轨迹图保存为 PNG",
    )

    args = parser.parse_args()

    if args.port is not None:
        return run_serial(args.port, args.baud, args.output, args.png)

    csv_path = args.csv_opt if args.csv_opt is not None else args.csv
    if csv_path is not None:
        return run_csv(csv_path, args.png)

    parser.error("回放模式请提供 CSV：位置参数 FILE 或 --csv FILE")


if __name__ == "__main__":
    raise SystemExit(main())
