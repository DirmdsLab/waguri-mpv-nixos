#include "player.hpp"

#include <QByteArray>
#include <QDebug>
#include <QMetaObject>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QPainter>
#include <QSurfaceFormat>
#include <QVBoxLayout>
#include <QVector>

#include <mpv/render_gl.h>

namespace {
bool canUseOpenGL()
{
    if (qEnvironmentVariableIntValue("WAGURI_SOFTWARE_RENDER"))
        return false;

    QOpenGLContext context;
    context.setFormat(QSurfaceFormat::defaultFormat());
    if (!context.create())
        return false;

    QOffscreenSurface surface;
    surface.setFormat(context.format());
    surface.create();
    if (!surface.isValid() || !context.makeCurrent(&surface))
        return false;

    const bool supportsRendering = context.format().majorVersion() >= 3;
    context.doneCurrent();
    return supportsRendering;
}

QVariant toVariant(const mpv_node &node)
{
    switch (node.format) {
    case MPV_FORMAT_STRING:
        return QString::fromUtf8(node.u.string);
    case MPV_FORMAT_FLAG:
        return bool(node.u.flag);
    case MPV_FORMAT_INT64:
        return QVariant::fromValue<qlonglong>(node.u.int64);
    case MPV_FORMAT_DOUBLE:
        return node.u.double_;
    case MPV_FORMAT_NODE_ARRAY: {
        QVariantList result;
        for (int i = 0; i < node.u.list->num; ++i)
            result.append(toVariant(node.u.list->values[i]));
        return result;
    }
    case MPV_FORMAT_NODE_MAP: {
        QVariantMap result;
        for (int i = 0; i < node.u.list->num; ++i)
            result.insert(QString::fromUtf8(node.u.list->keys[i]),
                          toVariant(node.u.list->values[i]));
        return result;
    }
    default:
        return {};
    }
}
}

class Player::OpenGLSurface final : public QOpenGLWidget {
public:
    explicit OpenGLSurface(Player *player) : QOpenGLWidget(player), owner(player) {}

protected:
    void initializeGL() override { owner->initializeOpenGL(); }
    void paintGL() override { owner->paintOpenGL(); }
    void mouseDoubleClickEvent(QMouseEvent *event) override { owner->mouseDoubleClickEvent(event); }

private:
    Player *owner;
};

Player::Player(QWidget *parent) : QWidget(parent)
{
    handle = mpv_create();
    if (!handle) {
        lastError = QStringLiteral("Could not allocate the mpv core.");
        return;
    }

    const char *options[][2] = {
        {"config", "no"},
        {"load-scripts", "no"},
        {"input-default-bindings", "no"},
        {"input-vo-keyboard", "no"},
        {"terminal", "no"},
        {"osc", "no"},
        {"idle", "yes"},
        {"vo", "libmpv"},
        {"hwdec", "auto-safe"},
        {"ao", "pulse,pipewire,alsa"},
        {"audio-client-name", "Waguri MPV"},
        {"audio-fallback-to-null", "no"},
        {"volume-max", "100"},
        {"audio-display", "no"},
        {"ytdl", "no"},
    };
    for (const auto &option : options) {
        int code = mpv_set_option_string(handle, option[0], option[1]);
        if (code < 0) {
            lastError = QStringLiteral("%1: %2").arg(QString::fromLatin1(option[0]),
                                                      QString::fromUtf8(mpv_error_string(code)));
            mpv_destroy(handle);
            handle = nullptr;
            return;
        }
    }

    int code = mpv_initialize(handle);
    if (code < 0) {
        lastError = QString::fromUtf8(mpv_error_string(code));
        mpv_destroy(handle);
        handle = nullptr;
        return;
    }

    const char *observed[] = {"pause", "media-title", "duration", "track-list",
                               "video-params", "volume", "speed", "eof-reached", "seekable"};
    for (const char *name : observed)
        mpv_observe_property(handle, 0, name, MPV_FORMAT_NONE);
    mpv_request_log_messages(handle, "warn");
    mpv_set_wakeup_callback(handle, wakeup, this);

    if (canUseOpenGL()) {
        glSurface = new OpenGLSurface(this);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(glSurface);
    } else {
        useSoftwareRenderer();
    }
}

Player::~Player()
{
    if (!handle)
        return;
    mpv_set_wakeup_callback(handle, nullptr, nullptr);
    QObject::disconnect(contextDestroyed);
    releaseRenderer();
    mpv_terminate_destroy(handle);
}

QVariant Player::property(const char *name) const
{
    mpv_node value{};
    if (!handle || mpv_get_property(handle, name, MPV_FORMAT_NODE, &value) < 0)
        return {};
    QVariant result = toVariant(value);
    mpv_free_node_contents(&value);
    return result;
}

