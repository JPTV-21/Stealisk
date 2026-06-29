# Stealisk

> 一个配置驱动的 USB 文件复制工具。

## 功能

- USB 设备插入自动检测（`WM_DEVICECHANGE` + 定时扫描）
- 可配置的文件过滤（目录名/文件名黑白名单，支持通配符）
- AES-256 加密（Windows Crypto API，可选）
- 去重机制（SHA-1 哈希，跨会话持久化）
- 目标目录隐藏（可选）
- 随机化文件名（可选）
- 托盘菜单
- 独立控制台界面，键盘操作
- 开机自启
- 自定义进程文件名及路径（支持绝对/相对/纯文件名）
- worker 资源剥离
- 缓存目录可配置
- 多语言支持
- 配置文件热重载
- RAII 资源管理，线程安全优化
- 所有运行时文件默认存放在 `.\cache\`（可自定义）

## 免责声明

**使用本软件即表示您已阅读、理解并同意以下条款：**

1. **用途声明**  
   Stealisk 是一款面向**合法数据管理场景**的工具，设计用途包括但不限于：
   - 个人文件的自动化备份与归档
   - 系统管理员对可移动存储设备的合规数据同步
   - 经明确授权的安全测试与渗透测试（需持有书面授权）
   - 其他不违反所在地法律法规的数据操作

2. **禁止行为**  
   严禁将本软件用于以下任何目的：
   - 未经授权访问、复制、窃取或监控他人的存储设备及数据
   - 商业间谍、隐私侵犯、敲诈勒索或其他违法犯罪活动
   - 规避企业安全策略或破坏信息系统完整性
   - 任何违反当地相关法律法规的行为

3. **责任归属**  
   - 本软件按“现状”（AS IS）提供，**不提供任何明示或暗示的担保**，包括但不限于适销性、特定用途适用性及不侵权的保证。
   - 作者及贡献者 **不对任何因使用或无法使用本软件而造成的直接、间接、偶然、特殊或 consequential 损害承担任何责任**，即使已被告知可能发生此类损害。
   - **使用者须独立承担因使用本软件而产生的一切法律、道德及安全责任**，包括但不限于因配置不当、误用、滥用或违反法律而引发的任何后果。

4. **默认配置**  
   本软件默认配置下，**所有自动复制、加密、开机自启及后台静默运行功能均处于关闭状态**。用户必须手动编辑配置文件后方可启用任何功能，此设计旨在**防止误操作和意外数据泄露**。

5. **法律遵从**  
   使用者有责任确保对本软件的使用：
   - 完全符合所在地、所在国家/地区及适用国际法律的规定
   - 已获得所有必要的授权和同意
   - 不侵犯任何第三方的合法权益

**如果您无法遵守上述条款，请立即停止使用并删除本软件。**

可别拿这玩意去拷别人没让你拷的存储设备了！小心被干爆。

## 默认行为

**所有非必须功能如自动复制、加密、开机自启等，默认均为关闭状态。** 你需要手动编辑 `Stealisk.ini` 才能启用任何复制、加密或隐藏行为。

## 编译

使用提供的 `compile.bat` 编译。

## 使用

1. 运行 `Stealisk.exe`（会自动生成 `Stealisk.ini` 和 `cache` 目录）
2. 编辑 `Stealisk.ini` 按需配置
3. 插入 USB，程序将会按配置工作

### 启动参数

| 参数 | 说明 |
|------|------|
| 无参数 | 启动 Controller（托盘 + 自动显示 Console） |
| `--no-console` | 开机/无控制台模式，显示托盘但不自动弹出 Console |
| `--console` | 独立启动控制台界面（由 Controller 自动调用，一般无需手动） |
| `--worker` | 启动后台 Worker 服务（由 Controller 自动调用，一般无需手动） |

### 控制台菜单（Console 窗口）

- `[0]` 启动/停止服务（Worker 进程）
- `[1]` 立即开始/停止一次扫描
- `[2]` 编辑配置（打开 notepad）
- `[3]` 隐藏控制台窗口（进程继续运行）

### 托盘菜单

- 启动/停止服务
- 立即开始/停止一次扫描
- 编辑配置
- 显示/隐藏控制台
- 退出（完全退出程序，同时停止 Worker 和 Controller）

## 文件结构

```
程序目录/
├── Stealisk.exe          # 主程序（Controller/Worker/Console 三合一）
├── Stealisk.ini          # 配置文件（用户可编辑）
├── cache/                # 默认缓存目录（可配置 cache_dir）
│   ├── Stealisk.log      # 普通日志
│   ├── Stealisk_debug.log # 调试日志
│   ├── Stealisk.pid      # Worker PID
│   ├── Stealisk_hashes.db # 已复制文件哈希
│   └── Stealisk_scanning.lock # 扫描锁
├── target/               # 默认目标目录（可配置 target_dir）
└── [worker 指定路径]     # Worker 可执行文件（由 Controller 自动生成）
```

## 配置说明

配置文件 `Stealisk.ini` 位于程序目录下，包含三个配置节：`[Filter]`、`[Options]` 和 `[Encryption]`。

---

### [Filter] 文件过滤器

| 键 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `case_sensitive` | bool | `false` | `true` 匹配时区分大小写，`false` 不区分 |
| `include_dirs` | 列表 | (空) | 逗号分隔，只扫描匹配的目录名（支持 `*` 通配符），留空=全部 |
| `exclude_dirs` | 列表 | `System Volume Information,$Recycle.Bin` | 逗号分隔，跳过匹配的目录名 |
| `include_files` | 列表 | (空) | 逗号分隔，只复制匹配的文件名（支持 `*` 通配符），留空=全部 |
| `exclude_files` | 列表 | `*.tmp,~$*,*.bak,desktop.ini,Thumbs.db,.DS_Store` | 逗号分隔，跳过匹配的文件名 |

**通配符示例：**
- `*` 匹配任意字符
- `*.tmp` 匹配所有 `.tmp` 文件
- `~$*` 匹配所有以 `~$` 开头的文件（Office 临时文件）
- `report_*.pdf` 匹配 `report_2024.pdf`、`report_final.pdf` 等

---

### [Options] 运行选项

| 键 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `auto_copy` | bool | `false` | `true` 插入 USB 后自动复制，`false` 需手动点“开始扫描” |
| `show_tray` | bool | `true` | `true` 显示托盘图标，`false` 隐藏 |
| `enable_encrypt` | bool | `false` | `true` 复制时用 AES-256 加密文件，需配置 `key` |
| `auto_start_boot` | bool | `false` | `true` 开机自启（根据 `show_tray` 决定以 `--no-console` 或 `--worker` 模式启动） |
| `auto_start_service` | bool | `false` | `true` 启动 Controller 时自动启动 Worker |
| `randomize_filename` | bool | `false` | `true` 复制后的文件名随机化（保留扩展名） |
| `hide_target_dir` | bool | `false` | `true` 目标目录设为隐藏，`false` 可见 |
| `scan_delay_seconds` | int | `1` | USB 插入后延迟 N 秒再开始扫描（给系统挂载时间） |
| `scan_interval_seconds` | int | `15` | 定时扫描间隔（秒），设为 0 可关闭定期扫描 |
| `target_dir` | string | (空) | 目标目录路径，支持 `%USERPROFILE%` 等环境变量，留空=程序目录下的 `target` |
| `worker` | string | `Stealisk_worker.exe` | **Worker 可执行文件路径。** 支持纯文件名（自动基于程序目录）、相对路径（如 `.\sub\worker.exe`）、绝对路径（如 `D:\path\worker.exe`） |
| `cache_dir` | string | (空) | **缓存目录路径。** 支持相对路径（如 `.\mycache`）或绝对路径（如 `D:\Cache`）。留空默认使用 `程序目录\cache` |
| `strip_resources` | bool | `false` | **是否剥离 Worker 资源。** `true` 会删除 Worker 中的版本信息和图标，使文件更小、更隐蔽；`false` 则保留完整资源。修改此项后，下次启动 Worker 时会根据新配置重新生成 Worker 文件，即可还原被剥离的资源 |
| `language` | string | `zh-CN` | 界面语言，内置 `zh-CN` 和 `en-US` |

---

### [Encryption] 加密配置

| 键 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `key` | string | 示例值 | AES-256 密钥，64 位十六进制字符（32 字节）。仅当 `enable_encrypt = true` 时生效 |

**生成随机密钥（PowerShell）：**
```powershell
-join ((1..32) | ForEach-Object { '{0:X2}' -f (Get-Random -Maximum 256) })
```

---

### 配置迁移与废弃键处理

当 `Stealisk.ini` 中存在不再被程序识别的配置项时（即不在内置默认模板中），程序会**以注释形式保留**这些键值对（行首添加 `; `），而不是删除或忽略。这样既避免了配置丢失，又保证了主配置的整洁。

例如，旧版 `worker_name` 会被自动注释，并提示用户使用新的 `worker` 选项。

所有废弃键的处理记录会写入 `Stealisk_debug.log`，方便您追溯。

---

### 配置示例

**只复制 `.docx` 和 `.pdf` 文件，排除临时文件，启用加密，自定义缓存路径，并剥离 Worker 资源：**

```ini
[Filter]
case_sensitive = false
include_dirs = 
exclude_dirs = System Volume Information,$Recycle.Bin
include_files = *.docx,*.pdf
exclude_files = *.tmp,~$*

[Options]
auto_copy = true
enable_encrypt = true
randomize_filename = true
target_dir = D:\Backup
scan_delay_seconds = 3
worker = .\bin\backuper.exe       # 相对路径
cache_dir = D:\MyStealiskCache    # 自定义缓存
strip_resources = true            # 剥离资源使 Worker 更隐蔽
language = zh-CN

[Encryption]
key = 1a2b3c4d5e6f7890abcdef1234567890abcdef1234567890abcdef1234567890
```

## 许可证

MIT