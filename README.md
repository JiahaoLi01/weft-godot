# weft-godot

把 [weft](https://github.com/JiahaoLi01/weft)（HTML、CSS、DOM 与 JavaScript 的游戏 UI 运行时）接进 Godot 4 的 GDExtension。页面在扩展自己的线程上运行，帧由 weft 对应 Godot 当前图形驱动的后端在 Godot 的渲染线程上画：用的是 Godot 自己的 GPU 设备与队列，不经 CPU 拷贝，也不另开设备。

当前状态（2026-10-17）：实验版。Windows x64；Godot 的 Forward+ 与 Mobile 渲染器，Vulkan 与 Direct3D 12 两个驱动。在 Godot 4.7.2 上，两个驱动 × 两种渲染线程模式（主线程渲染、独立渲染线程）的测试都通过，开着 Vulkan 验证层（含同步检查）与 D3D12 调试层（weft main 3379d90，godot-cpp 10.0.0）。扩展按 GDExtension 4.5 的接口编译，4.5 以后的 Godot 都能加载，但只在 4.7.2 上测过。

## 目录

| 位置 | 内容 |
| --- | --- |
| `addons/weft/` | 放进 Godot 工程的插件：`weft.gdextension`，编好的 DLL 在 `bin/`（不入库） |
| `native/` | 把 weft 的静态库、godot-cpp 与扩展自己的代码编成一个 DLL 的 CMake 工程 |
| `scripts/BuildWeft.ps1` | 用一份 weft 签出编译 DLL，放进 `addons/weft/bin`，并把插件拷进 `demo/addons/weft` |
| `scripts/RunTests.ps1` | 在 Godot 里逐个驱动、逐个渲染线程模式跑测试 |
| `tests/` | 测试工程：`test_runner.gd`（SceneTree 脚本，读回纹理像素检查） |
| `demo/` | 演示工程：全屏页面叠在 3D 场景上 |

`native/src` 里一个文件做一件事：

| 文件 | 做什么 |
| --- | --- |
| `owner_thread` | weft 运行时所属的线程（16 MiB 栈），扩展把运行时与 View 的工作交给它并等它做完 |
| `page_host` | 运行时与各 View：建、推进、收集页面的消息与诊断，分两步停（摘下、销毁），不依赖 Godot |
| `page_source` | 页面文件夹（`page.json`）的读取，与页面引用的文件（`url()` 图片）的读取（`FileAccess`，导出包里也能读） |
| `variant_pack` | Godot 的 `Variant` 与 MessagePack 互转（整数保持整数） |
| `surface` | 一个 View 在某个尺寸下的两张纹理（weft 画的图、Godot 采样的纹理） |
| `render_bridge` | 渲染线程上的工作：Surface 的建、画、放，渲染帧计数，统计 |
| `render_device` | 一个图形 API 的 weft 渲染器：按运行时换渲染器，按 GPU 完成情况回收录制 |
| `render_vulkan`、`render_d3d12` | 两个驱动：在 Godot 的设备上录自己的命令缓冲（命令列表），提交到 Godot 的队列 |
| `render_thread` | 把 C++ 函数包成 Callable 交给 Godot 的渲染线程 |
| `weft_server` | 引擎单例 `Weft` |
| `weft_view` | 控件 `WeftView` |
| `register_types` | 扩展入口 |

## 编译

需要 CMake 3.25+、Visual Studio 2022、Python 3（godot-cpp 生成绑定）、Vulkan SDK 1.4.363.0（weft 的 Vulkan 后端用它的 DXC 编着色器；Godot 在 Windows 上默认用 Vulkan，没有它不编）。

```powershell
powershell -File scripts/BuildWeft.ps1 -WeftSource F:/Projects/weft
```

不给 `-WeftSource` 时找环境变量 `WEFT_SOURCE_DIR`，再找本仓库旁边的 `weft` 目录。godot-cpp 由 CMake 下载，固定在 10.0.0 的提交（`507ed9d`）并校验 SHA256。DLL 静态链接 C++ 运行库，只依赖系统 DLL。编辑器加载 DLL 后不再放开它：重编前关掉用这个插件的编辑器。

## 安装

把 `addons/weft`（连同编好的 `bin/`）拷进工程的 `addons/`。GDExtension 不需要在插件列表里启用；第一次打开工程时编辑器扫描到 `weft.gdextension` 后加载它（可能提示重启编辑器）。工程要用 Forward+ 或 Mobile 渲染器：Compatibility（OpenGL）没有 RenderingDevice，weft 画不了（打一条错误，View 不显示）。

## 用法

```gdscript
# 场景里放一个 WeftView（Control），page 填页面文件夹：res://ui/hud（page.json，或 page.js 加 page.css、keyframes.css）
@onready var view: WeftView = $Hud

func _ready() -> void:
    view.message_received.connect(func(name, value): print(name, value))       # weft.engine.send
    view.call_received.connect(func(id, name, value):                          # weft.engine.call
        if name == "market.load":
            view.complete_call(id, true, {"gold": 12}))
    view.diagnostic_received.connect(func(kind, text): pass)                    # 脚本异常、样式、缺图

    # 低频数据：事件与视图模型（Variant 直接传，整数保持整数，Vector2、Color 成为 {x, y}、{r, g, b, a}）
    view.emit_event("units", units, true)          # weft.engine.on('units', ...)，true：替换未送达的同名事件
    view.set_model("camera", {"tier": "Mid"})      # weft.engine.model('camera')

func _process(_delta: float) -> void:
    # 每帧跟着场景走的元素：锚点（页面写 data-anchor="<key>"），下一次绘制直接摆放，不跑脚本、不重排
    var at := camera.unproject_position(unit.global_position)
    view.write_anchors(PackedInt64Array([1]), PackedVector2Array([at]))
```

- **尺寸**：View 的页面和控件一样大，一个 CSS px 是控件的一个单位；`device_scale`（默认 1）是每 CSS px 的设备像素，纹理是控件尺寸乘它（2 画得更清楚）。控件变大小时页面跟着重排。
- **鼠标**：控件只在页面元素（不是页面根）所在处接鼠标（`_has_point` 问 weft 的命中测试），点在空白处的事件穿过 View 给下面的控件或 `_unhandled_input`，即给游戏。`hit_test_input = false` 时整个控件接鼠标。光标跟着页面的 CSS `cursor`。`is_pointer_over_page()`、`hit_test(pos)` 给游戏判断。View 画在 3D 表面上时，由游戏用 `send_pointer(kind, pos, buttons, wheel)` 转发指针。
- **纹理**：`get_texture()` 是一张 `Texture2D`（`Texture2DRD`），可以拿去别处显示；像素是 sRGB 编码、预乘、首行在上，用预乘混合（`CanvasItemMaterial.BLEND_MODE_PREMULT_ALPHA`，着色器的 `blend_premul_alpha`）。控件自己用一个带预乘材质的画布项画它，`material` 属性留给你。
- **字体**：默认是 Windows 的系统字体。要加字体文件，在第一个页面载入之前（比如一个自动加载脚本的 `_ready`）：`Weft.start()`，然后 `Weft.add_font("Noto Serif SC", "res://fonts/NotoSerifSC.otf", 400)`。weft 自己按路径读字体文件：`res://` 下的字体在编辑器里直接用，导出包里的（或被 Godot 导入成字体资源的）先写到 `user://weft/fonts` 再给 weft。
- **运行时**：第一个 WeftView 进树时启动（`Weft.start(system_fonts, max_views)` 可以先手动启动），所有 View 共用。`Weft.stop()` 停掉后 View 不再画，再 `Weft.start()` 时在树里的 View 自动建回来并重新载入页面。`Weft.get_render_stats()` 给出渲染线程画了多少、失败多少与最后一次失败的原因。
- **编辑器里**：WeftView 在编辑器里也运行页面（预览）。

## 页面与导出

- `page.json`：`{"scripts": ["d3.js", "page.js"], "stylesheet": "page.css", "keyframes": "keyframes.css", "root": "#root", "crpgBridge": false}`，脚本按顺序执行。没有它时用文件夹里的 `page.js`、`page.css`、`keyframes.css`。`load_source(script, css, folder)` 从字符串做页面。
- 页面里的图片：CSS 写 `background-image: url(../textures/x.png)`（PNG），相对路径从样式表的路径（`res://ui/hud/page.css`）算起；`/x.png` 从 `res://` 算起；带协议的（`user://x.png`）原样读。读不到或不是 PNG 时是一条页面诊断，那个背景不画图。
- 导出：Godot 只把资源导出进包。页面的 `.js`、`.css`、`.json` 不是资源，要在导出预设的"资源 → 导出非资源文件"里加上（如 `ui/*.js, ui/*.css, ui/*.json`）；页面用的 PNG 会被 Godot 导入成纹理，原文件不进包，要在导入面板把它的导入方式改成"保留文件（按原样导出）"，即 `.import` 里 `importer="keep"`（演示的 `stripes.png` 就是这样）。
- weft 的 scene 页面只支持 CSS 的一部分（见 weft 的 `docs/compatibility.md`）：过渡写分开的属性，锚点元素给宽度并 `position: absolute`。

## 演示

先编译（`BuildWeft.ps1` 把插件拷进 `demo/addons/weft`），再用 Godot 打开 `demo/` 运行：一个全屏页面，可以试按钮的悬停、按下与点击（Godot 回事件确认）、页面调用 Godot（`weft.engine.call`）、滚轮缩放一张 `url()` 图片、指针坐标、Godot 推来的视图模型（驱动、帧率、鼠标是否在页面元素上、光标），以及跟着转动方块走的锚点标签；点页面元素以外的地方时点击归游戏，方块变色。换驱动：命令行 `--rendering-driver vulkan` 或 `d3d12`（或工程设置的 `rendering/rendering_device/driver.windows`）。`godot --path demo -- --screenshot=shot.png` 存一帧后退出。

## 线程与渲染

- weft 的页面要 8 MiB 以上的栈（React、D3）：扩展开一个 16 MiB 栈的线程（owner 线程）跑运行时，主线程的调用（推进、载入、指针、命中）交给它并等它做完。事件、视图模型、调用的完成、锚点与取帧是 weft 的"任意线程"函数，直接调。
- 每帧 `WeftView._process`：送尺寸、推进页面、发出信号；页面帧变了（或在动画、锚点动了）时，把一次绘制交给 Godot 的渲染线程（`RenderingServer.call_on_render_thread`；默认的单线程渲染就是主线程，独立渲染线程模式下按序排在这一帧的渲染命令里）。
- Godot 不把它的命令缓冲给 GDExtension，所以每次绘制是扩展自己在 Godot 设备上录的一个命令缓冲（D3D12 是命令列表），立刻提交到 Godot 的主队列，带一个 fence 说明 GPU 何时做完（weft 据此回收它的资源）。weft 的渲染器只认创建它的线程，所以建在渲染线程上；一个 weft 后端只画一个运行时的 View，运行时换了就换一个渲染器，旧的等它的录制都完成后销毁。
- 每个 View 在一个尺寸下有两张 RenderingDevice 纹理（8 位 BGRA UNORM）：
  - **图**：weft 画进去，Godot 只从它拷贝。Godot 的帧图第一次用一张纹理时从"未定义"布局转过去（内容作废），所以建图时先做一次异步读回作为它的第一次使用，把它放进拷贝源布局；之后 weft 的每次绘制都从拷贝源转到颜色附件、画完再转回拷贝源。这样 Godot 的帧图与 weft 对它的布局（D3D12 的状态；Godot 有增强屏障时用增强屏障）意见一致，谁也不需要知道谁。图在建它的下一个渲染帧之后才画（第一次使用的那一帧已提交）。
  - **纹理**：每次绘制后在 Godot 的帧图里从图拷贝过来，由 Godot 采样（`Texture2DRD`）；第一次画进去之后才显示。纹理带一个 sRGB 视图（Godot 的 `texture_rd_create` 会建它），Godot 在线性空间采样时用它解码。默认的 2D 画布在 sRGB 空间混合，和浏览器一样。
  - 尺寸变了就建一对新的，新的画好之前继续显示旧的。
- 渲染帧计数跟着 `RenderingServer.frame_pre_draw`（Godot 画一帧、提交之前）。

## 测试

```powershell
powershell -File scripts/RunTests.ps1                                # vulkan、d3d12 × safe、separate，都开验证层
powershell -File scripts/RunTests.ps1 -Drivers d3d12 -RenderThreads safe
powershell -File scripts/RunTests.ps1 -NoValidation
```

- 测试工程拷到临时目录（`-ProjectDir`，默认 `%TEMP%/weft-godot-tests`），在一个小窗口里跑 `tests/test_runner.gd`：画一个页面并在渲染线程读回纹理像素（位置、清空、首行在上、sRGB 编码与预乘）、事件/视图模型/调用的往返（整数、浮点、数组、向量与颜色）、给定的指针与命中测试、经 Godot GUI 的点击（点在按钮上归页面，点在空白处穿过 View 给下面的控件）、`url()` 图片与缺图诊断、`page.json` 文件夹、脚本异常、随控件变尺寸与缩放、停掉再起的运行时。
- 每次运行开 `--gpu-validation`：Vulkan SDK 的验证层（另开同步检查，查 weft 的提交与 Godot 帧之间的读写冒险）、D3D12 调试层。测试检查失败、Godot 打出任何错误（验证层的、weft 的、脚本的）、或超时，都算失败。日志在 `build/tests/<驱动>-<模式>.log`。
- 测试要 Godot 的编辑器可执行文件（`-Godot` 或环境变量 `GODOT`，默认 `D:/SteamLibrary/steamapps/common/Godot Engine/godot.windows.opt.tools.64.exe`）。

## 限制

- 只有 Windows x64；weft 有 Metal 等后端，接 macOS、Linux 时在 `render_*` 加对应的设备。
- 只有 Forward+ 与 Mobile 渲染器（Vulkan、D3D12）；Compatibility（OpenGL）不画。
- Vulkan 上扩展在 Godot 的渲染线程提交到 Godot 的主队列，但 Godot 的资源上传（transfer worker）可能从别的线程提交，它们用"能传输的能力最少的队列族"。在只有一个队列族兼做传输的 GPU 上那是同一个队列，而 Godot 提交时持有的锁扩展拿不到：这时在线程里加载资源与页面绘制同时进行不安全。扩展检测到这种 GPU 时打一条警告。NVIDIA、AMD 的独显有独立的传输队列族，不受影响。D3D12 的队列可以多线程提交，不受影响。
- 输入只有鼠标（移动、按键、滚轮、离开）；键盘、IME、触摸与手柄还没有。
- 同步函数（`weft.engine.invoke`）与引擎纹理（`-weft-texture()`）还没有接。
- 页面里的图片只有 PNG 背景图（`url()`），没有 `<img>`、JPEG 与 `data:` URL。
- 编辑器的类参考里还没有 `Weft`、`WeftView` 的文档（嵌入文档要另编一份 editor 目标的 DLL）。
- HDR 2D 与 3D 材质里用 `get_texture()` 的效果还没有测过。
