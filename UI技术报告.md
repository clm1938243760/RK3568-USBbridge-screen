# RK3568 USB Bridge UI 技术报告

## 1. 项目背景

本项目运行在 RK3568 嵌入式 Linux 开发板上，用于桥接 Windows 端医疗/报告软件与板端自动化流程。系统通过 USB Gadget 模拟键盘、鼠标和 USB 打印机，实现以下功能：

1. 扫码枪录入患者条码。
2. 板端访问患者信息 API。
3. 根据接口返回生成自动填表任务。
4. HID 模拟鼠标键盘在 Windows 软件中录入患者信息。
5. Windows 端打印报告到板端虚拟 USB 打印机。
6. 板端捕获打印数据并转换为 PDF。
7. 上传 PDF 到云端接口。

新增 UI 的目标是让操作者能够在 480×320 屏幕上实时看到当前流程状态，避免不知道系统正在扫码、查询、录入、等待打印还是上传。

## 2. UI 设计目标

UI 的核心目标不是做复杂交互，而是做一个清晰的“流程状态屏”。

设计目标如下：

- 开机后显示服务启动状态。
- 服务启动完成后显示等待扫码枪录入。
- 扫码后显示正在访问 API。
- API 成功后显示正在录入，并提醒不要操作鼠标键盘。
- API 失败后显示访问失败，并回到等待扫码。
- HID 录入完成后显示录入完成，等待打印报告。
- 打印报告接收后显示正在生成 PDF。
- PDF 上传中显示正在上传云端。
- 上传完成后显示上传完成，5 秒后回到初始等待扫码界面。

UI 尺寸固定为：

```text
480 × 320
```

这是为了适配板子上的小尺寸 LCD 屏幕，避免页面在嵌入式环境中出现布局漂移。

## 3. UI 总体架构

当前 UI 采用“状态文件驱动”的架构。

系统各业务进程不直接控制 UI，也不直接调用 Qt，而是统一写入一个 JSON 状态文件：

```text
/root/usb_bridge/state/ui_status.json
```

UI 程序每隔 500ms 读取这个状态文件，根据状态字段刷新画面。

架构如下：

```text
扫码程序 scan_patient_capture
        │
        ├── 写 ui_status.json: scan / error / ready_input / idle
        │
HID录入程序 hid_executor
        │
        ├── 写 ui_status.json: inputting / wait_print / idle
        │
打印捕获程序 printer_capture
        │
        ├── 写 ui_status.json: wait_print / printing / print_done / error
        │
报告上传程序 report_uploader
        │
        ├── 写 ui_status.json: converting / uploading / done / idle / error
        │
Qt Quick UI
        │
        └── 每 500ms 读取 ui_status.json 并刷新界面
```

这种方式的优点是：

- UI 与业务逻辑解耦。
- 任意业务进程都可以更新 UI 状态。
- UI 崩溃不会直接影响扫码、HID 录入、打印捕获和上传。
- 调试简单，直接查看 `ui_status.json` 就能知道当前状态。

## 4. 相关文件说明

### 4.1 `usb_bridge_ui.qml`

路径：

```text
/root/usb_bridge/ui/usb_bridge_ui.qml
```

工程源码中对应：

```text
usb_bridge_ui.qml
```

作用：

- 负责绘制 480×320 图形界面。
- 使用 Qt Quick 2.12。
- 每 500ms 读取 `/root/usb_bridge/state/ui_status.json`。
- 根据 `phase` 字段切换标题、提示语、颜色和进度条。

### 4.2 `bridge_ui_qml`

路径：

```text
/root/usb_bridge/bin/bridge_ui_qml
```

作用：

- 这是一个 shell 启动脚本。
- 负责设置 Qt 运行环境。
- 启动 `qmlscene` 或 `qml` 加载 QML 页面。

核心环境变量：

```sh
QT_QPA_PLATFORM=linuxfb:fb=/dev/fb0:size=480x320
QT_QUICK_BACKEND=software
QT_OPENGL=software
QT_QPA_FB_FORCE_FULLSCREEN=1
```

这些配置的目的：

- 使用 Linux framebuffer 显示，而不是桌面窗口系统。
- 使用软件渲染，避免 OpenGL/EGL 在嵌入式启动阶段不稳定。
- 强制全屏显示。

