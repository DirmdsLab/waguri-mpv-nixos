#include "library.hpp"

#include <algorithm>
#include <cerrno>
#include <memory>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QSaveFile>
#include <QSet>
#include <QStringConverter>
#include <QTextStream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

namespace {
const QSet<QString> videoExtensions{"mp4", "mkv", "webm", "mov", "avi", "m4v", "ts",
                                    "mpeg", "mpg", "ogv", "wmv", "flv"};
const QSet<QString> audioExtensions{"mp3", "m4a", "flac", "aac", "opus", "ogg", "oga",
                                    "wav", "wma", "aiff", "mka", "alac", "ape", "ac3"};
constexpr qint64 maxPlaylistBytes = 4 * 1024 * 1024;
constexpr int artworkSize = 768;

struct FormatCloser {
    void operator()(AVFormatContext *format) const { avformat_close_input(&format); }
};

std::unique_ptr<AVFormatContext, FormatCloser> openMedia(const QString &file)
{
    AVFormatContext *format = nullptr;
    AVDictionary *options = nullptr;
    av_dict_set(&options, "protocol_whitelist", "file", 0);
    av_dict_set(&options, "probesize", "1048576", 0);
    av_dict_set(&options, "analyzeduration", "1500000", 0);
    QByteArray path = QFile::encodeName(file);
    // An explicit file protocol also handles names containing a colon.
    int code = avformat_open_input(&format, ("file:" + path).constData(), nullptr, &options);
    av_dict_free(&options);
    if (code < 0)
        return {};
    return std::unique_ptr<AVFormatContext, FormatCloser>(format);
}

QString tag(AVDictionary *metadata, const char *name)
{
    const auto *entry = av_dict_get(metadata, name, nullptr, 0);
    return entry ? QString::fromUtf8(entry->value).trimmed() : QString();
}

QImage readImage(const QString &path)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (!size.isValid())
        return {};
    if (qMax(size.width(), size.height()) > artworkSize)
        reader.setScaledSize(size.scaled(artworkSize, artworkSize, Qt::KeepAspectRatio));
    return reader.read();
}

QImage decodeArtwork(AVFormatContext *format, int index, bool attached)
{
    AVStream *stream = format->streams[index];
    auto freeCodec = [](AVCodecContext *object) { avcodec_free_context(&object); };
    auto freeFrame = [](AVFrame *object) { av_frame_free(&object); };
    auto freePacket = [](AVPacket *object) { av_packet_free(&object); };
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec)
        return {};
    std::unique_ptr<AVCodecContext, decltype(freeCodec)> decoder(avcodec_alloc_context3(codec), freeCodec);
    std::unique_ptr<AVFrame, decltype(freeFrame)> frame(av_frame_alloc(), freeFrame);
    std::unique_ptr<AVPacket, decltype(freePacket)> packet(av_packet_alloc(), freePacket);
    if (!decoder || !frame || !packet || avcodec_parameters_to_context(decoder.get(), stream->codecpar) < 0)
        return {};
    decoder->thread_count = 1;
    decoder->max_pixels = 36LL * 1024 * 1024;
    if (avcodec_open2(decoder.get(), codec, nullptr) < 0)
        return {};

    int received = AVERROR(EAGAIN);
    if (attached) {
        if (avcodec_send_packet(decoder.get(), &stream->attached_pic) >= 0)
            received = avcodec_receive_frame(decoder.get(), frame.get());
    } else {
        if (format->duration > AV_TIME_BASE * 4)
            av_seek_frame(format, -1, 2LL * AV_TIME_BASE, AVSEEK_FLAG_BACKWARD);
        for (int attempt = 0; attempt < 160 && received == AVERROR(EAGAIN); ++attempt) {
            if (av_read_frame(format, packet.get()) < 0) {
                avcodec_send_packet(decoder.get(), nullptr);
                received = avcodec_receive_frame(decoder.get(), frame.get());
                break;
            }
            if (packet->stream_index == index && avcodec_send_packet(decoder.get(), packet.get()) >= 0)
                received = avcodec_receive_frame(decoder.get(), frame.get());
            av_packet_unref(packet.get());
        }
    }
    if (received < 0 || frame->width <= 0 || frame->height <= 0)
        return {};

    QSize size(frame->width, frame->height);
    if (qMax(size.width(), size.height()) > artworkSize)
        size.scale(artworkSize, artworkSize, Qt::KeepAspectRatio);
    if (size.width() < 1 || size.height() < 1)
        return {};
    QImage image(size, QImage::Format_RGBA8888);
    std::unique_ptr<SwsContext, decltype(&sws_freeContext)> converter(
        sws_getContext(frame->width, frame->height, AVPixelFormat(frame->format),
                       image.width(), image.height(), AV_PIX_FMT_RGBA, SWS_BILINEAR,
                       nullptr, nullptr, nullptr), sws_freeContext);
    if (!converter || image.isNull())
        return {};
    uint8_t *planes[] = {image.bits()};
    int stride[] = {int(image.bytesPerLine())};
    if (sws_scale(converter.get(), frame->data, frame->linesize, 0, frame->height, planes, stride) <= 0)
        return {};
    return image;
}
}

bool Library::isPlaylist(const QString &path)
{
    const QString extension = QFileInfo(path).suffix().toLower();
    return extension == "m3u" || extension == "m3u8";
}

bool Library::isMedia(const QString &path)
{
    const QString extension = QFileInfo(path).suffix().toLower();
    return videoExtensions.contains(extension) || audioExtensions.contains(extension);
}

