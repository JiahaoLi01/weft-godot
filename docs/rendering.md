# 渲染架构与当前边界

## 1. 源码模块划分

`native/src/` 按职责拆分为独立模块：

| 模块 | 职责 |
| --- | --- |
| `owner_thread` | 16 MiB 栈深的 Weft 专属逻辑线程（满足 React / D3 等深调用栈需求） |
| `page_host` | 独立于 Godot 的运行时与 View 生命周期管理（支持两阶段安全停机） |
| `page_source` | 通过 Godot `FileAccess` 读取页面清单与 `url()` 资源（兼容导出 `.pck`） |
| `variant_pack` | Godot `Variant` 与 MessagePack 的无损双向编解码 |
| `surface` / `render_bridge` | 双纹理管理、渲染线程任务派发、帧计数与统计 |
| `render_device` / `render_vulkan` / `render_d3d12` | 基于 Godot 设备的命令缓冲录制、提交与 Fence 资源回收 |
| `weft_server` / `weft_view` / `register_types` | 单例 `Weft`、控件 `WeftView` 与 GDExtension 注册入口 |

## 2. 线程模型与每帧流转

1. **主线程（`WeftView._process`）**：同步控件尺寸与缩放，向 Owner 线程派发页面推进并触发 GDScript 信号。事件、视图模型、异步调用完成、锚点写入与帧快照获取直接走 Weft 任意线程接口。
2. **渲染线程（`RenderingServer.call_on_render_thread`）**：仅在页面帧发生变化、动画运行或锚点移动时触发绘制；在单线程渲染模式下运行于主线程，在独立渲染线程模式下按帧顺序排入渲染命令队列。
3. **帧计数同步**：`Weft` 单例监听 `RenderingServer.frame_pre_draw`，在 Godot 提交帧前递增渲染帧计数，用于追踪资源回收周期。

## 3. Vulkan 与 D3D12 双纹理同步

由于 GDExtension 不直接暴露 Godot 正在录制的命令缓冲，扩展在 Godot 的 GPU 设备上录制独立命令缓冲并提交到主队列，通过 Fence 追踪完成时机。

为避免扩展与 Godot 帧图（Render Graph）对纹理布局/状态产生冲突，每个 `WeftView` 在当前尺寸下维护两张 8 位 `BGRA UNORM` 纹理：

1. **绘制目标（Draw Surface）**：新建时先触发一次异步读回，使其进入 `TRANSFER_SRC` / 拷贝源状态；此后每次绘制前切为颜色附件，绘制完成后切回拷贝源状态，保持与 Godot 帧图预期完全一致。
2. **展示纹理（Display Texture）**：每次绘制完成后由 Godot 帧图从绘制目标拷贝过来，经 `Texture2DRD` 提供给 2D 画布或 3D 材质采样（附带 sRGB 视图以供线性空间正确解码；2D 画布默认在 sRGB 空间做预乘混合，与浏览器视觉一致）。
3. **平滑缩放**：控件尺寸变化时新建一对纹理，在新纹理完成首次绘制前继续展示旧纹理，避免闪烁。

## 4. 当前边界与已知限制

- **平台与渲染器**：当前仅支持 Windows x64 下的 Forward+ 与 Mobile 渲染器（Vulkan / D3D12）；Compatibility（OpenGL）不支持。
- **Vulkan 多线程传输队列注意项**：在仅具备单一通用队列族（无独立 Transfer 队列族）的 GPU 上，Godot 后台资源上传线程可能与扩展共用同一 Vulkan 队列提交。扩展启动时会检测该情况并输出警告；主流 NVIDIA / AMD 独显具备独立传输队列族，以及 D3D12 驱动均不受影响。
- **输入范围**：当前已接入鼠标（移动、按键、滚轮、离开与穿透命中）；键盘、IME 输入法、触控与手柄尚未接入。
- **桥接与扩展能力**：暂未接入同步函数 `weft.engine.invoke`、引擎纹理（`-weft-texture()`）、自定义着色器效果（`-weft-effect()`）与 DevTools 面板。
- **编辑器文档与 HDR**：尚未为编辑器内置类参考生成 `Weft` / `WeftView` 嵌入文档；HDR 2D 与 3D 材质下使用 `get_texture()` 的表现尚未覆盖测试。
