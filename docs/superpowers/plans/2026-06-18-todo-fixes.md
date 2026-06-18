# TODO 功能修复 + 中文字体支持 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复 TODO 功能的 3 个上线问题（HTTP 404、Web "+" 不可见、lists 页无法返回）并接入 GB2312 中文字体。

**Architecture:** 4 个独立任务串行执行，每个任务范围聚焦单文件或单组件改动；按"先小后大、先后端后前端最后字体"的顺序，每完成一项 idf.py build → flash → monitor 验证一次再进下一项；不引入新框架。

**Tech Stack:** ESP-IDF v5.5.4, LVGL v9, esp_http_server, 思源黑体 SC, HTML/CSS/JS (嵌入式 gzip), bash 构建脚本。

## Global Constraints

- ESP-IDF v5.5.4，工具链在 E:\Espressif\tools
- Bash 工具调用 idf.py 必须用 env -u MSYSTEM powershell.exe 包装（见下方命令模板）
- 串口为 COM5，烧录命令 idf.py -p COM5 flash，监视 idf.py -p COM5 monitor
- 墨水屏 SSD1681 单色 200x200，PSRAM 已启用
- 触摸 IC FT6336 在屏幕边缘 1-2px 内识别不可靠
- LVGL 默认字体保留 Montserrat 14/20/28；中文字体新增 lv_font_chinese_14
- 源代码字符串保持 UTF-8 编码
- 项目当前无 git 仓库，"提交"步骤改为口述快照说明，由用户决定是否 git init/commit
- 每个任务完成后必须 idf.py build 通过，必要时烧录验证
- 不要改动任务范围外的无关代码

### idf.py 调用模板（Bash 工具）

    env -u MSYSTEM powershell.exe -ExecutionPolicy Bypass -Command \
      "[Environment]::SetEnvironmentVariable('MSYSTEM', $null, 'Process'); \
       . E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1; \
       cd E:\aiot; idf.py build"

烧录把末尾 idf.py build 换成 idf.py -p COM5 flash；监视换成 idf.py -p COM5 monitor。

---

## File Structure

### 修改

| 文件 | 改动概述 |
|---|---|
| components/todo_web/todo_web.cpp | TodoWeb_Start() 加 1 行 config.uri_match_fn |
| components/todo_ui/todo_ui.cpp | create_back_btn() 改尺寸/位置；两个 page create 函数末尾加 GESTURE_BUBBLE flag；6 处字体替换 |
| components/ui/ui_screen.c | 6 处字体替换 |
| components/lvgl_port/CMakeLists.txt | PRIV_REQUIRES 加 font_cn |
| web_dev/index.html | 拆分 "+" 为独立按钮 + CSS + JS 重绑 |
| components/todo_web/web_page.h | 由 scripts/build_web.sh 自动生成 |

### 新增

| 文件 | 责任 |
|---|---|
| components/font_cn/CMakeLists.txt | 字体组件构建脚本 |
| components/font_cn/font_cn.h | 暴露 extern const lv_font_t lv_font_chinese_14 |
| components/font_cn/lv_font_chinese_14.c | LVGL 字体生成产物（约 280 KB） |

---

## Task 1: HTTP 404 — 启用 uri 通配匹配

**Files:**
- Modify: components/todo_web/todo_web.cpp (TodoWeb_Start 函数, 第 395-398 行附近)

