# QDir::NoSymLinks 挡不住 Windows junction（P0-1 第一次没修好）

## 现象

`FileSearcher::buildIndex()` 按 A1 的写法给 `QDirIterator` 加了 `QDir::NoSymLinks`：

```cpp
QDirIterator it(root, QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                QDirIterator::Subdirectories);
```

看起来就是"不跟随符号链接"。但在 Desktop 放一个**指回父目录的 junction** 之后，
索引里照样出现了叠罗汉路径（实测 63 层）：

```
C:/Users/ice_tea/Desktop/!a1looptest/loop/loop/loop/.../loop/real/a1probe_alpha.txt
```

20000 条索引里 **126 条**是这种垃圾路径，同一个真实文件被重复收了好几遍。

## 根因（两条叠加，缺一不可）

1. **Qt 不把 junction 当符号链接。** `QFileInfo::isSymLink()` 在 Windows 上只认
   `IO_REPARSE_TAG_SYMLINK`，而 junction 是 `IO_REPARSE_TAG_MOUNT_POINT`。
   Qt 6.2 专门为此补了 `QFileInfo::isJunction()`——如果 junction 也算 symlink，
   这个 API 就没有存在意义。
2. **`QDirIterator` 没有"不下降进某个子目录"的开关。** 过滤器只决定
   "哪些条目被返回"，不决定"是否递归进去"。所以就算能识别 junction，
   在 `Subdirectories` 模式下也无从阻止它下探。

→ 结论：**P0-1 只能在"是否递归"这一层修**，`QDirIterator` 这条路走不通。

## 修法

自己控制递归（A1 的实现，`src/core/FileSearcher.cpp`）：

```cpp
void walkDir(const QString &dirPath)
{
    const QFileInfoList entries =
        QDir(dirPath).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                                    QDir::NoSort);   // NoSort = 保持文件系统原始顺序
    for (const QFileInfo &info : entries) {
        if (info.isDir()) {
            if (info.isSymLink() || info.isJunction())   // ← P0-1 真正生效的地方
                continue;
            if (isSkippedDirName(info.fileName())) {     // ← P0-2 目录级剪枝
                ++s_prunedDirs;
                continue;
            }
            walkDir(info.filePath());
        } else {
            if (info.isSymLink()) continue;
            /* 收进索引 */
        }
    }
}
```

顺带白拿两个好处：
- **目录级剪枝真的成立了**（命中 `node_modules` 直接不进子树，而不是逐条文件回看整条路径）。
- 顺序仍是前序 DFS，与旧 `QDirIterator` 一致，截断行为不变。

## 复现 / 验收手法（可复用）

用环境变量门控的临时仪器把索引里的全部路径落盘，再 grep：

```powershell
# Desktop 放一个指回父目录的 junction
$base = "$env:USERPROFILE\Desktop\!a1looptest"
New-Item -ItemType Directory "$base\real" -Force | Out-Null
Set-Content "$base\real\probe.txt" 'x'
New-Item -ItemType Junction -Path "$base\loop" -Target $base | Out-Null
```

| | 修复前 | 修复后 |
|---|---|---|
| 含 `/loop/` 的索引条目 | 126 | **0** |
| 探针文件条目数 | 128 | **2**（正好 `real/` 下那两个） |
| 最长路径 | 63 层 loop 拼接 | 正常长度 |

注意：本机 Desktop 有 3.5 万个文件、索引在 20000 就截断了，所以**探针必须放在
遍历顺序最靠前的根的最前排**（`!` 前缀 + Desktop 是 roots[0]），否则验证跑不到。

## 教训

- `QDir::NoSymLinks` ≠ "不跟随所有 reparse point"。Windows 上 junction / mount point
  是另一套 reparse tag，遇到目录遍历类的正确性需求，**必须实测**，不能按 API 名字推断。
- 一个"看起来只是加个 flag"的修复，如果修复点选在了错误的层次（过滤器 vs 递归控制），
  它会安静地什么都不做——**编译通过、日志正常、看起来更好**，只有把索引 dump 出来
  对着看才发现没生效。
