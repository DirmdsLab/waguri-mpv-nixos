#pragma once

#include "library.hpp"

#include <functional>

#include <QNetworkAccessManager>
#include <QWidget>

class QComboBox;
class QJsonObject;
class QLabel;
class QLineEdit;
class QNetworkReply;
class QPushButton;

class YouTubePanel final : public QWidget {
public:
    using Prepared = std::function<void(const QUrl &, const QString &, double, const QImage &, const QString &)>;
    explicit YouTubePanel(QWidget *parent = nullptr);
    QNetworkReply *prepare(const Library::Track &track, Prepared done);
    std::function<void(const QList<Library::Track> &)> playTracks;

private:
    using JsonResult = std::function<void(const QJsonObject &, const QString &)>;
    QUrl link();
    QNetworkReply *post(const QString &path, const QJsonObject &body, JsonResult done);
    void fetchFormats();
    void play();
    void openFormatPicker();
    void download();
    void setStatus(const QString &text);

    QNetworkAccessManager network;
    QLineEdit *input = nullptr;
    QLineEdit *downloadFormat = nullptr;
    QComboBox *format = nullptr;
    QLabel *status = nullptr;
    QPushButton *saveButton = nullptr;
    QPushButton *archiveButton = nullptr;
    QUrl resultArchive;
};
