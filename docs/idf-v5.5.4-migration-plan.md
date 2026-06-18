# AIoT 墨水屏固件开发计划

## 当前状态

### 已完成
- [x] 全部外设组件代码（EPD/SHTC3/RTC/Touch/Battery/WiFi）
- [x] LVGL UI 四象限布局 + 数据更新 API
- [x] main.cpp 初始化流程 + 数据采集任务
- [x] CMakeLists.txt 依赖修复（esp_driver_* → driver，适配 v5.5.4）
- [x] PowerShell profile 路径修复（C:\Espressif → E:\Espressif，两个版本都改了）
- [x] eim_idf.json 路径修复

### 待完成
- [ ] 用 ESP-IDF v5.5.4 编译验证
- [ ] 烧录测试
- [ ] 更新 CLAUDE.md 中的 IDF 版本和工具路径
- [ ] WiFi 凭据配置（当前是默认占位符）

---

## 下一步操作

### 1. 编译验证

```powershell
. E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1
cd E:\aiot
idf.py build
```

如果编译报错，检查：
- `build/` 目录是否残留 v6.0.1 缓存 → 删除 `E:\aiot\build` 重来
- `managed_components/` 是否有 v6.0.1 产物 → 删除重来
- 组件名是否都改成了 `driver`（见下方文件清单）

### 2. 烧录

```powershell
idf.py -p COM5 flash
idf.py -p COM5 monitor
```

### 3. 更新 CLAUDE.md

把以下内容更新：
- `E:\esp\.espressif\v6.0.1\esp-idf` → `E:\esp\.espressif\v5.5.4\esp-idf`
- `C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1` → `E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1`
- 命令参考中的加载脚本路径同步更新

### 4. WiFi 配置

修改 `E:\aiot\sdkconfig.defaults`：
```
CONFIG_WIFI_SSID="你的WiFi名"
CONFIG_WIFI_PASSWORD="你的WiFi密码"
```

或在 menuconfig 中修改：
```powershell
idf.py menuconfig → Component config → WiFi BSP
```

---

## 已修改文件清单

### CMakeLists.txt（esp_driver_* → driver）

| 文件 | 旧值 | 新值 |
|------|------|------|
| `main/CMakeLists.txt` | `esp_driver_gpio esp_driver_spi` | `driver` |
| `components/epaper_driver_bsp/CMakeLists.txt` | `esp_driver_spi esp_driver_gpio` | `driver` |
| `components/i2c_bsp/CMakeLists.txt` | `esp_driver_i2c` | `driver` |
| `components/port_touch/CMakeLists.txt` | `esp_driver_gpio` | `driver` |
| `components/lvgl_port/CMakeLists.txt` | `esp_driver_gpio` | `driver` |

### 工具路径修复

| 文件 | 改动 |
|------|------|
| `E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1` | `C:\Espressif\tools` → `E:\Espressif\tools` |
| `E:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1` | `C:\Espressif\tools` → `E:\Espressif\tools` |
| `E:\Espressif\tools\eim_idf.json` | `C:\\Espressif\\tools` → `E:\\Espressif\\tools` |

### 源代码

**不需要改动** — 所有 `#include` 路径（`driver/gpio.h`、`driver/spi_master.h`、`driver/i2c_master.h`、`esp_adc/adc_oneshot.h`）在 v5.5.4 中完全兼容。

---

## API 兼容性确认

v5.5.4 中以下 API 全部可用：
- `driver/i2c_master.h` — `i2c_new_master_bus()`, `i2c_master_bus_add_device()` ✓
- `driver/spi_master.h` — SPI master API ✓
- `driver/gpio.h` — GPIO API ✓
- `esp_adc/adc_oneshot.h` — `adc_oneshot_new_unit()` ✓
- `esp_netif/esp_netif_sntp.h` — `esp_netif_sntp_init()` ✓
