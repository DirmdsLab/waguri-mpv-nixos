#include "library.hpp"
#include "lyrics.hpp"
#include "mpris.hpp"
#include "player.hpp"
#include "spectrum.h"
#include "tracklist.hpp"
#include "youtube.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <clocale>

#include <QApplication>
#include <QIcon>
#include <QTabBar>
#include <QFile>
#include <numeric>
#include <QCommandLineParser>
#include <QCollator>
#include <QComboBox>
#include <QDBusConnection>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QNetworkReply>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSettings>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStackedLayout>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QtConcurrentRun>

namespace {
using Library::Track;
constexpr int seekSteps = 10000;

void configureFontFallbacks()
{
    const QString japanese = QStringLiteral("Noto Sans CJK JP");
    const QString korean = QStringLiteral("Noto Sans CJK KR");
    const QString simplifiedChinese = QStringLiteral("Noto Sans CJK SC");
    const QString traditionalChinese = QStringLiteral("Noto Sans CJK TC");

    // Register the packaged fonts directly when the session overrides Fontconfig.
    const QString fontDirectory = qEnvironmentVariable("WAGURI_CJK_FONTS");
    if (!fontDirectory.isEmpty() && !QFontDatabase::hasFamily(japanese)) {
        const QDir directory(fontDirectory);
        const QStringList fonts = directory.entryList({QStringLiteral("*.ttc"),
            QStringLiteral("*.otf"), QStringLiteral("*.ttf")}, QDir::Files);
        for (const QString &font : fonts)
            QFontDatabase::addApplicationFont(directory.filePath(font));
    }

    QString hanFamily = japanese;
    const QLocale locale;
    if (locale.language() == QLocale::Korean)
        hanFamily = korean;
    else if (locale.language() == QLocale::Chinese)
        hanFamily = locale.script() == QLocale::TraditionalHanScript ? traditionalChinese : simplifiedChinese;

    QFontDatabase::setApplicationFallbackFontFamilies(QChar::Script_Han, {hanFamily});
    QFontDatabase::setApplicationFallbackFontFamilies(QChar::Script_Hiragana, {japanese});
    QFontDatabase::setApplicationFallbackFontFamilies(QChar::Script_Katakana, {japanese});
    QFontDatabase::setApplicationFallbackFontFamilies(QChar::Script_Hangul, {korean});
}

QString clockText(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0)
        seconds = 0;
    const auto elapsed = qint64(seconds);
    auto result = QStringLiteral("%1:%2").arg((elapsed / 60) % 60, 2, 10, QChar(u'0'))
        .arg(elapsed % 60, 2, 10, QChar(u'0'));
    if (elapsed >= 3600)
        result.prepend(QString::number(elapsed / 3600) + u':');
    return result;
}

QStringList mediaFilters()
{
    return {QStringLiteral("Media files (*.mp3 *.flac *.m4a *.aac *.opus *.ogg *.wav *.mka *.mp4 *.mkv *.webm *.mov *.avi *.m4v *.ts *.m3u *.m3u8)"),
            QStringLiteral("All files (*)")};
}

bool playableUrl(const QUrl &url)
{
    if (url.isLocalFile())
        return QFileInfo(url.toLocalFile()).isFile();
    return url.isValid() && (url.scheme() == "https" || url.scheme() == "http") &&
           !url.host().isEmpty() && url.userInfo().isEmpty();
}

template <class Task, class Done>
void background(QObject *owner, Task task, Done done)
{
    using Result = decltype(task());
    auto *watcher = new QFutureWatcher<Result>(owner);
    QObject::connect(watcher, &QFutureWatcher<Result>::finished, owner, [watcher, done = std::move(done)] {
        done(watcher->result());
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run(std::move(task)));
}

QToolButton *tool(const QString &caption, const QString &description, const QString &style = {})
{
    auto *button = new QToolButton;
    button->setText(caption);
    button->setToolTip(description);
    button->setAccessibleName(description);
    button->setObjectName(style);
    return button;
}

QIcon controlIcon(const QString &name, bool checkable = false)
{
    const QString asset = QStringLiteral(":/waguri/") + name;
    QIcon icon(asset + QStringLiteral(".png"));
    if (checkable)
        icon.addFile(asset + QStringLiteral("-on.png"), {}, QIcon::Normal, QIcon::On);
    return icon;
}

QToolButton *iconTool(const QString &icon, const QString &description, const QString &style = {},
                      bool checkable = false)
{
    auto *button = tool({}, description, style);
    button->setCheckable(checkable);
    button->setIcon(controlIcon(icon, checkable));
    button->setIconSize(QSize(22, 22));
    return button;
}

class Spectrum final : public QWidget {
public:
    Spectrum()
    {
        animation.setInterval(16);
        animation.setTimerType(Qt::PreciseTimer);
        connect(&animation, &QTimer::timeout, this, [this] { advance(); });
    }

    void refresh(bool playing, bool enabled)
    {
        running = playing;
        if (!enabled) {
            animation.stop();
            levels.fill(0);
            if (!isHidden())
                hide();
            return;
        }
        if (isHidden())
            show();
        if (running && isVisible() && !animation.isActive()) {
            clock.start();
            animation.start();
        }
        if (!isVisible())
            animation.stop();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        QLinearGradient tint(0, height(), 0, 0);
        tint.setColorAt(0.0, QColor(169, 146, 229, 55));
        tint.setColorAt(1.0, QColor(225, 204, 255, 145));
        painter.setBrush(tint);

        const double column = double(width()) / levels.size();
        const double barWidth = qMax(1.0, column - 3);
        const int availableHeight = qMax(0, height() - 3);
        for (size_t band = 0; band < levels.size(); ++band) {
            const double barHeight = 3 + levels[band] * availableHeight;
            painter.drawRoundedRect(QRectF(band * column, height() - barHeight, barWidth, barHeight),
                                    barWidth / 2, barWidth / 2);
        }
    }

private:
    void advance()
    {
        if (!isVisible()) {
            animation.stop();
            return;
        }
        const double elapsed = qBound(1.0, double(clock.restart()), 60.0);
        const float attack = float(-std::expm1(-elapsed / 37.0));
        const float release = float(-std::expm1(-elapsed / 142.0));
        std::array<float, WAGURI_SPECTRUM_BANDS> current{};
        if (running)
            waguri_spectrum_read(current.data());

        bool hasMotion = false;
        bool changed = false;
        for (size_t band = 0; band < levels.size(); ++band) {
            const float before = band > 0 ? current[band - 1] : current[band];
            const float after = band + 1 < current.size() ? current[band + 1] : current[band];
            const float target = 0.84f * current[band] + 0.08f * (before + after);
            const float previousLevel = levels[band];
            const float change = target - previousLevel;
            levels[band] += change * (change > 0 ? attack : release);
            if (levels[band] < 0.002f)
                levels[band] = 0;
            hasMotion |= levels[band] > 0;
            changed |= levels[band] != previousLevel;
        }
        if (changed)
            update();
        if (!running && !hasMotion)
            animation.stop();
    }

    QElapsedTimer clock;
    QTimer animation;
    std::array<float, WAGURI_SPECTRUM_BANDS> levels{};
    bool running = false;
};

class Cover final : public QWidget {
public:
    explicit Cover(QWidget *parent) : QWidget(parent), fallback(QStringLiteral(":/waguri/cover.jpg"))
    {
        setAccessibleName(QStringLiteral("Player artwork"));
    }

    void setArtwork(const QImage &value)
    {
        artwork = value;
        update();
    }

    void setScale(bool crop, double value)
    {
        fill = crop;
        zoom = value;
        update();
    }

    std::function<void()> toggleFullscreen;
    std::function<void(int)> navigate;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.fillRect(rect(), QColor(15, 14, 21));
        const QImage &image = artwork.isNull() ? fallback : artwork;
        if (image.isNull())
            return;
        QSizeF size = image.size();
        size.scale(this->size(), fill ? Qt::KeepAspectRatioByExpanding : Qt::KeepAspectRatio);
        size *= zoom;
        const QRectF target(QPointF((width() - size.width()) / 2, (height() - size.height()) / 2), size);
        painter.drawImage(target, image);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        pressed = event->position();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        const auto distance = event->position() - pressed;
        if (navigate && qAbs(distance.x()) > 90 && qAbs(distance.y()) < 70)
            navigate(distance.x() > 0 ? -1 : 1);
    }

    void mouseDoubleClickEvent(QMouseEvent *) override
    {
        if (toggleFullscreen)
            toggleFullscreen();
    }

private:
    QImage fallback;
    QImage artwork;
    QPointF pressed;
    bool fill = false;
    double zoom = 1;
};

class Stage final : public QWidget {
public:
    explicit Stage(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMinimumHeight(140);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        frame = new QWidget(this);
        player = new Player(frame);
        cover = new Cover(frame);
        auto *stack = new QStackedLayout(frame);
        stack->setContentsMargins(0, 0, 0, 0);
        stack->setStackingMode(QStackedLayout::StackAll);
        stack->addWidget(player);
        stack->addWidget(cover);
        stack->setCurrentWidget(cover);
    }

    void shape(double ratio, bool full)
    {
        aspect = ratio;
        fullscreen = full;
        place();
    }

    Player *player = nullptr;
    Cover *cover = nullptr;

protected:
    void resizeEvent(QResizeEvent *) override { place(); }

private:
    void place()
    {
        QSize area = size();
        if (aspect > 0 && !fullscreen) {
            if (double(area.width()) > area.height() * aspect)
                area.setWidth(qRound(area.height() * aspect));
            else
                area.setHeight(qRound(area.width() / aspect));
        }
        frame->setGeometry((width() - area.width()) / 2, (height() - area.height()) / 2,
                           area.width(), area.height());
        if (fullscreen) {
            frame->clearMask();
        } else {
            QPainterPath clip;
            clip.addRoundedRect(QRectF(QPointF(0, 0), QSizeF(area)), 16, 16);
            frame->setMask(QRegion(clip.toFillPolygon().toPolygon()));
        }
    }

