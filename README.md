# 使用方法

## 1. 环境准备
1. 安装 esp-idf 5.3 或以上版本 (推荐 5.5)
2. 打开 esp-idf 5.5 cmd 或 esp-idf 5.5 powershell 终端

## 2. 编译与运行

### 进入项目目录
```shell
cd (文件夹路径)\yxt_code
```

### 方式一：使用脚本快速编译（推荐）
使用 `release.py` 脚本可以自动完成目标设定、配置和编译打包。

**编译 esp32s3 璀璨吊坠版:**
```shell
python scripts/release.py movecall-cuican-esp32s3
```

**编译 面包板开发版:**
```shell
python scripts/release.py bread-compact-wifi
```
编译完成后，固件压缩包会生成在 `releases/` 目录下。

### 方式二：手动开发调试
如果需要进行代码开发和调试，建议按以下步骤操作：

1. **设置目标芯片**
   如果是 S3 开发板 (如璀璨吊坠)：
   ```shell
   idf.py set-target esp32s3
   ```
   如果是经典 ESP32：
   ```shell
   idf.py set-target esp32
   ```

2. **配置菜单**
   ```shell
   idf.py menuconfig
   ```
   进入 `Xiaozhi Assistant` -> `Board Type` 选择对应的开发板类型。
   
   > 提示：`movecall-cuican-esp32s3` 对应 "Movecall CuiCan 璀璨AI吊坠"

3. **编译烧录监控**
   ```shell
   idf.py build flash monitor
   ```

## 3. 目录结构说明
* `main/boards/`: 包含各开发板的引脚定义和初始化代码
* `scripts/release.py`: 自动化编译可以在此查看具体逻辑

## 4. 常见问题排查 (Troubleshooting)

### 清空 Flash (Erase Flash)
如果遇到系统无法启动、分区表错误、乱码或需要完全重置设备配置（如清除 WiFi 信息）时，可以尝试清空 Flash。

**操作步骤：**
1. 确保设备已连接电脑。
2. 执行以下命令：
   ```shell
   idf.py erase-flash
   ```
3. 清空完成后，需要重新编译并烧录固件：
   ```shell
   idf.py flash monitor
   ```

### 串口无法连接/烧录失败
- **检查驱动**：确保电脑已安装对应的串口驱动（常见如 CH340, CP210x, FTDI 等）。
- **检查线材**：确认使用的 USB 线支持数据传输，而不仅仅是充电线。
- **进入下载模式**：如果自动烧录失败，尝试手动进入下载模式：
  1. 按住开发板上的 `BOOT` (或 `0`) 键不放。
  2. 短按 `RESET` (或 `EN`) 键重置。
  3. 松开 `BOOT` 键。
  4. 再次尝试执行烧录命令。

### 编译报错
- **环境问题**：确保使用的是 ESP-IDF 5.3 或更高版本。
- **缓存问题**：如果修改了 `sdkconfig` 或切换了 Target 后出现奇怪的编译错误，尝试清理构建文件：
  ```shell
  idf.py fullclean
  ```
  然后再重新编译。
