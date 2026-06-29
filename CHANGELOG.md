# Changelog

所有值得注意的更改都将记录在此文件中。

格式遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.0.0/)，版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## [2.0.0] - 2026-06-29

### Added
- 三进程架构：Controller、Console、Worker 独立运行，职责分离
- 独立 Console 进程，支持 `--console` 参数单独启动
- `--no-console` 启动模式，开机自启时无控制台窗口
- 控制台键盘操作菜单（0: 切换服务、1: 手动扫描、2: 编辑配置、3: 隐藏控制台）
- 回车键刷新控制台菜单
- 配置项 `cache_dir`，支持自定义缓存目录路径
- 配置项 `strip_resources`，可剥离 Worker 可执行文件的版本信息和图标
- 配置项 `hide_target_dir`，支持隐藏或显示目标目录
- 配置项 `include_dirs`，支持按目录名过滤（通配符匹配）
- 配置项 `include_files`，支持按文件名过滤（通配符匹配）
- 配置项 `exclude_dirs` 和 `exclude_files` 支持通配符匹配（`*`、`*keyword*`、`*.ext`）
- 配置项 `worker` 支持相对路径、绝对路径、纯文件名及环境变量展开（如 `%APPDATA%`）
- 配置文件废弃键自动注释功能（行首添加 `;`），保留用户配置历史
- Controller 与 Console 进程间通过 `WM_COPYDATA` + 结构化命令通信
- RAII 句柄包装类 `FileHandle` 和 `RegKeyHandle`
- 日志文件添加 UTF-8 BOM 头
- Worker 路径自动规范化及 `.exe` 后缀补全
- Worker 路径与主程序路径冲突自动检测及规避
- `GetValue<T>` 泛型配置获取方法，支持 `std::string`、`bool`、`int`、`std::wstring` 类型
- `std::atomic` 替换 `volatile bool` 保证线程安全

### Changed
- 架构从双进程（Controller + Worker）重构为三进程（Controller + Console + Worker）
- 控制台从 Controller 内置功能拆分为独立子进程
- 进程间通信从直接窗口消息升级为 `WM_COPYDATA` 结构化命令
- 配置管理从手动解析升级为泛型模板方法
- 文件过滤器从固定 `extensions`/`name_contains` 升级为完整的黑/白名单体系
- 配置键 `worker_name` 迁移为 `worker`
- 配置键 `extensions` 迁移为 `include_files`
- 配置键 `name_contains` 迁移为 `include_files`（结合通配符 `*keyword*`）
- 日志系统增加错误码记录
- `PerformScan` 增加 `hide_target_dir` 参数控制目录隐藏
- 配置文件写入逻辑优化，先写临时文件再替换，避免损坏
- `StopWorker` 增加超时等待和重试机制
- 编译目标从单一可执行文件改为支持多启动模式

### Fixed
- 修复 Worker 停止时可能无法正常退出的问题
- 修复控制台清屏时 `ACCESS_DENIED` 错误
- 修复配置热重载时定时器间隔未更新的问题
- 修复旧版 Worker 进程残留导致的 PID 文件冲突
- 修复路径中包含空格时启动参数解析错误
- 修复日志文件过大时读写异常
- 修复 `IsWorkerRunning` 在 Worker 崩溃后返回错误状态
- 修复多实例运行时资源竞争问题

### Removed
- 移除 Controller 内置控制台功能（迁移至独立 Console 进程）
- 移除配置键 `worker_name`（迁移至 `worker`）
- 移除配置键 `extensions`（迁移至 `include_files`）
- 移除配置键 `name_contains`（迁移至 `include_files`）
- 移除 `GetNameContainsW()` 和 `GetExtensions()` 方法
- 移除 `volatile bool` 全局标志（迁移至 `std::atomic`）

---

## [1.0.0] - 2026-06-21 (初始版本)

### Added
- USB 设备插入自动检测（`WM_DEVICECHANGE` + 定时扫描）
- 文件过滤功能（扩展名过滤、文件名关键词匹配）
- 文件名匹配支持大小写敏感/不敏感切换
- AES-256 加密（Windows Crypto API）
- 去重机制（SHA-1 哈希，跨会话持久化）
- 目标目录自动创建及隐藏
- 随机化文件名功能
- 系统托盘图标及右键菜单
- 开机自启（注册表 `Run` 项）
- 多语言支持（`zh-CN`、`en-US`）
- 配置文件热重载
- 日志记录（普通日志及调试日志）
- PID 文件管理防止多实例
- Worker 进程自动生成及管理
- 资源文件提取（默认配置、语言文件）
- UTF-8 ↔ UTF-16 转换函数
- 全局错误模式（`SetErrorMode`）
- 双进程架构（Controller + Worker）
- 控制台交互菜单（启动/停止服务、手动扫描、编辑配置、导出日志、退出）
- 支持 `--worker` 命令行参数

---

[2.0.0]: https://github.com/JPTV-21/Stealisk/compare/v1.0.0...v2.0.0
[1.0.0]: https://github.com/JPTV-21/Stealisk/releases/tag/v1.0.0