### 4.3 `bridge_ui.c`

路径：

```text
/root/usb_bridge/bin/bridge_ui
```

作用：

- 这是 C 语言 fallback UI。
- 当 Qt/QML 无法运行时，作为备用界面。
- 直接写 `/dev/fb0` framebuffer。
- 支持 `mmap()`，如果 `mmap()` 失败则退回 `write()`。

限制：

- 字体能力较弱。
- 主要显示英文状态。
- 不如 QML 界面美观。

### 4.4 `S98usbbridge`

路径：

```text
/etc/init.d/S98usbbridge
```

作用：

- 启动并监督所有业务进程。
- 优先启动 `bridge_ui_qml`。
- 如果 `bridge_ui_qml` 不存在，则启动 C 版 `bridge_ui`。
- 如果 QML 连续失败，可回退到 C 版 framebuffer UI。

相关逻辑：

```sh
if [ "${BRIDGE_UI_DISABLE:-0}" != "1" ]; then
    if [ -x "$BIN/bridge_ui_qml" ] && [ -f "$BASE/ui/usb_bridge_ui.qml" ]; then
        start_worker bridge_ui "$BIN/bridge_ui_qml"
    else
        start_worker bridge_ui "$BIN/bridge_ui"
    fi
fi
```

临时关闭 UI：

```sh
BRIDGE_UI_DISABLE=1 /etc/init.d/S98usbbridge restart
```

## 5. 状态文件设计

UI 状态文件路径：

```text
/root/usb_bridge/state/ui_status.json
```

典型内容：

```json
{
  "time": "2026-05-07 10:30:00",
  "phase": "idle",
  "title": "等待扫码",
  "message": "请使用扫码枪扫描患者ID",
  "scan_text": "",
  "patient_id": "",
  "report_no": ""
}
```

字段说明：

| 字段 | 说明 |
|---|---|
| `time` | 状态更新时间 |
| `phase` | 当前流程阶段，UI 主要根据它切换画面 |
| `title` | 后端写入的标题，主要用于调试或备用 |
| `message` | 后端写入的提示语，主要用于调试或备用 |
| `scan_text` | 当前扫码内容 |
| `patient_id` | 患者 ID |
| `report_no` | 报告号 |
| `project` | 可选字段，检查项目 |

## 6. 状态阶段说明

当前 UI 主要识别以下 `phase`：

| phase | 含义 | UI 显示 |
|---|---|---|
| `service_start` | 服务启动中 | 服务启动中 |
| `idle` | 等待扫码 | 等待扫码枪录入 |
| `scan` | 已扫码，正在访问 API | 正在访问接口 |
| `select_project` | 多项目选择 | 请选择检查项目 |
| `ready_input` | 患者信息已获取，准备录入 | 正在自动录入 |
| `inputting` | HID 正在录入 | 正在自动录入，请勿操作鼠标键盘 |
| `wait_print` | 录入完成，等待打印 | 录入完成 |
| `printing` | 正在接收 USB 打印数据 | 正在接收报告 |
| `print_done` | 打印数据已接收 | 正在转存 PDF |
| `converting` | Ghostscript 正在转 PDF | 正在转存 PDF |
| `uploading` | 正在上传云端 | 正在上传云端 |
| `done` | 上传完成 | 上传完成 |
| `error` | 流程异常 | 流程异常 |

## 7. UI 状态流转

完整状态流转如下：

```text
service_start
    ↓
idle
    ↓ 扫码枪输入
scan
    ↓ API 成功
ready_input
    ↓ HID 开始录入
inputting
    ↓ HID 录入完成
wait_print
    ↓ Windows 端打印报告
printing
    ↓ 打印数据接收完成
print_done
    ↓ Ghostscript 转换
converting
    ↓ 转换成功
uploading
    ↓ 上传成功
done
    ↓ 5 秒后
idle
```

异常流：

```text
scan
    ↓ API 失败
error
    ↓ 3 秒后
idle
```

```text
printing / converting / uploading
    ↓ 失败
error
```

## 8. 后端如何更新 UI

所有 C 程序通过公共函数更新 UI：

```c
ui_status_update(
    "phase",
    "标题",
    "提示文字",
    scan_text,
    patient_id,
    report_no
);
```

