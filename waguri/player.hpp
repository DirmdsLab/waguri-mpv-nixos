#pragma once

#include <functional>

#include <QImage>
#include <QWidget>
#include <QStringList>
#include <QVariant>

#include <mpv/client.h>
#include <mpv/render.h>

class Player final : public QWidget {
public:
    explicit Player(QWidget *parent = nullptr);
    ~Player() override;

    bool isReady() const { return handle != nullptr; }
    QString error() const { return lastError; }
    QVariant property(const char *name) const;
    bool set(const char *name, const QString &value);
    bool command(const QStringList &arguments);
    bool selectAudioDevice(const QString &device);
    bool load(const QString &path);
    void stop();
    QImage captureFrame();

    std::function<void()> fileLoaded;
    std::function<void(bool)> ended;
    std::function<void()> changed;
    std::function<void(const QString &)> audioWarning;
    std::function<void(const QString &)> failed;
    std::function<void()> doubleClicked;

protected:
    void paintEvent(QPaintEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;

private:
    class OpenGLSurface;
    void initializeOpenGL();
    void paintOpenGL();
    void useSoftwareRenderer();
    void readEvents();
    void releaseRenderer();
    static void wakeup(void *context);
    static void updateFrame(void *context);
    static void *getProcAddress(void *, const char *name);

    mpv_handle *handle = nullptr;
    mpv_render_context *renderer = nullptr;
    OpenGLSurface *glSurface = nullptr;
    QImage softwareFrame;
    QMetaObject::Connection contextDestroyed;
    int64_t playlistEntry = 0;
    bool waitingForRedirect = false;
    bool waitingForFile = false;
    QString lastError;
};
