# MaaPiCli

MaaFramework 官方提供的命令行 ProjectInterface Client，用于通过终端交互加载 `interface.json`、配置任务并运行。

## 获取

从 [MaaPiCli Releases](https://github.com/MaaXYZ/MaaPiCli/releases) 下载对应平台的 `MAA-MaaPiCli-<os>-<arch>-<version>.zip`。

## 快速开始

将 MaaPiCli 及其运行库放入包含 `interface.json` 的项目目录，然后在终端启动：

```bash
# 首次运行：交互式配置并运行任务
./MaaPiCli

# 使用已保存的配置直接运行
./MaaPiCli -d
```

Windows 下使用 `MaaPiCli.exe`。程序以 MaaFramework 运行库所在目录作为项目资源和用户数据目录，而不是以终端当前工作目录为准；通常该目录也是可执行文件所在的项目目录。用户配置保存在该目录下的 `config/maa_pi_config.json`。首次使用时请先通过交互模式完成配置，再使用 `-d`。

## 本地编译

仓库根目录的 `maafw-version.txt` 锁定本版本 MaaPiCli 使用的 MaaFramework release tag。fork、历史 tag 或 release 包中都带有这个文件，可以直接确认应使用的 runtime 版本。

编译需要 CMake 3.28 或更新版本、支持 C++20 的编译器，以及 Boost（system、regex）和 OpenCV（core、imgproc、imgcodecs）的开发包。Linux 还需要安装 `libssl-dev` 或发行版等价的 OpenSSL 开发包。

先查看 `maafw-version.txt`，从 [MaaFramework Releases](https://github.com/MaaXYZ/MaaFramework/releases) 下载对应版本的 `MAA-<os>-<arch>-<version>.zip`。解压后将其目录和其他依赖的安装前缀传入 `CMAKE_PREFIX_PATH`：

```bash
cat maafw-version.txt
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH="/path/to/maafw-prefix;/path/to/dependency-prefix" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --config RelWithDebInfo -j 16
```

## 贡献

由 AI 编写或辅助编写的 PR，请在 PR 描述末尾注明本次实际使用的 AI 编程工具（Agent）、模型名称或版本和思考强度（reasoning effort）。多个 Agent 或模型参与时，请分别列出对应信息。

```text
AI 使用信息：
- Agent：<AI 编程工具名称>
- 模型：<模型名称或版本>
- 思考强度：<思考强度>
```

AI 助手提交 PR 时还需遵循 [AGENTS.md](AGENTS.md) 中的要求。

如需构建并运行测试，可在配置时开启测试选项：

```bash
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH="/path/to/maafw-prefix;/path/to/dependency-prefix" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DMaaPiCli_BUILD_TESTS=ON
cmake --build build --config RelWithDebInfo -j 16
ctest --test-dir build -C RelWithDebInfo --output-on-failure
```

## PI 协议支持

下表列出当前支持的协议能力及其限制。Agent 通过 `PI_INTERFACE_VERSION` 声明协议扩展能力。

| 协议能力 | MaaPiCli 状态 |
|----------|--------------|
| 基础协议：项目信息、controller、resource、task、select/input/switch option、多资源加载与 pipeline override | ✅ 支持 |
| `controller.attach_resource_path` 附加资源；`import` 拆分导入 PI 片段 | ✅ 支持 |
| `checkbox` 多选；`option.controller/resource` 适用范围；全局、resource 级、controller 级 option；多级 option；`preset`；`import` 导入 preset；`focus.display` 渠道声明 | ⚠️ 部分支持：option 与 preset 可用；`focus.display` 因未处理 focus 回调而无效 |
| option 及其子 option 的 controller/resource 适用性过滤 | ✅ 支持 |
| 顶层 `group` 分组声明与 `task.group`；`import` 导入 group | ✅ 支持 |
| Agent 子进程 `PI_*` 环境变量：Client/UI 版本、语言、项目版本、当前 controller/resource 等 | ✅ 支持 |
| `resource.hash` 资源完整性校验；校验发生在 `resource.path` 加载后、`attach_resource_path` 加载前 | ✅ 支持 |
| `pretask` 在 Controller 启动前执行自定义程序；可把 option 当前值序列化为最后一个参数；`import` 导入 pretask | ✅ 支持 |
| `setting` 任务设置页 UI 声明；`hotkey` 快捷键 option；`import` 导入 `global_option` 与 `setting` | ⚠️ 部分支持：导入的 `global_option` 会合并并生效；`setting` 会解析与合并但不渲染；`hotkey` 未实现 |
| `pretask.controller/resource` 适用范围过滤 | ✅ 支持 |
| `telemetry.sentry` 匿名遥测配置：DSN、tracing、事务采样率、环境标签 | ❌ 未实现，不读取和上报遥测 |
| `focus` 模板对象的 `trace` 字段，按回调消息控制节点结果遥测 | ❌ 未实现；CLI 当前也未处理 focus 回调 |
| `telemetry.sentry.failure_attachments_sample_rate` 失败诊断附件采样率 | ❌ 未实现 |
| `input.inputs[].password` 标记密码/密钥输入；要求掩码显示、配置加密存储、日志/遥测脱敏、pretask 传参时内存中解密 | ✅ 支持：CLI 隐藏输入并掩码展示；Windows 使用 DPAPI、macOS 使用 Keychain、Linux 使用 AES-GCM 加密配置；pretask 与 pipeline 使用内存明文 |
| `checkbox` 的 `min_count` / `max_count`，限制可选和必选数量 | ✅ 支持：解析配置约束，在交互中阻止超出上限；已保存数量不足时要求补选，数量超限时清理选择 |
| `welcome` 支持非空字符串数组，按声明顺序展示多条公告，完整顺序或内容更新后重新展示 | ✅ 支持：兼容旧字符串写法；持久化公告声明与解析后的内容快照 |
| `preset.pretask` 设置预任务勾选状态与选项值 | ✅ 支持：预设声明时按其列表重设勾选项与配置值；未声明时保留现有预任务设置 |
| `preset.controller` 限制预设适用的控制器 | ✅ 支持：隐藏不适用于当前控制器的预设；未声明或空列表时不限制 |
| `input.inputs[].input_type` 指定 `text` / `file` / `directory` 编辑控件 | ⚠️ 降级支持：文件和目录显示路径提示，使用文本输入；保留默认值、正则校验和 `pipeline_type` 转换，不要求路径存在，不做分隔符转换或 `~` 展开 |
| `PI_USER_DATA_DIR` 指定 Client 与 Agent 共用的用户数据目录 | ✅ 支持：向每个 Agent 传入加载配置时确定的用户数据目录绝对路径；与 Agent 的资源工作目录分别处理 |

包含尚未支持字段的配置通常仍可被解析并加载其既有功能；具体支持情况见上表。使用 `hotkey` option 的项目可能无法得到预期交互，应优先为 CLI 提供其他 option 类型。

## 推理运行环境

MaaFramework 支持 WebGPU 推理后端；官方依赖包不再提供 CoreML。MaaPiCli 使用框架默认的 `Auto` 后端选择：依次尝试 DirectML、WebGPU，最后回退 CPU。该顺序在 Windows 上同样适用。官方 MaaDeps Windows x86_64 / aarch64 依赖包编译启用了 DirectML 和 WebGPU；使用这些包时，`Auto` 优先选择 DirectML。macOS 和 Linux 可使用 WebGPU。

部署时请保留发行包中的 ONNX Runtime、WebGPU Dawn 及其他运行库。显式选择推理后端或设备的 CLI 配置目前尚未提供。

## Linux 控制器

MaaPiCli 支持解析 ProjectInterface 的 `controller.linux` 配置，创建 `MaaLinuxControllerCreate` 控制器，并支持 `controller.display_expand` 截图缩放。

CLI 可组合以下 Linux 截图和输入方式：

| 能力 | 配置值 | 说明 |
|------|--------|------|
| 截图 | `Wlr` | 需要 Wayland socket 与合成器的 `wlr-screencopy-unstable-v1` 支持 |
| 截图 | `PipeWire` | `pipewire_source: Gamescope` 自动发现 gamescope 节点；`Portal` 通过 xdg-desktop-portal 打开 ScreenCast 流 |
| 输入 | `Wlr` | 需要 wlroots 虚拟键盘和虚拟指针协议，会提示输入 Wayland socket |
| 输入 | `UInput` | 需要访问 `/dev/uinput`，会提示输入绝对坐标范围的宽度和高度 |
| 输入 | `Libei` | 需要 EIS socket，会提示输入其路径；文本输入需要系统 libei 支持文本输入 |

`controller.linux.use_win32_vk_code` 可把 Win32 Virtual-Key 键码转换为 Linux evdev 键码。Linux 运行环境仍需按 MaaFramework 文档准备 Wayland、PipeWire、Portal、libei 或 uinput 相关权限和服务。

## iOS 控制器

需要使用包含 iOS 控制器的 MaaFramework SDK 与运行库，并安装 xdb Server 1.6.0 或更新版本。安装步骤见 [XDeviceBridge 文档](https://github.com/scrctl/XDeviceBridge/blob/main/docs/INSTALL_zh.md)。
先用 xdb 完成设备配对和 DDI 准备：

```bash
xdb pair --wifi
xdb devices -l
xdb -s '<UDID>' ddi prepare --wifi auto
```

在项目的 `interface.json` 中声明 `"type": "IOS"`，运行 MaaPiCli 后选择该控制器。
CLI 可以发现设备或手动输入 UDID，选择 Auto、USB 或 Wi-Fi 连接，以及 `Stream`、`ScreenshotService` 或两者组合的取图方式。
UDID、连接方式、取图方式和可选的 `config.server_port` 保存在 `config/maa_pi_config.json`；配对凭据与 DDI 由 xdb 管理。

```json
{
    "name": "iOS",
    "type": "IOS",
    "ios": {
        "udid": "<UDID>",
        "connection": "WiFi",
        "config": { "wifi_address": "auto" },
        "screencap": "Stream|ScreenshotService"
    }
}
```

`ios` 中的字段是项目默认值。`connection` 默认为 `Auto`；`config.wifi_address` 留空时优先 USB，再尝试已配对的 Wi-Fi，填 `"auto"` 时只尝试 Wi-Fi 自动发现，填设备 IP 时只尝试指定 Wi-Fi 地址。`WiFi` 模式下地址留空或填 `"auto"` 均使用 Wi-Fi 自动发现；`USB` 模式忽略 Wi-Fi 地址。CLI 地址提示中按回车保留当前值，输入 `-` 清空地址。
完整字段说明见 [MaaFramework iOS 控制方式](https://github.com/MaaXYZ/MaaFramework/blob/main/docs/zh_cn/2.4-%E6%8E%A7%E5%88%B6%E6%96%B9%E5%BC%8F%E8%AF%B4%E6%98%8E.md#ios)。

## CLI 局限性

以下 PI 协议功能由于终端环境的固有限制，无法实现或仅提供降级支持：

### 无法实现

| 功能 | 涉及字段 | 原因 |
|------|----------|------|
| 图标显示 | 所有层级的 `icon` 字段（项目、控制器、资源、任务、选项、case、分组、预设） | CLI 无法显示图片 |
| Markdown 渲染 | `contact`、`license`、`welcome`、`description` 等支持 Markdown 的文本字段 | CLI 以纯文本原样输出，不渲染 Markdown 语法 |
| focus 通知渠道 | `focus.display` 的 `toast`、`notification`、`dialog`、`modal` 渠道 | CLI 不具备 toast/系统通知/弹窗能力 |
| 分组折叠 | `group.default_expand` | CLI 无折叠/展开概念，始终平铺显示 |
| 软件更新 | `github` 字段的版本检查与自动更新 | CLI 仅展示 GitHub 地址，不提供更新功能 |
| 资源分发 | `mirrorchyan_rid`、`mirrorchyan_multiplatform` | 资源包分发管理不属于 CLI 职责 |

### 降级实现

| 功能 | 协议行为 | CLI 实际行为 |
|------|----------|-------------|
| `focus` 回调消息 | Client 注册回调，按 `display` 渠道分发模板消息 | 未注册回调，不处理 focus 消息 |
| `group.description` | 显示分组的详细描述 | 仅显示分组名称/label |
| task 禁用态显示 | 不满足 resource/controller 约束的 task 灰显 | 直接过滤不显示 |
| option 禁用态显示 | 不满足约束的 option 灰显 | 直接跳过不提示 |
| `setting` / `hotkey` | 渲染设置页分区；捕获快捷键并映射虚拟按键码 | `setting` 解析并合并但不渲染；`hotkey` 未实现 |
| `input_type: file/directory` | 系统文件/目录选择对话框及可编辑路径 | 终端文本输入并显示路径提示，路径字符串原样保存与传递 |
| `telemetry` 与 `focus.trace` | 经用户授权后向 Sentry 上报崩溃、任务与指定节点结果 | 不集成遥测 |