    QWidget *frame = nullptr;
    double aspect = 16.0 / 9;
    bool fullscreen = false;
};

class Window final : public QWidget {
public:
    Window();
    ~Window() override;
    void open(const QList<QUrl> &urls, bool autoplay = true);

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dropEvent(QDropEvent *) override;

private:
    void createPlayer(QWidget *panel);
    QWidget *createTracks();
    void setupMpris();
    void chooseFolder();
    void scanFolder();
    void chooseFiles();
    void openLink();
    void refreshLibrary();
    void playRow(int row);
    void setQueue(const QList<Track> &tracks, int index = 0);
    void rebuildOrder();
    void queueTrack(const Track &track, bool playNext);
    void playCurrent();
    void load(const QUrl &source);
    void fileLoaded();
    void playbackFailed(const QString &error);
    void stop();
    void togglePlayback();
    void move(int delta, bool natural = false);
    void seekTo(double seconds);
    void updateInfo();
    void updateControls();
    void tick();
    void cancelPreparation();
    void showQueue();
    void showLibraryMenu();
    void showOptions();
    void showTrackMenu();
    void showAudioDeviceMenu();
    void saveQueue();
    void openSavedPlaylist();
    void removeFromSavedPlaylist(int row);
    void exportTracks(const QList<Track> &tracks, const QString &title);
    void toggleFullscreen();
    void updateViewport();
    void applyVideoMode();
    void populateArtwork();
    void setArtwork(const QImage &image);
    void setAmbient(bool enabled);
    void transitionAmbient();
    void sampleAmbient(const QImage &image);
    void showStatus(const QString &message);
    const Track *currentTrack() const;
    QString playlistDirectory() const;

    QSettings state;
    QSplitter *splitter = nullptr;
    QWidget *playerPanel = nullptr;
    QWidget *header = nullptr;
    QWidget *details = nullptr;
    QWidget *transport = nullptr;
    QTabWidget *tabs = nullptr;
    Stage *stage = nullptr;
    Player *player = nullptr;
    Spectrum *spectrum = nullptr;
    LyricsPanel *lyrics = nullptr;
    YouTubePanel *youtube = nullptr;
    QLineEdit *search = nullptr;
    QComboBox *sort = nullptr;
    TrackList *mediaList = nullptr;
    QLabel *folderLabel = nullptr;
    QLabel *title = nullptr;
    QLabel *subtitle = nullptr;
    QLabel *elapsed = nullptr;
    QToolButton *total = nullptr;
    QLabel *status = nullptr;
    QSlider *timeline = nullptr;
    QToolButton *playButton = nullptr;
    QToolButton *previous = nullptr;
    QToolButton *next = nullptr;
    QToolButton *shuffle = nullptr;
    QToolButton *repeat = nullptr;
    QToolButton *visualizer = nullptr;
    QToolButton *video = nullptr;
    QToolButton *shape = nullptr;
    QToolButton *zoom = nullptr;
    QToolButton *options = nullptr;
    MprisPlayer *mpris = nullptr;
    QString dbusService;

    QString folder;
    QString playlistTitle;
    QString savedPlaylistPath;
    QList<Track> folderTracks;
    QList<Track> imported;
    QList<Track> playlist;
    QList<Track> displayed;
    QList<Track> queue;
    QList<int> order;
    QPointer<QNetworkReply> audioRequest;
    QColor ambientSource{QStringLiteral("#75539b")};
    QColor ambientTint{Qt::transparent};
    QVariantAnimation ambientAnimation;
    QString ambientStyle;
    QImage artwork;
    QUrl playbackUrl;
    QString playbackTitle;
    QString playbackArtist;
    QString playbackAlbum;
    int queueIndex = -1;
    int repeatMode = 0;
    int aspectMode = 0;
    int zoomStep = 0;
    int scanGeneration = 0;
    int sourceGeneration = 0;
    int playbackGeneration = 0;
    int ambientFrameTicks = 0;
    int ambientReach = 35;
    double duration = 0;
    double position = 0;
    bool active = false;
    bool loaded = false;
    bool videoMode = true;
    bool videoTrack = false;
    bool paused = false;
    bool hasSeek = false;
    bool remaining = false;
    bool ambient = false;
    bool autoLyrics = false;
    bool libraryVisible = true;
    bool ascending = true;
    bool scanning = false;
    QString playbackAudioWarning;
};

Window::Window()
{
    setWindowTitle(QStringLiteral("Waguri MPV"));
    setAcceptDrops(true);
    setMinimumSize(560, 470);
    resize(1100, 690);

    folder = state.value(QStringLiteral("library/folder")).toString();
    ascending = state.value(QStringLiteral("library/ascending"), true).toBool();
    ambient = state.value(QStringLiteral("player/ambient"), false).toBool();
    ambientReach = qBound(10, state.value(QStringLiteral("player/ambientReach"), 35).toInt(), 100);
    autoLyrics = state.value(QStringLiteral("player/autoLyrics"), false).toBool();
    videoMode = state.value(QStringLiteral("player/video"), true).toBool();
    aspectMode = qBound(0, state.value(QStringLiteral("player/aspect"), 3).toInt(), 3);
    zoomStep = qBound(0, state.value(QStringLiteral("player/zoom"), 0).toInt(), 3);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    splitter = new QSplitter(Qt::Horizontal);
    splitter->setChildrenCollapsible(false);
    playerPanel = new QWidget;
    createPlayer(playerPanel);
    tabs = new QTabWidget;
    tabs->setObjectName(QStringLiteral("libraryTabs"));
    tabs->setDocumentMode(true);
    tabs->tabBar()->setExpanding(true);
    tabs->addTab(createTracks(), QStringLiteral("Tracks"));
    lyrics = new LyricsPanel;
    tabs->addTab(lyrics, QStringLiteral("Lyrics"));
    youtube = new YouTubePanel;
    tabs->addTab(youtube, QStringLiteral("YouTube"));
    for (int index = 0; index < tabs->count(); ++index)
        tabs->widget(index)->setObjectName(QStringLiteral("libraryPage"));
    splitter->addWidget(playerPanel);
    splitter->addWidget(tabs);
    splitter->setStretchFactor(0, 4);
    splitter->setStretchFactor(1, 5);
    splitter->setSizes({480, 620});
    layout->addWidget(splitter);

    QFile ambientTheme(QStringLiteral(":/waguri/ambient.qss"));
    if (ambientTheme.open(QIODevice::ReadOnly))
        ambientStyle = QString::fromUtf8(ambientTheme.readAll());
    ambientAnimation.setDuration(480);
    ambientAnimation.setEasingCurve(QEasingCurve::InOutCubic);
    connect(&ambientAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        ambientTint = value.value<QColor>();
        update();
    });
    setAmbient(ambient);

    lyrics->seek = [this](double seconds) { seekTo(seconds); };
    youtube->playTracks = [this](const QList<Track> &tracks) { setQueue(tracks); };
    player->fileLoaded = [this] { fileLoaded(); };
    player->changed = [this] { updateControls(); };
    player->failed = [this](const QString &error) { playbackFailed(error); };
    player->audioWarning = [this](const QString &warning) {
        playbackAudioWarning = warning;
        if (active)
            showStatus(warning);
    };
    player->ended = [this](bool natural) {
        if (natural) {
            move(1, true);
        } else {
            stop();
        }
    };
    player->doubleClicked = [this] { toggleFullscreen(); };
    stage->cover->toggleFullscreen = [this] { toggleFullscreen(); };
    stage->cover->navigate = [this](int delta) { move(delta); };

    setupMpris();
    const auto shortcut = [this](const QKeySequence &key, auto action) {
        auto *binding = new QShortcut(key, this);
        connect(binding, &QShortcut::activated, this, action);
    };
    const auto playbackShortcut = [this](auto action) {
        return [action] {
            if (!qobject_cast<QLineEdit *>(QApplication::focusWidget()))
                action();
        };
    };
    shortcut(QKeySequence(Qt::Key_Space), playbackShortcut([this] { togglePlayback(); }));
    shortcut(QKeySequence(Qt::Key_Left), playbackShortcut([this] { seekTo(position - 5); }));
    shortcut(QKeySequence(Qt::Key_Right), playbackShortcut([this] { seekTo(position + 5); }));
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_O), [this] { chooseFiles(); });
    shortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O), [this] { chooseFolder(); });
    shortcut(QKeySequence(Qt::Key_F11), [this] { toggleFullscreen(); });
    shortcut(QKeySequence(Qt::Key_Escape), [this] { if (isFullScreen()) toggleFullscreen(); });
    shortcut(QKeySequence(Qt::Key_MediaPlay), [this] { togglePlayback(); });
    shortcut(QKeySequence(Qt::Key_MediaTogglePlayPause), [this] { togglePlayback(); });
    shortcut(QKeySequence(Qt::Key_MediaStop), [this] { stop(); });
    shortcut(QKeySequence(Qt::Key_MediaPrevious), [this] { move(-1); });
    shortcut(QKeySequence(Qt::Key_MediaNext), [this] { move(1); });

    const QString preferredDevice = qEnvironmentVariable("WAGURI_AUDIO_DEVICE",
        state.value(QStringLiteral("player/audioDevice"), QStringLiteral("auto")).toString());
    if (!player->selectAudioDevice(preferredDevice))
        player->selectAudioDevice(QStringLiteral("auto"));
    player->set("mute", QStringLiteral("no"));
    player->set("volume", state.value(QStringLiteral("player/volume"), 100).toString());
    if (mpris)
        mpris->setVolume(player->property("volume").toDouble() / 100.0);
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] { tick(); });
    timer->start(60);
    updateViewport();
    updateControls();
    scanFolder();
    if (!player->isReady())
        showStatus(player->error());
}

