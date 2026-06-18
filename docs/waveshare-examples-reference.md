# 微雪 ESP32-S3 1.54" 墨水屏 示例代码参考

> 基于 `E:\aiot\V2` 下 13 个示例工程整理，用于指导本项目固件开发。

## 1. 示例工程清单

| 目录 | 功能 | 关键组件 |
|------|------|----------|
| `01_ADC_Test` | ADC 电池电压采集 | `adc_bsp` |
| `02_I2C_PCF85063` | PCF85063 RTC 时钟 | `i2c_bsp`, `i2c_equipment` |
| `03_I2C_SHTC3` | SHTC3 温湿度传感器 | `i2c_bsp`, `i2c_equipment` |
| `04_SD_Card` | SD 卡读写 (SDMMC) | `sdcard_bsp` |
| `05_WIFI_AP` | WiFi AP 模式 | esp_wifi |
| `06_WIFI_STA` | WiFi STA 模式 | esp_wifi |
| `07_BATT_PWR_Test` | 电池供电 + 墨水屏 + LVGL v8 | `epaper_driver_bsp`, LVGL |
| `08_Audio_Test` | 音频录放 + 墨水屏 + LVGL v8 | `epaper_driver_bsp`, ES8311 |
| `09_LVGL_V8_Test` | LVGL v8 图片轮播 | `epaper_driver_bsp`, LVGL v8 |
| `10_LVGL_V9_Test` | LVGL v9 图片轮播 | `epaper_driver_bsp`, LVGL v9 |
| `11_FactoryProgram` | 工厂测试（全外设） | 所有 BSP + LVGL v9 |
| `12_RTC_Sleep_Test` | RTC 闹钟 + 深度睡眠 + 墨水屏 | `epaper_driver_bsp`, PCF85063 |
| `13_FT6336_Test` | FT6336 触摸屏测试 | `ft6336_bsp` |

每个工程是独立的 ESP-IDF 项目，有自己的 `CMakeLists.txt`、`main/`、`components/`。

---

## 2. 硬件引脚定义

定义在 `user_config.h`（工厂测试用 `epaper_config.h`），所有工程一致。

### 墨水屏 SPI

| 宏名 | GPIO | 说明 |
|------|------|------|
| `EPD_DC_PIN` | IO10 | 数据/命令 |
| `EPD_CS_PIN` | IO11 | 片选 |
| `EPD_SCK_PIN` | IO12 | SPI 时钟 |
| `EPD_MOSI_PIN` | IO13 | SPI MOSI |
| `EPD_RST_PIN` | IO9 | 复位 |
| `EPD_BUSY_PIN` | IO8 | 忙信号（输入） |
| `EPD_SPI_NUM` | SPI2_HOST | SPI 总线号 |
| `EPD_PWR_PIN` | IO6 | 电源使能，低有效 |

### I2C 总线（触摸屏 / RTC / SHTC3 共用）

| 宏名 | GPIO | 说明 |
|------|------|------|
| `ESP32_I2C_SDA_PIN` | IO47 | SDA |
| `ESP32_I2C_SCL_PIN` | IO48 | SCL |
| `ESP32_I2C_DEV_NUM` | I2C_NUM_0 | I2C 端口号 |

### I2C 设备地址

| 设备 | 地址 | 速率 |
|------|------|------|
| SHTC3 温湿度 | 0x70 | 300 kHz |
| PCF85063 RTC | 0x51 | 300 kHz |
| FT6336 触摸 | 0x38 | 400 kHz |

### 触摸屏

| 宏名 | GPIO | 说明 |
|------|------|------|
| `EPD_TP_RST_PIN` | IO7 | 触摸复位 |
| `EPD_TP_INT_PIN` | IO21 | 触摸中断（下降沿） |

### SD 卡 (SDMMC 1-bit)

| 宏名 | GPIO | 说明 |
|------|------|------|
| `SD_CLK_PIN` | IO39 | 时钟 |
| `SD_MISO_D0_PIN` | IO40 | 数据线 |
| `SD_MOSI_CMD_PIN` | IO41 | 命令线 |

### 其他

| 宏名 | GPIO | 说明 |
|------|------|------|
| `VBAT_PWR_PIN` | IO17 | 电池电源，高有效 |
| `Audio_PWR_PIN` | IO42 | 音频电源，低有效 |
| `BOOT_BUTTON_PIN` | IO0 | BOOT 按键 |
| `PWR_BUTTON_PIN` | IO18 | 电源按键 |
| `LED_PIN` | IO3 | LED |

