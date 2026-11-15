#include "tracklist.hpp"

#include <QFontMetrics>
#include <QFutureWatcher>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QtConcurrentRun>

namespace {
enum TrackRole {
    DetailRole = Qt::UserRole + 1,
    DurationRole,
    ArtworkKeyRole,
    MediaUrlRole,
    KindRole,
    UnavailableRole,
};

enum MediaKind { Audio, Video, Playlist };
constexpr int rowHeight = 74;
constexpr int artSize = 54;
constexpr int thumbnailSize = 112;
constexpr int concurrentRequests = 3;

class TrackRow final : public QStyledItemDelegate {
public:
    TrackRow(const QCache<QString, QImage> &art, TrackList *view)
        : QStyledItemDelegate(view), artworks(art), trackList(view)
    {
    }

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        return {0, rowHeight};
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setRenderHint(QPainter::SmoothPixmapTransform);

        const QRect row = option.rect.adjusted(0, 2, -2, -2);
        const bool selected = option.state & QStyle::State_Selected;
        const bool ambient = trackList->property("ambientActive").toBool();
        if (selected || (option.state & QStyle::State_MouseOver)) {
            painter->setPen(Qt::NoPen);
            const QColor rowColor = ambient
                ? (selected ? QColor(237, 208, 255, 55) : QColor(244, 225, 251, 22))
                : (selected ? QColor("#3a2c54") : QColor("#25222b"));
            painter->setBrush(rowColor);
            painter->drawRoundedRect(row, 14, 14);
        }
        if (option.state & QStyle::State_HasFocus) {
            painter->setBrush(Qt::NoBrush);
            painter->setPen(QPen(ambient ? QColor(230, 202, 255, 100) : QColor("#8e7fb1"), 1));
            painter->drawRoundedRect(row.adjusted(1, 1, -1, -1), 14, 14);
        }

        const QRect cover(row.x() + 8, row.center().y() - artSize / 2, artSize, artSize);
        QPainterPath clip;
        clip.addRoundedRect(cover, 11, 11);
        painter->save();
        painter->setClipPath(clip);
        const auto *artwork = artworks.object(index.data(ArtworkKeyRole).toString());
        if (artwork && !artwork->isNull()) {
            painter->drawImage(cover, *artwork);
        } else {
            const QColor placeholder = ambient
                ? (selected ? QColor(244, 212, 255, 52) : QColor(244, 212, 255, 25))
                : (selected ? QColor("#574377") : QColor("#35303f"));
            painter->fillRect(cover, placeholder);
            const int kind = index.data(KindRole).toInt();
            const QPixmap &icon = kind == Video ? videoIcon : kind == Playlist ? playlistIcon : audioIcon;
            const QRect symbol(cover.center().x() - 12, cover.center().y() - 12, 24, 24);
            painter->drawPixmap(symbol, icon);
        }
        painter->restore();

        const int start = cover.right() + 14;
        const QString duration = index.data(DurationRole).toString();
        QFont detailFont = option.font;
        detailFont.setPixelSize(13);
        painter->setFont(detailFont);
        const int durationWidth = duration.isEmpty() ? 0 : painter->fontMetrics().horizontalAdvance(duration) + 14;
        const int textWidth = qMax(0, row.right() - start - durationWidth - 12);

        QFont titleFont = option.font;
        titleFont.setPixelSize(15);
        titleFont.setWeight(selected ? QFont::DemiBold : QFont::Medium);
        painter->setFont(titleFont);
        const bool unavailable = index.data(UnavailableRole).toBool();
        painter->setPen(unavailable ? QColor("#8d8795") : QColor("#f3edf7"));
        const QString title = painter->fontMetrics().elidedText(index.data().toString(), Qt::ElideRight, textWidth);
        painter->drawText(QRect(start, row.center().y() - 21, textWidth, 23),
                          Qt::AlignVCenter | Qt::AlignLeft, title);

        painter->setFont(detailFont);
        painter->setPen(unavailable ? QColor("#8d8795") : QColor("#c9c0d0"));
        const QString detail = painter->fontMetrics().elidedText(
            index.data(DetailRole).toString(), Qt::ElideRight, textWidth);
        painter->drawText(QRect(start, row.center().y() + 3, textWidth, 20),
                          Qt::AlignVCenter | Qt::AlignLeft, detail);
        if (durationWidth > 0)
            painter->drawText(row.adjusted(0, 0, -12, 0), Qt::AlignRight | Qt::AlignVCenter, duration);

