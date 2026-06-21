# TODO UI Bug 修复设计文档

**日期**：2026-06-21
**状态**：设计已确认，待实现
**范围**：仅 `components/todo_ui/todo_ui.cpp`

---

## 1. Bug 列表与方案

| # | Bug | 根因 | 方案 |
|---|---|---|---|
| 1 | 无法删除任务 | 无 UI 触发 `TodoMgr_DeleteTask()` | 长按 checkbox 触发删除 |
| 2 | 完成任务文字不可见 | `lv_color_hex(0x999999)` 在墨水屏上几乎透明 | 黑色文字 + 删除线 |
| 3 | 长任务文本截断 | checkbox label 未设换行模式 | 启用 `LV_LABEL_LONG_WRAP` + 限宽 |
| 4 | 任务过多溢出 | 容器无滚动/翻页 | 启用容器垂直滚动 + 隐藏滚动条 |

**约束**：墨水屏无高帧动画，所有交互必须无动画。

---

## 2. Bug 1 — 长按删除

### 当前代码

`refresh_tasks_page()` 只注册了 `LV_EVENT_VALUE_CHANGED`（勾选/取消），没有删除入口。

### 方案

为每个 checkbox 添加 `LV_EVENT_LONG_PRESSED` 回调：

```cpp
static void task_long_press_cb(lv_event_t *e)
{
    const char *task_id = (const char *)lv_event_get_user_data(e);
    TodoMgr_DeleteTask(task_id);
    refresh_tasks_page();
}
```

注册方式（在 `refresh_tasks_page()` 的循环中）：

```cpp
lv_obj_add_event_cb(cb, task_check_cb, LV_EVENT_VALUE_CHANGED, (void *)s_task_id_pool[i]);
lv_obj_add_event_cb(cb, task_long_press_cb, LV_EVENT_LONG_PRESSED, (void *)s_task_id_pool[i]);
```

**交互**：长按 ≈ 500ms → 任务删除 → 列表刷新。不做确认弹窗（墨水屏刷新太慢）。

**注意**：`TodoMgr_DeleteTask()` 只需 `task_id` 一个参数，内部通过 `find_task_list_idx()` 自动定位所属清单。

---

## 3. Bug 2 — 完成任务文字不可见

### 当前代码

```cpp
// todo_ui.cpp:249-252
if (tasks[i].done) {
    lv_obj_add_state(cb, LV_STATE_CHECKED);
    lv_obj_set_style_text_color(cb, lv_color_hex(0x999999), 0);  // 灰色，墨水屏不可见
}
```

### 方案

替换为黑色文字 + 删除线：

```cpp
if (tasks[i].done) {
    lv_obj_add_state(cb, LV_STATE_CHECKED);
    lv_obj_set_style_text_color(cb, lv_color_black(), 0);
    lv_obj_set_style_text_decor(cb, LV_TEXT_DECOR_STRIKETHROUGH, LV_PART_MAIN);
}
```

删除线在墨水屏上非常清晰，是最自然的"已完成"视觉标记。

---

## 4. Bug 3 — 长任务文本换行

### 当前代码

`lv_checkbox_set_text(cb, label)` 创建的内部 label 默认 `LV_LABEL_LONG_CLIP`，超出宽度直接截断。

### 方案

获取 checkbox 的内部 label 子对象，设置换行模式 + 限宽：

```cpp
lv_checkbox_set_text(cb, label);
lv_obj_set_style_text_font(cb, &lv_font_chinese_14, LV_PART_MAIN);

/* 获取 checkbox 内部 label，启用换行 */
lv_obj_t *cb_label = lv_obj_get_child(cb, 1);  // child 0 = indicator, child 1 = label
if (cb_label) {
    lv_label_set_long_mode(cb_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(cb_label, 150);  // 196px 容器 - 2px pad - 2px pad - ~40px indicator ≈ 150px
}

/* checkbox 本身也需要限宽，否则 flex 布局不会触发换行 */
lv_obj_set_width(cb, 192);
```

**宽度计算**：
- 容器 196px，pad 2+2=4px，可用 192px
- checkbox indicator 约 16px + 4px gap = 20px
- label 可用 ≈ 192 - 20 = 172px，保守设 150px 留余量

---

## 5. Bug 4 — 任务过多翻页

### 当前代码

`s_task_container` 是 flex column 容器（196×160px），内容超出后无滚动。

### 方案

启用 LVGL 容器的垂直滚动，隐藏滚动条（墨水屏上无意义）：

```cpp
// create_tasks_page() 中，s_task_container 创建后
lv_obj_set_scrollbar_mode(s_task_container, LV_SCROLLBAR_MODE_OFF);
lv_obj_set_scroll_dir(s_task_container, LV_DIR_VER);
```

LVGL flex 容器内容超出后，启用滚动即可通过触摸上下滑动浏览。无需手动翻页逻辑。

**同样处理 `s_list_container`**（清单也可能多）。

---

## 6. 改动文件汇总

| 文件 | 改动 |
|---|---|
| `components/todo_ui/todo_ui.cpp` | 新增 `task_long_press_cb`；修改 `refresh_tasks_page()` 中的 done 样式、label 换行、checkbox 限宽；修改 `create_tasks_page()` 和 `create_lists_page()` 启用滚动 |

**不改动的文件**：`todo_ui.h`、`todo_manager.*`、`main.cpp` — 后端 API 已完备，无需新增。

---

## 7. 风险与边界

| 场景 | 处理 |
|---|---|
| 长按误触 | LVGL 默认长按阈值 500ms，足够区分点击和长按 |
| 删除最后一个任务 | `refresh_tasks_page()` 显示 "No tasks" |
| checkbox 内部 label 子对象索引 | LVGL v8 中 checkbox child 0=indicator, child 1=label；需验证 |
| 滚动后刷新列表 | `lv_obj_clean()` 清空容器时自动重置滚动位置 |
| 墨水屏滚动体验 | 触摸滑动 → LVGL 局刷 → 墨水屏部分刷新，体验可接受 |
