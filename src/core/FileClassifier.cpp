#include "core/FileClassifier.h"

namespace {

struct ExtGroup
{
    uint16_t    mask;
    const char *exts;   ///< 空格分隔，全部小写
};

/// 内置表（治理文档 §1.3 的类别清单）。
/// 增删条目只影响“新入库/下次重扫”的文件；旧库里的 `cat_mask` 由差量更新纠正。
const ExtGroup kBuiltin[] = {
    { FileKind::Document, "doc docx odt rtf txt md markdown tex htm html mht pages" },
    { FileKind::Sheet,    "xls xlsx xlsm csv ods tsv" },
    { FileKind::Slide,    "ppt pptx pps ppsx odp key" },
    { FileKind::Pdf,      "pdf" },
    { FileKind::Image,    "jpg jpeg png gif webp bmp tif tiff svg heic heif psd ai xcf raw cr2 nef" },
    { FileKind::Video,    "mp4 mkv avi mov webm flv wmv m4v mpg mpeg 3gp ts" },
    { FileKind::Audio,    "mp3 wav flac m4a aac ogg wma ape opus mid midi" },
    { FileKind::Archive,  "zip rar 7z tar gz bz2 xz zst tgz tbz2 txz cab lz lzma" },
    { FileKind::Archive,  "tar.gz tar.bz2 tar.xz tar.zst" },
    { FileKind::Installer,"exe msi msix appx dmg pkg deb rpm apk" },
    { FileKind::Code,     "cpp cxx cc c h hpp hxx py pyw js mjs cjs ts tsx jsx java go rs cs "
                          "sql sh bash ps1 bat cmd rb php swift kt kts lua pl r scala vue svelte "
                          "asm s vb fs fsi dart groovy gradle cmake mk make ninja" },
    { FileKind::Font,     "ttf otf woff woff2 eot fon" },
    { FileKind::Data,     "json jsonc xml yaml yml toml ini cfg conf db sqlite sqlite3 mdb "
                          "parquet avro protobuf pb ipynb" },
    { FileKind::Ebook,    "epub mobi azw azw3 lit fb2" },
    { FileKind::Shortcut, "lnk url desktop" },
    { FileKind::Disk,     "iso img vhd vhdx vmdk qcow2" },
};

/// 完整文件名表：先于扩展名判定（`.gitignore` 的“扩展名”是 gitignore，会误判成 Other）
struct NameGroup
{
    uint16_t    mask;
    const char *names;   ///< 空格分隔，全部小写
};

const NameGroup kNames[] = {
    { FileKind::Code,     "makefile gnumakefile dockerfile containerfile cmakelists.txt "
                          "vagrantfile gemfile rakefile jenkinsfile meson.build build.gradle "
                          "pom.xml gradlew mvnw" },
    { FileKind::Document, "readme license licence copying changelog authors notice" },
    { FileKind::Data,     ".gitignore .gitattributes .gitmodules .editorconfig .env "
                          ".npmrc .yarnrc .prettierrc .eslintrc" },
};

} // namespace

FileClassifier::FileClassifier()
{
    buildDefaults();
}

void FileClassifier::buildDefaults()
{
    for (const ExtGroup &g : kBuiltin) {
        const QByteArray bytes(g.exts);
        const QList<QByteArray> parts = bytes.split(' ');
        for (const QByteArray &raw : parts) {
            if (raw.isEmpty())
                continue;
            m_ext.insert(QString::fromLatin1(raw), g.mask);
        }
    }

    for (const NameGroup &g : kNames) {
        const QByteArray bytes(g.names);
        const QList<QByteArray> parts = bytes.split(' ');
        for (const QByteArray &raw : parts) {
            if (raw.isEmpty())
                continue;
            m_names.insert(QString::fromLatin1(raw), g.mask);
        }
    }
}

uint16_t FileClassifier::classify(const QString &fileName) const
{
    if (fileName.isEmpty())
        return FileKind::Other;

    // 只看最后一段（调用方有时传完整路径）
    const int slash = fileName.lastIndexOf(QLatin1Char('/'));
    const QString name = (slash >= 0 ? fileName.mid(slash + 1) : fileName).toLower();

    // 1. 完整文件名表
    const auto byName = m_names.constFind(name);
    if (byName != m_names.constEnd())
        return byName.value();

    // 2. 最长后缀优先：从左往右试每个点之后的部分
    int dot = name.indexOf(QLatin1Char('.'));
    while (dot > 0 && dot + 1 < name.size()) {
        const auto it = m_ext.constFind(name.mid(dot + 1));
        if (it != m_ext.constEnd())
            return it.value();
        dot = name.indexOf(QLatin1Char('.'), dot + 1);
    }

    // 3. 点文件（.gitignore 之类）已经由名称表处理；其余归 Other
    return FileKind::Other;
}

void FileClassifier::setOverride(const QString &ext, uint16_t mask)
{
    QString key = ext.trimmed().toLower();
    while (key.startsWith(QLatin1Char('.')))
        key.remove(0, 1);
    if (!key.isEmpty())
        m_ext.insert(key, mask ? mask : FileKind::Other);
}

void FileClassifier::addExtraExtension(const QString &ext)
{
    setOverride(ext, FileKind::Other);
}
