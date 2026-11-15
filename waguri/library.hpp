#pragma once

#include <QDateTime>
#include <QImage>
#include <QList>
#include <QString>
#include <QUrl>

namespace Library {
struct Track {
    QUrl url;
    QString title;
    QString artist;
    QString album;
    QDateTime modified;
    double duration = 0;
    bool video = false;
    bool youtube = false;
    QString audioFormat;
};

bool isPlaylist(const QString &path);
bool isMedia(const QString &path);
Track fromUrl(const QUrl &url);
QList<Track> scan(const QString &directory);
QList<Track> readPlaylist(const QString &filename, QString &error);
bool writePlaylist(const QString &filename, const QList<Track> &tracks, QString &error);
QImage artwork(const Track &track);
}
