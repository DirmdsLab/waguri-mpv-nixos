#include "youtube.hpp"

#include <algorithm>
#include <memory>

#include <QBuffer>
#include <QComboBox>
#include <QDesktopServices>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QPushButton>
#include <QRegularExpression>
#include <QUrlQuery>
#include <QVBoxLayout>

namespace {
const QUrl server(QStringLiteral("http://127.0.0.1:7010/"));
const QRegularExpression audioPath(R"(^/audio-cache/[a-f0-9]{64}/audioraw\.[A-Za-z0-9]{1,12}$)");
const QRegularExpression archivePath(R"(^/results/[A-Za-z0-9_-]{32}/archive\.zip$)");
const QRegularExpression coverPath(R"(^/audio-cache/[a-f0-9]{64}/thumbnail\.[A-Za-z0-9]{1,12}$)");
constexpr qsizetype maxJson = 2 * 1024 * 1024;
constexpr qsizetype maxThumbnail = 8 * 1024 * 1024;

bool supported(const QUrl &url)
{
    return url.isValid() && url.scheme() == "https" && url.userInfo().isEmpty() &&
        url.port() < 0 && (url.host() == "youtu.be" || url.host() == "youtube.com" ||
                           url.host().endsWith(".youtube.com"));
}

bool isPlaylist(const QUrl &url)
{
    return url.path() == "/playlist" && QUrlQuery(url).hasQueryItem(QStringLiteral("list"));
}

QNetworkRequest requestFor(const QString &path, int timeout = 180000)
{
    QNetworkRequest request(server.resolved(QUrl(path)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(timeout);
    return request;
}

QString responseError(QNetworkReply *reply, const QJsonObject &result)
{
    const QString message = result.value("error").toString();
    if (!message.isEmpty())
        return message;
    if (const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(); code > 0 && code != 200)
        return QStringLiteral("The server returned HTTP %1.").arg(code);
    if (reply->error() != QNetworkReply::NoError)
        return reply->errorString() + QStringLiteral(" · 127.0.0.1:7010");
    return QStringLiteral("The server returned an invalid response.");
}
}

YouTubePanel::YouTubePanel(QWidget *parent) : QWidget(parent)
{
    network.setProxy(QNetworkProxy::NoProxy);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 15, 18, 13);
    layout->setSpacing(10);
    auto *urlRow = new QHBoxLayout;
    input = new QLineEdit;
    input->setPlaceholderText(QStringLiteral("Paste a YouTube video or playlist URL"));
    input->setClearButtonEnabled(true);
    auto *browse = new QPushButton(QStringLiteral("Browse YouTube"));
    urlRow->addWidget(input, 1);
    urlRow->addWidget(browse);
    layout->addLayout(urlRow);

    auto *audioRow = new QHBoxLayout;
    format = new QComboBox;
    format->addItem(QStringLiteral("Best audio"), QString());
    auto *formats = new QPushButton(QStringLiteral("Load formats"));
    auto *playButton = new QPushButton(QStringLiteral("Play audio"));
    audioRow->addWidget(format, 1);
    audioRow->addWidget(formats);
    audioRow->addWidget(playButton);
    layout->addLayout(audioRow);

    auto *downloadRow = new QHBoxLayout;
    downloadFormat = new QLineEdit(QStringLiteral("251+399"));
    downloadFormat->setPlaceholderText(QStringLiteral("Audio+video format IDs"));
    downloadFormat->setAccessibleName(QStringLiteral("Download format IDs"));
    auto *picker = new QPushButton(QStringLiteral("Open format picker"));
    saveButton = new QPushButton(QStringLiteral("Download"));
    downloadRow->addWidget(downloadFormat, 1);
    downloadRow->addWidget(picker);
    downloadRow->addWidget(saveButton);
    layout->addLayout(downloadRow);

    status = new QLabel(QStringLiteral("Audio and downloads use the local yt-dlp server at 127.0.0.1:7010."));
    status->setObjectName(QStringLiteral("secondary"));
    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(status);
    archiveButton = new QPushButton(QStringLiteral("Retrieve result ZIP"));
    archiveButton->setEnabled(false);
    layout->addWidget(archiveButton, 0, Qt::AlignLeft);
    layout->addStretch();

    connect(archiveButton, &QPushButton::clicked, this, [this] {
        if (!resultArchive.isEmpty() && !QDesktopServices::openUrl(resultArchive))
            setStatus(QStringLiteral("Could not open the server result in the default browser."));
    });

    connect(browse, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.youtube.com/")));
    });
    connect(input, &QLineEdit::textChanged, this, [this] {
        format->clear();
        format->addItem(QStringLiteral("Best audio"), QString());
    });
    connect(input, &QLineEdit::returnPressed, this, [this] { play(); });
    connect(playButton, &QPushButton::clicked, this, [this] { play(); });
    connect(formats, &QPushButton::clicked, this, [this] { fetchFormats(); });
    connect(picker, &QPushButton::clicked, this, [this] { openFormatPicker(); });
    connect(saveButton, &QPushButton::clicked, this, [this] { download(); });
}