bool Player::set(const char *name, const QString &value)
{
    return handle && mpv_set_property_string(handle, name, value.toUtf8().constData()) >= 0;
}

bool Player::selectAudioDevice(const QString &device)
{
    if (device == QStringLiteral("auto"))
        return set("audio-device", device) && set("ao", QStringLiteral("pulse,pipewire,alsa"));
    return set("ao", QString()) && set("audio-device", device);
}

bool Player::command(const QStringList &arguments)
{
    if (!handle || arguments.isEmpty())
        return false;
    QVector<QByteArray> utf8;
    for (const QString &argument : arguments)
        utf8.append(argument.toUtf8());
    QVector<const char *> pointers;
    for (const QByteArray &argument : utf8)
        pointers.append(argument.constData());
    pointers.append(nullptr);
    return mpv_command_async(handle, 0, pointers.data()) >= 0;
}

bool Player::load(const QString &path)
{
    if (!handle || path.isEmpty())
        return false;
    if (glSurface && !renderer)
        useSoftwareRenderer();
    waitingForRedirect = false;
    waitingForFile = false;
    playlistEntry = 0;
    const QByteArray filename = path.toUtf8();
    const char *arguments[] = {"loadfile", filename.constData(), "replace", nullptr};
    mpv_node result{};
    const int error = mpv_command_ret(handle, arguments, &result);
    if (error < 0) {
        lastError = QString::fromUtf8(mpv_error_string(error));
        return false;
    }
    playlistEntry = toVariant(result).toMap().value(QStringLiteral("playlist_entry_id")).toLongLong();
    mpv_free_node_contents(&result);
    return playlistEntry != 0;
}

void Player::stop()
{
    waitingForRedirect = waitingForFile = false;
    playlistEntry = 0;
    const char *arguments[] = {"stop", nullptr};
    if (handle)
        mpv_command(handle, arguments);
}

void Player::readEvents()
{
    if (!handle)
        return;
    bool propertiesChanged = false;
    for (;;) {
        const mpv_event *event = mpv_wait_event(handle, 0);
        if (event->event_id == MPV_EVENT_NONE)
            break;
        switch (event->event_id) {
        case MPV_EVENT_START_FILE: {
            auto *start = static_cast<mpv_event_start_file *>(event->data);
            if (waitingForRedirect) {
                playlistEntry = start->playlist_entry_id;
                waitingForRedirect = false;
            }
            waitingForFile = playlistEntry && start->playlist_entry_id == playlistEntry;
            break;
        }
        case MPV_EVENT_FILE_LOADED:
            if (waitingForFile && playlistEntry) {
                waitingForFile = false;
                if (fileLoaded)
                    fileLoaded();
            }
            break;
        case MPV_EVENT_END_FILE: {
            auto *end = static_cast<mpv_event_end_file *>(event->data);
            if (!playlistEntry || end->playlist_entry_id != playlistEntry)
                break;
            if (end->reason == MPV_END_FILE_REASON_REDIRECT) {
                waitingForRedirect = true;
                break;
            }
            playlistEntry = 0;
            waitingForFile = false;
            if (end->reason == MPV_END_FILE_REASON_ERROR) {
                if (failed)
                    failed(QString::fromUtf8(mpv_error_string(end->error)));
            } else if (ended) {
                ended(end->reason == MPV_END_FILE_REASON_EOF);
            }
            break;
        }
        case MPV_EVENT_LOG_MESSAGE: {
            const auto *message = static_cast<mpv_event_log_message *>(event->data);
            const QString prefix = QString::fromUtf8(message->prefix);
            const QString text = QString::fromUtf8(message->text).trimmed();
            if (prefix.startsWith(QStringLiteral("ao")) ||
                text.contains(QStringLiteral("Could not open/initialize audio device"))) {
                qWarning().noquote() << QStringLiteral("mpv [%1]: %2").arg(prefix, text);
            }
            if (audioWarning && text.contains(QStringLiteral("Could not open/initialize audio device")))
                audioWarning(QStringLiteral("Audio output could not be opened. Choose another device in Player options."));
            break;
        }
        case MPV_EVENT_PROPERTY_CHANGE:
            propertiesChanged = true;
            break;
        default:
            break;
        }
    }
    if (propertiesChanged && changed)
        changed();
}

void Player::wakeup(void *context)
{
    auto *player = static_cast<Player *>(context);
    QMetaObject::invokeMethod(player, [player] { player->readEvents(); }, Qt::QueuedConnection);
}

void Player::updateFrame(void *context)
{
    auto *player = static_cast<Player *>(context);
    QMetaObject::invokeMethod(player, [player] {
        if (player->glSurface)
            player->glSurface->update();
        else
            player->update();
    }, Qt::QueuedConnection);
}

