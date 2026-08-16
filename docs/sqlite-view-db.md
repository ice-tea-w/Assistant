# 如何查看 SQLite .db 文件

## 安装 sqlite3

```powershell
winget install sqlite.sqlite
```

安装后需**重启 shell**（关掉重开 PowerShell），否则 PATH 不会生效。

## 常用命令

### 进入交互模式
```bash
sqlite3 "路径/到/文件.db"
```

进入后可用命令：
```sql
.tables          -- 列出所有表
.schema          -- 查看所有表结构
.schema 表名     -- 查看指定表结构
SELECT * FROM 表名 LIMIT 10;   -- 查看前10条数据
.quit            -- 退出
```

### 不进入交互模式，直接执行
```bash
# 列出表
sqlite3 "路径/文件.db" ".tables"

# 查看表结构
sqlite3 "路径/文件.db" ".schema 表名"

# 执行查询
sqlite3 "路径/文件.db" "SELECT * FROM 表名 LIMIT 10;"
```

## 图形化工具

[DB Browser for SQLite](https://sqlitebrowser.org/) — 免费、开源，比命令行直观。

## 示例（Assistant 的 history.db）

```bash
# 路径
C:\Users\ice_tea\AppData\Local\AssistantTeam\Assistant\history.db

# 查看表
sqlite3 "C:/Users/ice_tea/AppData/Local/AssistantTeam/Assistant/history.db" ".tables"
```
