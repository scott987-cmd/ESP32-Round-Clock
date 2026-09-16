#!/usr/bin/env python3
import argparse
import glob
import struct
import time
import zlib
from pathlib import Path

import serial


MAGIC = b"RSC1"
HEADER = struct.Struct("<4sHHIII")
VIEWS = {
    "desktop": b"RCVIEW0\n",
    "clock": b"RCVIEW1\n",
    "weather": b"RCVIEW2\n",
    "settings": b"RCVIEW3\n",
    "wifi": b"RCVIEW4\n",
    "remote": b"RCVIEW5\n",
    "bluetooth": b"RCVIEW6\n",
    "avatar": b"RCVIEW7\n",
    "quota": b"RCQUOTA\n",
    "reset": b"RCRESET\n",
    "music": b"RCMUSIC\n",
    "radio": b"RCRADIO\n",
    "wallpaper-studio": b"RCWALL\n",
    "overview": b"RCAPPS\n",
    "agents": b"RCAGENTS\n",
    "notes": b"RCNOTES\n",
    "stars": b"RCSTARS\n",
    "english": b"RCENGLISH\n",
    "library": b"RCLIBRARY\n",
    "pet": b"RCPET\n",
    "story": b"RCSTORY\n",
    "flip": b"RCFLIP\n",
}


def read_exact(connection, length, timeout=20):
    deadline = time.monotonic() + timeout
    data = bytearray()
    while len(data) < length and time.monotonic() < deadline:
        data.extend(connection.read(length - len(data)))
    if len(data) != length:
        raise RuntimeError(f"截图数据不完整：{len(data)}/{length} 字节")
    return bytes(data)


def read_header(connection):
    deadline = time.monotonic() + 20
    prefix = bytearray()
    while time.monotonic() < deadline:
        byte = connection.read(1)
        if not byte:
            continue
        prefix.extend(byte)
        if prefix.endswith(b"RSE1"):
            raise RuntimeError("设备截图失败")
        if prefix.endswith(MAGIC):
            rest = read_exact(connection, HEADER.size - len(MAGIC))
            return HEADER.unpack(MAGIC + rest)
        if len(prefix) > 32:
            del prefix[:-4]
    raise RuntimeError("设备未返回截图响应")


def fnv1a(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def png_chunk(kind, data):
    payload = kind + data
    return struct.pack(">I", len(data)) + payload + struct.pack(">I", zlib.crc32(payload))


def write_png(path, pixels, width, height, stride):
    rows = bytearray()
    center_x = (width - 1) / 2
    center_y = (height - 1) / 2
    radius_squared = (min(width, height) / 2) ** 2
    for y in range(height):
        rows.append(0)
        offset = y * stride
        for x in range(width):
            if (x - center_x) ** 2 + (y - center_y) ** 2 > radius_squared:
                rows.extend((0, 0, 0))
            else:
                value = pixels[offset + x * 2] | (pixels[offset + x * 2 + 1] << 8)
                rows.extend(((value >> 11) * 255 // 31,
                             ((value >> 5) & 63) * 255 // 63,
                             (value & 31) * 255 // 31))
    png = bytearray(b"\x89PNG\r\n\x1a\n")
    png.extend(png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
    png.extend(png_chunk(b"IDAT", zlib.compress(bytes(rows), 9)))
    png.extend(png_chunk(b"IEND", b""))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


def main():
    parser = argparse.ArgumentParser(description="抓取圆屏设备当前 LVGL 画面")
    parser.add_argument("--port", help="设备串口；默认自动查找")
    parser.add_argument("--output", type=Path, help="PNG 输出路径")
    parser.add_argument("--view", choices=VIEWS, help="截图前自动切换到指定页面")
    parser.add_argument("--pair", action="store_true", help="通过 USB 调试通道启动蓝牙配对")
    parser.add_argument("--bottom", action="store_true", help="将可滚动页面移到底部")
    parser.add_argument("--top", action="store_true", help="将可滚动页面移到顶部")
    parser.add_argument("--arm-alert", action="store_true", help="写入旧信号 ID，验收自动预警")
    args = parser.parse_args()

    ports = [args.port] if args.port else sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        raise SystemExit("没有找到圆屏设备 USB 串口")
    output = args.output or Path("artifacts") / time.strftime("device-screen-%Y%m%d-%H%M%S.png")

    connection = serial.Serial()
    connection.port = ports[0]
    connection.baudrate = 115200
    connection.timeout = 0.25
    connection.dtr = True
    connection.rts = False
    connection.open()
    with connection:
        connection.reset_input_buffer()
        if args.view:
            connection.write(VIEWS[args.view])
            connection.flush()
            if read_exact(connection, 4, timeout=20) != b"RCOK":
                raise RuntimeError("设备拒绝切换页面")
            time.sleep(0.5)
        if args.pair:
            connection.write(b"RCPAIR\n")
            connection.flush()
            if read_exact(connection, 4, timeout=8) != b"RCOK":
                raise RuntimeError("设备拒绝启动蓝牙配对")
            time.sleep(0.5)
        if args.bottom or args.top:
            scroll_commands = {
                "desktop": (b"RCDBOT\n", b"RCDTOP\n"),
                "settings": (b"RCSBOT\n", b"RCSTOP\n"),
                "quota": (b"RCQBOT\n", b"RCQTOP\n"),
                "overview": (b"RCABOT\n", b"RCATOP\n"),
            }
            commands = scroll_commands.get(args.view)
            if commands is None:
                raise RuntimeError("该页面不支持滚动到底部")
            command = commands[0] if args.bottom else commands[1]
            connection.write(command)
            connection.flush()
            if read_exact(connection, 4, timeout=8) != b"RCOK":
                raise RuntimeError("设备拒绝滚动页面")
            time.sleep(0.5)
        if args.arm_alert:
            connection.write(b"RCALERT\n")
            connection.flush()
            if read_exact(connection, 4, timeout=8) != b"RCOK":
                raise RuntimeError("设备拒绝预警验收命令")
            time.sleep(8)
        connection.write(b"RCSHOT\n")
        connection.flush()
        _, width, height, stride, length, checksum = read_header(connection)
        pixels = read_exact(connection, length)

    if fnv1a(pixels) != checksum:
        raise SystemExit("截图校验失败，请重试")
    if stride < width * 2 or length != stride * height:
        raise SystemExit("设备返回了无效的截图尺寸")
    write_png(output, pixels, width, height, stride)
    print(output.resolve())


if __name__ == "__main__":
    main()
