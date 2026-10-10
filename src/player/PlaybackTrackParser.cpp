#include "PlaybackTrackParser.h"

extern "C" {
#include <mpv/client.h>
}

#include <QByteArray>
#include <QLocale>
#include <QVariantMap>

#include <cstdint>
#include <cstring>

namespace Spool {

namespace {

    const mpv_node *mapValue(const mpv_node *node, const char *key)
    {
        if (!node || node->format != MPV_FORMAT_NODE_MAP || !node->u.list)
            return nullptr;

        const mpv_node_list *list = node->u.list;
        for (int i = 0; i < list->num; ++i) {
            if (list->keys[i] && std::strcmp(list->keys[i], key) == 0)
                return &list->values[i];
        }
        return nullptr;
    }

    QString nodeString(const mpv_node *node)
    {
        if (!node)
            return {};
        if (node->format == MPV_FORMAT_STRING && node->u.string)
            return QString::fromUtf8(node->u.string);
        if (node->format == MPV_FORMAT_INT64)
            return QString::number(node->u.int64);
        if (node->format == MPV_FORMAT_FLAG)
            return node->u.flag ? QStringLiteral("yes") : QStringLiteral("no");
        return {};
    }

    int64_t nodeInt(const mpv_node *node, int64_t fallback = 0)
    {
        if (!node)
            return fallback;
        if (node->format == MPV_FORMAT_INT64)
            return node->u.int64;
        if (node->format == MPV_FORMAT_STRING && node->u.string)
            return QByteArray(node->u.string).toLongLong();
        return fallback;
    }

    double nodeDouble(const mpv_node *node, double fallback = 0.0)
    {
        if (!node)
            return fallback;
        if (node->format == MPV_FORMAT_DOUBLE)
            return node->u.double_;
        if (node->format == MPV_FORMAT_INT64)
            return static_cast<double>(node->u.int64);
        if (node->format == MPV_FORMAT_STRING && node->u.string)
            return QByteArray(node->u.string).toDouble();
        return fallback;
    }

    bool nodeFlag(const mpv_node *node)
    {
        if (!node)
            return false;
        if (node->format == MPV_FORMAT_FLAG)
            return node->u.flag != 0;
        if (node->format == MPV_FORMAT_STRING && node->u.string)
            return std::strcmp(node->u.string, "yes") == 0 || std::strcmp(node->u.string, "true") == 0;
        return false;
    }

    QString prettyLanguage(QString lang)
    {
        lang = lang.trimmed().toLower();
        if (lang == QStringLiteral("und") || lang.isEmpty())
            return {};

        const QLocale::Language language = QLocale::codeToLanguage(QStringView(lang));
        if (language != QLocale::AnyLanguage)
            return QLocale::languageToString(language);

        const int hyphen = lang.indexOf(QLatin1Char('-'));
        const int underscore = lang.indexOf(QLatin1Char('_'));
        const int regionSeparator = hyphen < 0 ? underscore : (underscore < 0 ? hyphen : qMin(hyphen, underscore));
        if (regionSeparator > 0) {
            const QLocale::Language regionLanguage = QLocale::codeToLanguage(QStringView(lang).left(regionSeparator));
            if (regionLanguage != QLocale::AnyLanguage)
                return QLocale::languageToString(regionLanguage);
        }

        return lang.toUpper();
    }

    QString prettySubtitleCodec(QString codec)
    {
        codec = codec.trimmed().toLower();
        if (codec == QStringLiteral("subrip") || codec == QStringLiteral("srt"))
            return QStringLiteral("SRT");
        if (codec == QStringLiteral("ass") || codec.contains(QStringLiteral("ass")))
            return QStringLiteral("ASS");
        if (codec == QStringLiteral("ssa"))
            return QStringLiteral("SSA");
        if (codec.contains(QStringLiteral("pgs")) || codec.contains(QStringLiteral("hdmv")))
            return QStringLiteral("PGS");
        if (codec.contains(QStringLiteral("dvd")) || codec.contains(QStringLiteral("vobsub")))
            return QStringLiteral("DVD");
        if (codec.contains(QStringLiteral("webvtt")) || codec == QStringLiteral("vtt"))
            return QStringLiteral("VTT");
        if (codec.isEmpty())
            return {};
        return codec.toUpper();
    }

