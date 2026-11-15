#pragma once

#include "library.hpp"

#include <QCache>
#include <QListWidget>
#include <QSet>
#include <QTimer>

class TrackList final : public QListWidget {
public:
    explicit TrackList(QWidget *parent = nullptr);
    void addTrack(const Library::Track &track, qsizetype sourceIndex, const QString &detail,
                  const QString &duration, bool unavailable);
    void clearTracks();
    void setAmbient(bool enabled);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void requestVisibleArtwork();

    QTimer artworkTimer;
    QCache<QString, QImage> thumbnails{192};
    QSet<QString> pending;
};
