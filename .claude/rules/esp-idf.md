# ESP-IDF 项目规则（E:/aiot）

ESP32-S3 + ESP-IDF v5.5.4 墨水屏固件。本文件取代原 ECC 规则集（`.claude/rules/ecc/`，2026-09-19 移除），
只保留对本项目实际生效的条款；ECC 里面向 Web/OOP/主机测试的通用样板已丢弃。

C 为主（`*_bsp`、`port_*`、各 application 组件），仅 `main/main.cpp` 和少数组件用 C++。

## 命名与风格

- 函数 `snake_case`，类型 `PascalCase`，常量 `UPPER_SNAKE_CASE`
- 组件公开 API 带前缀：`TodoMgr_*`、`TimerEngine_*`
- 文件静态变量 `s_` 前缀
- 函数 <50 行，文件 <800 行，嵌套 ≤4 层（优先 early return）
- 不强制 clang-format / clang-tidy，靠 `-Wall -Werror` 编译器警告

## 错误处理与内存

- 每层显式处理 `esp_err_t`，禁止静默吞错；用 `ESP_ERROR_CHECK` 的地方要确认失败即致命
- C 文件用 `heap_caps_malloc()` / `free()`，不用 `new`/`delete`
- `snprintf()` / `strlcpy()`，不用 `strcpy`/`strcat`/`sprintf`
- 可选值用 out 参数 + 返回 `bool`/`esp_err_t`，不用 `std::optional`
- 互斥用 `xSemaphoreCreateMutex()` + `xSemaphoreTake`/`Give`，不用 `std::mutex`
- 资源管理靠 init/deinit 显式配对（如 `wifi_ap_init` / `wifi_ap_deinit`），C 无 RAII
- 大数组不要放栈上，走 heap/PSRAM
- I2C/SPI 操作必须带超时，防死锁

## ESP-IDF C 惯用法

- C/C++ 互操作用 `extern "C"` 头文件
- NVS 持久化：namespace + 单 blob/record，参考 `TodoMgr_*` 和 `TimerEngine_Persist`/`Restore`
- LVGL 事件 `user_data` 必须指向长生命周期内存 —— 用静态池（如 `todo_ui.cpp` 的 `s_list_id_pool`），不能用栈数组

## 安全

- 无硬编码密钥：WiFi 密码 / API Key 放 Kconfig 或 `sdkconfig.defaults`，不进源码；暴露后立即轮换
- NVS blob 读写必须校验长度字段，防越界
- HTTP 端点（`todo_web`）外部输入必须校验后再使用
- SPIFFS/LittleFS 路径需处理
- `ESP_LOG` 不打印 password / token

## 构建与验证

无主机单元测试框架 —— 硬件依赖（I2C/SPI/墨水屏/音频/触摸）无法在主机 mock，覆盖率与 TDD 循环不适用。
**验证 = `idf.py build` 编译通过 + `idf.py flash` + 设备实测。**

- 构建用 `idf.py build`（不是 `cmake --build`）
- 烧录 `idf.py -p COM5 flash`，大小检查 `idf.py size`
- **必须在 Git Bash 里通过 PowerShell 调用 `idf.py`** —— ESP-IDF v5.5.4 拒绝 MSys，需先清 `MSYSTEM`。
  完整命令模板见 `CLAUDE.md` 的 Build & Flash 段
- PC LVGL 仿真（`tools/pc_sim/`）是唯一的主机侧检查，且只覆盖 UI 逻辑
  （页面切换、布局、回调）；它走主机 gcc + CMake + Ninja，**不要**套上面的 PowerShell/MSYSTEM 流程
- 未来若提取出可主机测试的组件，对该组件恢复 TDD + 80% 覆盖率要求

## 资源预算

- 关注 `idf.py size`：PSRAM / Flash / 栈用量。DIRAM 约 85% 满，加大缓冲区前先复测
- FreeRTOS 任务栈按需设定，不浪费

## 开发流程

1. **Research**: 动手前先查现有实现/库，能复用/移植就不新写
2. **Plan**: 复杂功能先出方案。`docs/ecc/` 有匹配的 spec/plan 时直接用
3. **实现**: 小步迭代，每步可编译验证
4. **Review**: 写完代码走一遍下面的审查清单
5. **Commit**: conventional commits —— `feat` / `fix` / `refactor` / `docs` / `test` / `chore` / `perf` / `ci`

## 审查清单

- [ ] 可读且命名清晰
- [ ] 函数 <50 行，文件 <800 行
- [ ] 错误显式处理，无静默吞错
- [ ] 无硬编码密钥
- [ ] 外部输入（HTTP / NVS）已校验
- [ ] 大缓冲区/任务栈改动已用 `idf.py size` 复测

严重度：**CRITICAL**（安全漏洞/数据丢失）阻止合并 · **HIGH**（Bug/质量问题）修后再合并 ·
**MEDIUM**（可维护性）建议修 · **LOW**（风格）可选。

## 回归预防

`docs/ecc/` 的设计文档是行为规格。改功能时同步更新对应 spec，或在
`docs/ecc/specs/CHANGELOG.md` 记录偏离（见 `docs/NEXT-STEPS-2026-07.md`）。