该函数定义在：

```text
usb_bridge_common.c
```

声明在：

```text
usb_bridge_common.h
```

它会生成 JSON 并原子写入：

```text
/root/usb_bridge/state/ui_status.json
```

原子写入的好处是：

- UI 不会读到半截 JSON。
- 多进程更新时更稳定。
- 即使 UI 正在刷新，也不会破坏状态文件。

## 9. 各业务进程中的 UI 更新点

### 9.1 扫码程序 `scan_patient_capture`

扫码后：

```c
ui_status_update("scan", "已读取扫码ID", "正在查询患者信息...", scan, "", "");
```

接口失败：

```c
ui_status_update("error", "查询失败", "接口没有返回患者记录，请重新扫码", scan, "", "");
sleep(3);
ui_status_update("idle", "等待扫码", "请使用扫码枪扫描患者ID", "", "", "");
```

接口成功：

```c
ui_status_update(
    "ready_input",
    "患者信息已匹配",
    "准备自动录入，请勿操作键盘鼠标",
    scan,
    patient_id,
    report_no
);
```

### 9.2 HID 录入程序 `hid_executor`

开始录入：

```c
ui_status_update("inputting", "正在自动录入", "请勿操作键盘鼠标", scan_text, patient_id, report_no);
```

录入完成：

```c
ui_status_update("wait_print", "录入完成", "请确认信息并等待打印报告", scan_text, patient_id, report_no);
```

### 9.3 打印捕获程序 `printer_capture`

等待打印：

```c
ui_status_update("wait_print", "等待打印", "请在电脑端打印当前患者报告", ...);
```

接收打印数据：

```c
ui_status_update("printing", "正在接收打印数据", "请勿断开USB连接", ...);
```

打印数据接收完成：

```c
ui_status_update("print_done", "打印数据已接收", "正在准备生成PDF并上传", ...);
```

### 9.4 报告上传程序 `report_uploader`

生成 PDF：

```c
ui_status_update("converting", "正在生成PDF", "打印数据转换中", ...);
```

上传中：

```c
ui_status_update("uploading", "正在上传报告", "请保持网络连接", ...);
```

上传完成：

```c
ui_status_update("done", "上传完成", "可以继续扫描下一位患者", ...);
sleep(5);
ui_status_update("idle", "等待扫码", "请使用扫码枪扫描患者ID", "", "", "");
```

## 10. QML 页面原理

QML 页面根节点是：

```qml
Rectangle {
    width: 480
    height: 320
}
```

这表示界面固定为 480×320。

核心属性：

```qml
property string phase: "service_start"
property string scanText: "-"
property string patientId: "-"
property string reportNo: "-"
property string selectedProject: ""
property int progress: 0
```

这些属性由状态文件驱动。

### 10.1 状态读取

QML 使用 `XMLHttpRequest` 读取本地文件：

```qml
xhr.open("GET", "file:///root/usb_bridge/state/ui_status.json?t=" + Date.now())
```

后面加 `Date.now()` 是为了避免缓存，确保每次读取最新状态。

读取成功后：

```qml
var s = JSON.parse(xhr.responseText)
root.phase = s.phase || "idle"
root.scanText = s.scan_text || "-"
root.patientId = s.patient_id || "-"
root.reportNo = s.report_no || "-"
```

### 10.2 定时刷新

QML 使用 Timer 每 500ms 刷新：

```qml
Timer {
    interval: 500
    running: true
    repeat: true
    onTriggered: {
        timeText.text = Qt.formatTime(new Date(), "hh:mm:ss")
        loadStatus()
    }
}
```

这样 UI 不需要后端主动通知，只要状态文件变化，最多 500ms 后界面就会更新。

### 10.3 标题和提示语

标题由 `phase` 决定：

```qml
function titleText() {
    if (phase === "idle") return "等待扫码枪录入"
    if (phase === "scan") return "正在访问接口"
    ...
}
```

提示语同理：

```qml
function messageText() {
    if (phase === "inputting") return "已读取信息，正在自动录入，请勿操作鼠标键盘"
    ...
}
```

### 10.4 进度条

进度由 `phase` 映射：

```qml
function calcProgress() {
    if (phase === "idle") return 0
    if (phase === "scan") return 18
    if (phase === "inputting") return 65
    if (phase === "uploading") return 95
    if (phase === "done") return 100
}
```