    QString cleanTrackTitle(QString title)
    {
        title = title.trimmed();
        while (true) {
            const int open = title.lastIndexOf(QLatin1Char('['));
            const int close = title.endsWith(QLatin1Char(']')) ? title.size() - 1 : -1;
            if (open < 0 || close < 0 || open >= close)
                break;
            title = title.left(open).trimmed();
        }
        return title;
    }

} // namespace

ParsedPlaybackTracks PlaybackTrackParser::parseTracks(const mpv_node *node)
{
    ParsedPlaybackTracks result;
    if (!node || node->format != MPV_FORMAT_NODE_ARRAY || !node->u.list)
        return result;

    const mpv_node_list *tracks = node->u.list;
    for (int i = 0; i < tracks->num; ++i) {
        const mpv_node *track = &tracks->values[i];
        const QString type = nodeString(mapValue(track, "type"));
        const int id = static_cast<int>(nodeInt(mapValue(track, "id"), -1));
        if (id < 0)
            continue;

        if (type == QStringLiteral("sub")) {
            const QString language = prettyLanguage(nodeString(mapValue(track, "lang")));
            const QString title = cleanTrackTitle(nodeString(mapValue(track, "title")));
            const QString codec = prettySubtitleCodec(nodeString(mapValue(track, "codec")));
            QString label = language.isEmpty() ? QStringLiteral("Subtitle %1").arg(id) : language;
            if (!title.isEmpty() && title.compare(language, Qt::CaseInsensitive) != 0)
                label += QStringLiteral(" (%1)").arg(title);
            if (nodeFlag(mapValue(track, "forced")) && !title.contains(QStringLiteral("forced"), Qt::CaseInsensitive))
                label += QStringLiteral(" (Forced)");
            if (nodeFlag(mapValue(track, "external")))
                label += QStringLiteral(" (External)");
            if (!codec.isEmpty())
                label += QStringLiteral(" - %1").arg(codec);

            result.subtitleLabels.push_back(label);
            result.subtitleIds.push_back(id);
            if (nodeFlag(mapValue(track, "selected")))
                result.selectedSubtitleIndex = result.subtitleIds.size() - 1;
        } else if (type == QStringLiteral("audio")) {
            const QString language = prettyLanguage(nodeString(mapValue(track, "lang")));
            const QString title = cleanTrackTitle(nodeString(mapValue(track, "title")));
            const QString codec = nodeString(mapValue(track, "codec")).toUpper();
            const int channels = static_cast<int>(nodeInt(mapValue(track, "audio-channels"), 0));
            QString label = language.isEmpty() ? QStringLiteral("Audio %1").arg(id) : language;
            if (!title.isEmpty() && title.compare(language, Qt::CaseInsensitive) != 0)
                label += QStringLiteral(" (%1)").arg(title);

            QStringList tail;
            if (channels > 0) {
                tail.push_back(channels == 1 ? QStringLiteral("Mono")
                        : channels == 2      ? QStringLiteral("Stereo")
                                             : QStringLiteral("%1ch").arg(channels));
            }
            if (!codec.isEmpty())
                tail.push_back(codec);
            if (!tail.isEmpty())
                label += QStringLiteral(" - %1").arg(tail.join(QStringLiteral(", ")));

            result.audioLabels.push_back(label);
            result.audioIds.push_back(id);
            if (nodeFlag(mapValue(track, "selected")))
                result.selectedAudioIndex = result.audioIds.size() - 1;
        }
    }

    return result;
}

QVariantList PlaybackTrackParser::parseChapters(const mpv_node *node)
{
    QVariantList chapters;
    if (!node || node->format != MPV_FORMAT_NODE_ARRAY || !node->u.list)
        return chapters;

    const mpv_node_list *list = node->u.list;
    for (int i = 0; i < list->num; ++i) {
        const mpv_node *chapter = &list->values[i];
        QString title = nodeString(mapValue(chapter, "title"));
        if (title.isEmpty())
            title = QStringLiteral("Chapter %1").arg(i + 1);

        QVariantMap entry;
        entry.insert(QStringLiteral("title"), title);
        entry.insert(QStringLiteral("start"), nodeDouble(mapValue(chapter, "time"), 0.0));
        chapters.push_back(entry);
    }
    return chapters;
}

} // namespace Spool
