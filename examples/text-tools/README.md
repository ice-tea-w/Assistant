# 文本工具（示例插件）

Assistant 的**参考插件**：一个纯本地、不联网、不读写文件的最小可用插件，用来演示
"插件到底能长什么样"。

## 它演示了什么

| 能力 | 在哪 |
|---|---|
| 清单怎么写（含 `engines.api`） | `plugin.json` |
| 入口对象 + `host.setting()` 持久化 | `main.qml` |
| 页面贡献 + 自己配色（不用宿主 `Style`） | `Page.qml` |
| 无权限插件（`permissions: []`） | `plugin.json` |

## 试装

**方式一：直接放目录**（开发期最快）

```powershell
Copy-Item -Recurse .\examples\text-tools "$env:LOCALAPPDATA\AssistantTeam\Assistant\plugins\com.assistant.texttools"
```

重启 Assistant → 主面板 →「插件市场 → 已安装」→ 打开开关 → 左侧导航出现「文本工具」。

**方式二：打成 zip 侧载**

```powershell
.\tools\plugin-pack.ps1 -PluginDir .\examples\text-tools -OutDir .\dist
# 然后在「插件市场」里用「从文件安装」选择 dist\com.assistant.texttools-1.0.0.zip
```

## 改动它来验证接口

1. 把 `Page.qml` 里按钮的 `label` 改掉 → 重启 → 看有没有变（验证页面确实来自插件目录）；
2. 在 `main.qml` 里 `host.log("warn", "test")` → 看 `%LOCALAPPDATA%\AssistantTeam\Assistant\logs\assistant.log`；
3. 故意把 `plugin.json` 的 `version` 改成 `1.0`（不是 SemVer）→ 应该被判 `invalid-manifest`，
   卡片标「不可用」而不是崩溃。

## 不要做的事

- 别往插件目录里写文件（升级会整个替换），持久化用 `host.setSetting()`；
- 别指望 `host` 之外的对象——宿主内部对象一个都不给，这是设计边界。