void *Player::getProcAddress(void *, const char *name)
{
    return reinterpret_cast<void *>(QOpenGLContext::currentContext()->getProcAddress(name));
}

void Player::initializeOpenGL()
{
    if (!handle)
        return;
    mpv_opengl_init_params gl{getProcAddress, nullptr};
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    const int code = mpv_render_context_create(&renderer, handle, params);
    if (code < 0) {
        QMetaObject::invokeMethod(this, [this] { useSoftwareRenderer(); }, Qt::QueuedConnection);
        return;
    }
    contextDestroyed = connect(glSurface->context(), &QOpenGLContext::aboutToBeDestroyed,
                               this, [this] { releaseRenderer(); });
    mpv_render_context_set_update_callback(renderer, updateFrame, this);
}

void Player::paintOpenGL()
{
    if (!renderer) {
        auto *functions = glSurface->context()->functions();
        functions->glClearColor(0, 0, 0, 1);
        functions->glClear(GL_COLOR_BUFFER_BIT);
        return;
    }
    const double scale = glSurface->devicePixelRatioF();
    mpv_opengl_fbo target{int(glSurface->defaultFramebufferObject()), qRound(glSurface->width() * scale),
                          qRound(glSurface->height() * scale), 0};
    int flip = 1;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &target},
        {MPV_RENDER_PARAM_FLIP_Y, &flip},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    mpv_render_context_update(renderer);
    mpv_render_context_render(renderer, params);
}

void Player::useSoftwareRenderer()
{
    if (!glSurface && renderer)
        return;

    if (glSurface) {
        QObject::disconnect(contextDestroyed);
        releaseRenderer();
        glSurface->hide();
        glSurface->deleteLater();
        glSurface = nullptr;
    }

    set("hwdec", QStringLiteral("no"));
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_SW)},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    const int code = mpv_render_context_create(&renderer, handle, params);
    if (code < 0) {
        lastError = QStringLiteral("Software renderer: %1").arg(QString::fromUtf8(mpv_error_string(code)));
        if (failed)
            failed(lastError);
        return;
    }
    mpv_render_context_set_update_callback(renderer, updateFrame, this);
    update();
}

void Player::paintEvent(QPaintEvent *event)
{
    if (glSurface) {
        QWidget::paintEvent(event);
        return;
    }

    QPainter painter(this);
    QSize frameSize = size();
    if (frameSize.width() > 1280 || frameSize.height() > 720)
        frameSize.scale(1280, 720, Qt::KeepAspectRatio);
    if (!renderer || frameSize.isEmpty()) {
        painter.fillRect(rect(), Qt::black);
        return;
    }

    const bool resized = softwareFrame.size() != frameSize;
    if (resized) {
        softwareFrame = QImage(frameSize, QImage::Format_RGB32);
        softwareFrame.fill(Qt::black);
    }

    const auto updates = mpv_render_context_update(renderer);
    if (!softwareFrame.isNull() && (resized || (updates & MPV_RENDER_UPDATE_FRAME))) {
        int dimensions[] = {frameSize.width(), frameSize.height()};
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
        char format[] = "bgr0";
#else
        char format[] = "0rgb";
#endif
        auto stride = size_t(softwareFrame.bytesPerLine());
        auto *pixels = softwareFrame.bits();
        int block = 0;
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_SW_SIZE, dimensions},
            {MPV_RENDER_PARAM_SW_FORMAT, format},
            {MPV_RENDER_PARAM_SW_STRIDE, &stride},
            {MPV_RENDER_PARAM_SW_POINTER, pixels},
            {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        if (mpv_render_context_render(renderer, params) >= 0) {
            for (int y = 0; y < dimensions[1]; ++y) {
                auto *row = reinterpret_cast<QRgb *>(pixels + y * stride);
                for (int x = 0; x < dimensions[0]; ++x)
                    row[x] |= 0xff000000u;
            }
        }
    }

    if (softwareFrame.isNull()) {
        painter.fillRect(rect(), Qt::black);
        return;
    }
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(rect(), softwareFrame);
}

QImage Player::captureFrame()
{
    if (!glSurface)
        return softwareFrame;
    return glSurface->isValid() ? glSurface->grabFramebuffer() : QImage();
}

void Player::releaseRenderer()
{
    if (!renderer)
        return;
    if (glSurface)
        glSurface->makeCurrent();
    mpv_render_context_set_update_callback(renderer, nullptr, nullptr);
    mpv_render_context_free(renderer);
    renderer = nullptr;
    if (glSurface)
        glSurface->doneCurrent();
}

void Player::mouseDoubleClickEvent(QMouseEvent *)
{
    if (doubleClicked)
        doubleClicked();
}
