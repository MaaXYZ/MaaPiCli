# MaaPiCli

MaaFramework 官方提供的命令行 ProjectInterface Client，用于通过终端交互加载 `interface.json`、配置任务并运行。

## 获取

从 MaaFramework Release 下载对应平台的 `MAA-MaaPiCli-<os>-<arch>` 独立包。`MaaPiCli` 不再包含在 `MAA-*` 主包中。

## 本地编译

仓库根目录的 `maafw-version.txt` 锁定本版本 MaaPiCli 使用的 MaaFramework release tag。fork、历史 tag 或 release 包中都带有这个文件，可以直接确认应使用的 runtime 版本。

编译前先读取该文件，并从 [MaaFramework Releases](https://github.com/MaaXYZ/MaaFramework/releases) 下载同名版本的 `MAA-<os>-<arch>-<version>.zip`。解压后把该目录传入 `CMAKE_PREFIX_PATH`，同时按平台安装 Boost 与 OpenCV 的开发配置。Linux 还需要安装 `libssl-dev` 或发行版等价的 OpenSSL 开发包。

```bash
maafw_version="$(cat maafw-version.txt)"
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH="/path/to/maafw-prefix;/path/to/dependency-prefix" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --config RelWithDebInfo -j 16
```

## PI 协议支持版本

MaaPiCli 以 PI v2.6.0 为基线，额外支持 v2.7.0 引入的 `pretask`、v2.8.1 的 pretask 适用范围过滤、v2.10.0 的密码输入和 v2.10.1 的 `checkbox` 数量限制。PI 语义版本与 MaaFramework release 版本、`interface.json` 中的 `interface_version: 2` 是三套不同概念。

`maafw-version.txt` 当前锁定的 MaaFramework `v5.13.0` 文档定义到 PI v2.10.1。下表列出协议能力，便于对照 MaaPiCli 的实际实现状态。

| PI 版本 | 协议新增/变更 | MaaPiCli 状态 |
|---------|----------------|--------------|
| v2.1.0 | 基础协议：项目信息、controller、resource、task、select/input/switch option、多资源加载与 pipeline override | ✅ 支持 |
| v2.2.0 | `controller.attach_resource_path` 附加资源；`import` 拆分导入 PI 片段 | ✅ 支持 |
| v2.3.0 | `checkbox` 多选；`option.controller/resource` 适用范围；全局、resource 级、controller 级 option；多级 option；`preset`；`import` 导入 preset；`focus.display` 渠道声明 | ⚠️ 部分支持：option 与 preset 可用；`focus.display` 因未处理 focus 回调而无效 |
| v2.3.1 | 明确 option 及其子 option 的 controller/resource 适用性过滤 | ✅ 支持 |
| v2.4.0 | 顶层 `group` 分组声明与 `task.group`；`import` 导入 group | ✅ 支持 |
| v2.5.0 | Agent 子进程 `PI_*` 环境变量：Client/UI 版本、语言、项目版本、当前 controller/resource 等 | ✅ 支持 |
| v2.6.0 | `resource.hash` 资源完整性校验；校验发生在 `resource.path` 加载后、`attach_resource_path` 加载前 | ✅ 支持 |
| v2.7.0 | `pretask` 在 Controller 启动前执行自定义程序；可把 option 当前值序列化为最后一个参数；`import` 导入 pretask | ✅ 支持 |
| v2.8.0 | `setting` 任务设置页 UI 声明；`hotkey` 快捷键 option；`import` 导入 `global_option` 与 `setting` | ⚠️ 部分支持：导入的 `global_option` 会合并并生效；`setting` 会解析与合并但不渲染；`hotkey` 未实现 |
| v2.8.1 | `pretask.controller/resource` 适用范围过滤 | ✅ 支持 |
| v2.9.0 | `telemetry.sentry` 匿名遥测配置：DSN、tracing、事务采样率、环境标签 | ❌ 未实现，不读取和上报遥测 |
| v2.9.1 | `focus` 模板对象的 `trace` 字段，按回调消息控制节点结果遥测 | ❌ 未实现；CLI 当前也未处理 focus 回调 |
| v2.9.2 | `telemetry.sentry.failure_attachments_sample_rate` 失败诊断附件采样率 | ❌ 未实现 |
| v2.10.0 | `input.inputs[].password` 标记密码/密钥输入；要求掩码显示、配置加密存储、日志/遥测脱敏、pretask 传参时内存中解密 | ✅ 支持：CLI 隐藏输入并掩码展示；Windows 使用 DPAPI、macOS 使用 Keychain、Linux 使用 AES-GCM 加密配置；pretask 与 pipeline 使用内存明文 |
| v2.10.1 | `checkbox` 新增 `min_count` / `max_count`，限制可选和必选数量 | ✅ 支持：解析配置约束，在交互中阻止超出上限；已保存数量不足时要求补选，数量超限时清理选择 |

由于 `interface_version` 仍为 `2`，包含 v2.6.0 及以后新增字段的配置通常仍可被解析并加载其既有功能；但这些新增字段不会被 MaaPiCli 启用。使用 `hotkey` option 的项目可能无法得到预期交互，应优先为 CLI 提供其他 option 类型。

## Linux 控制器

MaaFramework v5.13.0 新增 Linux 控制器。MaaPiCli 支持解析 ProjectInterface V2 的 `controller.linux` 配置，创建 `MaaLinuxControllerCreate` 控制器，并支持 `controller.display_expand` 截图缩放。

CLI 可组合以下 Linux 截图和输入方式：

| 能力 | 配置值 | 说明 |
|------|--------|------|
| 截图 | `Wlr` | 需要 Wayland socket 与合成器的 `wlr-screencopy-unstable-v1` 支持 |
| 截图 | `PipeWire` | `pipewire_source: Gamescope` 自动发现 gamescope 节点；`Portal` 通过 xdg-desktop-portal 打开 ScreenCast 流 |
| 输入 | `Wlr` | 需要 wlroots 虚拟键盘和虚拟指针协议，会提示输入 Wayland socket |
| 输入 | `UInput` | 需要访问 `/dev/uinput`，会提示输入绝对坐标范围的宽度和高度 |
| 输入 | `Libei` | 需要 EIS socket，会提示输入其路径；文本输入建议系统 libei >= 1.6.0 |

`controller.linux.use_win32_vk_code` 可把 Win32 Virtual-Key 键码转换为 Linux evdev 键码。Linux 运行环境仍需按 MaaFramework 文档准备 Wayland、PipeWire、Portal、libei 或 uinput 相关权限和服务。

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
| `welcome` 变更追踪 | Client 记录已展示内容，内容更新时重新弹窗 | 每次启动都展示，不追踪变更 |
| `focus` 回调消息 | Client 注册回调，按 `display` 渠道分发模板消息 | 未注册回调，不处理 focus 消息 |
| `group.description` | 显示分组的详细描述 | 仅显示分组名称/label |
| task 禁用态显示 | 不满足 resource/controller 约束的 task 灰显 | 直接过滤不显示 |
| option 禁用态显示 | 不满足约束的 option 灰显 | 直接跳过不提示 |
| `setting` / `hotkey` | 渲染设置页分区；捕获快捷键并映射虚拟按键码 | `setting` 解析并合并但不渲染；`hotkey` 未实现 |
| `telemetry` 与 `focus.trace` | 经用户授权后向 Sentry 上报崩溃、任务与指定节点结果 | 不集成遥测 |

## 用法

```bash
# 交互模式
./MaaPiCli

# 直接运行（使用已保存的配置）
./MaaPiCli -d
```

工作目录应为包含 `interface.json` 的项目根目录（即 MaaPiCli 可执行文件所在目录）。  
用户配置保存在 `config/maa_pi_config.json`。