**Interfaces:**
- Consumes: httpd_uri_match_wildcard (声明在 esp_http_server.h，ESP-IDF 自带)
- Produces: 无新接口；修复 /api/* 路由匹配

- [ ] **Step 1: 定位 TodoWeb_Start 的 httpd_config_t 配置块**

打开 components/todo_web/todo_web.cpp，找到 TodoWeb_Start 函数（约第 390 行）。当前内容：

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 4;
    config.stack_size = 8192;

- [ ] **Step 2: 加入 uri_match_fn 字段**

将上面三行改为：

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 4;
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;

用 Edit 工具，old_string 为：

    config.max_uri_handlers = 4;
    config.stack_size = 8192;

new_string 为：

    config.max_uri_handlers = 4;
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;

- [ ] **Step 3: 构建**

运行 idf.py build（用上方调用模板）。
Expected: 构建成功，无 httpd_uri_match_wildcard 未定义错误。

- [ ] **Step 4: 烧录并验证**

运行 idf.py -p COM5 flash monitor。

验证步骤：
1. 长按 BOOT 2 秒开启 AP（串口出现 "AP started, connect to 'AIoT-TODO'"）
2. 手机连 WiFi "AIoT-TODO"
3. 手机浏览器访问 http://192.168.4.1/api/lists/l_001/tasks
4. 串口应出现：

       I (xxxx) todo_web: API: GET /api/lists/l_001/tasks len=0

   而不再出现：

       W (xxxx) httpd_uri: httpd_uri: URI '/api/lists/l_001/tasks' not found

Expected: 200 JSON 响应，串口无 404 警告。

- [ ] **Step 5: 快照说明**

本任务改动 1 行。记录："启用 esp_http_server 通配匹配，修复 /api/lists/{id}/tasks 404"。

---

## Task 2: lists 页返回 — 按钮放大 + 手势冒泡

**Files:**
- Modify: components/todo_ui/todo_ui.cpp (create_back_btn, create_lists_page, create_tasks_page)

**Interfaces:**
- Consumes: 无新依赖
- Produces: 无接口变化（仅行为修复）

- [ ] **Step 1: 放大返回按钮**

打开 components/todo_ui/todo_ui.cpp，找到 create_back_btn 函数（约第 42 行）。当前：

    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 28, 20);
    lv_obj_set_pos(btn, 2, 2);

用 Edit 改为：

    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 40, 32);
    lv_obj_set_pos(btn, 4, 4);

- [ ] **Step 2: 给 lists 页加手势冒泡**

找到 create_lists_page 函数末尾（约第 101 行，flex_flow 设置之后、函数结束之前）。当前末尾：

    lv_obj_set_style_pad_all(s_list_container, 2, 0);
    lv_obj_set_flex_flow(s_list_container, LV_FLEX_FLOW_COLUMN);
    }

用 Edit 在 flex_flow 行后、闭合大括号前插入：

    lv_obj_set_style_pad_all(s_list_container, 2, 0);
    lv_obj_set_flex_flow(s_list_container, LV_FLEX_FLOW_COLUMN);

    /* Let swipe gestures bubble up to the screen handler */
    lv_obj_add_flag(s_lists_page, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(s_list_container, LV_OBJ_FLAG_GESTURE_BUBBLE);
    }

- [ ] **Step 3: 给 tasks 页加手势冒泡**

找到 create_tasks_page 函数末尾（约第 128 行）。当前末尾：

    lv_obj_set_style_pad_all(s_task_container, 2, 0);
    lv_obj_set_flex_flow(s_task_container, LV_FLEX_FLOW_COLUMN);
    }

用 Edit 在 flex_flow 行后插入：

    lv_obj_set_style_pad_all(s_task_container, 2, 0);
    lv_obj_set_flex_flow(s_task_container, LV_FLEX_FLOW_COLUMN);

    /* Let swipe gestures bubble up to the screen handler */
    lv_obj_add_flag(s_tasks_page, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(s_task_container, LV_OBJ_FLAG_GESTURE_BUBBLE);
    }

- [ ] **Step 4: 构建**

运行 idf.py build。
Expected: 构建成功。

- [ ] **Step 5: 烧录并验证返回操作**

运行 idf.py -p COM5 flash monitor。

验证步骤：
1. 进入 TODO 页（点 dashboard 左下象限进入 lists 页）
2. 点左上 < 按钮 → 应回到 dashboard
3. 再进 lists 页，从屏幕左边缘往右划 → 应回到 dashboard
4. 进 tasks 页（点任一清单），点左上 < → 应回 lists 页
5. tasks 页从左→右划 → 应回 lists 页
6. tasks 页勾选任务 checkbox → 仍能正常切换完成状态（确认手势冒泡没破坏点击）

Expected: 4 种返回路径全部生效，checkbox 勾选不受影响。

- [ ] **Step 6: 快照说明**

记录："返回按钮放大到 40x32 并下移到 (4,4)；lists/tasks 页开启 GESTURE_BUBBLE，手势冒泡到 screen 处理器"。

---

## Task 3: Web "+" 可见 — 独立新建清单按钮

**Files:**
- Modify: web_dev/index.html
- Regenerate: components/todo_web/web_page.h (由脚本生成)

**Interfaces:**
- Consumes: POST /api/lists (Task 1 已修复可达)
- Produces: 无接口变化

- [ ] **Step 1: 加独立按钮的 CSS**