Window::~Window()
{
    cancelPreparation();
    waguri_spectrum_enable(false);
    if (!dbusService.isEmpty()) {
        auto bus = QDBusConnection::sessionBus();
        bus.unregisterObject(QStringLiteral("/org/mpris/MediaPlayer2"));
        bus.unregisterService(dbusService);
    }
}

void Window::createPlayer(QWidget *panel)
{
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(22, 16, 22, 16);
    layout->setSpacing(11);
    header = new QWidget;
    auto *tools = new QHBoxLayout(header);
    tools->setContentsMargins(0, 0, 0, 0);
    tools->setSpacing(4);
    auto *brand = new QLabel(QStringLiteral("UwU Player"));
    brand->setObjectName(QStringLiteral("brand"));
    visualizer = iconTool(QStringLiteral("visualizer"), QStringLiteral("Audio visualizer"),
                          QStringLiteral("toggle"), true);
    visualizer->setChecked(state.value(QStringLiteral("player/spectrum"), true).toBool());
    shape = tool(QStringLiteral("16:9"), QStringLiteral("Cycle viewport aspect ratio"));
    zoom = tool(QStringLiteral("100%"), QStringLiteral("Cycle zoom"));
    auto *queueButton = iconTool(QStringLiteral("queue"), QStringLiteral("Playback queue"));
    video = iconTool(QStringLiteral("video-mode"), QStringLiteral("Toggle audio-only and video mode"),
                     QStringLiteral("toggle"), true);
    video->setChecked(videoMode);
    video->setIcon(controlIcon(videoMode ? QStringLiteral("video-mode") : QStringLiteral("audio-mode"), videoMode));
    options = iconTool(QStringLiteral("more"), QStringLiteral("Player options"));
    tools->addWidget(brand);
    tools->addStretch();
    for (auto *button : {visualizer, shape, zoom, queueButton, video, options})
        tools->addWidget(button);
    layout->addWidget(header);

    stage = new Stage;
    player = stage->player;
    layout->addWidget(stage, 1);

    details = new QWidget;
    auto *description = new QVBoxLayout(details);
    description->setContentsMargins(0, 0, 0, 0);
    description->setSpacing(5);
    title = new QLabel(QStringLiteral("Nothing Playing"));
    title->setObjectName(QStringLiteral("title"));
    title->setWordWrap(true);
    title->setTextFormat(Qt::PlainText);
    subtitle = new QLabel(QStringLiteral("Choose a folder or open a file to play."));
    subtitle->setObjectName(QStringLiteral("secondary"));
    subtitle->setWordWrap(true);
    subtitle->setTextFormat(Qt::PlainText);
    description->addWidget(title);
    description->addWidget(subtitle);
    auto *timeRow = new QHBoxLayout;
    elapsed = new QLabel(QStringLiteral("00:00"));
    elapsed->setObjectName(QStringLiteral("time"));
    total = tool(QStringLiteral("00:00"), QStringLiteral("Toggle remaining time"));
    total->setStyleSheet(QStringLiteral("QToolButton { background: transparent; color: #b8b5c4; padding: 0; }"));
    timeRow->addWidget(elapsed);
    timeRow->addStretch();
    timeRow->addWidget(total);
    description->addLayout(timeRow);
    timeline = new QSlider(Qt::Horizontal);
    timeline->setRange(0, seekSteps);
    timeline->setAccessibleName(QStringLiteral("Playback position"));
    description->addWidget(timeline);
    layout->addWidget(details);

    transport = new QWidget;
    transport->setMinimumHeight(59);
    auto *layers = new QStackedLayout(transport);
    layers->setStackingMode(QStackedLayout::StackAll);
    layers->setContentsMargins(0, 0, 0, 0);
    spectrum = new Spectrum;
    layers->addWidget(spectrum);
    auto *controls = new QWidget;
    auto *buttons = new QHBoxLayout(controls);
    buttons->setContentsMargins(2, 0, 2, 0);
    shuffle = iconTool(QStringLiteral("shuffle"), QStringLiteral("Shuffle"), QStringLiteral("transport"), true);
    shuffle->setChecked(state.value(QStringLiteral("player/shuffle"), false).toBool());
    previous = tool({}, QStringLiteral("Previous track"), QStringLiteral("transport"));
    playButton = tool({}, QStringLiteral("Play"), QStringLiteral("play"));
    next = tool({}, QStringLiteral("Next track"), QStringLiteral("transport"));
    previous->setIcon(QIcon(QStringLiteral(":/waguri/previous.png")));
    next->setIcon(QIcon(QStringLiteral(":/waguri/next.png")));
    for (auto *button : {previous, playButton, next})
        button->setIconSize(QSize(24, 24));
    repeat = iconTool(QStringLiteral("repeat"), QStringLiteral("Repeat off"), QStringLiteral("transport"), true);
    repeatMode = qBound(0, state.value(QStringLiteral("player/repeat"), 0).toInt(), 2);
    buttons->addStretch();
    for (auto *button : {shuffle, previous, playButton, next, repeat}) {
        buttons->addWidget(button);
        buttons->addStretch();
    }
    layers->addWidget(controls);
    layers->setCurrentWidget(controls);
    layout->addWidget(transport);

    status = new QLabel;
    status->setObjectName(QStringLiteral("status"));
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    status->hide();
    layout->addWidget(status);

    connect(visualizer, &QToolButton::toggled, this, [this](bool enabled) {
        state.setValue(QStringLiteral("player/spectrum"), enabled);
        waguri_spectrum_enable(enabled && active);
        spectrum->refresh(active && !paused, enabled);
    });
    connect(shape, &QToolButton::clicked, this, [this] {
        aspectMode = (aspectMode + 1) % 4;
        state.setValue(QStringLiteral("player/aspect"), aspectMode);
        updateViewport();
    });
    connect(zoom, &QToolButton::clicked, this, [this] {
        zoomStep = (zoomStep + 1) % 4;
        state.setValue(QStringLiteral("player/zoom"), zoomStep);
        updateViewport();
    });
    connect(queueButton, &QToolButton::clicked, this, [this] { showQueue(); });
    connect(video, &QToolButton::toggled, this, [this](bool enabled) {
        videoMode = enabled;
        state.setValue(QStringLiteral("player/video"), videoMode);
        applyVideoMode();
    });
    connect(options, &QToolButton::clicked, this, [this] { showOptions(); });
    connect(total, &QToolButton::clicked, this, [this] { remaining = !remaining; tick(); });
    connect(timeline, &QSlider::sliderMoved, this, [this](int value) {
        if (duration > 0)
            elapsed->setText(clockText(duration * value / seekSteps));
    });
    connect(timeline, &QSlider::sliderReleased, this, [this] {
        seekTo(duration * timeline->value() / seekSteps);
    });
    connect(timeline, &QSlider::actionTriggered, this, [this](int action) {
        if (action != QAbstractSlider::SliderMove && !timeline->isSliderDown())
            seekTo(duration * timeline->sliderPosition() / seekSteps);
    });
    connect(shuffle, &QToolButton::toggled, this, [this](bool checked) {
        state.setValue(QStringLiteral("player/shuffle"), checked);
        rebuildOrder();
        updateControls();
    });
    connect(previous, &QToolButton::clicked, this, [this] { move(-1); });
    connect(playButton, &QToolButton::clicked, this, [this] { togglePlayback(); });
    connect(next, &QToolButton::clicked, this, [this] { move(1); });
    connect(repeat, &QToolButton::clicked, this, [this] {
        repeatMode = (repeatMode + 1) % 3;
        state.setValue(QStringLiteral("player/repeat"), repeatMode);
        updateControls();
    });
}