---

## 3. 墨水屏驱动 (SSD1681)

**文件**: `components/epaper_driver_bsp/epaper_driver_bsp.h` + `.cpp`
**跨工程一致**: 07/08/09/10/12 使用相同代码；11 用 C 函数版 `port_display.cpp`

### 类接口

```cpp
class epaper_driver_display {
public:
    epaper_driver_display(int width, int height, custom_lcd_spi_t lcd_spi_data);

    void EPD_Init();                 // 全刷初始化
    void EPD_Clear();                // 清屏（缓冲区填 0xFF = 白）
    void EPD_Display();              // 全刷：将缓冲区写入屏幕
    void EPD_DisplayPartBaseImage(); // 写入 BW + RED RAM，全刷显示
    void EPD_Init_Partial();         // 局刷初始化
    void EPD_DisplayPart();          // 局刷：将缓冲区写入屏幕
    void EPD_DrawColorPixel(uint16_t x, uint16_t y, uint8_t color);  // 画单像素
};
```

### SPI 配置

- 时钟：40 MHz
- 模式：SPI Mode 0
- DMA：SPI_DMA_CH_AUTO
- 无 MISO（只写）
- CS 由软件手动控制（不走 SPI 驱动管理）
- 显存缓冲区分配在 PSRAM（`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`）

### 像素缓冲区格式

- 1 bpp 单色位图，200×200 像素
- 每行 25 字节（200 / 8），总 5000 字节
- **1 = 白色，0 = 黑色**

```cpp
uint16_t index = y * 25 + (x >> 3);  // 字节偏移
uint8_t bit = 7 - (x & 0x07);        // 位偏移（MSB 在左）
if (color == DRIVER_COLOR_WHITE)
    buffer[index] |= (1 << bit);      // 置 1 = 白
else
    buffer[index] &= ~(1 << bit);     // 清 0 = 黑
```

### SSD1681 命令集（驱动中用到的）

| 命令 | 说明 |
|------|------|
| `0x12` | 软复位 (SWRESET) |
| `0x01` | 驱动输出控制 |
| `0x11` | 数据入口模式 (0x01 = Y 递减, X 递增) |
| `0x44` | 设置 RAM X 起止地址 |
| `0x45` | 设置 RAM Y 起止地址 |
| `0x4E` | 设置 RAM X 计数器 |
| `0x4F` | 设置 RAM Y 计数器 |
| `0x24` | 写 BW RAM（黑/白缓冲） |
| `0x26` | 写 RED RAM（局刷时存旧画面） |
| `0x32` | 写 LUT（波形查找表） |
| `0x18` | 温度传感器控制 (0x80 = 用内部传感器) |
| `0x22` | 显示更新控制 2 |
| `0x20` | 主激活 (Master Activation) |
| `0x3C` | 边框波形 |
| `0x2C` | VCOM 电压 |
| `0x03` | Gate 驱动电压 |
| `0x04` | Source 驱动电压 |
| `0x37` | 局刷模式寄存器 |

### LUT 波形表

- `WF_Full_1IN54[159]` — 全刷波形
- `WF_PARTIAL_1IN54_0[159]` — 局刷波形
- 各 159 字节

### LVGL Flush 回调（RGB565 → 单色）

```cpp
// LVGL 用 RGB565，墨水屏是 1-bit 单色
uint8_t color = (*buffer < 0x7fff) ? DRIVER_COLOR_BLACK : DRIVER_COLOR_WHITE;
driver->EPD_DrawColorPixel(x, y, color);
// 最后调用 EPD_DisplayPart() 局刷
// LVGL 必须配置 full_refresh = 1
```

### 初始化流程

```
EPD_Init()           // 硬复位 + 软复位 + 发送初始化命令序列 + 写全刷 LUT
EPD_Clear()          // 缓冲区填白
EPD_Display()        // 全刷白屏
EPD_DisplayPartBaseImage()  // 写入基图（BW + RED 双 RAM）
EPD_Init_Partial()   // 切换到局刷模式 + 写局刷 LUT
```

---

## 4. SHTC3 温湿度传感器

**文件**: `components/i2c_equipment/` (类版) 或 `port_bsp/port_shtc3.cpp` (C 函数版)
**地址**: 0x70, 16-bit 命令, MSB 优先

### I2C 命令

