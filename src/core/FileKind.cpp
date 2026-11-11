#include "core/FileKind.h"

#include <QHash>

namespace FileKind {

namespace {

struct Alias
{
    const char *name;
    uint16_t    mask;
};

/// 规范名 + 常见别名。全部小写；查表前调用方已折叠（这里再稳一手）。
const Alias kAliases[] = {
    { "doc",       Document  },  { "document", Document },
    { "sheet",     Sheet     },  { "xls",      Sheet    },  { "excel", Sheet },
    { "slide",     Slide     },  { "ppt",      Slide    },  { "presentation", Slide },
    { "pdf",       Pdf       },
    { "img",       Image     },  { "image",    Image    },  { "pic", Image }, { "photo", Image },
    { "vid",       Video     },  { "video",    Video    },
    { "aud",       Audio     },  { "audio",    Audio    },  { "music", Audio },
    { "zip",       Archive   },  { "archive",  Archive  },  { "rar", Archive },
    { "exe",       Installer },  { "installer", Installer }, { "app", Installer },
    { "code",      Code      },  { "src",      Code     },  { "source", Code },
    { "font",      Font      },
    { "data",      Data      },  { "json",     Data     },
    { "ebook",     Ebook     },  { "book",     Ebook    },
    { "lnk",       Shortcut  },  { "shortcut", Shortcut },
    { "disk",      Disk      },  { "iso",      Disk     },
    { "other",     Other     },
    { "all",       kAll      },
};

const QHash<QString, uint16_t> &aliasMap()
{
    static const QHash<QString, uint16_t> map = [] {
        QHash<QString, uint16_t> m;
        for (const Alias &a : kAliases)
            m.insert(QString::fromLatin1(a.name), a.mask);
        return m;
    }();
    return map;
}

} // namespace

uint16_t fromToken(QStringView token)
{
    if (token.isEmpty())
        return 0;

    // 允许 ":doc" 这种带冒号的写法（调用方通常已经去掉了）
    if (token.startsWith(QLatin1Char(':')))
        token = token.mid(1);

    const auto it = aliasMap().constFind(token.toString().toLower());
    return it == aliasMap().constEnd() ? 0 : it.value();
}

QString token(uint16_t mask)
{
    switch (mask) {
    case Document:  return QStringLiteral("doc");
    case Sheet:     return QStringLiteral("sheet");
    case Slide:     return QStringLiteral("slide");
    case Pdf:       return QStringLiteral("pdf");
    case Image:     return QStringLiteral("img");
    case Video:     return QStringLiteral("vid");
    case Audio:     return QStringLiteral("aud");
    case Archive:   return QStringLiteral("zip");
    case Installer: return QStringLiteral("exe");
    case Code:      return QStringLiteral("code");
    case Font:      return QStringLiteral("font");
    case Data:      return QStringLiteral("data");
    case Ebook:     return QStringLiteral("ebook");
    case Shortcut:  return QStringLiteral("lnk");
    case Disk:      return QStringLiteral("disk");
    case Other:     return QStringLiteral("other");
    case kAll:      return QStringLiteral("all");
    default:        return QString();
    }
}

QStringList allTokens()
{
    return {
        QStringLiteral("doc"),   QStringLiteral("sheet"), QStringLiteral("slide"),
        QStringLiteral("pdf"),   QStringLiteral("img"),   QStringLiteral("vid"),
        QStringLiteral("aud"),   QStringLiteral("zip"),   QStringLiteral("exe"),
        QStringLiteral("code"),  QStringLiteral("font"),  QStringLiteral("data"),
        QStringLiteral("ebook"), QStringLiteral("lnk"),   QStringLiteral("disk"),
        QStringLiteral("other"),
    };
}

} // namespace FileKind