QWidget *Window::createTracks()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(18, 15, 18, 13);
    layout->setSpacing(10);
    auto *actions = new QHBoxLayout;
    auto *choose = new QPushButton(QStringLiteral("Choose Folder"));
    choose->setObjectName(QStringLiteral("primaryAction"));
    auto *files = new QPushButton(QStringLiteral("Open Media"));
    auto *playlists = new QPushButton(QStringLiteral("Playlists"));
    auto *more = iconTool(QStringLiteral("more"), QStringLiteral("Library options"));
    actions->addWidget(choose);
    actions->addWidget(files);
    actions->addWidget(playlists);
    actions->addStretch();
    actions->addWidget(more);

    folderLabel = new QLabel(QStringLiteral("No folder selected"));
    folderLabel->setObjectName(QStringLiteral("collectionHeading"));
    folderLabel->setWordWrap(true);
    folderLabel->setTextFormat(Qt::PlainText);

    auto *filters = new QHBoxLayout;
    search = new QLineEdit;
    search->setPlaceholderText(QStringLiteral("Search your library"));
    search->setClearButtonEnabled(true);
    sort = new QComboBox;
    sort->addItems({QStringLiteral("Name"), QStringLiteral("Last Modified"), QStringLiteral("Duration")});
    sort->setCurrentIndex(qBound(0, state.value(QStringLiteral("library/sort"), 0).toInt(), 2));
    auto *direction = tool(ascending ? QStringLiteral("↑") : QStringLiteral("↓"), QStringLiteral("Reverse sort"));
    filters->addWidget(search, 1);
    filters->addWidget(sort);
    filters->addWidget(direction);

    mediaList = new TrackList;
    mediaList->setContextMenuPolicy(Qt::CustomContextMenu);
    auto *jump = new QHBoxLayout;
    auto *top = tool(QStringLiteral("⇤"), QStringLiteral("Jump to first track"));
    auto *bottom = tool(QStringLiteral("⇥"), QStringLiteral("Jump to last track"));
    jump->addStretch();
    jump->addWidget(top);
    jump->addWidget(bottom);

    layout->addWidget(folderLabel);
    layout->addLayout(actions);
    layout->addLayout(filters);
    layout->addWidget(mediaList, 1);
    layout->addLayout(jump);

    connect(choose, &QPushButton::clicked, this, [this] { chooseFolder(); });
    connect(files, &QPushButton::clicked, this, [this] { chooseFiles(); });
    connect(playlists, &QPushButton::clicked, this, [this] { openSavedPlaylist(); });
    connect(more, &QToolButton::clicked, this, [this] { showLibraryMenu(); });
    connect(search, &QLineEdit::textChanged, this, [this] { refreshLibrary(); });
    connect(sort, &QComboBox::currentIndexChanged, this, [this](int index) {
        state.setValue(QStringLiteral("library/sort"), index);
        refreshLibrary();
    });
    connect(direction, &QToolButton::clicked, this, [this, direction] {
        ascending = !ascending;
        state.setValue(QStringLiteral("library/ascending"), ascending);
        direction->setText(ascending ? QStringLiteral("↑") : QStringLiteral("↓"));
        refreshLibrary();
    });
    connect(mediaList, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        playRow(mediaList->row(item));
    });
    connect(mediaList, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        const auto row = mediaList->row(item);
        if (row >= 0 && row < displayed.size() && (!active || !currentTrack() ||
            currentTrack()->url != displayed.at(row).url))
            playRow(row);
    });
    connect(mediaList, &QListWidget::customContextMenuRequested, this, [this](const QPoint &point) {
        auto *item = mediaList->itemAt(point);
        if (!item)
            return;
        int row = mediaList->row(item);
        Track track = displayed.at(row);
        QMenu menu(this);
        menu.addAction(QStringLiteral("Play"), this, [this, row, track] {
            if (row < displayed.size() && displayed.at(row).url == track.url)
                playRow(row);
        });
        if (!Library::isPlaylist(track.url.path())) {
            menu.addAction(QStringLiteral("Play next"), this, [this, track] { queueTrack(track, true); });
            menu.addAction(QStringLiteral("Add to queue"), this, [this, track] { queueTrack(track, false); });
            menu.addAction(QStringLiteral("Add to saved playlist…"), this, [this, track] {
                bool ok = false;
                QString name = QInputDialog::getText(this, QStringLiteral("Add to playlist"),
                    QStringLiteral("Playlist name"), QLineEdit::Normal, {}, &ok).trimmed();
                if (!ok || name.isEmpty() || name.contains(u'/') || name.contains(u'\\') ||
                    name.startsWith(u'.') || name.contains(u'\n'))
                    return;
                const QString directory = playlistDirectory();
                if (!QDir().mkpath(directory)) {
                    showStatus(QStringLiteral("Could not create the playlists directory."));
                    return;
                }
                const QString path = QDir(directory).filePath(name + QStringLiteral(".m3u8"));
                QString error;
                QList<Track> tracks;
                if (QFileInfo::exists(path))
                    tracks = Library::readPlaylist(path, error);
                if (!error.isEmpty()) {
                    showStatus(error);
                    return;
                }
                tracks.append(track);
                if (Library::writePlaylist(path, tracks, error)) {
                    showStatus(QStringLiteral("Added to %1.").arg(name));
                    if (savedPlaylistPath == path) {
                        playlist.append(track);
                        refreshLibrary();
                    }
                } else {
                    showStatus(error);
                }
            });
        }
        if (!savedPlaylistPath.isEmpty())
            menu.addAction(QStringLiteral("Remove from this playlist"), this, [this, row] { removeFromSavedPlaylist(row); });
        if (track.url.isLocalFile() && QFileInfo::exists(track.url.toLocalFile())) {
            menu.addAction(QStringLiteral("Move file to Trash…"), this, [this, track] {
                if (QMessageBox::question(this, QStringLiteral("Move file to Trash"),
                    QStringLiteral("Move “%1” to Trash?").arg(QFileInfo(track.url.toLocalFile()).fileName())) != QMessageBox::Yes)
                    return;
                if (currentTrack() && currentTrack()->url == track.url)
                    stop();
                QFile file(track.url.toLocalFile());
                if (!file.moveToTrash()) {
                    showStatus(file.errorString());
                    return;
                }
                imported.removeIf([&](const Track &entry) { return entry.url == track.url; });
                scanFolder();
            });
        }
        menu.exec(mediaList->viewport()->mapToGlobal(point));
    });
    connect(top, &QToolButton::clicked, mediaList, &QListWidget::scrollToTop);
    connect(bottom, &QToolButton::clicked, mediaList, &QListWidget::scrollToBottom);
    return page;
}

void Window::chooseFolder()
{
    const QString selected = QFileDialog::getExistingDirectory(this, QStringLiteral("Choose a media folder"),
        folder.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::MusicLocation) : folder);
    if (selected.isEmpty())
        return;
    ++sourceGeneration;
    folder = selected;
    state.setValue(QStringLiteral("library/folder"), folder);
    playlistTitle.clear();
    savedPlaylistPath.clear();
    search->clear();
    scanFolder();
}

void Window::scanFolder()
{
    const int request = ++scanGeneration;
    if (folder.isEmpty()) {
        scanning = false;
        folderTracks.clear();
        refreshLibrary();
        return;
    }
    if (!QFileInfo(folder).isReadable()) {
        scanning = false;
        folderTracks.clear();
        refreshLibrary();
        folderLabel->setText(QStringLiteral("The selected folder is unavailable."));
        return;
    }
    scanning = true;
    refreshLibrary();
    background(this, [directory = folder] { return Library::scan(directory); },
               [this, request](const QList<Track> &tracks) {
        if (request != scanGeneration)
            return;
        scanning = false;
        folderTracks = tracks;
        refreshLibrary();
    });
}

void Window::chooseFiles()
{
    QFileDialog dialog(this, QStringLiteral("Open media or M3U playlist"), folder);
    dialog.setFileMode(QFileDialog::ExistingFiles);
    dialog.setNameFilters(mediaFilters());
    if (dialog.exec() == QDialog::Accepted)
        open(dialog.selectedUrls());
}

void Window::openLink()
{
    bool accepted = false;
    const QString value = QInputDialog::getText(this, QStringLiteral("Open media URL"),
        QStringLiteral("HTTP or HTTPS URL"), QLineEdit::Normal, {}, &accepted);
    if (!accepted)
        return;
    const QUrl url(value.trimmed());
    if (!playableUrl(url)) {
        showStatus(QStringLiteral("Enter a valid HTTP or HTTPS media URL."));
        return;
    }
    open({url});
}

void Window::open(const QList<QUrl> &urls, bool autoplay)
{
    const int request = ++sourceGeneration;
    const int generation = playbackGeneration;
    const bool onePlaylist = urls.size() == 1 && urls.first().isLocalFile() && Library::isPlaylist(urls.first().toLocalFile());
    if (onePlaylist) {
        const QString path = urls.first().toLocalFile();
        const QString name = QFileInfo(path).completeBaseName();
        showStatus(QStringLiteral("Reading playlist…"));
        background(this, [path] {
            QString error;
            auto tracks = Library::readPlaylist(path, error);
            return qMakePair(tracks, error);
        }, [this, request, generation, path, name, autoplay](const auto &result) {
            if (request != sourceGeneration)
                return;
            if (!result.second.isEmpty()) {
                showStatus(result.second);
                return;
            }
            playlist = result.first;
            playlistTitle = name;
            savedPlaylistPath = QFileInfo(path).absolutePath() == playlistDirectory() ? path : QString();
            search->clear();
            refreshLibrary();
            tabs->setCurrentIndex(0);
            showStatus({});
            if (autoplay && generation == playbackGeneration) {
                QList<Track> playable;
                for (const Track &track : playlist)
                    if (playableUrl(track.url))
                        playable.append(track);
                setQueue(playable);
            }
        });
        return;
    }

    background(this, [urls] {
        QList<Track> tracks;
        for (const auto &url : urls) {
            if (!playableUrl(url))
                continue;
            if (url.isLocalFile() && Library::isPlaylist(url.toLocalFile())) {
                QString error;
                const auto entries = Library::readPlaylist(url.toLocalFile(), error);
                for (const Track &entry : entries)
                    if (playableUrl(entry.url))
                        tracks.append(entry);
            } else {
                tracks.append(Library::fromUrl(url));
            }
        }
        return tracks;
    }, [this, request, generation, autoplay](const QList<Track> &tracks) {
        if (request != sourceGeneration)
            return;
        if (tracks.isEmpty()) {
            showStatus(QStringLiteral("No readable media was selected."));
            return;
        }
        playlistTitle.clear();
        savedPlaylistPath.clear();
        for (const Track &track : tracks) {
            if (std::none_of(imported.begin(), imported.end(), [&](const Track &other) { return other.url == track.url; }))
                imported.append(track);
        }
        refreshLibrary();
        if (autoplay && generation == playbackGeneration)
            setQueue(tracks);
    });
}

