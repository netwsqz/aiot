## Task 6: ui 组件改动 - 页面路由

**状态:** 完成 (ui_screen.c 编译通过; 整体构建失败来自其他组件，非本任务引起)

---

### 改动内容

#### `components/ui/ui.h`
- 在 `#endif` 之前添加了 `ui_page_t` 枚举（PAGE_DASHBOARD, PAGE_TODO_LISTS, PAGE_TODO_TASKS）
- 添加了 `ui_switch_page()` 和 `ui_get_current_page()` 函数声明

#### `components/ui/ui_screen.c`
- 添加 `#include "todo_ui.h"`
- 添加静态变量 `s_current_page`（初始化为 PAGE_DASHBOARD）和 `s_dashboard_screen`
- 在 `ui_init()` 中保存 `ui.screen` 到 `s_dashboard_screen`
- 在文件末尾添加 `ui_switch_page()` 和 `ui_get_current_page()` 函数

#### `components/ui/CMakeLists.txt`
- 在 `PRIV_REQUIRES` 中添加 `todo_ui` 依赖（传递头文件搜索路径）

---

### 构建验证

- `ui_screen.c` 编译成功，无警告无错误
- `idf.py build` 整体失败，但失败来自其他组件（task 1-5 的代码）：
  - `todo_manager.cpp`: 缺少 `esp_timer.h`（CMakeLists 未依赖 esp_timer）
  - `wifi_ap.cpp`: 缺少 `esp_timer.h`（同上）
  - `todo_ui.cpp`: void* 到 lv_obj_t* 隐式转换错误 + `lv_font_montserrat_12` 未声明
- 这些错误属于 Task 1-5 的范畴，不在本任务范围内
