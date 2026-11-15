#include "mpris.hpp"

#include <QDBusConnection>
#include <QDBusMessage>

namespace {
const QString objectPath = QStringLiteral("/org/mpris/MediaPlayer2");
const QString playerInterface = QStringLiteral("org.mpris.MediaPlayer2.Player");
}

MprisPlayer::MprisPlayer(QObject *object) : QDBusAbstractAdaptor(object) {}
MprisRoot::MprisRoot(QObject *object) : QDBusAbstractAdaptor(object) {}

void MprisRoot::Raise() { if (raise) raise(); }
void MprisRoot::Quit() { if (quit) quit(); }
void MprisPlayer::Next() { if (canGoNext() && next) next(); }
void MprisPlayer::Previous() { if (canGoPrevious() && previous) previous(); }
void MprisPlayer::Pause() { if (canPause() && pause) pause(); }
void MprisPlayer::PlayPause() { if (canPlay() && playPause) playPause(); }
void MprisPlayer::Stop() { if (stop) stop(); }
void MprisPlayer::Play() { if (canPlay() && play) play(); }

void MprisPlayer::Seek(qlonglong offset)
{
    if (canSeek() && seekRelative)
        seekRelative(offset / 1000000.0);
}

void MprisPlayer::SetPosition(const QDBusObjectPath &id, qlonglong requested)
{
    if (canSeek() && seekAbsolute && requested >= 0 &&
        track.value(QStringLiteral("mpris:trackid")).value<QDBusObjectPath>() == id)
        seekAbsolute(requested / 1000000.0);
}

void MprisPlayer::OpenUri(const QString &uri)
{
    if (open)
        open(QUrl(uri));
}

void MprisPlayer::update(const QVariantMap &metadata, const QString &nextState,
                         bool canPlay, bool canSeek, bool previousAvailable, bool nextAvailable)
{
    const bool previousCanPause = canPause();
    QVariantMap changes;
    auto record = [&changes](const QString &name, auto &old, const auto &value) {
        if (old != value) {
            old = value;
            changes.insert(name, value);
        }
    };
    record(QStringLiteral("Metadata"), track, metadata);
    record(QStringLiteral("PlaybackStatus"), state, nextState);
    record(QStringLiteral("CanPlay"), playable, canPlay);
    record(QStringLiteral("CanSeek"), seekable, canSeek);
    record(QStringLiteral("CanGoNext"), nextTrack, nextAvailable);
    record(QStringLiteral("CanGoPrevious"), previousTrack, previousAvailable);
    if (previousCanPause != canPause())
        changes.insert(QStringLiteral("CanPause"), canPause());
    sendChanges(changes);
}

void MprisPlayer::positionChanged(double seconds)
{
    positionUs = qRound64(seconds * 1000000);
}

void MprisPlayer::notifySeek(double seconds)
{
    positionChanged(seconds);
    emit Seeked(positionUs);
}

void MprisPlayer::setRate(double value)
{
    if (value >= minimumRate() && value <= maximumRate() && value != playbackRate) {
        playbackRate = value;
        if (rateChanged)
            rateChanged(value);
        sendChanges({{QStringLiteral("Rate"), playbackRate}});
    }
}

void MprisPlayer::setVolume(double value)
{
    const double requested = qBound(0.0, value, 1.0);
    if (requested != audioVolume) {
        audioVolume = requested;
        if (volumeChanged)
            volumeChanged(requested);
        sendChanges({{QStringLiteral("Volume"), audioVolume}});
    }
}

void MprisPlayer::sendChanges(const QVariantMap &changes)
{
    if (changes.isEmpty())
        return;
    auto signal = QDBusMessage::createSignal(objectPath, QStringLiteral("org.freedesktop.DBus.Properties"),
                                             QStringLiteral("PropertiesChanged"));
    signal << playerInterface << changes << QStringList{};
    QDBusConnection::sessionBus().send(signal);
}