void Window::refreshLibrary()
{
    displayed.clear();
    QList<Track> sources = playlistTitle.isEmpty() ? folderTracks : playlist;
    if (playlistTitle.isEmpty()) {
        QSet<QUrl> known;
        for (const Track &track : sources)
            known.insert(track.url);
        for (const Track &track : imported)
            if (!known.contains(track.url))
                sources.append(track);
    }

    const bool showFolder = playlistTitle.isEmpty();
    const QString label = showFolder ? QFileInfo(folder).fileName() : playlistTitle;
    if (scanning && showFolder)
        folderLabel->setText(QStringLiteral("Scanning %1…").arg(label));
    else if (label.isEmpty() && sources.isEmpty())
        folderLabel->setText(QStringLiteral("No folder selected"));
    else
        folderLabel->setText(QStringLiteral("%1 · %2 files").arg(label.isEmpty() ?
            QStringLiteral("Open Media") : label).arg(sources.size()));

    if (showFolder) {
        QCollator collator;
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        collator.setNumericMode(true);
        const int mode = sort->currentIndex();
        std::stable_sort(sources.begin(), sources.end(), [&](const Track &a, const Track &b) {
            int comparison = 0;
            if (mode == 1 && a.modified != b.modified)
                comparison = a.modified < b.modified ? -1 : 1;
            if (mode == 2 && a.duration != b.duration)
                comparison = a.duration < b.duration ? -1 : 1;
            if (comparison == 0)
                comparison = collator.compare(a.title, b.title);
            return ascending ? comparison < 0 : comparison > 0;
        });
    }

    const QString filter = search->text().trimmed();
    const auto *current = currentTrack();
    const QUrl selected = current && active ? current->url : QUrl();
    const int scroll = mediaList->verticalScrollBar()->value();
    mediaList->clearTracks();
    int selectedRow = -1;
    for (qsizetype index = 0; index < sources.size(); ++index) {
        const Track &track = sources.at(index);
        const QString filename = QFileInfo(track.url.path()).fileName();
        if (!filter.isEmpty() && !(track.title + u' ' + track.artist + u' ' + filename).contains(filter, Qt::CaseInsensitive))
            continue;
        const bool isPlaylist = Library::isPlaylist(track.url.path());
        const bool unavailable = track.url.isLocalFile() && !QFileInfo::exists(track.url.toLocalFile());
        QStringList details;
        if (!track.artist.isEmpty())
            details.append(track.artist);
        if (!track.album.isEmpty())
            details.append(track.album);
        if (details.isEmpty() && !filename.isEmpty() && filename != track.title)
            details.append(filename);
        if (isPlaylist || unavailable)
            details.append(unavailable ? QStringLiteral("Unavailable") : QStringLiteral("Playlist"));
        mediaList->addTrack(track, index, details.join(QStringLiteral(" · ")),
                            track.duration > 0 ? clockText(track.duration) : QString(), unavailable);
        if (selectedRow < 0 && track.url == selected)
            selectedRow = mediaList->count() - 1;
        displayed.append(track);
    }
    if (selectedRow >= 0)
        mediaList->setCurrentRow(selectedRow);
    mediaList->verticalScrollBar()->setValue(scroll);
    if (displayed.isEmpty() && !scanning && !filter.isEmpty())
        folderLabel->setText(QStringLiteral("No media matches “%1”.").arg(filter));
    else if (displayed.isEmpty() && !scanning && showFolder && !folder.isEmpty())
        folderLabel->setText(QStringLiteral("No supported media in %1.").arg(label));
}

void Window::playRow(int row)
{
    if (row < 0 || row >= displayed.size())
        return;
    const Track selected = displayed.at(row);
    if (!playableUrl(selected.url)) {
        showStatus(QStringLiteral("This track is unavailable."));
        return;
    }
    if (selected.url.isLocalFile() && Library::isPlaylist(selected.url.toLocalFile())) {
        open({selected.url});
        return;
    }
    ++sourceGeneration;
    QList<Track> tracks;
    int index = 0;
    for (int i = 0; i < displayed.size(); ++i) {
        const Track &track = displayed.at(i);
        if (i == row)
            index = int(tracks.size());
        if (!Library::isPlaylist(track.url.path()) && playableUrl(track.url))
            tracks.append(track);
    }
    setQueue(tracks, index);
}

void Window::setQueue(const QList<Track> &tracks, int index)
{
    if (tracks.isEmpty()) {
        showStatus(QStringLiteral("No playable tracks."));
        return;
    }
    queue = tracks;
    queueIndex = qBound(0, index, int(queue.size()) - 1);
    rebuildOrder();
    playCurrent();
}

void Window::rebuildOrder()
{
    order.resize(queue.size());
    std::iota(order.begin(), order.end(), 0);
    if (!shuffle->isChecked() || queueIndex < 0)
        return;
    order.move(queueIndex, 0);
    for (int i = int(order.size()) - 1; i > 1; --i)
        order.swapItemsAt(i, QRandomGenerator::global()->bounded(1, i + 1));
}

void Window::queueTrack(const Track &track, bool playNext)
{
    if (!playableUrl(track.url)) {
        showStatus(QStringLiteral("This track is unavailable."));
        return;
    }
    auto found = std::find_if(queue.begin(), queue.end(), [&](const Track &item) { return item.url == track.url; });
    int index = int(found - queue.begin());
    if (index == queueIndex)
        return;
    if (found == queue.end()) {
        queue.append(track);
        order.append(index);
    }
    if (queueIndex < 0)
        queueIndex = index;
    else if (playNext) {
        order.removeOne(index);
        order.insert(order.indexOf(queueIndex) + 1, index);
    }
    showStatus(playNext ? QStringLiteral("Queued to play next.") : QStringLiteral("Added to queue."));
    updateControls();
}

const Track *Window::currentTrack() const
{
    return queueIndex >= 0 && queueIndex < queue.size() ? &queue.at(queueIndex) : nullptr;
}

void Window::cancelPreparation()
{
    ++playbackGeneration;
    auto previousRequest = audioRequest;
    audioRequest.clear();
    if (previousRequest)
        previousRequest->abort();
}

void Window::playCurrent()
{
    const Track *track = currentTrack();
    if (!track || !player->isReady())
        return;
    cancelPreparation();
    if (!playableUrl(track->url)) {
        playbackFailed(QStringLiteral("The selected file is unavailable."));
        return;
    }
    player->stop();
    active = true;
    loaded = false;
    paused = false;
    hasSeek = false;
    videoTrack = track->video;
    duration = track->duration;
    position = 0;
    playbackUrl.clear();
    playbackTitle = track->title;
    playbackArtist = track->artist;
    playbackAlbum = track->album;
    playbackAudioWarning.clear();
    setArtwork({});
    waguri_spectrum_enable(visualizer->isChecked());
    lyrics->setTrack({}, {}, {}, false);
    showStatus(track->youtube ? QStringLiteral("Preparing YouTube audio…") : QString());
    updateInfo();
    updateControls();
    refreshLibrary();
    if (track->youtube) {
        const int generation = playbackGeneration;
        audioRequest = youtube->prepare(*track, [this, generation](const QUrl &url, const QString &name,
                                      double length, const QImage &cover, const QString &error) {
            if (generation != playbackGeneration)
                return;
            audioRequest.clear();
            if (!error.isEmpty()) {
                playbackFailed(error);
                return;
            }
            if (!name.isEmpty())
                playbackTitle = name;
            duration = length;
            setArtwork(cover);
            updateInfo();
            load(url);
        });
        return;
    }
    populateArtwork();
    load(track->url);
}

void Window::load(const QUrl &url)
{
    playbackUrl = url;
    player->set("vid", videoMode ? QStringLiteral("auto") : QStringLiteral("no"));
    player->set("aid", QStringLiteral("auto"));
    player->set("sid", QStringLiteral("auto"));
    player->set("pause", QStringLiteral("no"));
    const QString source = url.isLocalFile() ? url.toLocalFile() : QString::fromUtf8(url.toEncoded());
    if (!player->load(source))
        playbackFailed(player->error().isEmpty() ? QStringLiteral("The media could not be opened.") : player->error());
}

void Window::fileLoaded()
{
    if (!active)
        return;
    loaded = true;
    const QVariantMap metadata = player->property("metadata").toMap();
    const Track *track = currentTrack();
    if (track && !track->youtube) {
        const QString newTitle = player->property("media-title").toString();
        if (!newTitle.isEmpty() && !(track->url.isLocalFile() &&
            newTitle == QFileInfo(track->url.toLocalFile()).fileName()))
            playbackTitle = newTitle;
        for (auto it = metadata.begin(); it != metadata.end(); ++it) {
            if (it.key().compare(QStringLiteral("artist"), Qt::CaseInsensitive) == 0)
                playbackArtist = it.value().toString();
            if (it.key().compare(QStringLiteral("album"), Qt::CaseInsensitive) == 0)
                playbackAlbum = it.value().toString();
        }
    }
    const QVariantList tracks = player->property("track-list").toList();
    videoTrack = std::any_of(tracks.begin(), tracks.end(), [](const QVariant &entry) {
        const auto item = entry.toMap();
        return item.value("type").toString() == "video" && !item.value("albumart").toBool() &&
            !item.value("image").toBool();
    });
    applyVideoMode();
    lyrics->setTrack(track ? track->url : playbackUrl, playbackTitle, playbackArtist, autoLyrics);
    showStatus(playbackAudioWarning);
    updateInfo();
    updateControls();
}

void Window::playbackFailed(const QString &error)
{
    if (active) {
        stop();
        showStatus(QStringLiteral("Unable to play media. %1").arg(error));
    } else {
        showStatus(error);
    }
}

void Window::stop()
{
    cancelPreparation();
    active = loaded = paused = hasSeek = videoTrack = false;
    player->stop();
    waguri_spectrum_enable(false);
    duration = position = 0;
    playbackUrl.clear();
    playbackTitle.clear();
    playbackArtist.clear();
    playbackAlbum.clear();
    lyrics->setTrack({}, {}, {}, false);
    setArtwork({});
    if (isFullScreen())
        toggleFullscreen();
    applyVideoMode();
    showStatus({});
    updateInfo();
    updateControls();
    refreshLibrary();
    tick();
}

void Window::togglePlayback()
{
    if (!active)
        playCurrent();
    else if (loaded)
        player->set("pause", paused ? QStringLiteral("no") : QStringLiteral("yes"));
}

void Window::move(int delta, bool natural)
{
    const int current = int(order.indexOf(queueIndex));
    if (current < 0)
        return;
    int destination = natural && repeatMode == 2 ? current : current + delta;
    const int count = int(order.size());
    if (destination < 0 || destination >= count) {
        if (repeatMode == 1)
            destination = (destination + count) % count;
        else {
            if (natural)
                stop();
            else if (delta < 0 && loaded)
                seekTo(0);
            return;
        }
    }
    queueIndex = order.at(destination);
    playCurrent();
}