打开 web_dev/index.html，找到 .tab-add 样式行（约第 14 行）：

    .tab-add { background: #4CAF50; color: white; border-color: #4CAF50; }

在其后新增一行样式：

    .tab-add { background: #4CAF50; color: white; border-color: #4CAF50; }
    .add-list-btn { display: block; width: calc(100% - 16px); margin: 8px; padding: 12px; background: #4CAF50; color: white; border: none; border-radius: 8px; font-size: 16px; cursor: pointer; }

- [ ] **Step 2: 加独立按钮的 HTML**

找到 body 内 header 与 tabs 之间（约第 39-40 行）：

    <div class="header">AIoT 任务管理</div>
    <div class="tabs" id="tabs"></div>

改为：

    <div class="header">AIoT 任务管理</div>
    <button class="add-list-btn" id="addListBtn">+ 新建清单</button>
    <div class="tabs" id="tabs"></div>

- [ ] **Step 3: 删除 renderTabs 里拼接的 +**

找到 renderTabs 函数（约第 99 行）。当前：

    tabs.innerHTML = lists.map(l =>
      `<div class="tab ${l.id === currentListId ? 'active' : ''}" onclick="switchList('${l.id}')">${l.name}</div>`
    ).join('') + '<div class="tab tab-add" onclick="openListModal()">+</div>';

改为（去掉末尾 + 块）：

    tabs.innerHTML = lists.map(l =>
      `<div class="tab ${l.id === currentListId ? 'active' : ''}" onclick="switchList('${l.id}')">${l.name}</div>`
    ).join('');

- [ ] **Step 4: 绑定新按钮点击事件**

找到文件末尾的初始化区（约第 182 行，document.getElementById('addTaskBtn').onclick 之后）。当前：

    document.getElementById('addTaskBtn').onclick = openModal;

在其后加一行：

    document.getElementById('addTaskBtn').onclick = openModal;
    document.getElementById('addListBtn').onclick = openListModal;

- [ ] **Step 5: 重新生成 web_page.h**

运行（Bash 工具，普通 bash 即可，不需要 idf 环境）：

    bash scripts/build_web.sh

Expected: 输出 "Generated components/todo_web/web_page.h (xxx bytes -> yyy bytes gzip)"。

- [ ] **Step 6: 构建**

运行 idf.py build。
Expected: 构建成功，web_page.h 被编译进固件。

- [ ] **Step 7: 烧录并验证**

运行 idf.py -p COM5 flash monitor。

验证步骤：
1. 开 AP，手机连 "AIoT-TODO"
2. 浏览器访问 http://192.168.4.1
3. "+ 新建清单" 绿色按钮应在 header 下方、tabs 上方始终可见
4. 点 "+ 新建清单" → 弹出对话框 → 输入"测试" → 确定 → tabs 出现"测试"
5. 连续新建 6 个清单 → tabs 可横滚，"+ 新建清单"按钮不被遮挡

Expected: 按钮始终可见，新建清单功能正常。

- [ ] **Step 8: 快照说明**

记录："新建清单按钮从 tabs 末尾拆出为独立按钮放 header 下方，永远可见；重生成 web_page.h"。

---

## Task 4: 中文字体 — 接入 GB2312 lv_font_chinese_14

**Files:**
- Create: components/font_cn/CMakeLists.txt
- Create: components/font_cn/font_cn.h
- Create: components/font_cn/lv_font_chinese_14.c
- Modify: components/lvgl_port/CMakeLists.txt
- Modify: components/ui/ui_screen.c (6 处字体)
- Modify: components/todo_ui/todo_ui.cpp (6 处字体)

**Interfaces:**
- Consumes: lvgl (字体结构体 lv_font_t)
- Produces: extern const lv_font_t lv_font_chinese_14 (供 ui_screen.c / todo_ui.cpp 使用)

**前置说明（字体生成，本地完成）：**
字体文件 lv_font_chinese_14.c 用 lv_font_conv 本地生成。生成参数：
- 字体源：C:/Windows/Fonts/simhei.ttf（系统自带 SimHei 黑体）
- 字号：14px
- bpp：4
- 字符范围：0x20-0x7E (ASCII) + GB2312 一级字库 3755 字 + 中文标点补充
- 输出名：lv_font_chinese_14
- 生成产物约 200-300 KB，落盘到 components/font_cn/lv_font_chinese_14.c

- [ ] **Step 1: 创建 font_cn 组件目录与头文件**

用 Write 工具创建 components/font_cn/font_cn.h，内容：

    #ifndef FONT_CN_H
    #define FONT_CN_H

    #ifdef __cplusplus
    extern "C" {
    #endif

    #include "lvgl.h"

    extern const lv_font_t lv_font_chinese_14;

    #ifdef __cplusplus
    }
    #endif

    #endif /* FONT_CN_H */

- [ ] **Step 2: 创建 font_cn 的 CMakeLists.txt**

用 Write 创建 components/font_cn/CMakeLists.txt，内容：

    idf_component_register(
        SRCS "lv_font_chinese_14.c"
        INCLUDE_DIRS "."
        REQUIRES lvgl)

- [ ] **Step 3: 放入字体生成产物**

将在线生成的 lv_font_chinese_14.c 放入 components/font_cn/。
确认文件首部有 `const lv_font_t lv_font_chinese_14 = { ... };` 定义。

- [ ] **Step 4: lvgl_port 依赖 font_cn**

打开 components/lvgl_port/CMakeLists.txt。当前：

    idf_component_register(
        SRCS "lvgl_port.cpp"
        INCLUDE_DIRS "."
        PRIV_REQUIRES lvgl esp_timer epaper_driver_bsp driver port_touch)

改为（PRIV_REQUIRES 末尾加 font_cn）：

    idf_component_register(
        SRCS "lvgl_port.cpp"
        INCLUDE_DIRS "."
        PRIV_REQUIRES lvgl esp_timer epaper_driver_bsp driver port_touch font_cn)

注意：ui 与 todo_ui 组件也需要能链接到 font_cn。检查它们的 CMakeLists，若 PRIV_REQUIRES 不含 lvgl_port 或 font_cn，则补加 font_cn。先查看：

    cat components/ui/CMakeLists.txt
    cat components/todo_ui/CMakeLists.txt

若 ui 的 REQUIRES 含 lvgl_port 即可间接拿到（传递依赖），否则显式加 font_cn。todo_ui 同理。

- [ ] **Step 5: 替换 ui_screen.c 的 6 处字体**

打开 components/ui/ui_screen.c。在文件顶部 include 区加：

    #include "font_cn.h"

然后用 Edit 的 replace_all 把 6 处 `&lv_font_montserrat_14` 替换为 `&lv_font_chinese_14`：

old_string: `&lv_font_montserrat_14`
new_string: `&lv_font_chinese_14`
replace_all: true

注意：保留 `&lv_font_montserrat_20` 和 `&lv_font_montserrat_28` 不动（dashboard 大字时间数字仍用英文字体）。replace_all 只匹配精确字符串 `&lv_font_montserrat_14`，不会误伤 20/28。

确认替换后 grep 计数为 0：

    grep -c "lv_font_montserrat_14" components/ui/ui_screen.c
    Expected: 0

- [ ] **Step 6: 替换 todo_ui.cpp 的 6 处字体**

打开 components/todo_ui/todo_ui.cpp。在 include 区加：

    #include "font_cn.h"

同样用 Edit replace_all：

old_string: `&lv_font_montserrat_14`
new_string: `&lv_font_chinese_14`
replace_all: true

确认：

    grep -c "lv_font_montserrat_14" components/todo_ui/todo_ui.cpp
    Expected: 0

- [ ] **Step 7: 构建**

运行 idf.py build。
Expected: 构建成功，无 lv_font_chinese_14 未定义符号。

若链接报错 "section .flash.rodata will not fit"，说明 Flash 接近上限，执行 Step 7b。

- [ ] **Step 7b（仅当链接失败）: 扩大 app 分区**

打开 partitions.csv，把 app 分区扩大、缩小未用分区。当前典型布局 nvs/data/app，将 app 从 1.5MB 调到 1.9MB，相应缩小其它。改完重新 idf.py build。

- [ ] **Step 8: 烧录并验证中文显示**

运行 idf.py -p COM5 flash monitor。

验证步骤：
1. 开 AP，手机新建清单名"工作"、任务"写报告"
2. 进 TODO 页，清单名"工作"、任务"写报告"应正常显示中文，不是问号或方块
3. dashboard 上时间数字仍 Montserrat 28（大号英文数字），温湿度 Montserrat 20，WiFi/电池符号正常
4. 试一条含较生僻字的任务文本（如"预约"），常用字应正常显示

Expected: 中文字段正常显示，dashboard 英文大字不受影响。

- [ ] **Step 9: 快照说明**

记录："新增 font_cn 组件（思源黑体 SC 14px 4bpp GB2312 一级），ui_screen.c 与 todo_ui.cpp 共 12 处 montserrat_14 替换为 chinese_14；保留 20/28 英文字体"。

---

## Self-Review

**Spec coverage（规格 7 节对照）：**
- 第 2 节 HTTP 404 → Task 1 ✓
- 第 3 节 Web "+" → Task 3 ✓
- 第 4 节 lists 页返回 → Task 2 ✓
- 第 5 节 中文字体 → Task 4 ✓
- 第 6 节 实施顺序（404→返回→Web→字体）→ Task 1→2→3→4 ✓
- 第 7 节 风险（wildcard 语义/Flash 上限/手势冒泡影响点击/字体离线生成）→ Task 1 Step 4 验证根路径、Task 4 Step 7b 扩分区、Task 2 Step 5 验证 checkbox、Task 4 前置说明离线生成 ✓

**Placeholder scan:** 无 TBD/TODO/"appropriate error handling"/"similar to"。字体 .c 文件内容由在线工具生成，已在 Task 4 前置说明中明确"由人工完成"，非占位符。

**Type consistency:** lv_font_chinese_14 在 font_cn.h 声明为 `extern const lv_font_t`，Task 4 Step 5/6 替换引用统一用 `&lv_font_chinese_14`，类型一致。

**风险提示：**
- Task 4 Step 3 依赖在线字体生成，若执行环境无网络，此步需人工介入，其余 Step 可先空跑。
- 项目无 git，"快照说明"为口述，不执行 git commit。
