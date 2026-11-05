#include "core/plugin/PluginSpec.h"

#include "core/plugin/SemVer.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

namespace {

/// The id rule is duplicated from PluginManifest on purpose: this header must stay
/// free of manifest parsing so `PluginSpec` can be used before a package exists.
bool idPatternOk(const QString &id)
{
    static const QRegularExpression re(QStringLiteral("^[a-z0-9][a-z0-9._-]{2,63}$"));
    return re.match(id).hasMatch();
}

bool looksLikeUrl(const QString &text)
{
    return text.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)
        || text.startsWith(QLatin1String("http://"), Qt::CaseInsensitive);
}

/// UNC share, drive-letter path, or anything with a separator / a .zip suffix.
bool looksLikePath(const QString &text)
{
    if (text.startsWith(QLatin1String("\\\\")))
        return true;                                  // \\server\share
    if (text.contains(QLatin1Char('\\')))
        return true;
    if (text.contains(QLatin1Char('/')) && !looksLikeUrl(text))
        return true;
    if (text.endsWith(QLatin1String(".zip"), Qt::CaseInsensitive))
        return true;

    // "C:foo.zip" style (relative to the drive's current directory)
    if (text.size() >= 2 && text.at(1) == QLatin1Char(':') && text.at(0).isLetter())
        return true;

    return false;
}

} // namespace

bool PluginSpec::isValidId(const QString &id)
{
    return idPatternOk(id);
}

std::optional<PluginSpec> PluginSpec::parse(const QString &raw, QString *error)
{
    const auto fail = [error](const QString &message) -> std::optional<PluginSpec> {
        if (error)
            *error = message;
        return std::nullopt;
    };

    const QString text = raw.trimmed();
    if (text.isEmpty())
        return fail(QStringLiteral("安装源为空"));

    // ---- URL ----
    if (looksLikeUrl(text)) {
        const QUrl url(text);
        if (!url.isValid() || url.host().isEmpty())
            return fail(QStringLiteral("URL 无效：%1").arg(text));

        PluginSpec spec;
        spec.kind = Kind::Url;
        spec.url  = url;
        return spec;
    }

    // ---- local path ----
    if (looksLikePath(text)) {
        const QFileInfo info(text);
        if (!info.exists())
            return fail(QStringLiteral("文件不存在：%1").arg(text));
        if (!info.isFile())
            return fail(QStringLiteral("不是文件：%1").arg(text));

        PluginSpec spec;
        spec.kind = Kind::LocalPath;
        spec.path = info.absoluteFilePath();
        return spec;
    }

    // ---- id / id@version ----
    QString idPart   = text;
    QString versionPart;

    const int at = text.indexOf(QLatin1Char('@'));
    if (at >= 0) {
        idPart      = text.left(at);
        versionPart = text.mid(at + 1).trimmed();

        if (versionPart.isEmpty())
            return fail(QStringLiteral("版本号为空：%1").arg(text));

        // Ranges are not versions. "^1.2" here would silently never match a
        // market release, so refuse it up front.
        if (!semver::parse(QStringView(versionPart))) {
            return fail(QStringLiteral("版本号不是合法的语义化版本：%1（不支持范围写法）")
                            .arg(versionPart));
        }
    }

    if (!idPatternOk(idPart))
        return fail(QStringLiteral("插件 id 非法：%1").arg(idPart));

    PluginSpec spec;
    spec.id = idPart;

    if (versionPart.isEmpty()) {
        spec.kind = Kind::Id;
    } else {
        spec.kind    = Kind::IdVersion;
        spec.version = versionPart;
    }

    return spec;
}
