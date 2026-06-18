# AIoT - ESP32-S3 1.54" 墨水屏 固件项目

## 开发环境

ESP-IDF v5.5.4，安装在 `E:\esp\.espressif\v5.5.4\esp-idf`

**重要：Claude Code 调用 idf.py 的方式**
- Bash 工具运行在 Git Bash (MSys) 环境，ESP-IDF v5.5.4 拒绝 MSys
- 必须通过 `env -u MSYSTEM` 去除 MSys 环境变量后调用 powershell.exe：
  ```bash
  env -u MSYSTEM powershell.exe -ExecutionPolicy Bypass -Command \
    "[Environment]::SetEnvironmentVariable('MSYSTEM', \$null, 'Process'); \
     . E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1; \
     cd E:\aiot; idf.py build"
  ```
- 用户在 Windows Terminal 里直接用 PowerShell 加载 profile 即可

## 硬件平台

微雪 ESP32-S3 1.54" 墨水屏 AIoT 开发板

### GPIO 分配

| GPIO | 功能 | 备注 |
|------|------|------|
| IO0  | BOOT 按键 | 低有效，带内部上拉 |
| IO3  | LED | |
| IO4  | 电池 ADC | ADC1_CH3, 分压比 2:1 |
| IO5  | RTC 闹钟中断 | 深睡眠唤醒源 |
| IO6  | EPD 电源使能 (EPD3V3_EN) | **低有效** |
| IO7  | 触摸屏复位 (EPD_TP_RST) | |
| IO8  | 墨水屏忙信号 (EPD_BUSY) | 输入 |
| IO9  | 墨水屏复位 (EPD_RST) | |
| IO10 | 墨水屏 D/C | |
| IO11 | 墨水屏 SPI CS | 软件手动控制 |
| IO12 | 墨水屏 SPI SCLK | |
| IO13 | 墨水屏 SPI SDI (MOSI) | |
| IO14 | I2S MCLK | |
| IO15 | I2S SCLK | |
| IO16 | I2S ASDOUT | |
| IO17 | 电池电源控制 (VBAT_PWR) | **高有效** |
| IO18 | PWR 按键 / PA EN | 低有效，深睡眠唤醒源 |
| IO21 | 触摸屏中断 (EPD_TP_INT) | 下降沿 |
| IO38 | I2S LRCK | |
| IO39 | SD 卡 CLK | SDMMC 1-bit |
| IO40 | SD 卡 MISO (D0) | |
| IO41 | SD 卡 MOSI (CMD) | |
| IO42 | 音频功放电源 (PA EN) | **低有效** |
| IO43 | UART TX | |
| IO44 | UART RX | |
| IO45 | I2S DSDIN | |
| IO46 | PA CTRL | |
| IO47 | I2C SDA | 触摸屏/RTC/SHTC3 共用 |
| IO48 | I2C SCL | 触摸屏/RTC/SHTC3 共用 |

### 外设

- **墨水屏**：1.54" V2, SSD1681 驱动, 200x200, SPI 接口, SPI2_HOST, 40MHz
- **温湿度传感器**：SHTC3 (I2C 0x70, 300kHz)
- **触摸屏**：FT6336 (I2C 0x38, 400kHz), 复位 IO7, 中断 IO21
- **RTC**：PCF85063 (I2C 0x51, 300kHz), 闹钟中断 IO5
- **SD 卡**：SDMMC 1-bit (CLK=IO39, D0=IO40, CMD=IO41)
- **I2S 音频**：MCLK=IO14, SCLK=IO15, ASDOUT=IO16, LRCK=IO38, DSDIN=IO45
- **功放**：PA EN=IO18/IO42, PA CTRL=IO46
- **电池**：ADC=IO4 (ADC1_CH3, 分压 2:1), Control=IO17
- **UART**：TX=IO43, RX=IO44

### 硬件要点

- **PSRAM 必须启用** — 墨水屏显存和 LVGL 缓冲区分配在 PSRAM
- **I2C 总线共用** — SHTC3/RTC/触摸屏共用 IO47/IO48，地址 0x70/0x51/0x38 不冲突
- **墨水屏电源低有效** — EPD_PWR_PIN=LOW 开机，HIGH 关机
- **电源顺序** — EPD(IO6)、Audio(IO42)、VBAT(IO17) 需同时开启，否则 EPD 可能不工作
- **局刷前必须全刷基图** — `EPD_DisplayPartBaseImage()` 写入 BW+RED 双 RAM
- **SSD1681 LUT 数据必须完整** — `WF_PARTIAL_1IN54_0` 必须 159 个值，少一个都会导致控制器卡死 BUSY 引脚
- **SHTC3 无寄存器地址** — 用 write_read 方式通信
- **深睡眠唤醒后重新执行 app_main()** — 不是从睡眠点恢复
- **read_busy() 需要安全超时** — V2 原版无限等待，已加 5 秒超时防止死锁

### 微雪官方示例

`E:\aiot\V2/` 下有 13 个示例工程，完整参考见 `docs/waveshare-examples-reference.md`

## 开发进度

### Phase 1 - 本地功能 ✅
- [x] 项目骨架 (CMakeLists / sdkconfig.defaults / 分区表)
- [x] I2C 总线初始化 (`i2c_bsp`)
- [x] SHTC3 温湿度采集 (`port_shtc3`)
- [x] RTC 时间 (`port_rtc`)
- [x] 墨水屏驱动 (`epaper_driver_bsp`) — LUT Bug 已修复
- [x] LVGL UI 四象限布局 (`lvgl_port` + `ui`)
- [x] 触摸屏 (`port_touch`)
- [x] 电池 ADC (`port_battery`)
- [x] WiFi STA + NTP 同步 (`wifi_bsp`)
- [x] 完整 main.cpp 初始化流程 + 数据采集任务
- [ ] WiFi 凭据配置（当前是默认占位符，需改 sdkconfig.defaults）

### Phase 2 - AIoT 集成
- [ ] 小智AI 云端集成
- [ ] AI 回复 + 温湿度混合显示

### 已知问题
- WiFi SSID/密码需在 `sdkconfig.defaults` 或 `idf.py menuconfig` 中配置

## 命令参考

```powershell
# 用户在 Windows Terminal 里
. E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1

# 构建
idf.py build

# 烧录
idf.py -p COM5 flash

# 串口监视
idf.py -p COM5 monitor

# 清理
idf.py fullclean

# 菜单配置 (WiFi 凭据等)
idf.py menuconfig
```

```bash
# Claude Code (Bash 工具) 构建/烧录
env -u MSYSTEM powershell.exe -ExecutionPolicy Bypass -Command \
  "[Environment]::SetEnvironmentVariable('MSYSTEM', \$null, 'Process'); \
   . E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1; \
   cd E:\aiot; idf.py build"
```