| 命令 | 值 | 说明 |
|------|------|------|
| READ_ID | 0xEFC8 | 读芯片 ID |
| SOFT_RESET | 0x805D | 软复位 |
| SLEEP | 0xB098 | 进入睡眠 |
| WAKEUP | 0x3517 | 唤醒 |
| MEAS_T_RH_POLLING | 0x7866 | 测量（温度优先，无时钟拉伸） |
| MEAS_RH_T_POLLING | 0x58E0 | 测量（湿度优先，无时钟拉伸） |

### 初始化

```
WAKEUP (等 50ms)
SOFT_RESET (等 20ms)
READ_ID → 读 3 字节 (2 数据 + 1 CRC)
```

### 读取温湿度

```
WAKEUP
MEAS_T_RH_POLLING (等 20ms)
读 6 字节: [T_MSB, T_LSB, T_CRC, H_MSB, H_LSB, H_CRC]
CRC-8 校验 (多项式 0x131, 初始值 0xFF)
SLEEP
```

### 数据转换

```c
temperature = 175.0 * rawValue / 65536.0 - 45.0 - 4.0;  // -4 = PETP 补偿
humidity    = 100.0 * rawValue / 65536.0;
```

### 错误码

| 值 | 含义 |
|------|------|
| 0x00 | NO_ERROR |
| 0x01 | ACK_ERROR |
| 0x02 | CHECKSUM_ERROR |

---

## 5. I2C 通信层

**文件**: `components/i2c_bsp/i2c_bsp.h` + `.c`（C 版）或 `main/i2c_bsp.cpp`（C++ 单例版）
**API**: ESP-IDF v5.x `driver/i2c_master.h` 新主站 API

### 函数

| 函数 | 说明 |
|------|------|
| `i2c_master_Init()` | 创建 I2C 总线，内部上拉，添加 RTC/SHTC3 设备 |
| `i2c_write_buff(handle, reg, buf, len)` | 写数据；reg=-1 时发原始缓冲区 |
| `i2c_read_buff(handle, reg, buf, len)` | 读数据；reg=-1 时直接接收 |
| `i2c_master_write_read_dev(handle, wBuf, wLen, rBuf, rLen)` | 先写后读（用于 SHTC3 等无寄存器设备） |

所有事务前调用 `i2c_master_bus_wait_all_done()` 超时 1s，数据超时 5s。

---

## 6. FT6336 触摸屏

**文件**: `ft6336_bsp.h` + `.cpp`
**地址**: 0x38, 400 kHz, 单例类 `I2cFt6336Dev`

### 函数

| 函数 | 说明 |
|------|------|
| `Ft6336_Reset(pin)` | 复位：HIGH 100ms → LOW 100ms → HIGH 100ms |
| `GetTouchPoint(&x, &y)` | 读取触摸坐标 |

### 坐标解析

```c
// 读寄存器 0x02 得触摸点数，0x03 读 4 字节坐标
x = ((buf[0] & 0x0F) << 8) | buf[1];
y = ((buf[2] & 0x0F) << 8) | buf[3];
// 钳位到 0~199
```

触摸中断：GPIO21，下降沿，ISR 队列方式。

---

## 7. 电源管理

**文件**: `components/board_power_bsp/`

### 函数

| 函数 | GPIO | 说明 |
|------|------|------|
| `POWEER_EPD_ON/OFF()` | IO6 | 墨水屏电源，低有效 |
| `POWEER_Audio_ON/OFF()` | IO42 | 音频电源，低有效 |
| `VBAT_POWER_ON/OFF()` | IO17 | 电池电源，高有效 |
| `EnableDeepLowPowerMode()` | — | 配置 EXT1 唤醒 + 进入深度睡眠 |

### 深度睡眠唤醒源

- EXT1: GPIO0 (BOOT), GPIO5 (RTC 闹钟), GPIO18 (PWR)
- GPIO Hold: VBAT 引脚在睡眠时保持状态

---

## 8. 按键驱动 (Multi-Button)

**文件**: `components/button_bsp/multi_button.h` + `.c`

### 事件

| 事件 | 说明 |
|------|------|
| `BTN_PRESS_DOWN` | 按下 |
| `BTN_PRESS_UP` | 松开 |
| `BTN_SINGLE_CLICK` | 单击 |
| `BTN_DOUBLE_CLICK` | 双击 |
| `BTN_LONG_PRESS_START` | 长按开始 (1000ms) |
| `BTN_LONG_PRESS_HOLD` | 长按持续 |

