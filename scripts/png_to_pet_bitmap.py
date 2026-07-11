"""
把 PNG 转成 NxN 1bpp C 数组，供 pet_ui.c 的 s_pet_bitmap 使用。

用法:
  python png_to_pet_bitmap.py <src.png> <LABEL> [SIZE]
默认 SIZE=48。SIZE=192 时输出 192x192 字模供桌宠占满屏使用。

输出格式：行优先，每行 (SIZE+7)//8 字节，MSB=最左像素，1=黑前景，0=白背景。
不通过 Read 读图片，纯命令行处理。
"""
import sys
from PIL import Image

SRC = sys.argv[1] if len(sys.argv) > 1 else r"E:\aiot\gpt-image-20260706_220318-1.png"
LABEL = sys.argv[2] if len(sys.argv) > 2 else "HAPPY"
SIZE = int(sys.argv[3]) if len(sys.argv) > 3 else 48
W = H = SIZE
ROW_BYTES = (W + 7) // 8


def main():
    img = Image.open(SRC)
    print(f"# 原图: {img.size} mode={img.mode}", file=sys.stderr)

    # 转 RGBA 再转灰度，统一处理透明背景
    if img.mode not in ("L", "RGB", "RGBA"):
        img = img.convert("RGBA")
    if img.mode == "RGBA":
        # 透明像素按白处理，避免 alpha 混到前景
        bg = Image.new("RGBA", img.size, (255, 255, 255, 255))
        bg.alpha_composite(img)
        img = bg.convert("L")
    elif img.mode == "RGB":
        img = img.convert("L")
    else:
        img = img.convert("L")

    # 缩到 SIZE x SIZE（用 LANCZOS 保细节）
    img = img.resize((W, H), Image.LANCZOS)

    px = img.load()
    out = bytearray(W * H // 8)

    for y in range(H):
        for x in range(W):
            v = px[x, y]  # 0=黑 255=白
            if v < 128:  # 暗 -> 黑前景 -> 1
                byte_idx = y * ROW_BYTES + (x >> 3)
                bit = 0x80 >> (x & 7)
                out[byte_idx] |= bit

    # 输出 C 数组
    print(f"/* {LABEL} - {W}x{H} 1bpp, from {SRC} */")
    lines = []
    for y in range(H):
        row = out[y * ROW_BYTES:(y + 1) * ROW_BYTES]
        hexs = ", ".join(f"0x{b:02X}" for b in row)
        lines.append(f"    {hexs},")
    for i in range(0, len(lines), 6):
        print("\n".join(lines[i:i + 6]))

    print(f"\n# total bytes: {len(out)} (expect {W*H//8})", file=sys.stderr)


if __name__ == "__main__":
    main()
