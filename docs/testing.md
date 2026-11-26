# 自动化测试

`weft-godot` 的测试工程位于 `tests/`（入口脚本 `tests/test_runner.gd`），由 `scripts/RunTests.ps1` 自动驱动多驱动与多渲染线程模式组合验证。

## 1. 运行测试

运行前需先通过 `scripts/BuildWeft.ps1` 编译扩展，并指定 Godot 编辑器可执行文件路径（通过 `-Godot` 参数或环境变量 `GODOT`）：

```powershell
# 全量组合：Vulkan & D3D12 × safe（单线程渲染）& separate（独立渲染线程），默认开启 GPU 验证层
powershell -File scripts/RunTests.ps1 -Godot "D:/Godot/godot.windows.opt.tools.64.exe"

# 指定单一驱动与渲染模式
powershell -File scripts/RunTests.ps1 -Drivers d3d12 -RenderThreads safe

# 关闭验证层加速本地回归
powershell -File scripts/RunTests.ps1 -NoValidation
```

测试脚本会将 `tests/` 复制到临时工作目录（默认 `%TEMP%/weft-godot-tests`，可通过 `-ProjectDir` 指定），并将各组合的运行日志写入 `build/tests/<driver>-<mode>.log`。

## 2. 校验范围

测试在真实渲染窗口中运行，并默认开启 `--gpu-validation`（Vulkan 验证层 + 同步冒险检查，以及 D3D12 调试层）：

- **像素回读校验**：在渲染线程读回显示纹理，验证几何位置、背景清屏、首行在上行序以及 sRGB 预乘像素值。
- **桥接往返**：验证事件、视图模型与异步调用中整数、浮点、数组、字典、`Vector2` 和 `Color` 的精确往返。
- **输入与穿透**：验证编程指针事件、命中测试，以及经由 Godot GUI 的点击穿透（点击页面按钮归页面，点击空白处落到下层 Godot 控件）。
- **页面与生命周期**：验证 `page.json` 清单、`url()` PNG 加载与缺图诊断、脚本异常捕获、控件尺寸与 `device_scale` 动态调整，以及 `Weft.stop()` / `Weft.start()` 运行时重建。

测试断言失败、触发任何 Godot / Weft / GPU 验证层错误或超时，均会判定该组合失败。