void Window::seekTo(double seconds)
{
    if (!hasSeek || !std::isfinite(seconds))
        return;
    const double target = qBound(0.0, seconds, duration);
    if (player->command({QStringLiteral("seek"), QString::number(target, 'f', 3),
                         QStringLiteral("absolute+exact")}) && mpris)
        mpris->notifySeek(target);
}

void Window::updateInfo()
{
    title->setText(active ? playbackTitle : QStringLiteral("Nothing Playing"));
    QStringList description;
    if (!playbackArtist.isEmpty())
        description.append(playbackArtist);
    if (!playbackAlbum.isEmpty())
        description.append(playbackAlbum);
    if (active && currentTrack()) {
        if (currentTrack()->youtube)
            description.append(QStringLiteral("YouTube audio"));
        else if (const QString extension = QFileInfo(currentTrack()->url.path()).suffix().toUpper(); !extension.isEmpty())
            description.append(extension);
    }
    subtitle->setText(active ? description.join(QStringLiteral(" · ")) :
        (currentTrack() ? QStringLiteral("Press Play to start the selected track.")
                        : QStringLiteral("Choose a folder or open a file to play.")));
}

void Window::updateControls()
{
    if (loaded) {
        paused = player->property("pause").toBool();
        const double knownDuration = player->property("duration").toDouble();
        if (std::isfinite(knownDuration) && knownDuration > 0)
            duration = knownDuration;
        hasSeek = duration > 0 && player->property("seekable").toBool();
    }
    const int orderIndex = int(order.indexOf(queueIndex));
    const bool canPrevious = hasSeek || orderIndex > 0 || (repeatMode == 1 && order.size() > 1);
    const bool canNext = (orderIndex >= 0 && orderIndex + 1 < order.size()) || (repeatMode == 1 && order.size() > 1);
    const bool canPlay = currentTrack() != nullptr && player->isReady();
    playButton->setEnabled(canPlay && (!active || loaded));
    previous->setEnabled(canPrevious);
    next->setEnabled(canNext);
    timeline->setEnabled(hasSeek);
    const QString playText = active && !paused ? QStringLiteral("Pause") : QStringLiteral("Play");
    playButton->setIcon(QIcon(active && !paused ? QStringLiteral(":/waguri/pause.png")
                                                : QStringLiteral(":/waguri/play.png")));
    playButton->setToolTip(playText);
    playButton->setAccessibleName(playText);
    repeat->setChecked(repeatMode != 0);
    const QString repeatText[] = {QStringLiteral("Repeat off"), QStringLiteral("Repeat all"), QStringLiteral("Repeat one")};
    repeat->setToolTip(repeatText[repeatMode]);
    repeat->setAccessibleName(repeatText[repeatMode]);
    repeat->setIcon(controlIcon(repeatMode == 2 ? QStringLiteral("repeat-one") : QStringLiteral("repeat"), true));

    if (mpris) {
        QVariantMap values;
        if (active) {
            values = {{QStringLiteral("mpris:trackid"), QVariant::fromValue(QDBusObjectPath(
                QStringLiteral("/io/waguri/track/%1").arg(playbackGeneration)))},
                      {QStringLiteral("xesam:title"), playbackTitle},
                      {QStringLiteral("xesam:artist"), QStringList{playbackArtist}},
                      {QStringLiteral("xesam:album"), playbackAlbum},
                      {QStringLiteral("xesam:url"), currentTrack()->url.toString()}};
            if (duration > 0)
                values.insert(QStringLiteral("mpris:length"), qRound64(duration * 1000000.0));
        }
        mpris->update(values, !active || !loaded ? QStringLiteral("Stopped") :
                      (paused ? QStringLiteral("Paused") : QStringLiteral("Playing")),
                      canPlay, hasSeek, canPrevious, canNext);
    }
}

void Window::tick()
{
    if (loaded) {
        const double value = player->property("time-pos").toDouble();
        if (std::isfinite(value))
            position = qMax(0.0, value);
    }
    if (!timeline->isSliderDown()) {
        elapsed->setText(clockText(position));
        const int value = duration > 0 ? qRound(qBound(0.0, position / duration, 1.0) * seekSteps) : 0;
        const QSignalBlocker block(timeline);
        timeline->setValue(value);
    }
    total->setText(remaining && duration > 0 ? QStringLiteral("−") + clockText(duration - position)
                                             : clockText(duration));
    lyrics->sync(position);
    if (mpris)
        mpris->positionChanged(position);
    spectrum->refresh(loaded && !paused, visualizer->isChecked());
    if (++ambientFrameTicks >= 18) {
        ambientFrameTicks = 0;
        if (ambient && loaded && !paused && videoMode && videoTrack && !isFullScreen())
            sampleAmbient(player->captureFrame());
    }
}

void Window::showQueue()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Playback queue"));
    dialog.resize(480, 420);
    auto *layout = new QVBoxLayout(&dialog);
    auto *list = new QListWidget;
    auto *bar = new QHBoxLayout;
    auto *up = new QPushButton(QStringLiteral("Move up"));
    auto *down = new QPushButton(QStringLiteral("Move down"));
    auto *remove = new QPushButton(QStringLiteral("Remove"));
    auto *save = new QPushButton(QStringLiteral("Save playlist…"));
    bar->addWidget(up);
    bar->addWidget(down);
    bar->addWidget(remove);
    bar->addWidget(save);
    layout->addWidget(list);
    layout->addLayout(bar);

    const auto refill = [&] {
        list->clear();
        for (int index : order) {
            const auto &track = queue.at(index);
            list->addItem((index == queueIndex ? QStringLiteral("▶  ") : QString()) + track.title);
        }
        list->setCurrentRow(int(order.indexOf(queueIndex)));
    };
    refill();
    auto shift = [&](int delta) {
        const int row = list->currentRow();
        const int destination = row + delta;
        if (row >= 0 && destination >= 0 && destination < order.size()) {
            order.move(row, destination);
            updateControls();
            refill();
            list->setCurrentRow(destination);
        }
    };
    connect(up, &QPushButton::clicked, &dialog, [&] { shift(-1); });
    connect(down, &QPushButton::clicked, &dialog, [&] { shift(1); });
    connect(remove, &QPushButton::clicked, &dialog, [&] {
        const int row = list->currentRow();
        if (row < 0 || row >= order.size())
            return;
        const int index = order.takeAt(row);
        queue.removeAt(index);
        for (int &entry : order)
            if (entry > index)
                --entry;
        if (index == queueIndex) {
            queueIndex = order.isEmpty() ? -1 : order.at(qMin(row, int(order.size()) - 1));
            stop();
        } else if (queueIndex > index) {
            --queueIndex;
        }
        updateControls();
        refill();
    });
    connect(save, &QPushButton::clicked, &dialog, [this] { saveQueue(); });
    connect(list, &QListWidget::itemActivated, &dialog, [&](QListWidgetItem *item) {
        const int row = list->row(item);
        if (row >= 0 && row < order.size()) {
            queueIndex = order.at(row);
            playCurrent();
            dialog.accept();
        }
    });
    dialog.exec();
}

void Window::showLibraryMenu()
{
    QMenu menu(this);
    menu.addAction(QStringLiteral("Refresh folder"), this, [this] { scanFolder(); });
    menu.addAction(QStringLiteral("Open URL…"), this, [this] { openLink(); });
    menu.addAction(QStringLiteral("Save queue as playlist…"), this, [this] { saveQueue(); });
    menu.addAction(QStringLiteral("Export current tracks as M3U…"), this, [this] {
        exportTracks(displayed, playlistTitle.isEmpty() ? QStringLiteral("Waguri") : playlistTitle);
    });
    if (!playlistTitle.isEmpty()) {
        menu.addAction(QStringLiteral("Back to folder"), this, [this] {
            ++sourceGeneration;
            playlistTitle.clear();
            savedPlaylistPath.clear();
            search->clear();
            refreshLibrary();
        });
    }
    menu.exec(QCursor::pos());
}

void Window::showOptions()
{
    QMenu menu(this);
    menu.addAction(libraryVisible ? QStringLiteral("Hide library") : QStringLiteral("Show library"), this, [this] {
        libraryVisible = !libraryVisible;
        tabs->setVisible(libraryVisible);
    });
    menu.addAction(QStringLiteral("Fullscreen"), this, [this] { toggleFullscreen(); });
    menu.addAction(QStringLiteral("Audio and subtitle tracks…"), this, [this] { showTrackMenu(); })->setEnabled(loaded);
    menu.addAction(QStringLiteral("Audio output…"), this, [this] { showAudioDeviceMenu(); });
    auto *mute = menu.addAction(QStringLiteral("Mute"));
    mute->setCheckable(true);
    mute->setChecked(player->property("mute").toBool());
    connect(mute, &QAction::toggled, this, [this](bool muted) {
        player->set("mute", muted ? QStringLiteral("yes") : QStringLiteral("no"));
    });
    menu.addAction(QStringLiteral("Volume…"), this, [this] {
        bool ok = false;
        const int value = QInputDialog::getInt(this, QStringLiteral("Volume"), QStringLiteral("Volume (%)"),
            int(player->property("volume").toDouble()), 0, 100, 1, &ok);
        if (ok && value > 0)
            player->set("mute", QStringLiteral("no"));
        if (ok && mpris)
            mpris->setVolume(value / 100.0);
        else if (ok) {
            player->set("volume", QString::number(value));
            state.setValue(QStringLiteral("player/volume"), value);
        }
    });
    menu.addAction(QStringLiteral("Playback speed…"), this, [this] {
        bool ok = false;
        const double value = QInputDialog::getDouble(this, QStringLiteral("Playback speed"), QStringLiteral("Speed"),
            player->property("speed").toDouble(), 0.25, 4, 2, &ok, {}, 0.05);
        if (ok && mpris)
            mpris->setRate(value);
        else if (ok)
            player->set("speed", QString::number(value));
    });
    auto *light = menu.addAction(QStringLiteral("Ambient light"));
    light->setCheckable(true);
    light->setChecked(ambient);
    connect(light, &QAction::toggled, this, [this](bool enabled) {
        setAmbient(enabled);
        state.setValue(QStringLiteral("player/ambient"), enabled);
    });
    menu.addAction(QStringLiteral("Ambient light reach…"), this, [this] {
        bool ok = false;
        const int value = QInputDialog::getInt(this, QStringLiteral("Ambient light"), QStringLiteral("Reach (%)"),
            ambientReach, 10, 100, 1, &ok);
        if (ok) {
            ambientReach = value;
            state.setValue(QStringLiteral("player/ambientReach"), value);
            update();
        }
    });
    auto *automatic = menu.addAction(QStringLiteral("Auto lyrics"));
    automatic->setCheckable(true);
    automatic->setChecked(autoLyrics);
    connect(automatic, &QAction::toggled, this, [this](bool enabled) {
        autoLyrics = enabled;
        state.setValue(QStringLiteral("player/autoLyrics"), enabled);
        if (enabled && active && currentTrack())
            lyrics->setTrack(currentTrack()->url, playbackTitle, playbackArtist, true);
    });
    menu.addSeparator();
    menu.addAction(QStringLiteral("Stop Playback"), this, [this] { stop(); })->setEnabled(active);
    menu.addAction(QStringLiteral("Exit"), this, &QWidget::close);
    menu.exec(options->mapToGlobal(options->rect().bottomLeft()));
}