Library::Track Library::fromUrl(const QUrl &url)
{
    const QFileInfo info(url.isLocalFile() ? url.toLocalFile() : url.path());
    Track track;
    track.url = url;
    track.title = info.completeBaseName();
    if (track.title.isEmpty())
        track.title = url.host();
    track.video = videoExtensions.contains(info.suffix().toLower());
    if (!url.isLocalFile())
        return track;

    track.modified = info.lastModified();
    if (isPlaylist(info.fileName()))
        return track;
    auto format = openMedia(info.absoluteFilePath());
    if (!format)
        return track;

    avformat_find_stream_info(format.get(), nullptr);
    if (format->duration > 0)
        track.duration = double(format->duration) / AV_TIME_BASE;

    if (const QString title = tag(format->metadata, "title"); !title.isEmpty())
        track.title = title;
    track.artist = tag(format->metadata, "artist");
    track.album = tag(format->metadata, "album");
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        const AVStream *stream = format->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC))
            track.video = true;
    }
    return track;
}

QList<Library::Track> Library::scan(const QString &directory)
{
    QList<Track> tracks;
    if (directory.isEmpty())
        return tracks;
    QDirIterator entries(directory, QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
    while (entries.hasNext()) {
        const QString path = entries.next();
        if (isMedia(path) || isPlaylist(path))
            tracks.append(fromUrl(QUrl::fromLocalFile(path)));
    }
    return tracks;
}

QList<Library::Track> Library::readPlaylist(const QString &filename, QString &error)
{
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly)) {
        error = file.errorString();
        return {};
    }
    if (file.size() > maxPlaylistBytes) {
        error = QStringLiteral("This playlist is larger than 4 MiB.");
        return {};
    }
    QString contents = QString::fromUtf8(file.readAll());
    if (contents.startsWith(QChar::ByteOrderMark))
        contents.remove(0, 1);
    const auto lines = contents.split(u'\n');
    if (std::any_of(lines.begin(), lines.end(), [](const QString &line) { return line.startsWith("#EXT-X-"); })) {
        // Leave HLS manifests intact so mpv can resolve segments and variants.
        return {fromUrl(QUrl::fromLocalFile(filename))};
    }

    const QDir directory = QFileInfo(filename).absoluteDir();
    QList<Track> tracks;
    QString label;
    for (const QString &entry : lines) {
        const QString line = entry.trimmed();
        if (line.startsWith("#EXTINF:")) {
            const qsizetype comma = line.indexOf(u',');
            label = comma >= 0 ? line.mid(comma + 1).trimmed() : QString();
            continue;
        }
        if (line.isEmpty() || line.startsWith(u'#'))
            continue;
        QUrl url(line);
        if (url.scheme().isEmpty()) {
            QString path = directory.absoluteFilePath(line);
            if (!QFileInfo::exists(path) && line.contains(u'\\')) {
                QString portablePath = line;
                path = directory.absoluteFilePath(portablePath.replace(u'\\', u'/'));
            }
            url = QUrl::fromLocalFile(QDir::cleanPath(path));
        }
        const bool playable = (url.isLocalFile() && !isPlaylist(url.toLocalFile())) ||
                              (!url.host().isEmpty() && (url.scheme() == "http" || url.scheme() == "https"));
        if (playable) {
            auto track = fromUrl(url);
            if (!label.isEmpty())
                track.title = label;
            tracks.append(track);
        }
        label.clear();
    }
    if (tracks.isEmpty())
        error = QStringLiteral("This playlist has no supported entries.");
    return tracks;
}

bool Library::writePlaylist(const QString &filename, const QList<Track> &tracks, QString &error)
{
    QSaveFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        error = file.errorString();
        return false;
    }
    QTextStream output(&file);
    output.setEncoding(QStringConverter::Utf8);
    output << "#EXTM3U\n";
    for (const Track &track : tracks) {
        QString title = track.title;
        title.replace(u'\n', u' ').replace(u'\r', u' ');
        // Keep line delimiters encoded in URLs and leave path resolution to the importer.
        output << "#EXTINF:" << (track.duration > 0 ? qRound(track.duration) : -1)
               << ',' << title << '\n' << track.url.toEncoded() << '\n';
    }
    output.flush();
    if (output.status() == QTextStream::Ok && file.commit())
        return true;
    error = file.errorString();
    return false;
}

QImage Library::artwork(const Track &track)
{
    if (!track.url.isLocalFile())
        return {};
    const QFileInfo file(track.url.toLocalFile());
    const QString stem = file.absoluteDir().filePath(file.completeBaseName());
    for (const auto &suffix : {u"_thumbnail.jpg", u"_thumbnail.webp", u"_thumbnail.png", u".jpg", u".png", u".webp"}) {
        if (QImage image = readImage(stem + QString::fromUtf16(suffix)); !image.isNull())
            return image;
    }
    auto format = openMedia(file.absoluteFilePath());
    if (!format)
        return {};
    avformat_find_stream_info(format.get(), nullptr);
    int video = -1;
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        const AVStream *stream = format->streams[i];
        if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
            continue;
        if (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) {
            QImage image = decodeArtwork(format.get(), int(i), true);
            if (!image.isNull())
                return image;
        } else if (video < 0) {
            video = int(i);
        }
    }
    if (video >= 0)
        return decodeArtwork(format.get(), video, false);
    return readImage(file.absoluteDir().filePath(QStringLiteral("cover.jpg")));
}
