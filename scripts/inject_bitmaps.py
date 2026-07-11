"""把 bmp_*_192.txt 的字模数据替换进 pet_ui.c 的对应状态分支。

用法: python inject_bitmaps.py
"""
import re

PET_UI = r"E:\aiot\components\pet_ui\pet_ui.c"

# (状态名, 字模文件) — 必须与 pet_ui.h 的枚举顺序一致
SLOTS = [
    ("HAPPY",    r"E:\aiot\bmp_happy_192.txt"),
    ("BUSY",     r"E:\aiot\bmp_busy_192.txt"),
    ("ANXIOUS",  r"E:\aiot\bmp_anxious_192.txt"),
    ("FOCUS",    r"E:\aiot\bmp_focus_192.txt"),
    ("LOW_BATT", r"E:\aiot\bmp_low_batt_192.txt"),
    ("SLEEP",    r"E:\aiot\bmp_sleep_192.txt"),
]


def load_array_body(path):
    """读 txt，返回去除注释/空行后的纯数组体（含缩进和逗号结尾的行）"""
    lines = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for ln in f:
            ln = ln.rstrip("\n")
            if ln.lstrip().startswith("/*") or ln.lstrip().startswith("#"):
                continue
            if ln.strip() == "":
                continue
            lines.append(ln)
    return "\n".join(lines)


def replace_block(src, label, new_body):
    """替换 src 中 '/* LABEL */' 后的 { ... } 块内容为 new_body。

    支持两种块形态：
      1) 多行块:  /* LABEL */ \n { \n ... \n },
      2) 单行块:  /* LABEL */ { 0 },
    """
    pat_comment = re.compile(rf"/\*\s*{re.escape(label)}\s*\*/")
    m = pat_comment.search(src)
    if not m:
        raise RuntimeError(f"找不到 /* {label} */ 注释")
    brace_start = src.find("{", m.end())
    if brace_start < 0:
        raise RuntimeError(f"{label}: 找不到 '{{'")
    # 找块结束：'},' 或 '}\n};'
    end1 = src.find("},", brace_start)
    end2 = src.find("}\n};", brace_start)
    if end1 < 0:
        end = end2
    elif end2 < 0:
        end = end1
    else:
        end = min(end1, end2)
    if end < 0:
        raise RuntimeError(f"{label}: 找不到块结束")
    new_block = "\n" + new_body + "\n"
    return src[:brace_start + 1] + new_block + src[end:]


def main():
    with open(PET_UI, "r", encoding="utf-8") as f:
        src = f.read()

    for label, path in SLOTS:
        body = load_array_body(path)
        src = replace_block(src, label, body)
        rows = len([ln for ln in body.splitlines() if ln.strip()])
        print(f"injected {label}: {rows} rows")

    with open(PET_UI, "w", encoding="utf-8") as f:
        f.write(src)
    print("done")


if __name__ == "__main__":
    main()
