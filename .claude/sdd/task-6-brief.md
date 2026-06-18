## Task 6: ui 组件改动 - 页面路由

**目标:** 添加 swipe 手势检测和页面切换

**Files:**
- Modify: `components/ui/ui.h`
- Modify: `components/ui/ui_screen.c`

---

### Step 1: 修改 ui.h 添加页面枚举

在 `ui.h` 末尾 `#endif` 之前添加：

```c
/* ── Page management ── */
typedef enum {
    PAGE_DASHBOARD,
    PAGE_TODO_LISTS,
    PAGE_TODO_TASKS,
} ui_page_t;

void ui_switch_page(ui_page_t page);
ui_page_t ui_get_current_page(void);
```

---

### Step 2: 修改 ui_screen.c 添加页面管理

在文件顶部添加：

```c
#include "todo_ui.h"

static ui_page_t s_current_page = PAGE_DASHBOARD;
static lv_obj_t *s_dashboard_screen = NULL;
```

在 `ui_init()` 函数开头保存 screen 引用：

```c
s_dashboard_screen = ui.screen;
```

在文件末尾添加新函数：

```c
void ui_switch_page(ui_page_t page)
{
    s_current_page = page;
    if (page == PAGE_DASHBOARD) {
        lv_screen_load(s_dashboard_screen);
    } else {
        // TODO 页面由 todo_ui 管理
    }
}

ui_page_t ui_get_current_page(void)
{
    return s_current_page;
}
```

---

### Step 3: 验证编译

```bash
cd E:/aiot
idf.py build
```

**预期:** 编译通过

---

### Step 4: 提交

```bash
git add components/ui/
git commit -m "feat: add page routing to ui component"
```
