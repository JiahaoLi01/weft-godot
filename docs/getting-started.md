# 接入、页面与导出

本文介绍如何编译安装 `weft-godot`、使用 `WeftView` 和 `Weft` 单例接口，以及正确配置 Godot 项目的页面资源导出。

## 1. 编译与安装

构建环境要求：

- **CMake 3.25+** 与 **Visual Studio 2022**
- **Python 3**（用于 `godot-cpp` 生成绑定）
- **Vulkan SDK 1.4.363.0**（用于编译 Weft Vulkan 后端着色器）
- 本地 [Weft](https://github.com/JiahaoLi01/weft) 源码签出

在仓库根目录执行：

```powershell
powershell -File scripts/BuildWeft.ps1 -WeftSource F:/Projects/weft
```

不传 `-WeftSource` 时会依次查找环境变量 `WEFT_SOURCE_DIR` 以及本仓库同级目录 `weft`。`godot-cpp` 会由 CMake 自动拉取固定版本（10.0.0，含 SHA256 校验）；生成的 DLL 静态链接 C++ 运行库，不引入额外运行库依赖。

> Godot 编辑器加载扩展 DLL 后不会主动释放句柄；重新编译前请先关闭打开了本插件的编辑器。

编译产物位于 `addons/weft/bin/`（并会自动同步一份到 `demo/addons/weft`）。将整个 `addons/weft` 复制到目标项目的 `addons/` 目录后，首次打开项目时 Godot 会自动识别 `weft.gdextension`（无需在插件列表中手动勾选启用）。

> 项目渲染器必须选择 **Forward+** 或 **Mobile**；Compatibility（OpenGL）不含 `RenderingDevice`，扩展无法绘制并会输出错误日志。

## 2. WeftView 与 Weft 单例 API

### 加载页面

在场景中添加 `WeftView`（继承自 `Control`），在检查器中将 `page` 设为页面目录（如 `res://ui/hud`），进入场景树（含编辑器实时预览）后即自动加载。也可在代码中动态切换：

```gdscript
view.load_page("res://ui/inventory")
view.load_source(js_code, css_code, "res://ui/inventory")
```

页面目录支持 `page.json` 清单，缺省时按 `page.js`、`page.css` 与可选 `keyframes.css` 加载：

```json
{
  "scripts": ["d3.js", "page.js"],
  "stylesheet": "page.css",
  "keyframes": "keyframes.css",
  "root": "#root",
  "crpgBridge": false
}
```

### 通信与 Variant 映射

`Variant` 会自动编码为 MessagePack 并保持整数精度；`Vector2` 转为 `{x, y}`，`Color` 转为 `{r, g, b, a}`：

- **页面 → Godot**：连接 `message_received(name, value)`、`call_received(call_id, name, value)`（配合 `complete_call(call_id, succeeded, value)` 返回结果）与 `diagnostic_received(kind, text)`。
- **Godot → 页面**：调用 `emit_event(name, value, latest)` 推送事件，调用 `set_model(model, fields)` 合并更新视图模型。

### 3D 世界锚点

在 HTML 元素上添加 `data-anchor="<key>"`（需显式指定宽度与 `position: absolute`），每帧在 `_process` 中写入投影后的控件坐标：

```gdscript
view.write_anchors(
    PackedInt64Array([1]),
    PackedVector2Array([camera.unproject_position(target.global_position)])
)
```

### 尺寸、纹理与鼠标输入

- **尺寸与高清缩放**：页面逻辑尺寸与 `Control` 尺寸保持 1:1；`device_scale`（默认 `1.0`）决定实际渲染纹理倍率（如 `2.0` 输出两倍清晰度）。
- **鼠标穿透**：当 `hit_test_input = true`（默认）时，`_has_point` 查询 Weft 命中测试：命中页面元素则拦截输入并同步 CSS `cursor`，命中空白处则事件穿过控件交给下层 UI 或 `_unhandled_input`。也可随时调用 `is_pointer_over_page()` 与 `hit_test(pos)`。
- **离屏与 3D 表面**：`get_texture()` 返回 `Texture2DRD`（sRGB 编码、预乘 Alpha、首行在上），配合 `CanvasItemMaterial.BLEND_MODE_PREMULT_ALPHA` 或着色器 `blend_premul_alpha` 渲染，并用 `send_pointer(kind, pos, buttons, wheel)` 转发 3D 射线指针事件。

### 字体与运行时控制

默认自动使用 Windows 系统字体。要注册项目自带字体，在首个页面加载之前（例如 Autoload 的 `_ready`）调用：

```gdscript
Weft.start()
Weft.add_font("Noto Serif SC", "res://fonts/NotoSerifSC.otf", 400)
Weft.add_fallback_family("Noto Serif SC")
```

对于已打包到 `.pck` 或被 Godot 导入为资源的字体，扩展会自动将其拷贝至 `user://weft/fonts` 后交由 Weft 读取。需要重启运行时或排查渲染错误时，可使用 `Weft.stop()`、`Weft.start()`、`Weft.get_build()` 与 `Weft.get_render_stats()`。

## 3. 页面图片与项目导出

CSS 中通过 `background-image: url(...)` 引用 PNG 图片：

- 相对路径基于样式表路径解析（例如 `res://ui/hud/page.css` 旁的 `../textures/icon.png`）。
- `/` 开头的路径从 `res://` 根解析；`user://` 等带协议路径原样读取。

导出独立游戏时，Godot 默认只打包已导入资源，因此需要两项配置：

1. **非资源文件白名单**：在导出预设的 **资源 → 导出非资源文件的过滤器** 中添加页面的 `.js`、`.css`、`.json`（例如 `ui/*.js, ui/*.css, ui/*.json`）。
2. **保留原始 PNG 文件**：页面引用的 PNG 若按默认方式导入成纹理，原始字节不会进入 `.pck`。在导入面板中将这些图片的导入器改为 **保留文件（按原样导出）**（即 `.import` 文件中为 `importer="keep"`，参考 `demo/ui/stripes.png.import`）。

## 4. 运行示例工程

执行 `scripts/BuildWeft.ps1` 后直接用 Godot 打开 `demo/` 运行即可体验按钮交互、异步调用、滚轮缩放图片、实时视图模型、3D 旋转方块锚点与空白区域穿透。

- 切换图形驱动：附加 `--rendering-driver vulkan` 或 `--rendering-driver d3d12`（或在项目设置修改 `rendering/rendering_device/driver.windows`）。
- 无界面截图：运行 `godot --path demo -- --screenshot=shot.png` 保存首帧画面后自动退出。
