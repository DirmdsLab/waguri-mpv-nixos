#pragma once

#include <functional>

#include <QDBusAbstractAdaptor>
#include <QDBusObjectPath>
#include <QObject>
#include <QUrl>
#include <QVariantMap>

class MprisPlayer final : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(qlonglong Position READ position)
    Q_PROPERTY(double Rate READ rate WRITE setRate)
    Q_PROPERTY(double MinimumRate READ minimumRate)
    Q_PROPERTY(double MaximumRate READ maximumRate)
    Q_PROPERTY(double Volume READ volume WRITE setVolume)
    Q_PROPERTY(bool CanGoNext READ canGoNext)
    Q_PROPERTY(bool CanGoPrevious READ canGoPrevious)
    Q_PROPERTY(bool CanPlay READ canPlay)
    Q_PROPERTY(bool CanPause READ canPause)
    Q_PROPERTY(bool CanSeek READ canSeek)
    Q_PROPERTY(bool CanControl READ canControl)

public:
    explicit MprisPlayer(QObject *object);
    void update(const QVariantMap &metadata, const QString &state,
                bool canPlay, bool canSeek, bool previous, bool next);
    void positionChanged(double seconds);
    void notifySeek(double seconds);

    QString playbackStatus() const { return state; }
    QVariantMap metadata() const { return track; }
    qlonglong position() const { return positionUs; }
    bool canPlay() const { return playable; }
    bool canPause() const { return state != QStringLiteral("Stopped"); }
    bool canSeek() const { return seekable; }
    bool canGoNext() const { return nextTrack; }
    bool canGoPrevious() const { return previousTrack; }
    bool canControl() const { return true; }
    double rate() const { return playbackRate; }
    double minimumRate() const { return 0.25; }
    double maximumRate() const { return 4.0; }
    double volume() const { return audioVolume; }
    void setRate(double rate);
    void setVolume(double volume);

    std::function<void()> next, previous, pause, playPause, stop, play;
    std::function<void(double)> seekRelative, seekAbsolute, rateChanged, volumeChanged;
    std::function<void(const QUrl &)> open;

public slots:
    void Next();
    void Previous();
    void Pause();
    void PlayPause();
    void Stop();
    void Play();
    void Seek(qlonglong offset);
    void SetPosition(const QDBusObjectPath &id, qlonglong position);
    void OpenUri(const QString &uri);

signals:
    void Seeked(qlonglong position);

private:
    void sendChanges(const QVariantMap &changes);
    QVariantMap track;
    QString state = QStringLiteral("Stopped");
    qlonglong positionUs = 0;
    double playbackRate = 1;
    double audioVolume = 1;
    bool playable = false;
    bool seekable = false;
    bool nextTrack = false;
    bool previousTrack = false;
};

class MprisRoot final : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
    Q_PROPERTY(bool CanQuit READ canQuit)
    Q_PROPERTY(bool CanRaise READ canRaise)
    Q_PROPERTY(bool HasTrackList READ hasTrackList)
    Q_PROPERTY(QString Identity READ identity)
    Q_PROPERTY(QString DesktopEntry READ desktopEntry)
    Q_PROPERTY(QStringList SupportedUriSchemes READ supportedUriSchemes)
    Q_PROPERTY(QStringList SupportedMimeTypes READ supportedMimeTypes)

public:
    explicit MprisRoot(QObject *object);
    bool canQuit() const { return true; }
    bool canRaise() const { return true; }
    bool hasTrackList() const { return false; }
    QString identity() const { return QStringLiteral("Waguri MPV"); }
    QString desktopEntry() const { return QStringLiteral("io.waguri.Player"); }
    QStringList supportedUriSchemes() const { return {"file", "http", "https"}; }
    QStringList supportedMimeTypes() const { return {"audio/mpeg", "audio/flac", "video/mp4", "video/webm"}; }
    std::function<void()> raise, quit;

public slots:
    void Raise();
    void Quit();
};
