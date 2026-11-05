#pragma once

/// @file IHostApi.h
/// @brief 插件 SPI 的**唯一对外承诺**（契约头）。
///
/// 这个文件只允许出现常量、枚举和注释 —— **不得包含任何实现头**，也不得被
/// 插件作者以外的代码拿去当内部依赖。它存在的意义是：把"我们承诺给插件作者
/// 什么"从散落在 `PluginHost.cpp` 的实现细节里抽出来，变成可审查、可版本化
/// 的一份事实。
///
/// 三层版本号，别混：
///
/// | 名字 | 管什么 | 现在 | 什么时候升 |
/// |---|---|---|---|
/// | `manifestVersion` | `plugin.json` 的**格式** | 1 | 增删必填字段、改字段语义 |
/// | `kApiVersion` | 插件能调用的**宿主 API 面** | 1 | 新增/删除 `host` 的方法或属性 |
/// | `engines.assistant` | 兼容的**宿主版本** | — | 由宿主发版决定 |
///
/// 兼容承诺（`kApiVersion` 不变时）：
/// * 只增不改：新增属性/方法允许，**删除或改变已有语义不允许**；
/// * 要改语义 → 升 `kApiVersion`，同时升 `manifestVersion`，旧插件拒绝加载并说明原因。
///
/// 对应文档：`docs/plugins/README.md`（面向作者）、
///          `docs/architecture/插件接口对外-可行性分析.md`（为什么这么切）。
///
/// 关联：`插件市场-模块框架.md` §4.5 —— 插件只能看到 `host`，拿不到任何宿主内部对象。
namespace contract {

/// 当前宿主实现的插件 API 版本。
///
/// 插件用 `host.apiVersion` 读取它；也可以用清单的 `engines.api` 声明区间。
/// **不要**用它替代 `engines.assistant`：前者管能力面，后者管宿主版本。
inline constexpr int kApiVersion = 1;

/// `host.apiVersion` 的 SemVer 形式（`1` → `"1.0.0"`），用于和 `engines.api` 比较。
/// 定义在这里而不是调用处，避免两边各拼一次拼歪。
inline constexpr char kApiVersionString[] = "1.0.0";

/// 清单 `permissions` 里允许出现的名字。
///
/// **v1 现状：这些只是"声明"，宿主不会因此拦截或放行任何调用。**
/// 它们的用途只有两个：装之前的知情同意、出问题时的排障线索。
/// 之所以先把名字固定下来，是为了让作者现在写的清单在将来真正启用能力检查时
/// 不用改字段 —— 但**不要**据此认为声明了就安全，也不要认为没声明就不能用。
namespace capability {

inline constexpr char kNetwork[]        = "network";          ///< 访问网络
inline constexpr char kFsRead[]         = "fs.read";          ///< 读取文件
inline constexpr char kFsWrite[]        = "fs.write";         ///< 写入文件
inline constexpr char kClipboardRead[]  = "clipboard.read";   ///< 读剪贴板
inline constexpr char kClipboardWrite[] = "clipboard.write";  ///< 写剪贴板
inline constexpr char kHotkey[]         = "hotkey";           ///< 注册全局热键
inline constexpr char kShell[]          = "shell";            ///< 打开外部程序/链接

} // namespace capability

/// 安装/装载失败的稳定错误码。
///
/// 插件作者可以用它们做分支（例如"是校验失败还是网络失败"），
/// 但**不要**把具体文案写死 —— 文案属于 UI，可能变。
namespace error {

inline constexpr char kInvalidSpec[]        = "invalid-spec";         ///< 安装源无法识别
inline constexpr char kNotFound[]           = "not-found";            ///< 索引/文件不存在
inline constexpr char kAlreadyInstalled[]   = "already-installed";    ///< 同版本已装
inline constexpr char kNotAPlugin[]         = "not-a-plugin";         ///< 包里没有 plugin.json
inline constexpr char kInvalidManifest[]    = "invalid-manifest";     ///< 清单校验失败
inline constexpr char kIncompatibleVersion[] = "incompatible-version";///< engines.* 不满足
inline constexpr char kNoMatchingVersion[]  = "no-matching-version";  ///< 没有兼容版本
inline constexpr char kVersionMismatch[]    = "version-mismatch";     ///< 包内版本≠索引声明
inline constexpr char kZipSlip[]            = "zip-slip";             ///< 压缩包路径越界
inline constexpr char kChecksum[]           = "checksum";             ///< sha256 不符
inline constexpr char kNetwork[]            = "network";              ///< 网络失败
inline constexpr char kHttpStatus[]         = "http-status";          ///< 非 2xx
inline constexpr char kDiskFull[]           = "disk-full";            ///< 磁盘满
inline constexpr char kPermission[]         = "permission";           ///< 写盘被拒
inline constexpr char kLoadFailed[]         = "load-failed";          ///< QML 编译/实例化失败
inline constexpr char kNotRemovable[]       = "not-removable";        ///< 内置插件
inline constexpr char kCancelled[]          = "cancelled";            ///< 用户取消
inline constexpr char kTooLate[]            = "too-late";             ///< 取消时已提交
inline constexpr char kUnknown[]            = "unknown";              ///< 兜底

} // namespace error

} // namespace contract