进度条宽度绑定：

```qml
width: Math.max(10, parent.width * progress / 100)
```

并使用动画：

```qml
Behavior on width {
    NumberAnimation { duration: 350; easing.type: Easing.OutCubic }
}
```

## 11. UI 布局设计

页面分为 5 个区域：

1. 顶部标题栏。
2. 主状态卡片。
3. 扫码 ID 信息卡片。
4. 进度条。
5. 底部流程提示。

### 11.1 顶部标题栏

显示：

```text
USB Bridge
当前时间
```

### 11.2 主状态卡片

根据状态改变颜色：

- `error`：红色，表示异常。
- `ready_input` / `inputting`：黄色，提示不要操作鼠标键盘。
- `select_project`：蓝色，表示用户选择。
- 其他状态：深蓝/绿色，表示正常流程。

### 11.3 扫码信息区

显示：

```text
扫码ID
患者ID
报告号
```

这些字段来自 `ui_status.json`。

### 11.4 进度条

根据流程阶段显示整体进度。

进度含义不是精确百分比，而是流程位置：

```text
扫码 -> 查询 -> 录入 -> 上传
```

### 11.5 底部流程提示

默认显示：

```text
流程：扫码枪录入 -> API查询 -> HID自动录入 -> 打印报告 -> PDF上传
```

如果选择了项目，则显示当前项目。

## 12. 启动流程

开机后系统启动脚本：

```text
/etc/init.d/S98usbbridge
```

执行流程：

1. 创建目录。
2. 写入初始 `ui_status.json`，状态为 `service_start`。
3. 启动 UI。
4. 等待 `/dev/hidg0`、`/dev/hidg1`、`/dev/g_printer0`。
5. 启动 HID 录入程序。
6. 启动扫码程序。
7. 启动打印捕获程序。
8. 启动报告上传程序。

UI 启动优先级：

```text
bridge_ui_qml
    ↓ 如果不存在或失败
bridge_ui
```

## 13. QML 启动参数

`bridge_ui_qml` 中设置：

```sh
QT_QPA_PLATFORM=linuxfb:fb=/dev/fb0:size=480x320
QT_QUICK_BACKEND=software
QT_OPENGL=software
QT_QPA_FB_FORCE_FULLSCREEN=1
```

含义：

| 参数 | 作用 |
|---|---|
| `QT_QPA_PLATFORM` | 指定 Qt 输出到 Linux framebuffer |
| `fb=/dev/fb0` | 指定 framebuffer 设备 |
| `size=480x320` | 指定界面尺寸 |
| `QT_QUICK_BACKEND=software` | 使用软件渲染 |
| `QT_OPENGL=software` | 避免依赖硬件 OpenGL |
| `QT_QPA_FB_FORCE_FULLSCREEN=1` | 强制全屏 |

如果屏幕不是 `/dev/fb0`，可以这样启动：

```sh
QT_QPA_PLATFORM='linuxfb:fb=/dev/fb1:size=480x320' /root/usb_bridge/bin/bridge_ui_qml
```

## 14. 部署方式

源码目录中执行交叉编译：

```sh
make clean
make CC=aarch64-linux-gnu-gcc
```

上传到板子：

```sh
scp -r * root@板子IP:/root/c_usb_bridge/
```

板子上安装：

```sh
cd /root/c_usb_bridge
sh install_to_board.sh
```

安装后关键文件：

```text
/root/usb_bridge/bin/bridge_ui_qml
/root/usb_bridge/bin/bridge_ui
/root/usb_bridge/ui/usb_bridge_ui.qml
/root/usb_bridge/state/ui_status.json
/etc/init.d/S98usbbridge
```

## 15. 手动测试 UI

停止已有 UI：

```sh
pkill -f bridge_ui_qml
pkill -f bridge_ui
```

手动启动 QML UI：

```sh
/root/usb_bridge/bin/bridge_ui_qml
```

查看日志：

```sh
tail -n 100 /root/usb_bridge/logs/bridge_ui_qml.log
```

测试状态切换：

