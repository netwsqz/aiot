#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
HTML_FILE="$PROJECT_DIR/web_dev/index.html"
OUTPUT="$PROJECT_DIR/components/todo_web/web_page.h"

if [ ! -f "$HTML_FILE" ]; then
  echo "Error: $HTML_FILE not found"
  exit 1
fi

# gzip 压缩到临时目录（避免 /tmp 路径被 xxd 编码进变量名）
TMPDIR_BUILD=$(mktemp -d)
trap 'rm -rf "$TMPDIR_BUILD"' EXIT

gzip -c "$HTML_FILE" > "$TMPDIR_BUILD/web_page.bin"

# 用 xxd 生成 hex dump，然后提取纯 hex 行
xxd -i "$TMPDIR_BUILD/web_page.bin" > "$TMPDIR_BUILD/raw.txt"

# 提取行内 hex 值（去掉地址和 ASCII 列）
HEX_LINES=$(grep -oP '0x[0-9a-f]{2}(,\s*0x[0-9a-f]{2})*' "$TMPDIR_BUILD/raw.txt")

# 统计字节数
GZ_SIZE=$(wc -c < "$TMPDIR_BUILD/web_page.bin")

# 生成干净的头文件
cat > "$OUTPUT" << 'HEADER_TOP'
#ifndef WEB_PAGE_H
#define WEB_PAGE_H

#include <stdint.h>

const uint8_t web_page_gz[] = {
HEADER_TOP

# 写入 hex 数据，每行 12 字节（确保逗号分隔正确）
echo "$HEX_LINES" | while IFS= read -r line; do
  # 每行最后一个字节后必须有逗号（xxd 输出的行末没有）
  echo "  $line,"
done >> "$OUTPUT"

cat >> "$OUTPUT" << 'HEADER_BOTTOM'
};
const unsigned int web_page_gz_len = GZSIZE_PLACEHOLDER;

#endif
HEADER_BOTTOM

# 替换占位符为实际大小
sed -i "s/GZSIZE_PLACEHOLDER/$GZ_SIZE/" "$OUTPUT"

echo "Generated $OUTPUT ($(wc -c < "$HTML_FILE") bytes -> $GZ_SIZE bytes gzip)"