QUrl YouTubePanel::link()
{
    const QUrl url(input->text().trimmed());
    if (supported(url))
        return url;
    setStatus(QStringLiteral("Enter a valid HTTPS YouTube URL."));
    return {};
}

void YouTubePanel::setStatus(const QString &text)
{
    status->setText(text);
}

QNetworkReply *YouTubePanel::post(const QString &path, const QJsonObject &body, JsonResult done)
{
    auto *reply = network.post(requestFor(path), QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::readyRead, this, [reply] {
        if (reply->bytesAvailable() > maxJson)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [reply, done = std::move(done)] {
        if (reply->error() == QNetworkReply::OperationCanceledError) {
            reply->deleteLater();
            return;
        }
        const auto result = QJsonDocument::fromJson(reply->readAll()).object();
        const bool ok = reply->error() == QNetworkReply::NoError &&
                        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 &&
                        result.value("success").toBool();
        done(result, ok ? QString() : responseError(reply, result));
        reply->deleteLater();
    });
    return reply;
}

void YouTubePanel::fetchFormats()
{
    const QUrl url = link();
    if (url.isEmpty())
        return;
    if (isPlaylist(url)) {
        setStatus(QStringLiteral("Choose a single video to list its formats."));
        return;
    }
    setStatus(QStringLiteral("Loading audio formats…"));
    post(QStringLiteral("/api/formats"), {{"url", url.toString()}},
         [this, url](const QJsonObject &result, const QString &error) {
        if (url != QUrl(input->text().trimmed()))
            return;
        if (!error.isEmpty()) {
            setStatus(error);
            return;
        }
        auto entries = result.value("formats").toArray();
        QList<QPair<QString, QString>> choices;
        for (const auto &entry : entries) {
            const QJsonObject item = entry.toObject();
            const QString kind = item.value("kind").toString();
            const QString id = item.value("id").toString();
            if ((kind != "audio" && kind != "combined") || id.isEmpty())
                continue;
            QString title = id + QStringLiteral(" · ") + item.value("ext").toString()
                + QStringLiteral(" · ") + item.value("audio_codec").toString();
            const int bitrate = item.value("audio_bitrate").toInt();
            if (bitrate > 0)
                title += QStringLiteral(" · %1 kbps").arg(bitrate);
            choices.append({title, id});
        }
        std::stable_partition(choices.begin(), choices.end(), [](const auto &item) { return item.second == "251"; });
        format->clear();
        format->addItem(QStringLiteral("Best audio"), QString());
        for (const auto &[title, id] : choices)
            format->addItem(title, id);
        setStatus(choices.isEmpty() ? QStringLiteral("No audio formats returned.")
                                    : QStringLiteral("Choose an audio format and play."));
    });
}

void YouTubePanel::play()
{
    const QUrl url = link();
    if (url.isEmpty() || !playTracks)
        return;
    if (!isPlaylist(url)) {
        const auto track = Library::Track{.url = url, .title = QStringLiteral("YouTube audio"),
            .artist = {}, .album = {}, .modified = {}, .youtube = true, .audioFormat = format->currentData().toString()};
        setStatus(QStringLiteral("Preparing audio…"));
        playTracks({track});
        return;
    }
    setStatus(QStringLiteral("Loading playlist…"));
    post(QStringLiteral("/api/playlist"), {{"url", url.toString()}},
         [this, url](const QJsonObject &result, const QString &error) {
        if (url != QUrl(input->text().trimmed()))
            return;
        if (!error.isEmpty()) {
            setStatus(error + QStringLiteral(" Playlist playback requires /api/playlist on the Waguri server."));
            return;
        }
        QList<Library::Track> tracks;
        const QJsonArray entries = result.value("tracks").toArray();
        for (const auto &entry : entries) {
            auto item = entry.toObject();
            const QUrl entryUrl(item.value("url").toString());
            if (supported(entryUrl))
                tracks.append(Library::Track{.url = entryUrl, .title = item.value("title").toString(),
                    .artist = {}, .album = {}, .modified = {}, .youtube = true, .audioFormat = {}});
        }
        if (tracks.isEmpty()) {
            setStatus(QStringLiteral("No playable videos were returned."));
            return;
        }
        setStatus(QStringLiteral("Queued %1 videos.").arg(tracks.size()));
        playTracks(tracks);
    });
}

QNetworkReply *YouTubePanel::prepare(const Library::Track &track, Prepared done)
{
    QJsonObject body{{"url", track.url.toString()}};
    if (!track.audioFormat.isEmpty())
        body.insert("audio", track.audioFormat);
    setStatus(QStringLiteral("Preparing audio…"));
    return post(QStringLiteral("/api/play-audio"), body,
                [this, done = std::move(done)](const QJsonObject &result, const QString &error) {
        const QString path = result.value("audio").toString();
        if (!error.isEmpty() || !audioPath.match(path).hasMatch()) {
            const QString message = error.isEmpty()
                ? QStringLiteral("The audio path returned by the server is invalid.")
                : error + QStringLiteral(" Temporary playback requires /api/play-audio on the Waguri server.");
            setStatus(message);
            done({}, {}, 0, {}, message);
            return;
        }
        const QUrl audio = server.resolved(QUrl(path));
        const QString title = result.value("title").toString();
        const double duration = qMax(0.0, result.value("duration_ms").toDouble() / 1000.0);
        const QString thumbnail = result.value("thumbnail").toString();
        setStatus(QStringLiteral("Playing temporary server audio."));
        if (!coverPath.match(thumbnail).hasMatch()) {
            done(audio, title, duration, {}, {});
            return;
        }
        auto *reply = network.get(requestFor(thumbnail, 5000));
        connect(reply, &QNetworkReply::readyRead, this, [reply] {
            if (reply->bytesAvailable() > maxThumbnail)
                reply->abort();
        });
        connect(reply, &QNetworkReply::finished, this, [reply, done, audio, title, duration] {
            QImage cover;
            if (reply->error() == QNetworkReply::NoError &&
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {
                QBuffer bytes;
                bytes.setData(reply->readAll());
                bytes.open(QIODevice::ReadOnly);
                QImageReader reader(&bytes);
                const QSize size = reader.size();
                if (size.isValid()) {
                    if (qMax(size.width(), size.height()) > 768)
                        reader.setScaledSize(size.scaled(768, 768, Qt::KeepAspectRatio));
                    cover = reader.read();
                }
            }
            done(audio, title, duration, cover, {});
            reply->deleteLater();
        });
    });
}

void YouTubePanel::openFormatPicker()
{
    QUrl picker = server;
    QUrlQuery parameters;
    if (!input->text().trimmed().isEmpty()) {
        const auto url = link();
        if (url.isEmpty())
            return;
        parameters.addQueryItem(QStringLiteral("url"), url.toString(QUrl::FullyEncoded));
    }
    picker.setQuery(parameters);
    if (!QDesktopServices::openUrl(picker))
        setStatus(QStringLiteral("Could not open the default browser."));
}

void YouTubePanel::download()
{
    const QUrl url = link();
    if (url.isEmpty())
        return;
    if (isPlaylist(url)) {
        setStatus(QStringLiteral("Choose a single video to download."));
        return;
    }
    static const QRegularExpression pattern(R"(^[A-Za-z0-9_-]{1,40}\+[A-Za-z0-9_-]{1,40}$)");
    const QString selection = downloadFormat->text().trimmed();
    if (!pattern.match(selection).hasMatch()) {
        setStatus(QStringLiteral("Enter audio and video format IDs as AUDIO+VIDEO."));
        return;
    }

    QNetworkRequest request = requestFor(QStringLiteral("/api/download"), 0);
    request.setRawHeader("Accept", "application/x-ndjson");
    const auto codes = selection.split(u'+');
    const QJsonObject body{{"url", url.toString()}, {"mode", "full"},
        {"audio", codes.at(0)}, {"video", codes.at(1)}, {"container", "mp4"}, {"thumbnail", true}};
    auto *reply = network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    auto final = std::make_shared<bool>(false);
    saveButton->setEnabled(false);
    archiveButton->setEnabled(false);
    resultArchive.clear();
    setStatus(QStringLiteral("Preparing download on the local server…"));
    auto read = [this, reply, final](bool last) {
        if (reply->bytesAvailable() > maxJson) {
            reply->abort();
            return;
        }
        while (reply->canReadLine() || (last && reply->bytesAvailable())) {
            const QByteArray raw = reply->readLine().trimmed();
            if (raw.isEmpty())
                continue;
            const auto item = QJsonDocument::fromJson(raw).object();
            const QString type = item.value("type").toString();
            if (type == "result" || item.contains("success")) {
                *final = true;
                if (!item.value("success").toBool()) {
                    setStatus(responseError(reply, item));
                    continue;
                }
                const QString archive = item.value("downloads").toObject().value("archive").toString();
                if (archivePath.match(archive).hasMatch()) {
                    resultArchive = server.resolved(QUrl(archive));
                    archiveButton->setEnabled(true);
                }
                setStatus(resultArchive.isEmpty() ? QStringLiteral("Download completed on the local server.")
                                                  : QStringLiteral("Download completed. Your result ZIP is available."));
            } else if (type == "progress") {
                setStatus(QStringLiteral("%1 · %2 · %3%").arg(item.value("stage").toString(),
                    item.value("state").toString()).arg(item.value("percent").toDouble(), 0, 'f', 0));
            } else if (type == "status") {
                setStatus(item.value("message").toString());
            }
        }
    };
    connect(reply, &QNetworkReply::readyRead, this, [read] { read(false); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, final, read] {
        read(true);
        if (!*final)
            setStatus(reply->error() != QNetworkReply::NoError ? responseError(reply, {})
                : QStringLiteral("Server connection closed before it confirmed the download."));
        saveButton->setEnabled(true);
        reply->deleteLater();
    });
}