```sh
cat > /root/usb_bridge/state/ui_status.json <<'EOF'
{
  "time": "test",
  "phase": "inputting",
  "title": "正在自动录入",
  "message": "请勿操作鼠标键盘",
  "scan_text": "TEST001",
  "patient_id": "P001",
  "report_no": "R001"
}
EOF
```

如果 UI 正常，屏幕应在 500ms 内切换到录入状态。

## 16. 常见问题与排查

### 16.1 开机后 QML 一直 Aborted

现象：

```text
bridge_ui exited rc=134
Aborted
```

可能原因：

- Qt 启动过早，framebuffer 尚未稳定。
- Qt Quick 依赖模块缺失。
- `linuxfb` 参数不匹配。
- QML 文件语法错误。
- 中文编码异常导致 QML 字符串未闭合。

排查：

```sh
tail -n 100 /root/usb_bridge/logs/bridge_ui_qml.log
```

临时关闭 UI：

```sh
BRIDGE_UI_DISABLE=1 /etc/init.d/S98usbbridge restart
```

手动延迟启动：

```sh
sleep 10
/root/usb_bridge/bin/bridge_ui_qml &
```

### 16.2 屏幕没有显示

检查 framebuffer：

```sh
ls -l /dev/fb*
cat /proc/fb
```

如果不是 `/dev/fb0`，指定正确设备：

```sh
QT_QPA_PLATFORM='linuxfb:fb=/dev/fb1:size=480x320' /root/usb_bridge/bin/bridge_ui_qml
```

### 16.3 QML 不读取状态

检查文件：

```sh
cat /root/usb_bridge/state/ui_status.json
```

检查权限：

```sh
ls -l /root/usb_bridge/state/ui_status.json
```

### 16.4 中文乱码

QML 文件必须以 UTF-8 保存。

如果看到类似：

```text
绛夊緟鎵...
```

说明文件编码被错误转换过。需要用 UTF-8 重新保存 QML 文件。

### 16.5 UI 影响业务启动

UI 与业务是分离的。可以临时禁用 UI：

```sh
BRIDGE_UI_DISABLE=1 /etc/init.d/S98usbbridge restart
```

业务仍会继续运行：

```text
scan_patient_capture
hid_executor
printer_capture
report_uploader
```

## 17. 与 C framebuffer UI 的对比

| 项目 | QML UI | C framebuffer UI |
|---|---|---|
| 中文显示 | 支持，依赖 Qt 字体 | 不完善 |
| 动画 | 支持 | 简单 |
| 布局 | 灵活 | 手写坐标 |
| 依赖 | Qt Quick/QML | 只依赖 framebuffer |
| 稳定性 | 受 Qt 环境影响 | 更底层，更少依赖 |
| 美观度 | 高 | 一般 |
| 推荐用途 | 正式界面 | 备用界面 |

当前推荐：

- 正常使用：`bridge_ui_qml`
- Qt 异常时：`bridge_ui`
- 调试业务时：可禁用 UI

## 18. 后续优化建议

1. 修复并统一所有源码文件编码为 UTF-8。
2. 给 QML 启动增加开机延迟，避免 framebuffer 尚未准备好。
3. 增加项目选择结果回写机制，目前 QML 中项目选择只在界面本地变化。
4. 增加 UI 状态超时判断，例如长时间停在 `printing` 时提示检查打印。
5. 增加网络状态显示，例如 API 可达性、上传接口可达性。
6. 增加错误码字段，例如 `error_code`、`error_message`，方便现场定位。
7. 如果屏幕触摸可用，可以将项目选择从演示逻辑接入后端流程。

## 19. 总结

本 UI 方案采用 Qt Quick + JSON 状态文件驱动。业务进程只负责写状态，QML 负责读取状态并渲染界面。该方案结构清晰、解耦程度高、易于调试，适合 RK3568 这类嵌入式 Linux 场景。

当前系统还保留 C framebuffer UI 作为兜底方案。这样即使 Qt/QML 因系统环境、启动时序或依赖问题无法运行，仍可以通过底层 framebuffer 显示基础状态。

整体设计满足以下现场需求：

- 操作者能看到服务是否启动。
- 操作者能知道是否可以扫码。
- 自动录入时明确提示不要操作鼠标键盘。
- 录入完成后提示等待打印。
- PDF 上传完成后提示完成并自动回到等待扫码。
- 接口失败时提示失败并回到扫码流程。