        painter->restore();
    }

private:
    const QCache<QString, QImage> &artworks;
    TrackList *trackList;
    const QPixmap audioIcon{QStringLiteral(":/waguri/audio-mode.png")};
    const QPixmap videoIcon{QStringLiteral(":/waguri/video-mode.png")};
    const QPixmap playlistIcon{QStringLiteral(":/waguri/queue.png")};
};

QImage loadThumbnail(const QUrl &url)
{
    Library::Track track;
    track.url = url;
    const QImage image = Library::artwork(track);
    if (image.isNull())
        return {};
    const QImage square = image.scaled(thumbnailSize, thumbnailSize,
        Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    return square.copy((square.width() - thumbnailSize) / 2, (square.height() - thumbnailSize) / 2,
                       thumbnailSize, thumbnailSize);
}
}

TrackList::TrackList(QWidget *parent) : QListWidget(parent)
{
    setObjectName(QStringLiteral("mediaTracks"));
    setAccessibleName(QStringLiteral("Media library"));
    setItemDelegate(new TrackRow(thumbnails, this));
    setUniformItemSizes(true);
    setMouseTracking(true);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    artworkTimer.setSingleShot(true);
    artworkTimer.setInterval(35);
    connect(&artworkTimer, &QTimer::timeout, this, &TrackList::requestVisibleArtwork);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { artworkTimer.start(); });
}

void TrackList::addTrack(const Library::Track &track, qsizetype sourceIndex, const QString &detail,
                         const QString &duration, bool unavailable)
{
    const bool isPlaylist = Library::isPlaylist(track.url.path());
    auto *item = new QListWidgetItem(track.title, this);
    item->setData(Qt::UserRole, sourceIndex);
    item->setData(Qt::AccessibleDescriptionRole, detail);
    item->setData(DetailRole, detail);
    item->setData(DurationRole, duration);
    item->setData(KindRole, isPlaylist ? Playlist : track.video ? Video : Audio);
    item->setData(UnavailableRole, unavailable);
    item->setToolTip(track.url.toDisplayString(QUrl::PreferLocalFile));

    if (!isPlaylist && track.url.isLocalFile() && !unavailable) {
        const QString key = QString::fromUtf8(track.url.toEncoded()) + u'#' +
                            QString::number(track.modified.toMSecsSinceEpoch());
        item->setData(ArtworkKeyRole, key);
        item->setData(MediaUrlRole, track.url);
    }
    if (!artworkTimer.isActive())
        artworkTimer.start();
}

void TrackList::clearTracks()
{
    artworkTimer.stop();
    clear();
}

void TrackList::setAmbient(bool enabled)
{
    setProperty("ambientActive", enabled);
    viewport()->update();
}

void TrackList::resizeEvent(QResizeEvent *event)
{
    QListWidget::resizeEvent(event);
    artworkTimer.start();
}

void TrackList::showEvent(QShowEvent *event)
{
    QListWidget::showEvent(event);
    artworkTimer.start();
}

void TrackList::requestVisibleArtwork()
{
    if (!isVisible() || count() == 0)
        return;
    const int firstIndex = indexAt(QPoint(1, 1)).row();
    const int first = qMax(0, firstIndex - 1);
    const int last = qMin(count(), first + viewport()->height() / rowHeight + 3);

    for (int row = first; row < last && pending.size() < concurrentRequests; ++row) {
        const auto *rowItem = item(row);
        const QString key = rowItem->data(ArtworkKeyRole).toString();
        if (key.isEmpty() || pending.contains(key) || thumbnails.contains(key))
            continue;

        const QUrl url = rowItem->data(MediaUrlRole).toUrl();
        pending.insert(key);
        auto *watcher = new QFutureWatcher<QImage>(this);
        connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, key] {
            const QImage image = watcher->result();
            thumbnails.insert(key, new QImage(image));
            pending.remove(key);
            if (!image.isNull())
                viewport()->update();
            watcher->deleteLater();
            artworkTimer.start();
        });
        watcher->setFuture(QtConcurrent::run(loadThumbnail, url));
    }
}
