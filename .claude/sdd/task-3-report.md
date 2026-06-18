## Task 3: todo_web HTTP 服务器 -- 完成

**状态:** 编译通过

### 创建的文件

- `components/todo_web/CMakeLists.txt` -- 组件注册，依赖 esp_http_server, todo_manager, json
- `components/todo_web/todo_web.h` -- 公开 API: Init/Start/Stop/IsRunning，带 extern "C" guard
- `components/todo_web/todo_web.cpp` -- 完整 REST API 实现

### REST API 端点

| 方法 | 路径 | 功能 |
|------|------|------|
| GET | /api/lists | 获取所有列表（含已完成数） |
| POST | /api/lists | 创建列表 |
| PUT | /api/lists/{id} | 更新列表 |
| DELETE | /api/lists/{id} | 删除列表（含级联删除任务） |
| GET | /api/lists/{id}/tasks | 获取列表下的所有任务 |
| POST | /api/lists/{id}/tasks | 创建任务 |
| PUT | /api/tasks/{id} | 更新任务（done/priority/due） |
| DELETE | /api/tasks/{id} | 删除任务 |
| OPTIONS | /* | CORS preflight |

### 验证

- 编译通过: `idf.py build` 成功
- binary size: 0x122870 bytes (71% 空间余量)

### 注意事项

- HTTP 服务器默认监听 80 端口
- CORS header `Access-Control-Allow-Origin: *` 已添加
- 最大 URI handler 数量设为 12（含 9 个 API + 1 个 OPTIONS）
- 栈大小 8192 字节
- 请求体限制 2KB
