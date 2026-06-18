## Task 4: 嵌入式网页开发

**目标:** 手机端 Web UI，纯原生 JS SPA

**Files:**
- Create: `web_dev/index.html`
- Create: `scripts/build_web.sh`
- Create: `components/todo_web/web_page.h` (generated)
- Modify: `components/todo_web/todo_web.cpp` (add root route)

---

### Step 1: 创建 web_dev/index.html

完整单文件 SPA，包含：
- 响应式布局，手机竖屏优化
- 标签页切换清单
- 任务列表：点击勾选、删除
- 添加任务弹窗
- 新建/删除清单
- fetch() 调 REST API

```html
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>AIoT 任务管理</title>
<style>
* { margin: 0; padding: 0; box-sizing: border-box; }
body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; background: #f5f5f5; }
.header { background: #2196F3; color: white; padding: 16px; text-align: center; font-size: 18px; }
.tabs { display: flex; overflow-x: auto; padding: 8px; gap: 8px; background: white; }
.tab { padding: 8px 16px; border-radius: 20px; border: 1px solid #ddd; background: white; cursor: pointer; white-space: nowrap; }
.tab.active { background: #2196F3; color: white; border-color: #2196F3; }
.tab-add { background: #4CAF50; color: white; border-color: #4CAF50; }
.task-list { padding: 8px; }
.task { background: white; border-radius: 8px; padding: 12px; margin-bottom: 8px; display: flex; align-items: center; gap: 12px; }
.task.done { opacity: 0.6; }
.task-checkbox { width: 24px; height: 24px; cursor: pointer; }
.task-content { flex: 1; }
.task-text { font-size: 16px; }
.task-text.done { text-decoration: line-through; color: #999; }
.task-meta { font-size: 12px; color: #666; margin-top: 4px; }
.task-delete { color: #f44336; cursor: pointer; font-size: 20px; }
.add-btn { display: block; width: calc(100% - 16px); margin: 8px; padding: 12px; background: #4CAF50; color: white; border: none; border-radius: 8px; font-size: 16px; cursor: pointer; }
.danger-btn { display: block; width: calc(100% - 16px); margin: 8px; padding: 12px; background: #f44336; color: white; border: none; border-radius: 8px; font-size: 16px; cursor: pointer; }
.modal { display: none; position: fixed; top: 0; left: 0; right: 0; bottom: 0; background: rgba(0,0,0,0.5); z-index: 100; align-items: center; justify-content: center; }
.modal.active { display: flex; }
.modal-content { background: white; border-radius: 12px; padding: 20px; width: 90%; max-width: 400px; }
.modal-title { font-size: 18px; margin-bottom: 16px; }
.modal-input { width: 100%; padding: 12px; border: 1px solid #ddd; border-radius: 8px; margin-bottom: 12px; font-size: 16px; }
.modal-select { width: 100%; padding: 12px; border: 1px solid #ddd; border-radius: 8px; margin-bottom: 12px; font-size: 16px; }
.modal-btns { display: flex; gap: 8px; justify-content: flex-end; }
.modal-btn { padding: 8px 16px; border-radius: 8px; border: none; cursor: pointer; font-size: 14px; }
.btn-cancel { background: #ddd; }
.btn-ok { background: #2196F3; color: white; }
</style>
</head>
<body>
<div class="header">AIoT 任务管理</div>
<div class="tabs" id="tabs"></div>
<div class="task-list" id="taskList"></div>
<button class="add-btn" id="addTaskBtn">+ 添加任务</button>
<button class="danger-btn" id="delListBtn">删除清单</button>

<div class="modal" id="taskModal">
<div class="modal-content">
  <div class="modal-title">新建任务</div>
  <input class="modal-input" id="taskText" placeholder="任务内容">
  <select class="modal-select" id="taskPriority">
    <option value="0">普通</option>
    <option value="1">重要</option>
    <option value="2">紧急</option>
  </select>
  <input class="modal-input" id="taskDue" type="date">
  <div class="modal-btns">
    <button class="modal-btn btn-cancel" onclick="closeModal()">取消</button>
    <button class="modal-btn btn-ok" onclick="submitTask()">确定</button>
  </div>
</div>
</div>

<div class="modal" id="listModal">
<div class="modal-content">
  <div class="modal-title">新建清单</div>
  <input class="modal-input" id="listName" placeholder="清单名称">
  <div class="modal-btns">
    <button class="modal-btn btn-cancel" onclick="closeListModal()">取消</button>
    <button class="modal-btn btn-ok" onclick="submitList()">确定</button>
  </div>
</div>
</div>

<script>
const API = '';
let lists = [];
let currentListId = null;

async function loadLists() {
  const res = await fetch(API + '/api/lists');
  const data = await res.json();
  if (data.ok) {
    lists = data.data;
    if (!currentListId && lists.length > 0) currentListId = lists[0].id;
    renderTabs();
    loadTasks();
  }
}

async function loadTasks() {
  if (!currentListId) {
    document.getElementById('taskList').innerHTML = '<p style="padding:16px;color:#999">暂无清单，请先创建</p>';
    return;
  }
  const res = await fetch(API + '/api/lists/' + currentListId + '/tasks');
  const data = await res.json();
  if (data.ok) renderTasks(data.data);
}

function renderTabs() {
  const tabs = document.getElementById('tabs');
  tabs.innerHTML = lists.map(l =>
    `<div class="tab ${l.id === currentListId ? 'active' : ''}" onclick="switchList('${l.id}')">${l.name}</div>`
  ).join('') + '<div class="tab tab-add" onclick="openListModal()">+</div>';
}

function renderTasks(tasks) {
  const list = document.getElementById('taskList');
  if (tasks.length === 0) {
    list.innerHTML = '<p style="padding:16px;color:#999">暂无任务</p>';
    return;
  }
  list.innerHTML = tasks.map(t => {
    const pri = ['', '🟡重要', '🔴紧急'][t.priority] || '';
    const due = t.due ? `📅 ${t.due}` : '';
    return `<div class="task ${t.done ? 'done' : ''}">
      <input type="checkbox" class="task-checkbox" ${t.done ? 'checked' : ''} onchange="toggleTask('${t.id}', this.checked)">
      <div class="task-content">
        <div class="task-text ${t.done ? 'done' : ''}">${t.text}</div>
        <div class="task-meta">${pri} ${due}</div>
      </div>
      <span class="task-delete" onclick="deleteTask('${t.id}')">×</span>
    </div>`;
  }).join('');
}

function switchList(id) {
  currentListId = id;
  loadLists();
}

async function toggleTask(id, done) {
  await fetch(API + '/api/tasks/' + id, {
    method: 'PUT', headers: {'Content-Type':'application/json'},
    body: JSON.stringify({done})
  });
  loadTasks();
}

async function deleteTask(id) {
  if (!confirm('删除此任务？')) return;
  await fetch(API + '/api/tasks/' + id, {method: 'DELETE'});
  loadTasks();
}

function openModal() { document.getElementById('taskModal').classList.add('active'); }
function closeModal() { document.getElementById('taskModal').classList.remove('active'); }
function openListModal() { document.getElementById('listModal').classList.add('active'); }
function closeListModal() { document.getElementById('listModal').classList.remove('active'); }

async function submitTask() {
  const text = document.getElementById('taskText').value.trim();
  if (!text) return alert('请输入任务内容');
  const priority = parseInt(document.getElementById('taskPriority').value);
  const due = document.getElementById('taskDue').value;
  await fetch(API + '/api/lists/' + currentListId + '/tasks', {
    method: 'POST', headers: {'Content-Type':'application/json'},
    body: JSON.stringify({text, priority, due: due || ''})
  });
  closeModal();
  document.getElementById('taskText').value = '';
  loadTasks();
}

async function submitList() {
  const name = document.getElementById('listName').value.trim();
  if (!name) return alert('请输入清单名称');
  await fetch(API + '/api/lists', {
    method: 'POST', headers: {'Content-Type':'application/json'},
    body: JSON.stringify({name, icon: '[]'})
  });
  closeListModal();
  document.getElementById('listName').value = '';
  loadLists();
}

document.getElementById('addTaskBtn').onclick = openModal;
document.getElementById('delListBtn').onclick = async () => {
  if (!currentListId) return;
  if (!confirm('删除此清单及所有任务？')) return;
  await fetch(API + '/api/lists/' + currentListId, {method: 'DELETE'});
  currentListId = null;
  loadLists();
};

loadLists();
</script>
</body>
</html>
```

