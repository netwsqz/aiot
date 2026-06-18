## Task 2: wifi_ap 热点管理

**Status:** DONE

**Build result:** `idf.py build` -- 编译通过，aiot.bin 二进制大小 0x122870 (71% 空闲)

**完成的文件：**
- `components/wifi_ap/CMakeLists.txt`
- `components/wifi_ap/wifi_ap.h`
- `components/wifi_ap/wifi_ap.cpp`

**接口已实现：**
- `WifiAp_Init()` -- 创建 AP netif、注册事件、创建超时定时器
- `WifiAp_Start()` -- 启动 APSTA 模式，SSID "AIoT-TODO"，无密码
- `WifiAp_Stop()` -- 停止 WiFi AP，停止定时器
- `WifiAp_IsActive()` -- 查询当前状态
- `WifiAp_ResetTimeout()` -- 重置 15 分钟超时定时器
- `WifiAp_SetStateCallback()` -- 设置状态变更回调

**Concerns:**
- 代码严格按照 brief 提供的内容实现，无偏差
