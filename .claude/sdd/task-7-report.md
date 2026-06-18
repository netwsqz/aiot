# Task 7: main.cpp 集成报告

**状态:** ✅ 编译通过

## 完成内容

按 brief 步骤修改了 2 个文件，修复了 3 个编译错误：

1. **sdkconfig.defaults** — 添加了 WiFi AP Support 和 HTTP Server 配置
2. **main.cpp** — 添加了 4 个 include、boot_key_task 函数、4 个初始化调用、TODO UI 定时刷新
3. **main/CMakeLists.txt** — 添加了 4 个新组件依赖

## 修复的编译问题

1. **gpio_num_t 类型转换** — `gpio_get_level(0)` → `gpio_get_level((gpio_num_t)0)`
2. **web_page_gz 链接错误** — todo_web.cpp 改为 `#include "web_page.h"` 替代 extern 声明
3. **DRAM 溢出（14008 字节）** — 三步修复：
   - `sdkconfig.defaults` 添加 `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`
   - data_task 栈从 8KB 减为 7KB
   - `TODO_MAX_TASKS` 从 100 减为 32（`todo_manager.h`）

## 启动时序

```
Power → I2C → EPD → LVGL → SHTC3+RTC → WiFi+NTP → Touch+Battery → UI
→ data_task(Core 0) → TodoMgr → TodoUi → WiFi AP + TodoWeb → boot_key_task(Core 1)
```

## 运行时行为

- **BOOT 键长按 2 秒** → 开启/关闭 WiFi AP "AIoT-TODO" + Web Server
- **每 30 秒** → data_task 自动刷新 TODO UI