void Window::showAudioDeviceMenu()
{
    QMenu menu(this);
    const QString current = player->property("current-ao").toString();
    if (!current.isEmpty()) {
        menu.addSection(QStringLiteral("Active output: %1").arg(current));
    }
    const QString selected = player->property("audio-device").toString();
    const auto addDevice = [this, selected](QMenu &section, const QString &name, const QString &device) {
        auto *action = section.addAction(name);
        action->setCheckable(true);
        action->setChecked(selected == device || (selected.isEmpty() && device == "auto"));
        connect(action, &QAction::triggered, this, [this, device] {
            if (!player->selectAudioDevice(device)) {
                showStatus(QStringLiteral("Could not select the audio output."));
                return;
            }
            player->set("mute", QStringLiteral("no"));
            state.setValue(QStringLiteral("player/audioDevice"), device);
            playbackAudioWarning.clear();
            showStatus({});
        });
    };
    addDevice(menu, QStringLiteral("System default"), QStringLiteral("auto"));
    addDevice(menu, QStringLiteral("PulseAudio"), QStringLiteral("pulse/"));
    addDevice(menu, QStringLiteral("PipeWire"), QStringLiteral("pipewire/"));
    addDevice(menu, QStringLiteral("ALSA"), QStringLiteral("alsa/"));

    const auto devices = player->property("audio-device-list").toList();
    if (!devices.isEmpty()) {
        auto *available = menu.addMenu(QStringLiteral("Detected devices"));
        for (const QVariant &entry : devices) {
            const auto record = entry.toMap();
            const QString device = record.value("name").toString();
            if (device.isEmpty() || device == "auto" || device == "null" || device.startsWith("null/"))
                continue;
            const QString name = record.value("description").toString();
            addDevice(*available, name.isEmpty() ? device : name, device);
        }
    }
    menu.exec(options->mapToGlobal(options->rect().bottomLeft()));
}

void Window::showTrackMenu()
{
    QMenu menu(this);
    auto *audio = menu.addMenu(QStringLiteral("Audio"));
    auto *subtitleMenu = menu.addMenu(QStringLiteral("Subtitles"));
    subtitleMenu->addAction(QStringLiteral("Off"), this, [this] { player->set("sid", QStringLiteral("no")); });
    const auto tracks = player->property("track-list").toList();
    for (const auto &entry : tracks) {
        const auto track = entry.toMap();
        const QString type = track.value("type").toString();
        if (type != "audio" && type != "sub")
            continue;
        const QString id = track.value("id").toString();
        QStringList label{id, track.value("title").toString(), track.value("lang").toString()};
        label.removeAll(QString());
        auto *section = type == "audio" ? audio : subtitleMenu;
        auto *action = section->addAction(label.join(QStringLiteral(" · ")));
        action->setCheckable(true);
        action->setChecked(track.value("selected").toBool());
        connect(action, &QAction::triggered, this, [this, type, id] {
            player->set(type == "audio" ? "aid" : "sid", id);
        });
    }
    subtitleMenu->addAction(QStringLiteral("Open subtitles…"), this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Open subtitles"), folder,
            QStringLiteral("Subtitles (*.srt *.ass *.ssa *.vtt *.sub *.lrc);;All files (*)"));
        if (!path.isEmpty())
            player->command({QStringLiteral("sub-add"), path, QStringLiteral("select")});
    });
    subtitleMenu->addAction(QStringLiteral("Subtitle delay…"), this, [this] {
        bool ok = false;
        const double delay = QInputDialog::getDouble(this, QStringLiteral("Subtitle delay"), QStringLiteral("Seconds"),
            player->property("sub-delay").toDouble(), -120, 120, 2, &ok, {}, 0.1);
        if (ok)
            player->set("sub-delay", QString::number(delay));
    });
    menu.exec(options->mapToGlobal(options->rect().bottomLeft()));
}

QString Window::playlistDirectory() const
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("playlists"));
}

void Window::saveQueue()
{
    QList<Track> ordered;
    for (int index : order)
        ordered.append(queue.at(index));
    if (ordered.isEmpty()) {
        showStatus(QStringLiteral("The playback queue is empty."));
        return;
    }
    bool accepted = false;
    QString name = QInputDialog::getText(this, QStringLiteral("Save playlist"),
        QStringLiteral("Playlist name"), QLineEdit::Normal, {}, &accepted).trimmed();
    if (!accepted || name.isEmpty() || name.contains(u'/') || name.contains(u'\\') || name.startsWith(u'.') || name.contains(u'\n'))
        return;
    const QString directory = playlistDirectory();
    if (!QDir().mkpath(directory)) {
        showStatus(QStringLiteral("Could not create the playlists directory."));
        return;
    }
    const QString path = QDir(directory).filePath(name + QStringLiteral(".m3u8"));
    if (QFileInfo::exists(path) && QMessageBox::question(this, QStringLiteral("Replace playlist"),
        QStringLiteral("Replace “%1”?").arg(name)) != QMessageBox::Yes)
        return;
    QString error;
    if (!Library::writePlaylist(path, ordered, error))
        showStatus(error);
    else
        showStatus(QStringLiteral("Playlist saved: %1").arg(name));
}

void Window::openSavedPlaylist()
{
    const QDir directory(playlistDirectory());
    const auto files = directory.entryInfoList({QStringLiteral("*.m3u8")}, QDir::Files, QDir::Name);
    if (files.isEmpty()) {
        showStatus(QStringLiteral("No saved playlists. Use Save playlist in the playback queue."));
        return;
    }
    QStringList names;
    for (const QFileInfo &file : files)
        names.append(file.completeBaseName());
    bool accepted = false;
    const QString choice = QInputDialog::getItem(this, QStringLiteral("Playlists"),
        QStringLiteral("Open a saved playlist"), names, 0, false, &accepted);
    if (!accepted || !names.contains(choice))
        return;
    const QString path = files.at(names.indexOf(choice)).absoluteFilePath();
    QMenu menu(this);
    menu.addAction(QStringLiteral("Open"), this, [this, path] { open({QUrl::fromLocalFile(path)}, false); });
    menu.addAction(QStringLiteral("Export…"), this, [this, path, choice] {
        QString error;
        const auto tracks = Library::readPlaylist(path, error);
        if (error.isEmpty())
            exportTracks(tracks, choice);
        else
            showStatus(error);
    });
    menu.addAction(QStringLiteral("Delete…"), this, [this, path, choice] {
        if (QMessageBox::question(this, QStringLiteral("Delete playlist"),
            QStringLiteral("Delete “%1”?").arg(choice)) != QMessageBox::Yes)
            return;
        QFile file(path);
        if (!file.remove()) {
            showStatus(file.errorString());
            return;
        }
        if (savedPlaylistPath == path) {
            ++sourceGeneration;
            savedPlaylistPath.clear();
            playlistTitle.clear();
            refreshLibrary();
        }
    });
    menu.exec(QCursor::pos());
}

void Window::removeFromSavedPlaylist(int row)
{
    if (row < 0 || row >= mediaList->count() || savedPlaylistPath.isEmpty())
        return;
    const int index = mediaList->item(row)->data(Qt::UserRole).toInt();
    if (index < 0 || index >= playlist.size())
        return;
    auto updated = playlist;
    updated.removeAt(index);
    QString error;
    if (!Library::writePlaylist(savedPlaylistPath, updated, error)) {
        showStatus(error);
        return;
    }
    playlist = updated;
    refreshLibrary();
}

void Window::exportTracks(const QList<Track> &tracks, const QString &name)
{
    const QList<Track> snapshot = tracks;
    if (snapshot.isEmpty()) {
        showStatus(QStringLiteral("There are no tracks to export."));
        return;
    }
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export playlist"), name + QStringLiteral(".m3u8"),
        QStringLiteral("M3U playlist (*.m3u8 *.m3u)"));
    if (path.isEmpty())
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".m3u8");
    QString error;
    if (!Library::writePlaylist(path, snapshot, error))
        showStatus(error);
    else
        showStatus(QStringLiteral("Playlist exported."));
}

void Window::toggleFullscreen()
{
    const bool enter = !isFullScreen();
    tabs->setVisible(!enter && libraryVisible);
    header->setVisible(!enter);
    details->setVisible(!enter);
    transport->setVisible(!enter);
    status->setVisible(!enter && !status->text().isEmpty());
    auto *rootLayout = qobject_cast<QVBoxLayout *>(playerPanel->layout());
    rootLayout->setContentsMargins(enter ? 0 : 22, enter ? 0 : 16, enter ? 0 : 22, enter ? 0 : 16);
    if (enter)
        showFullScreen();
    else
        showNormal();
    updateViewport();
}

