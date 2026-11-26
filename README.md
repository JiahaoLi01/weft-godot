<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/JiahaoLi01/weft/main/docs/images/weft-logo-dark.gif">
    <img src="https://raw.githubusercontent.com/JiahaoLi01/weft/main/docs/images/weft-logo-light.gif" width="320" alt="Weft">
  </picture>
</p>

<h3 align="center">用 Web，写 Godot 界面。</h3>

<p align="center">
  <strong>Godot 4</strong> · <strong>GDExtension</strong> · <strong>Windows x64</strong> · <strong>Vulkan &amp; D3D12</strong>
</p>

---

**weft-godot** 将 [Weft](https://github.com/JiahaoLi01/weft) 嵌入式 Web UI 运行时集成到 Godot 4。放入一个 `WeftView` 控件节点即可运行 HTML、CSS 与 JavaScript；渲染直接复用 Godot 自己的 Vulkan / D3D12 设备与队列，不引入独立浏览器进程，也没有 CPU 像素拷贝。

<p align="center">
  <img src="https://raw.githubusercontent.com/JiahaoLi01/weft/main/docs/images/showcase/cozy-workshop.png" width="99%" alt="Weft UI">
</p>

## 为什么是 weft-godot

- **原生 Control 节点。** `WeftView` 继承自 `Control`，所见即所得地在编辑器视口里实时预览；支持 `device_scale` 高清缩放，也可通过 `get_texture()` 将界面贴到 3D 表面或任意材质上。
- **直接跑在 Godot 的 GPU 上。** 原生适配 Forward+ 与 Mobile 渲染器的 Vulkan 和 Direct3D 12 驱动，兼容单线程与独立渲染线程模式，全程零 CPU 回读。
- **信号、Variant 与 3D 锚点。** GDScript 的 `Variant`（整数、数组、字典、`Vector2`、`Color`）与页面 MessagePack 零样板互转；`write_anchors` 直接在绘制前更新 3D 世界标记位置，不触发脚本或重排。
- **按像素穿透鼠标。** `_has_point` 自动调用 Weft 命中测试：点在界面元素上由页面响应并同步 CSS 光标，点在空白处直接透传给下层控件或 3D 游戏世界。

## 编译与安装

需要 **CMake 3.25+**、**Visual Studio 2022**、**Python 3**、**Vulkan SDK 1.4.363.0** 与一份 [Weft](https://github.com/JiahaoLi01/weft) 源码：

```powershell
powershell -File scripts/BuildWeft.ps1 -WeftSource F:/Projects/weft
```

编译完成后将 `addons/weft`（含编好的 `bin/`）复制到你的 Godot 项目 `addons/` 目录，重新打开项目即可自动加载 GDExtension（项目需使用 Forward+ 或 Mobile 渲染器）。

## 快速上手

```gdscript
@onready var view: WeftView = $Hud # 在 Inspector 将 page 设为 "res://ui/hud"

func _ready() -> void:
    # 接收页面消息与异步调用（weft.engine.send / call）
    view.message_received.connect(func(name, value): print(name, value))
    view.call_received.connect(func(id, name, value):
        if name == "market.load":
            view.complete_call(id, true, {"gold": 12}))

    # 向页面推送视图模型与事件（直接传 Godot Variant）
    view.set_model("camera", {"tier": "Mid"})
    view.emit_event("units", units, true)

func _process(_delta: float) -> void:
    # 3D 世界锚点：页面元素写 data-anchor="1"，每帧零重排跟随目标
    var pos := camera.unproject_position(unit.global_position)
    view.write_anchors(PackedInt64Array([1]), PackedVector2Array([pos]))
```

## 文档

| 文档 | 内容 |
| --- | --- |
| [接入、页面与导出](docs/getting-started.md) | 编译安装、`WeftView` 与 `Weft` 单例 API、字体、资源导出配置与示例工程 |
| [渲染架构与当前边界](docs/rendering.md) | 线程模型、Vulkan / D3D12 双驱动与纹理同步、多队列族注意项与功能边界 |
| [自动化测试](docs/testing.md) | `RunTests.ps1` 用法、Vulkan 验证层与 D3D12 调试层检查项 |

## 目录结构

| 目录 | 说明 |
| --- | --- |
| `addons/weft/` | Godot 插件目录（`weft.gdextension` 及构建产物 `bin/`） |
| `native/` | 链接 Weft、godot-cpp 与扩展实现的 CMake 工程 |
| `scripts/` | `BuildWeft.ps1`（编译扩展并同步到 demo）与 `RunTests.ps1`（多驱动多模式测试） |
| `demo/` | 交互示例工程（全屏界面叠在 3D 场景上） |
| `tests/` | 自动化像素与交互测试工程（`test_runner.gd`） |
