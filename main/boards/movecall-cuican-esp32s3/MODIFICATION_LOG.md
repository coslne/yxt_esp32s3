# 修改日记

## 2026-01-19
- **禁用显示屏**: 
  - 定义了 `NoBacklight` 类替代 `PwmBacklight`，用于空实现背光控制。
  - 在 `MovecallCuicanESP32S3` 构造函数中：
    - 注释掉了 `InitializeSpi()` (SPI总线初始化) 和 `InitializeGc9a01Display()` (GC9A01屏幕驱动初始化)。
    - 将 `display_` 初始化更改为 `new NoDisplay()`，使用空显示驱动。
    - 注释掉了 `GetBacklight()->RestoreBrightness()`，防止恢复背光亮度。
  - 修改 `GetBacklight()` 方法，使其返回 `NoBacklight` 静态实例，彻底禁用背光硬件操作。