---

### Step 2: 创建 scripts/build_web.sh

```bash
#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
HTML_FILE="$PROJECT_DIR/web_dev/index.html"
OUTPUT="$PROJECT_DIR/components/todo_web/web_page.h"

if [ ! -f "$HTML_FILE" ]; then
  echo "Error: $HTML_FILE not found"
  exit 1
fi

# gzip 压缩
gzip -c "$HTML_FILE" > /tmp/web_page.gz

# 生成 C 数组
xxd -i /tmp/web_page.gz > "$OUTPUT"

# 添加头文件保护
sed -i '1i\
#ifndef WEB_PAGE_H\
#define WEB_PAGE_H\
\
#include <stdint.h>\
\
' "$OUTPUT"

echo 'extern const uint8_t web_page_gz[];' >> "$OUTPUT"
echo 'extern const unsigned int web_page_gz_len;' >> "$OUTPUT"
echo '#endif' >> "$OUTPUT"

# 清理变量名
sed -i 's/web_page_gz/const uint8_t web_page_gz[]/g' "$OUTPUT"
sed -i 's/web_page_gz_len/const unsigned int web_page_gz_len/g' "$OUTPUT"

rm -f /tmp/web_page.gz

echo "Generated $OUTPUT ($(wc -c < "$HTML_FILE") bytes → gzip compressed)"
```

---

### Step 3: 生成 web_page.h

```bash
cd E:/aiot
mkdir -p scripts web_dev
chmod +x scripts/build_web.sh
./scripts/build_web.sh
```

**预期:** 生成 `components/todo_web/web_page.h`，包含 gzip 压缩的 HTML

---

### Step 4: 修改 todo_web.cpp 添加网页路由

在 `handle_get_lists` 之前添加：

```cpp
// 嵌入式网页数据
extern const uint8_t web_page_gz[];
extern const unsigned int web_page_gz_len;

// GET /
static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, (const char *)web_page_gz, web_page_gz_len);
    return ESP_OK;
}
```

在 `TodoWeb_Start()` 中注册路由（在其他路由之前）：

```cpp
httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = handle_root };
httpd_register_uri_handler(s_server, &uri_root);
```

---

### Step 5: 验证编译

```bash
cd E:/aiot
idf.py build
```

**预期:** 编译通过，固件大小增加约 8-12KB

---

### Step 6: 提交

```bash
git add web_dev/ scripts/ components/todo_web/web_page.h components/todo_web/todo_web.cpp
git commit -m "feat: add embedded web UI for todo management"
```