void Window::updateViewport()
{
    const QString shapes[] = {QStringLiteral("16:9"), QStringLiteral("1:1"), QStringLiteral("Fill"), QStringLiteral("Auto")};
    const double ratios[] = {16.0 / 9, 1, 0, 0};
    shape->setText(shapes[aspectMode]);
    zoom->setText(QStringLiteral("%1%").arg(100 + 25 * zoomStep));
    const double ratio = aspectMode == 3 ? (videoMode && videoTrack ? 16.0 / 9 : 1.0) : ratios[aspectMode];
    stage->shape(ratio, isFullScreen());
    stage->cover->setScale(aspectMode == 2, 1.0 + 0.25 * zoomStep);
    player->set("panscan", aspectMode == 2 ? QStringLiteral("1") : QStringLiteral("0"));
    player->set("video-zoom", QString::number(std::log2(1.0 + 0.25 * zoomStep)));
}

void Window::applyVideoMode()
{
    video->setIcon(controlIcon(videoMode ? QStringLiteral("video-mode") : QStringLiteral("audio-mode"), videoMode));
    player->set("vid", videoMode ? QStringLiteral("auto") : QStringLiteral("no"));
    stage->cover->setVisible(!loaded || !videoMode || !videoTrack);
    if (aspectMode == 3)
        stage->shape(videoMode && videoTrack ? 16.0 / 9 : 1.0, isFullScreen());
    sampleAmbient(artwork);
}

void Window::populateArtwork()
{
    const int generation = playbackGeneration;
    const Track copy = *currentTrack();
    if (!copy.url.isLocalFile())
        return;
    background(this, [copy] { return Library::artwork(copy); }, [this, generation](const QImage &image) {
        if (generation == playbackGeneration)
            setArtwork(image);
    });
}

void Window::setArtwork(const QImage &image)
{
    artwork = image;
    stage->cover->setArtwork(image);
    if (stage->cover->isHidden() && !loaded)
        stage->cover->show();
    sampleAmbient(image);
}

void Window::setAmbient(bool enabled)
{
    ambient = enabled;
    splitter->setStyleSheet(enabled ? ambientStyle : QString());
    mediaList->setAmbient(enabled);
    transitionAmbient();
}

void Window::transitionAmbient()
{
    QColor target = ambientSource;
    target.setAlpha(ambient ? 150 : 0);
    if (ambientAnimation.endValue().isValid() && ambientAnimation.endValue().value<QColor>() == target)
        return;

    ambientAnimation.stop();
    QColor start = ambientTint;
    if (start.alpha() == 0) {
        start = target;
        start.setAlpha(0);
    }
    ambientAnimation.setStartValue(start);
    ambientAnimation.setEndValue(target);
    if (start == target) {
        ambientTint = target;
        update();
    } else {
        ambientAnimation.start();
    }
}

void Window::sampleAmbient(const QImage &image)
{
    if (image.isNull()) {
        ambientSource = QColor(QStringLiteral("#75539b"));
        transitionAmbient();
        return;
    }

    const QImage small = image.scaled(12, 12, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
        .convertToFormat(QImage::Format_RGB32);
    double red = 0, green = 0, blue = 0, weight = 0;
    for (int y = 0; y < small.height(); ++y) {
        for (int x = 0; x < small.width(); ++x) {
            const QRgb pixel = small.pixel(x, y);
            const int highest = qMax(qRed(pixel), qMax(qGreen(pixel), qBlue(pixel)));
            const int lowest = qMin(qRed(pixel), qMin(qGreen(pixel), qBlue(pixel)));
            const double strength = (0.15 + (highest - lowest) / 255.0) * (0.35 + highest / 255.0);
            red += strength * qRed(pixel);
            green += strength * qGreen(pixel);
            blue += strength * qBlue(pixel);
            weight += strength;
        }
    }
    if (weight <= 0)
        return;

    const QColor sample(qRound(red / weight), qRound(green / weight), qRound(blue / weight));
    const float hue = sample.hslHueF();
    const float saturation = sample.hslSaturationF();
    const QColor color = QColor::fromHslF(hue < 0 || saturation < 0.08f ? 0.74f : hue,
         qBound(0.26f, saturation, 0.83f), qBound(0.27f, sample.lightnessF(), 0.54f));
    const int difference = qAbs(color.red() - ambientSource.red()) +
        qAbs(color.green() - ambientSource.green()) + qAbs(color.blue() - ambientSource.blue());
    if (difference < 8)
        return;

    ambientSource = color;
    if (ambient)
        transitionAmbient();
}

void Window::showStatus(const QString &message)
{
    status->setText(message);
    status->setVisible(!message.isEmpty() && !isFullScreen());
}

void Window::setupMpris()
{
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return;
    const QString service = QStringLiteral("org.mpris.MediaPlayer2.waguri.instance%1")
        .arg(QCoreApplication::applicationPid());
    if (!bus.registerService(service))
        return;
    dbusService = service;
    auto *root = new MprisRoot(this);
    mpris = new MprisPlayer(this);
    root->raise = [this] { show(); raise(); activateWindow(); };
    root->quit = [this] { close(); };
    mpris->next = [this] { move(1); };
    mpris->previous = [this] { move(-1); };
    mpris->stop = [this] { stop(); };
    mpris->pause = [this] { if (loaded) player->set("pause", QStringLiteral("yes")); };
    mpris->playPause = [this] { togglePlayback(); };
    mpris->play = [this] {
        if (!active)
            playCurrent();
        else if (loaded)
            player->set("pause", QStringLiteral("no"));
    };
    mpris->seekRelative = [this](double offset) { seekTo(position + offset); };
    mpris->seekAbsolute = [this](double seconds) { seekTo(seconds); };
    mpris->rateChanged = [this](double speed) { player->set("speed", QString::number(speed)); };
    mpris->volumeChanged = [this](double volume) {
        const int value = qRound(volume * 100);
        player->set("volume", QString::number(value));
        state.setValue(QStringLiteral("player/volume"), value);
    };
    mpris->open = [this](const QUrl &url) { if (playableUrl(url)) open({url}); };
    bus.registerObject(QStringLiteral("/org/mpris/MediaPlayer2"), this, QDBusConnection::ExportAdaptors);
}

void Window::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const QColor base = isFullScreen() ? QColor(Qt::black) : QColor(QStringLiteral("#151219"));
    painter.fillRect(rect(), base);
    if (!ambientTint.isValid() || ambientTint.alpha() == 0 || isFullScreen())
        return;

    const double coverage = ambientReach / 100.0;
    const auto light = [this](double intensity) {
        QColor shade = ambientTint;
        shade.setAlphaF(ambientTint.alphaF() * intensity);
        return shade;
    };

    QLinearGradient wash(0, 0, width(), height());
    wash.setColorAt(0, light(0.44));
    wash.setColorAt(0.53, light(0.25 + 0.28 * coverage));
    wash.setColorAt(1, light(0.12 + 0.23 * coverage));
    painter.fillRect(rect(), wash);

    const QPoint artworkCenter = stage->mapTo(this, stage->rect().center());
    const double radius = qMax(width(), height()) * (0.48 + 0.72 * coverage);
    QRadialGradient glow(artworkCenter, radius);
    glow.setColorAt(0, light(0.88));
    glow.setColorAt(0.44, light(0.46));
    glow.setColorAt(1, light(0));
    painter.fillRect(rect(), glow);
}

void Window::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const auto orientation = width() < 840 && !isFullScreen() ? Qt::Vertical : Qt::Horizontal;
    if (splitter->orientation() != orientation) {
        splitter->setOrientation(orientation);
        splitter->setSizes({orientation == Qt::Horizontal ? width() / 2 : height() * 3 / 5,
                            orientation == Qt::Horizontal ? width() / 2 : height() * 2 / 5});
    }
}

void Window::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void Window::dropEvent(QDropEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        open(event->mimeData()->urls());
        event->acceptProposedAction();
    }
}
}

int main(int argc, char **argv)
{
    QCoreApplication::setOrganizationName(QStringLiteral("Waguri"));
    QCoreApplication::setApplicationName(QStringLiteral("Waguri MPV"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0"));
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(format);

    QApplication app(argc, argv);
    QGuiApplication::setDesktopFileName(QStringLiteral("io.waguri.Player"));
    app.setWindowIcon(QIcon(QStringLiteral(":/waguri/app-icon.png")));
    std::setlocale(LC_NUMERIC, "C");
    configureFontFallbacks();
    app.setStyle(QStringLiteral("Fusion"));
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(QStringLiteral("#151219")));
    palette.setColor(QPalette::Base, QColor(QStringLiteral("#15161d")));
    palette.setColor(QPalette::Text, QColor(QStringLiteral("#f5f3fa")));
    palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#f5f3fa")));
    palette.setColor(QPalette::Button, QColor(QStringLiteral("#242630")));
    palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#f5f3fa")));
    palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#584383")));
    palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#f5f3fa")));
    app.setPalette(palette);
    QFile theme(QStringLiteral(":/waguri/theme.qss"));
    if (theme.open(QIODevice::ReadOnly))
        app.setStyleSheet(QString::fromUtf8(theme.readAll()));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Waguri desktop audio and video player"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("media"), QStringLiteral("Media paths, URLs or an M3U playlist."),
                                 QStringLiteral("[media…]"));
    parser.process(app);
    Window player;
    player.show();
    QList<QUrl> urls;
    for (const auto &entry : parser.positionalArguments())
        urls.append(QUrl::fromUserInput(entry, QDir::currentPath(), QUrl::AssumeLocalFile));
    if (!urls.isEmpty())
        QTimer::singleShot(0, &player, [&player, urls] { player.open(urls); });
    return app.exec();
}
