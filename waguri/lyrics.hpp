#pragma once

#include <functional>

#include <QColor>
#include <QList>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QUrl>
#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QDoubleSpinBox;
class QPropertyAnimation;
class QSpinBox;

namespace Lyrics {
struct Line {
    qint64 milliseconds = -1;
    QString text;
};

QList<Line> parse(const QString &source);
}

class LyricsPanel final : public QWidget {
public:
    explicit LyricsPanel(QWidget *parent = nullptr);
    void setTrack(const QUrl &url, const QString &title, const QString &artist, bool autoSearch);
    void sync(double seconds);
    std::function<void(double)> seek;

protected:
    bool eventFilter(QObject *source, QEvent *event) override;

private:
    void applyAppearance();
    void chooseFont();
    void chooseColor(bool activeLine);
    void resetAppearance();
    void search(bool automatic);
    void display(const QString &source, const QString &status);
    void save();
    QString sidecar() const;

    QNetworkAccessManager network;
    QPointer<QNetworkReply> request;
    QUrl track;
    QList<Lyrics::Line> lines;
    QLineEdit *query = nullptr;
    QListWidget *items = nullptr;
    QLabel *notice = nullptr;
    QPushButton *saveButton = nullptr;
    QDoubleSpinBox *delay = nullptr;
    QSpinBox *textSize = nullptr;
    QPropertyAnimation *scrollAnimation = nullptr;
    QString fontFamily;
    QColor textColor;
    QColor activeColor;
    int highlighted = -1;
    double position = 0;
};