### 参数

- 去抖：15ms (3 ticks × 5ms)
- 短按阈值：300ms
- 长按阈值：1000ms

物理按键：BOOT (GPIO0)、PWR (GPIO18)，均低有效带上拉。

---

## 9. ADC 电池电压

**文件**: `components/adc_bsp/`

- ADC1 Channel 3, 12-bit, 12dB 衰减 (3.3V 量程)
- 分压比 2:1，实际电压 = 读数 × 2
- `voltage = 0.001 * voltage_mv * 2`

---

## 10. SD 卡

**文件**: `components/sdcard_bsp/`

- SDMMC 1-bit 模式
- 引脚：CLK=IO39, D0=IO40, CMD=IO41
- 高速频率
- FAT 文件系统挂载到 `/sdcard`

---

## 11. LVGL 集成

### v8 API（工程 07/08/09/12）

```c
lv_disp_drv_t disp_drv;
lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buf_size);
lv_disp_drv_init(&disp_drv);
disp_drv.flush_cb = epaper_flush_cb;
disp_drv.full_refresh = 1;  // 墨水屏必须
lv_disp_drv_register(&disp_drv);
```

### v9 API（工程 10/11）

```c
lv_display_t *disp = lv_display_create(200, 200);
lv_display_set_flush_cb(disp, epaper_flush_cb);
lv_display_set_buffers(disp, &draw_buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_FULL);
```

### 公共配置

- Tick 定时器：5ms 周期
- LVGL 任务：Core 1, 8KB 栈, mutex 保护
- 墨水屏必须 `full_refresh = 1`

---

## 12. 应用初始化流程（通用模式）

```
1. 上电：VBAT_POWER_ON() → POWEER_EPD_ON()
2. I2C 初始化：i2c_master_Init()
3. 墨水屏初始化：
   spi_master_Init()  → EPD_Init() → EPD_Clear()
   → EPD_Display() → EPD_DisplayPartBaseImage() → EPD_Init_Partial()
4. LVGL 初始化 → 设置 flush 回调
5. 创建 UI
6. 启动 FreeRTOS 任务
```

### 典型任务结构

| 任务 | Core | 栈 | 说明 |
|------|------|------|------|
| LED 闪烁 | 1 | — | GPIO3, 200ms 周期 |
| LVGL Handler | 1 | 8KB | `lv_timer_handler()` 循环 |
| 传感器读取 | — | — | SHTC3 每 1s |
| 按键处理 | — | — | 事件组等待 |
| 触摸处理 | — | — | 中断驱动 |

---

## 13. 开发建议（从示例中总结）

### 直接可用的组件

以下 BSP 组件代码质量可，可直接移植到本项目：

- `i2c_bsp` — I2C 通信层
- `epaper_driver_bsp` — 墨水屏驱动
- `i2c_equipment` / `port_shtc3` — SHTC3 传感器
- `board_power_bsp` — 电源管理
- `button_bsp` + `multi_button` — 按键驱动

### 注意事项

1. **PSRAM 必须启用** — 墨水屏显存 (5000B) 和 LVGL 缓冲区都分配在 PSRAM
2. **墨水屏电源低有效** — `EPD_PWR_PIN=LOW` 开机，`HIGH` 关机
3. **局刷前必须全刷基图** — `EPD_DisplayPartBaseImage()` 把当前画面同时写入 BW 和 RED RAM
4. **I2C 总线共用** — SHTC3、RTC、触摸屏共用 IO47/IO48，注意地址不冲突 (0x70/0x51/0x38)
5. **SHTC3 无寄存器地址** — 用 `write_read` 方式通信，不是标准的 `reg + read`
6. **LVGL 到单色转换阈值** — RGB565 < 0x7FFF 视为黑色，否则白色
7. **深睡眠注意** — 唤醒后是 `app_main()` 重新执行，不是从睡眠点恢复
8. **C++ 文件用 `extern "C"` 包裹 ESP-IDF C 头文件**

### Phase 1 最小依赖

温湿度显示功能只需：

```
i2c_bsp (I2C 通信)
i2c_equipment / port_shtc3 (SHTC3 驱动)
epaper_driver_bsp (墨水屏驱动)
board_power_bsp (电源管理)
```

可选：LVGL（如果要做复杂 UI），或直接操作像素缓冲区画简单图形+文字。
