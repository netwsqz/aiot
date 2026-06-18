## Task 4: 嵌入式网页开发 -- 完成

**状态:** 编译通过

### 创建的文件

- `web_dev/index.html` -- 手机端 SPA，纯原生 JS，响应式布局
- `scripts/build_web.sh` -- 构建脚本，gzip 压缩 HTML 并生成 C 头文件
- `components/todo_web/web_page.h` -- 生成的 gzip 压缩 HTML 数据（2424 bytes）

### 修改的文件

- `components/todo_web/todo_web.cpp` -- 新增：
  - `handle_root()` 路由处理器（gzip 编码返回嵌入式网页）
  - `web_page_gz` / `web_page_gz_len` extern 声明
  - `/` 路由注册（在其他路由之前）

### Web UI 功能

- 标签页切换清单
- 任务列表：点击勾选、删除
- 添加任务弹窗（支持优先级和截止日期）
- 新建/删除清单
- 所有操作通过 `fetch()` 调用 REST API

### 构建说明

`scripts/build_web.sh` 修复了原始 brief 中的两个问题：
1. **sed 变量名污染** -- xxd 输出的临时路径被编码进变量名，导致 sed 全局替换产生无效 C 代码。改用临时目录 + 手动拼接的方式生成干净的头文件。
2. **逗号缺失** -- grep 提取的 hex 行尾缺少逗号，导致 C 数组初始化语法错误。在每行末尾追加逗号。

### 验证

- `idf.py build` 编译通过
- binary size: 0x122870 bytes（与 Task 3 相同，因为 `web_page.h` 是 include 而非源文件重编译）
- `web_page_gz_len = 2424` 字节（gzip 压缩，原 HTML 7706 字节）

### 新增路由端点

| 方法 | 路径 | 功能 |
|------|------|------|
| GET | / | 返回 gzip 压缩的嵌入式网页 |

### 注意事项

- 每次修改 `web_dev/index.html` 后需重新运行 `bash scripts/build_web.sh` 再编译
- 网页通过 `Content-Encoding: gzip` 头告诉浏览器自动解压
- 手机连接 ESP32-S3 热点后访问 `http://192.168.4.1/` 即可使用
