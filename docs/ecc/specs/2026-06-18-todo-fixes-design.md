# TODO 功能修复 + 中文字体支持设计文档

**日期**: 2026-06-18
**状态**: 设计完成，待实现
**前置文档**: docs/ecc/specs/2026-06-18-todo-list-design.md

---

## 1. 概述

修复 TODO 功能上线后发现的 3 个问题，并补齐中文字体支持。一次合并提交，覆盖以下 4 件事：

1. **HTTP 404**：手机访问 /api/lists/{id}/tasks 全部返回 404，导致添加/查看任务失败
2. **Web "新建清单" 按钮不可见**：+ 被横向滚出屏幕外
3. **lists 页无法返回 dashboard**：左上 < 按钮命中率低，左→右滑手势也不生效
4. **中文显示**：LVGL 默认 Montserrat 不含 CJK，所有中文字段都是问号或方块

### 非目标

- 不重构 TODO UI 整体布局
- 不优化墨水屏刷新策略
- 不引入字体动态加载（字体直接编入固件）
- 不改 Web UI 整体视觉风格

---

## 2. 问题 1：HTTP 404 — uri 通配未启用

### 根因

esp_http_server 默认匹配函数是 httpd_uri_match_simple，做精确字符串比较。todo_web.cpp 第 410-420 行注册的 URI 是 /api/*，但默认匹配器把 * 当字面字符，所以 GET /api/lists/l_001/tasks 找不到任何处理器，落到 ESP-IDF 的 404 默认处理。

日志佐证：

    W (50518) httpd_uri: httpd_uri: URI '/api/lists/l_001/tasks' not found
    W (50518) httpd_txrx: httpd_resp_send_err: 404 Not Found - Nothing matches the given URI

### 修复

在 TodoWeb_Start() 的 httpd_config_t 配置中加一行：

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 4;
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;  // 新增

httpd_uri_match_wildcard 是 ESP-IDF 自带的通配匹配函数（声明在 esp_http_server.h），支持 * 匹配任意后缀，与现有路由模式（/api/*）契合。

### 影响范围

- 文件：components/todo_web/todo_web.cpp（1 行）
- 不影响已有的精确路由 /

### 验证

- 手机连 AP 后访问 http://192.168.4.1/api/lists → 200，返回清单数组
- 任意 GET /api/lists/{id}/tasks → 200，不再 404
- 提交 POST /api/lists/{id}/tasks 创建任务后串口出现 Task created: t_xxx in l_xxx

---

## 3. 问题 2：Web "+" 不可见 — 横滚把按钮藏掉了

### 根因

web_dev/index.html 的 .tabs 是 display:flex; overflow-x:auto。当清单数量超过容器宽度时，renderTabs() 拼接到末尾的 tab tab-add "+" 就被滚到右侧屏幕外。手机 viewport 上没明显滚动条提示，用户根本不知道要左滑才能看到 "+"。

### 修复

把"新建清单"按钮独立出来放在 .tabs 上方，永远可见。.tabs 区只放清单标签（保留横滚，清单多时仍可滑动选择）。

### Web UI 改动后的结构

    +----------------------+
    |  AIoT 任务管理         |
    +----------------------+
    |     [+ 新建清单]      |  <- 新增独立按钮
    +----------------------+
    |  [工作] [购物] [学习] |  <- tabs 仍可横滚（不再有 +）
    +----------------------+
    |   ... 任务列表 ...    |
    |     [+ 添加任务]      |
    +----------------------+

### 具体改动（web_dev/index.html）

- HTML：在 .header 和 .tabs 之间新增 <button class="add-list-btn">+ 新建清单</button>
- HTML：删除 renderTabs() 中拼接的 tab tab-add "+" 块
- CSS：新增 .add-list-btn，复用 .add-btn 样式（绿色、宽度满减边距）
- JS：openListModal() 绑定到新按钮（原 + 的 onclick 改为 document.getElementById('addListBtn').onclick）

### 影响范围

- 文件：web_dev/index.html（约 15 行 HTML/CSS/JS 改动）
- 重新跑 scripts/build_web.sh 生成 web_page.h
- todo_web.cpp 不动（仅 web_page.h 内容变化）

### 验证

- 手机连 AP 打开 http://192.168.4.1，"新建清单"按钮在 tabs 上方始终可见
- 点击"新建清单"弹出对话框，输入名称确定后清单出现在 tabs 中
- 6 个以上清单时 tabs 仍可横滚，"+"按钮不被遮挡

---

## 4. 问题 3：lists 页无法返回 dashboard

### 根因

返回回调注册链路正常（main.cpp:235 调 TodoUi_SetBackCallback(todo_back_cb)，todo_back_cb 调 ui_switch_page(PAGE_DASHBOARD)），所以问题在 LVGL 事件层：

1. 左上 < 返回按钮尺寸 28×20，位于 (2,2)，触摸 IC FT6336 在屏幕边缘 1-2px 内识别不可靠，加上按钮太小，命中率极低。
2. 手势事件 swipe_gesture_cb 绑在 s_todo_screen 上，但子对象 s_lists_page / s_list_container 默认不开启 GESTURE_BUBBLE（手势冒泡），手指在页面区划动时事件被子对象吞掉，冒不到 s_todo_screen。

注意：tasks 页能返回，是因为用户实际点的是左上 < 按钮（在该页命中率相对可用）或手势偶尔触发，并不是手势路径完全 OK。本修复同时加固两页。

### 修复（两层互为兜底）

1. **放大并下移返回按钮**：从 28×20 改为 40×32，位置 (4,4)。create_back_btn() 内 lv_obj_set_size / lv_obj_set_pos 改这两个值。视觉变化最小但命中率显著提升。

2. **开启手势冒泡**：在 create_lists_page() / create_tasks_page() 末尾给页面与列表容器加 GESTURE_BUBBLE：

       lv_obj_add_flag(s_lists_page, LV_OBJ_FLAG_GESTURE_BUBBLE);
       lv_obj_add_flag(s_list_container, LV_OBJ_FLAG_GESTURE_BUBBLE);
       lv_obj_add_flag(s_tasks_page, LV_OBJ_FLAG_GESTURE_BUBBLE);
       lv_obj_add_flag(s_task_container, LV_OBJ_FLAG_GESTURE_BUBBLE);

3. **手势方向逻辑保持现有**：LV_DIR_RIGHT（手指从左→右）= 返回。lists 页 → dashboard，tasks 页 → lists 页。swipe_gesture_cb 不改。

### 影响范围

- 文件：components/todo_ui/todo_ui.cpp（约 8 行）
- 不改 ui_screen.c、不改 main.cpp

### 验证

- lists 页点左上 < 按钮 → 回到 dashboard
- lists 页从屏幕左边缘往右划 → 回到 dashboard
- tasks 页点左上 < 按钮 → 回到 lists 页
- tasks 页从左→右划 → 回到 lists 页

---

## 5. 问题 4：中文显示 — 接入 GB2312 字体

### 根因

LVGL 默认字体 Montserrat 不含 CJK 字形。sdkconfig.defaults 只启用了 Montserrat 14/20/28。所有中文字段（清单名、任务文本、清单图标若是中文标点）渲染为问号或方块。Web 端不受影响（浏览器自带字体），问题仅在墨水屏 LVGL 侧。

### 字体方案

- **字体源**：思源黑体 SC Regular（Source Han Sans SC，Adobe+Google 联合开源，OFL 协议，墨水屏可读性最佳）
- **字符集**：
  - ASCII 0x20-0x7E（95 字）
  - GB2312 一级字库 3755 字（最常用汉字）
  - 中文标点补充约 30 字（、。"" ''《》（）【】…— 等）
  - 共约 3880 字
- **规格**：14px，4bpp 抗锯齿，输出文件约 280 KB
- **生成方式**：LVGL 在线字体转换器 https://lvgl.io/tools/fontconverter 一次性生成，无需本地装 Node

### 集成方式

新增组件 components/font_cn/：

    font_cn/
    ├── CMakeLists.txt
    ├── font_cn.h            // extern const lv_font_t lv_font_chinese_14;
    └── lv_font_chinese_14.c // 生成产物（约 280 KB）

- components/lvgl_port/CMakeLists.txt 加 REQUIRES font_cn
- ui_screen.c：9 处 lv_font_montserrat_14 替换为 lv_font_chinese_14；保留 lv_font_montserrat_20 / 28（dashboard 大字时间数字仍用英文字体，视觉更紧凑、省 Flash）
- todo_ui.cpp：6 处 lv_font_montserrat_14 全部替换为 lv_font_chinese_14
- 源代码字符串保持 UTF-8（C++17 默认支持，sdkconfig 编译选项不变）

### Flash 容量校验

- 当前 partitions.csv app 分区约 1.5 MB，Flash 4 MB
- 4bpp 字体约 280 KB，加进去 app 分区还有余量
- 若接近上限，调整 partitions.csv（扩大 app 分区，缩小未用分区）
- 字体是只读数据，仅占 Flash 不占 RAM

### 影响范围

- 新增：components/font_cn/（3 文件）
- 修改：components/lvgl_port/CMakeLists.txt、components/ui/ui_screen.c（9 处）、components/todo_ui/todo_ui.cpp（6 处）
- 不动：sdkconfig.defaults（字体编入组件，不走 Kconfig，避免误关）

### 验证

- 构建成功，无符号未定义
- 烧录后 dashboard：时间数字仍 Montserrat 28，温湿度数字 Montserrat 20，WiFi/电池符号正常
- todo 页：清单名、任务文本中文正常显示，不再是问号
- 单条任务文本含生僻字（如"龘"）→ 不在 GB2312 一级库，会显示问号，属预期行为（任务文本常用字覆盖率 99%+）

### 风险

- 字体文件 280 KB 是大块只读数据，编译时间增加几秒，运行时无影响
- 思源黑体 14px 在墨水屏上可读性需烧录后实物确认；若偏粗可改用更细字重或降字号到 13
- GB2312 一级字库不包含部分繁体/生僻字，任务文本遇此类字会显示问号；如后续有需求可扩到 GB2312 二级（再 +3008 字，+约 200 KB）

---

## 6. 实施顺序

1. **HTTP 通配符**（1 行，先解 404）— todo_web.cpp
2. **lists 页返回按钮 + 手势冒泡**（解第 3 痛点）— todo_ui.cpp
3. **Web + 重排**（HTML 改 + 重跑 build_web.sh）— web_dev/index.html
4. **中文字体**（最大块改动放最后）— 新增 font_cn 组件 + 替换字体引用

每步独立构建烧录验证，避免一次改太多难定位。

---

## 7. 风险汇总

| 风险 | 影响 | 缓解 |
|------|------|------|
| httpd_uri_match_wildcard 改变匹配语义 | 精确路由 / 仍命中（wildcard 优先精确） | 验证根路径仍返回网页 |
| 字体 Flash 占用接近上限 | 链接失败 | 构建后看 app 分区使用率，必要时调 partitions.csv |
| 手势冒泡影响子对象点击 | 理论上 checkbox 点击仍正常（LV_EVENT_VALUE_CHANGED 是点击不是手势） | 烧录后实测勾选任务仍可用 |
| 字体生成需在线工具 | 离线环境无法生成 | 生成产物提交仓库，后续不再依赖在线工具 |
