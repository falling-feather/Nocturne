# 夜航 / Nocturne

**所见所思，杂而成章。** 原生 C++ / Qt 桌面笔记软件，用笔记、便签与待办整理工作内容。

[下载 Windows x64 即用包](https://github.com/falling-feather/Nocturne/releases/download/v0.2.0/Nocturne-0.2.0-windows-x64.zip) · [全部 Releases](https://github.com/falling-feather/Nocturne/releases) · [问题反馈](https://github.com/falling-feather/Nocturne/issues)

## 下载与启动

下载 **Nocturne-0.2.0-windows-x64.zip**，完整解压到固定目录，双击 `Nocturne.exe`。无需安装 Qt、Python 或开发工具；请保留随包 DLL 和插件目录。

这是解压即用包，不是安装向导。关闭主窗口会驻留托盘，“文件 → 退出夜航”才会完全退出。当前仍为预发布版，尚未进行代码签名。

![夜航主题](doc/image/Nocturne-redesign-night.png)

另有[雾港](doc/image/Nocturne-redesign-harbor.png)、[月白](doc/image/Nocturne-redesign-moonlight.png)和[专注模式](doc/image/Nocturne-redesign-focus.png)。截图使用隔离示例资料。

## v0.2.0 功能

| 能力 | 使用方式 |
| --- | --- |
| 多级目录 | 创建、移动、重命名分组，以目录树组织笔记 |
| 文档导入 | 导入 MD、TXT、HTML 或整个文件夹，保留层级及来源识别，适配常见文本编码 |
| Markdown | H1—H6、列表、引用、代码块和常见行内格式，支持 Markdown 插入与导出 |
| 文段待办 | 选中文字右键“设置待办”；点击标记查看明细，支持单项删除 |
| 图片 | 等比适应正文；单击编辑可选图注，双击打开大图并缩放 |
| 表格 | 手动指定行列、粘贴 Markdown 表格；单元格右键可增删行列 |
| 选区转表格 | 右键或“插入”菜单；识别制表符、Markdown 管道和连续空格，已有无边框表格可就地整理；Ctrl+Z 撤销 |
| 原创界面 | 夜航、雾港、月白三主题，专注模式、原创弹窗、中文编辑菜单，无浏览器内核 |
| 统一资料位置 | 日常库移出可能受打包宿主重定向的 AppData；文件菜单可打开实际资料目录 |

同时保留自动保存、搜索、回收站、多枚置顶便签、可配置全局快捷键、“收舟入册”与每日备份。

## 资料与升级

v0.2.0 日常库位于：

```text
%USERPROFILE%/NocturneData/
├── notebook.sqlite3
├── attachments/
└── backups/
```

可通过“文件 → 打开当前资料目录”确认。软件与资料目录分开；移动软件不会移动笔记。

首次升级且新目录没有数据库时，程序从旧 `FeatherNote/FeatherNote` AppData 目录创建一致性快照、复制附件并调整图片引用。旧资料保留；新目录已经存在时不会用旧库覆盖。若历史上由打包宿主启动，旧 AppData 可能实际指向宿主缓存；多份旧库须分别备份核对，不应只比较路径文字或数量。个人资料不会打进发布包。

更新前先正常退出旧夜航、保留备份，再替换程序文件。本项目开发验收固定使用 `dist/Nocturne-desktop/Nocturne.exe`，所有更新替换同一日常入口。

## 备份与恢复

“文件 → 立即备份本地资料”创建 SQLite 一致性快照、附件与 `backup.json` 清单，并检查 `quick_check`。自动备份保留最近 7 份，手动备份保留最近 10 份。

恢复前完全退出程序，另存当前资料，再恢复所需数据库及附件。不要只复制运行中的主库：最新内容可能仍在 WAL 中。备份为本地明文资料，不属于云同步。

## 快捷操作

| 操作 | 快捷键 |
| --- | --- |
| 新建笔记 | Ctrl+N |
| 搜索 | Ctrl+K |
| 专注 | F11 / Esc |
| 全局新建便签 | Ctrl+Alt+N，可在设置中修改 |
| 收舟入册 | Ctrl+Shift+B |
| 导入文件 | Ctrl+O |
| 导出笔记 | Ctrl+Shift+S |

## 构建与测试

C++17、Qt 6 Widgets / SQL、CMake 3.21+、Ninja。Qt 最低 6.5；已验证环境为 Windows UCRT64、Qt 6.10.1。不要混用不同 ABI。

```powershell
.\build.ps1 -Configuration Release
.\build.ps1 -Configuration Release -Package
```

中间产物位于 `%LOCALAPPDATA%/NocturnePrototypeBuild`，分发包位于 `dist/artifacts/`。脚本运行持久化迁移、数据库性能、真实 QWidget UI 与本地备份四项测试，覆盖文档、目录、待办、图片交互与表格。

测试使用独立资料。应用测试/基准参数需显式进程环境 `NOCTURNE_ALLOW_TEST_PROFILE=1`，不要持久写入用户环境。

包内含 `licenses/`、`THIRD_PARTY_NOTICES.md` 和精确版本源码链接；Release 提供 `SHA256SUMS.txt`。最终软件运行无需 Python。

## 当前边界

仅面向 Windows；尚无云同步、多人协作、加密或 DOCX 导入。Markdown 与 Qt 富文本并不完全等价，复杂 HTML/CSS、嵌套及合并表格往返可能存在差异。选区转表格识别规则结构，不推测任意文章的语义行列。

原生 Qt 仍有基础内存成本；开发机短时性能样本不能代表所有机器。当前实现及验证见[开发者文档](doc/01-开发者文档.md)，计划见[项目规划](doc/02-项目规划.md)，提交记录见[开发历史](doc/03-开发历史.md)。
