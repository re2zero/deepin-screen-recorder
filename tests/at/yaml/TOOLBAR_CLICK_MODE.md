# 工具栏按钮 AT-SPI 点击假成功 — 结论与维护约定

> 适用: deepin-screen-recorder 的 AT-SPI YAML suite（`tests/at/yaml/*.suite.yaml`）
> 结论来源: 源码核对 + 实测验证（save_local_button 假通过根因调查，2025）

## 根因

项目只有两个自定义按钮类（[widgets/toolbutton.h:18](../../src/widgets/toolbutton.h#L18)、[widgets/savebutton.h:15](../../src/widgets/savebutton.h#L15)）：

```
SaveButton → ToolButton → DToolButton → QToolButton → QAbstractButton
```

AT-SPI 的 `Press` action 走 `QAbstractButton::click()` **程序化路径**，不经过
`mousePressEvent` / `mouseReleaseEvent`。因此**交互逻辑写在鼠标事件重载里**的按钮，
`element_action` 点击会"假成功"（不抛异常、无真实点击）。

## 判断标准

| 交互逻辑位置 | AT-SPI click 结果 | 处理 |
|---|---|---|
| `mousePressEvent` / `mouseReleaseEvent` 重载内 | 假成功 | **必须 `click_mode: coordinate`** |
| `clicked` 信号（`connect(&Btn::clicked, ...)` / `QButtonGroup::buttonClicked`） | 有效（emit clicked()） | 无需坐标（可加，行为更真实） |

## A 类：必须坐标（逻辑在鼠标事件里）

| accessible_id | 类 | 逻辑位置 | suite 状态 |
|---|---|---|---|
| `save_local_button` | SaveButton | savebutton.cpp mouseReleaseEvent emit saveAction() | ✅ 已加 coordinate |
| `record_option_but` | ToolButton + setMenu | toolbutton.cpp mouseReleaseEvent `showMenu()`（subtoolwidget.cpp L347 setMenu） | ✅ 已加 coordinate（7 处） |
| `shot_option_but` | ToolButton + setMenu | subtoolwidget.cpp L1168 setMenu | ⚠️ 仅断言，未点击 |
| `scroll_option_but` | ToolButton + setMenu | subtoolwidget.cpp L1738 setMenu | ⚠️ suite 未用到 |
| `pin_save_local_but` | **SaveButton** | pin_screenshots/ui/subtoolwidget.cpp L61 | ⚠️ 仅断言，未点击 |

## B 类：clicked 信号驱动（AT-SPI 有效）

`gio_button`、`line_button`、`arrow_button`、`pen_button`、`mosaic_button`、
`text_button`、`scrollshot_button`、`ocr_button`、`pinscreenshots_button`、
`ai_assistant_button`、`rectangle_button`、`oval_button`、`record_btn`、`shot_btn`、
`keyboard_button`、`camera_button`、`undo_button`、`close_button`、`confirm_button`、
`recorder_button`、pin 窗口按钮（`pin_save_but`/`pin_close_but`/`pin_ocr_but`）、
AI 面板按钮（`ai_assistant_explain_button` 等）。

> 当前 suite 对 B 类也统一加了 `click_mode: coordinate`（真实鼠标事件链，更接近用户操作）。
> 严格来说只有 A 类是"必需"。

## 维护约定

新增/修改工具栏按钮点击步骤时：

1. 先查按钮类与逻辑位置（`src/widgets/` 下 `new ToolButton` / `new SaveButton` / `setMenu` / `connect(&Xxx::clicked`）。
2. 命中 A 类（SaveButton、带 QMenu 的 ToolButton）→ **必须** `do: click` 后加 `click_mode: coordinate`。
3. 菜单项（`dtk_main_menu`）与 check box（role: `check box`，如 `sysAudioAction`）**不加**——菜单项 AT-SPI 有效，check box 由框架 `_is_check_box` 自动坐标。
4. 断言（`assert_element`）不点击，不加。

## 相关文件

- 框架 click_mode 机制: YouQu `src/at/executor/handlers.py`（`click_mode == "coordinate"` → `_coordinate_click`）
- 根因调查: YouQu 会话记录（session_start 崩溃修复 + click_mode 机制, commit b4cb663）