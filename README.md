# FeatherNote

FeatherNote 是一款面向 Windows 的本地轻量笔记原型。它使用 C++17、Qt 6 Widgets 与 SQLite，目标是在大型游戏或其他高负载应用运行时也能常驻，并可随时用 `Ctrl+Alt+N` 呼出快速便签。

> 当前仓库是 `0.1.0` 原型，不是可托管重要资料的正式版本。首次测试前请先阅读[数据与备份](#数据与备份)和[原型方案与边界](docs/prototype-plan.md)。

项目当前实现、任务与提交历史分别以 [`doc/01-开发者文档.md`](doc/01-开发者文档.md)、[`doc/02-项目规划.md`](doc/02-项目规划.md) 和 [`doc/03-开发历史.md`](doc/03-开发历史.md) 为权威入口；本文继续作为构建、测试和备份的兼容入口。

## 当前原型

| 能力 | 当前范围 |
| --- | --- |
| 主笔记 | 新建、列表、搜索、编辑与软删除；编辑停止约 `650 ms` 后自动保存 |
| 富文本 | 粗体、斜体、下划线、标题、列表、字号和文字颜色 |
| 图片 | 从剪贴板粘贴、拖入本地图片或通过选择文件插入 |
| 文档交换 | HTML、Markdown、TXT 导入与导出；格式往返并不保证完全无损 |
| 快速便签 | `Ctrl+Alt+N` 全局快捷键呼出置顶便签；也可从主窗口或托盘打开 |
| 待办 | 新增、切换完成状态、清理已完成项目 |
| 后台驻留 | 关闭主窗口后驻留系统托盘；需要从托盘执行“退出”才会完全结束进程 |
| 本地存储 | SQLite 数据库与本地 `attachments` 图片目录；笔记删除采用软删除；无账号、无云同步 |

程序采用单实例运行。如果 FeatherNote 已经在后台运行，再次启动不会创建第二个数据库连接；请使用快捷键或托盘图标唤回。

## 构建与运行

已知开发环境为 Windows、`D:/msys64/ucrt64` 工具链、Qt `6.10.1`。项目本身要求 Qt `6.5` 或更新版本，并使用 CMake 与 Ninja。仓库路径包含中文时，MSYS2 的 `moc` 在部分区域设置下无法向源码旁的构建目录写文件，因此脚本会把**临时中间文件**放到纯英文的 `%LOCALAPPDATA%\FeatherNotePrototypeBuild`：

```powershell
.\build.ps1 -Configuration Release
& "$env:LOCALAPPDATA\FeatherNotePrototypeBuild\Release\FeatherNote.exe"
```

构建脚本同时运行 SQLite 持久化烟雾测试。需要跳过时可加 `-SkipTests`。不要在同一构建目录中混用 MSVC、MinGW 或不同 ABI 的 Qt。

## 打包可测试目录

直接运行构建目录中的 `FeatherNote.exe` 时，系统需要能从 `PATH` 找到 Qt 与 UCRT64 运行库。生成独立便携测试包：

```powershell
.\build.ps1 -Configuration Release -Package
& .\dist\FeatherNote-0.1.0-portable\FeatherNote.exe
```

MSYS2 版本的 `windeployqt` 没有完整复制 UCRT64 的非 Qt 依赖，因此仓库的打包脚本会递归扫描所选 EXE、Qt DLL 和插件，并只收集实际引用的 UCRT64 DLL。脚本不会把个人笔记打进发布目录。本轮生成的便携包已在不向 `PATH` 加入 Qt 的情况下独立启动，并完成 SQLite 重启恢复验证。

## 本轮验证快照

- Release 构建与链接通过；SQLite 持久化自动测试 `1/1` 通过。
- 人工/UI 冒烟通过：新建笔记、中文富文本输入、约 `650 ms` 自动保存、待办新增、重启恢复、全局快捷键呼出置顶便签、图片文件选择器打开。
- 便携包为 `36` 个文件、`75.9 MiB`；全部二进制依赖扫描均已解析。
- 2026-08-30 的一次短样本：主窗口稳定约 `102.9 MiB` 工作集、`78.4 MiB` 私有内存；10 秒内累计 `0.1406` CPU 秒（22 逻辑处理器上约占全机 `0.064%`）。这不是正式 10 分钟基准，但已经说明 Qt 原型的内存与包体超过目标，正式轻量版需要继续优化或做纯 Win32 A/B 样机。

## 数据与备份

应用使用 Qt 的 `QStandardPaths::AppLocalDataLocation`。按当前组织名和应用名，Windows 上的典型数据根目录为：

```text
%LOCALAPPDATA%\FeatherNote\FeatherNote\
```

其中数据库文件为 `notebook.sqlite3`，插入的图片保存在同一数据根目录下的 `attachments`。程序运行时还可能出现 SQLite 的 `notebook.sqlite3-wal` 和 `notebook.sqlite3-shm`；不要只复制主数据库文件来做在线备份。

当前原型**没有自动备份**。建议测试前完全退出 FeatherNote，再复制整个数据目录：

```powershell
$running = Get-Process -Name FeatherNote -ErrorAction SilentlyContinue
if ($running) {
  throw "FeatherNote 仍在运行；请先从系统托盘完全退出。"
}

$dataDir = Join-Path $env:LOCALAPPDATA "FeatherNote\FeatherNote"
if (-not (Test-Path -LiteralPath $dataDir)) {
  throw "尚未找到数据目录；请先启动一次 FeatherNote。"
}

$backupRoot = Join-Path $env:USERPROFILE "Documents\FeatherNote-Backups"
$backupDir = Join-Path $backupRoot (Get-Date -Format "yyyyMMdd-HHmmss")
New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
Copy-Item -LiteralPath $dataDir -Destination (Join-Path $backupDir "data") -Recurse

Get-ChildItem -LiteralPath (Join-Path $backupDir "data") -Force
```

恢复时也必须先从托盘完全退出程序；先给现有数据目录再做一份副本，然后用某个备份中的完整 `data` 目录替换数据目录，启动后核对笔记、图片和待办。备份是未加密的本地资料副本，不应上传到不可信位置。

## 已知边界

- 仅面向 Windows；全局快捷键依赖 Windows 系统能力，若已被其他程序占用，可能无法注册。
- Markdown、HTML 与富文本模型不完全等价，导入再导出可能丢失原型不支持的样式或结构。
- 尚不支持 DOCX、云同步、多人协作、加密、提醒通知、自动备份与数据库迁移承诺。
- 图片已初步外置保存，但正式版的附件去重、移动、孤儿清理和一致性恢复规则仍在规划中。
- 当前只有一次短时资源快照，不等于正式性能验收；目标、口径与“目标/实测”分栏见[原型方案](docs/prototype-plan.md#8-性能验收目标)。

## 下一步

计划优先验证常驻资源占用、快捷呼出和数据可靠性，再推进纯 Win32 可行性、附件外置工程化、Markdown 往返一致性与可选 DOCX 转换。详细范围见[原型方案与后续路线](docs/prototype-plan.md)。
