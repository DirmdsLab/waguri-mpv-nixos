#include "lyrics.hpp"

#include <algorithm>

#include <QColorDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFontComboBox>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkReply>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTextStream>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace {
constexpr int defaultTextSize = 22;
constexpr int textSizeStep = 2;
const QString textSizeKey = QStringLiteral("lyrics/textSize");
const QString fontFamilyKey = QStringLiteral("lyrics/fontFamily");
const QString textColorKey = QStringLiteral("lyrics/textColor");
const QString activeColorKey = QStringLiteral("lyrics/activeColor");
const QColor defaultTextColor("#f3edf7");
const QColor defaultActiveColor("#dbcbff");

QColor savedColor(const QSettings &settings, const QString &key, const QColor &fallback)
{
    const QColor color(settings.value(key).toString());
    return color.isValid() ? color : fallback;
}
constexpr qsizetype maximumLyricsBytes = 1024 * 1024;
const QRegularExpression timestamp(R"(\[(\d{1,5}):(\d{1,2})(?:[.:](\d{1,3}))?\])");
const QRegularExpression offsetTag(R"(^\[offset:([+-]?\d{1,8})\]$)", QRegularExpression::CaseInsensitiveOption);
const QRegularExpression metadata(R"(^\[[a-zA-Z]+:.*\]$)");
const QRegularExpression wordTimestamp(R"(<\d{1,5}:\d{1,2}(?:[.:]\d{1,3})?>)");
}

QList<Lyrics::Line> Lyrics::parse(const QString &source)
{
    QList<Line> timed;
    QList<Line> plain;
    qint64 offset = 0;
    for (QString part : source.split(u'\n')) {
        part = part.trimmed();
        if (part.startsWith(QChar::ByteOrderMark))
            part.remove(0, 1);
        if (part.isEmpty())
            continue;
        auto offsetMatch = offsetTag.match(part);
        if (offsetMatch.hasMatch()) {
            offset = offsetMatch.captured(1).toLongLong();
            continue;
        }
        if (metadata.match(part).hasMatch())
            continue;

        const QString text = QString(part).remove(timestamp).remove(wordTimestamp).trimmed();
        auto matches = timestamp.globalMatch(part);
        if (!matches.hasNext()) {
            plain.append({-1, text});
            continue;
        }
        while (matches.hasNext()) {
            const auto match = matches.next();
            const int seconds = match.captured(2).toInt();
            if (seconds >= 60)
                continue;
            const int fraction = match.captured(3).leftJustified(3, u'0').toInt();
            const qint64 time = (match.captured(1).toLongLong() * 60 + seconds) * 1000 + fraction;
            timed.append({time, text});
        }
    }
    if (timed.isEmpty())
        return plain;
    for (Line &line : timed)
        line.milliseconds = qMax(0LL, line.milliseconds - offset);
    std::stable_sort(timed.begin(), timed.end(),
                     [](const Line &a, const Line &b) { return a.milliseconds < b.milliseconds; });
    return timed;
}

LyricsPanel::LyricsPanel(QWidget *parent) : QWidget(parent)
{
    QSettings settings;
    fontFamily = settings.value(fontFamilyKey).toString();
    textColor = savedColor(settings, textColorKey, defaultTextColor);
    activeColor = savedColor(settings, activeColorKey, defaultActiveColor);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 15, 18, 13);
    layout->setSpacing(10);
    auto *bar = new QHBoxLayout;
    query = new QLineEdit;
    query->setPlaceholderText(QStringLiteral("Search lyrics"));
    query->setClearButtonEnabled(true);
    auto *find = new QPushButton(QStringLiteral("Search"));
    saveButton = new QPushButton(QStringLiteral("Save LRC"));
    saveButton->setEnabled(false);
    bar->addWidget(query, 1);
    bar->addWidget(find);
    bar->addWidget(saveButton);

    notice = new QLabel(QStringLiteral("Select a track to read its lyrics."));
    notice->setObjectName(QStringLiteral("secondary"));
    notice->setWordWrap(true);
    notice->setTextFormat(Qt::PlainText);
    items = new QListWidget;
    items->setObjectName(QStringLiteral("lyricLines"));
    items->setWordWrap(true);
    items->setTextElideMode(Qt::ElideNone);
    items->setResizeMode(QListView::Adjust);
    items->setUniformItemSizes(false);
    items->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    items->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    items->viewport()->installEventFilter(this);
    scrollAnimation = new QPropertyAnimation(items->verticalScrollBar(), "value", this);
    scrollAnimation->setDuration(210);
    scrollAnimation->setEasingCurve(QEasingCurve::OutCubic);

    auto *timing = new QHBoxLayout;
    timing->addWidget(new QLabel(QStringLiteral("Lyrics delay")));
    delay = new QDoubleSpinBox;
    delay->setRange(-120, 120);
    delay->setDecimals(2);
    delay->setSingleStep(0.1);
    delay->setSuffix(QStringLiteral(" s"));
    delay->setEnabled(false);
    timing->addWidget(delay);
    timing->addStretch();
    auto *textSizeLabel = new QLabel(QStringLiteral("Text size"));
    textSize = new QSpinBox;
    textSize->setRange(14, 42);
    textSize->setSingleStep(textSizeStep);
    textSize->setSuffix(QStringLiteral(" px"));
    textSize->setKeyboardTracking(false);
    textSize->setToolTip(QStringLiteral("Use Ctrl + mouse wheel over the lyrics to zoom"));
    textSize->setValue(settings.value(textSizeKey, defaultTextSize).toInt());
    textSizeLabel->setBuddy(textSize);
    timing->addWidget(textSizeLabel);
    timing->addWidget(textSize);

    auto *appearance = new QPushButton(QStringLiteral("Appearance"));
    auto *styleMenu = new QMenu(appearance);
    styleMenu->addAction(QStringLiteral("Font family…"), this, [this] { chooseFont(); });
    styleMenu->addAction(QStringLiteral("Text color…"), this, [this] { chooseColor(false); });
    styleMenu->addAction(QStringLiteral("Active line color…"), this, [this] { chooseColor(true); });
    styleMenu->addSeparator();
    styleMenu->addAction(QStringLiteral("Reset appearance"), this, [this] { resetAppearance(); });
    appearance->setMenu(styleMenu);
    timing->addWidget(appearance);
    applyAppearance();
    connect(textSize, &QSpinBox::valueChanged, this, [this](int pixels) {
        applyAppearance();
        QSettings().setValue(textSizeKey, pixels);
    });
    layout->addLayout(bar);
    layout->addWidget(notice);
    layout->addWidget(items, 1);
    layout->addLayout(timing);

    connect(query, &QLineEdit::returnPressed, this, [this] { search(false); });
    connect(find, &QPushButton::clicked, this, [this] { search(false); });
    connect(saveButton, &QPushButton::clicked, this, [this] { save(); });
    connect(delay, &QDoubleSpinBox::valueChanged, this, [this] { sync(position); });
    connect(items, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        const auto &line = lines.at(items->row(item));
        if (seek && line.milliseconds >= 0)
            seek(qMax(0.0, line.milliseconds / 1000.0 + delay->value()));
    });
}

bool LyricsPanel::eventFilter(QObject *source, QEvent *event)
{
    if (items && source == items->viewport() && event->type() == QEvent::Wheel) {
        auto *wheel = static_cast<QWheelEvent *>(event);
        const int delta = wheel->angleDelta().y();
        if ((wheel->modifiers() & Qt::ControlModifier) && delta != 0) {
            textSize->setValue(textSize->value() + (delta > 0 ? textSizeStep : -textSizeStep));
            return true;
        }
    }
    return QWidget::eventFilter(source, event);
}

void LyricsPanel::applyAppearance()
{
    scrollAnimation->stop();
    QString family = fontFamily;
    family.remove(u'\n').remove(u'\r');
    family.replace(u'\\', QStringLiteral("\\\\")).replace(u'"', QStringLiteral("\\\""));
    const QString familyRule = family.isEmpty() ? QString() : QStringLiteral("font-family: \"%1\";").arg(family);
    items->setStyleSheet(QStringLiteral(
        "QListWidget#lyricLines { font-size: %1px; %2 }"
        "QListWidget#lyricLines::item { color: %3; }"
        "QListWidget#lyricLines::item:selected { color: %4; font-weight: 700; }")
        .arg(textSize->value()).arg(familyRule, textColor.name(), activeColor.name()));
    items->doItemsLayout();
    if (auto *activeLine = items->item(highlighted))
        items->scrollToItem(activeLine, QAbstractItemView::PositionAtCenter);
}

void LyricsPanel::chooseFont()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Lyrics font"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *families = new QFontComboBox(&dialog);
    families->setAccessibleName(QStringLiteral("Font family"));
    const QFont current = fontFamily.isEmpty() ? items->font() : QFont(fontFamily);
    families->setCurrentFont(current);
    layout->addWidget(families);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;
    fontFamily = families->currentFont().family();
    QSettings().setValue(fontFamilyKey, fontFamily);
    applyAppearance();
}

void LyricsPanel::chooseColor(bool activeLine)
{
    QColor &color = activeLine ? activeColor : textColor;
    const QString caption = activeLine ? QStringLiteral("Active line color") : QStringLiteral("Lyrics text color");
    const QColor selected = QColorDialog::getColor(color, this, caption);
    if (!selected.isValid())
        return;
    color = selected;
    QSettings().setValue(activeLine ? activeColorKey : textColorKey, color.name());
    applyAppearance();
}

void LyricsPanel::resetAppearance()
{
    fontFamily.clear();
    textColor = defaultTextColor;
    activeColor = defaultActiveColor;
    const QSignalBlocker blocker(textSize);
    textSize->setValue(defaultTextSize);
    QSettings settings;
    for (const auto &key : {fontFamilyKey, textColorKey, activeColorKey, textSizeKey})
        settings.remove(key);
    applyAppearance();
}

QString LyricsPanel::sidecar() const
{
    if (!track.isLocalFile())
        return {};
    const QFileInfo info(track.toLocalFile());
    return info.absoluteDir().filePath(info.completeBaseName() + QStringLiteral(".lrc"));
}

void LyricsPanel::setTrack(const QUrl &url, const QString &title, const QString &artist, bool autoSearch)
{
    auto previous = request;
    request.clear();
    if (previous)
        previous->abort();
    track = url;
    position = 0;
    query->setText(artist.isEmpty() ? title : artist + QStringLiteral(" - ") + title);
    display({}, url.isEmpty() ? QStringLiteral("Select a track to read its lyrics.")
                             : QStringLiteral("No local LRC. Search LRCLIB to find lyrics."));

    QFile file(sidecar());
    if (!file.fileName().isEmpty() && file.open(QIODevice::ReadOnly) && file.size() <= maximumLyricsBytes) {
        display(QString::fromUtf8(file.readAll()), QStringLiteral("Local LRC"));
        if (!lines.isEmpty())
            return;
    }
    if (autoSearch && !query->text().isEmpty())
        search(true);
}

void LyricsPanel::display(const QString &source, const QString &status)
{
    lines = Lyrics::parse(source);
    scrollAnimation->stop();
    items->clear();
    highlighted = -1;
    const QSignalBlocker blocker(delay);
    delay->setValue(0);
    delay->setEnabled(!lines.isEmpty() && lines.first().milliseconds >= 0);
    saveButton->setEnabled(!lines.isEmpty() && !sidecar().isEmpty());
    for (const auto &line : lines)
        items->addItem(line.text);
    notice->setText(status);
    sync(position);
}

void LyricsPanel::sync(double seconds)
{
    position = seconds;
    if (lines.isEmpty() || lines.first().milliseconds < 0)
        return;
    const qint64 time = qRound64((seconds - delay->value()) * 1000.0);
    auto current = std::upper_bound(lines.cbegin(), lines.cend(), time,
        [](qint64 ms, const Lyrics::Line &line) { return ms < line.milliseconds; });
    int row = int(current - lines.cbegin()) - 1;
    if (row == highlighted)
        return;
    highlighted = row;
    auto *scroll = items->verticalScrollBar();
    scrollAnimation->stop();
    const int original = scroll->value();
    items->setCurrentRow(row);
    if (row < 0)
        return;

    items->scrollToItem(items->item(row), QAbstractItemView::PositionAtCenter);
    const int target = scroll->value();
    if (isVisible() && items->isVisible() && target != original) {
        scroll->setValue(original);
        scrollAnimation->setStartValue(original);
        scrollAnimation->setEndValue(target);
        scrollAnimation->start();
    }
}

void LyricsPanel::search(bool automatic)
{
    const QString searchTerm = query->text().trimmed();
    if (searchTerm.isEmpty())
        return;
    auto previous = request;
    request.clear();
    if (previous)
        previous->abort();
    QUrl url(QStringLiteral("https://lrclib.net/api/search"));
    QUrlQuery parameters;
    parameters.addQueryItem(QStringLiteral("q"), searchTerm);
    url.setQuery(parameters);
    QNetworkRequest options(url);
    options.setRawHeader("User-Agent", "Waguri-MPV-Linux/1.0 (https://github.com/DirmdsLab/waguri-mpv)");
    options.setTransferTimeout(15000);
    options.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    QNetworkReply *reply = network.get(options);
    request = reply;
    notice->setText(QStringLiteral("Searching LRCLIB…"));
    connect(reply, &QNetworkReply::readyRead, this, [reply] {
        if (reply->bytesAvailable() > maximumLyricsBytes)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, automatic] {
        reply->deleteLater();
        if (reply != request)
            return;
        request.clear();
        if (reply->error() != QNetworkReply::NoError) {
            notice->setText(reply->errorString());
            return;
        }
        const QJsonArray results = QJsonDocument::fromJson(reply->readAll()).array();
        QStringList labels;
        QStringList sources;
        for (const QJsonValue &value : results) {
            const QJsonObject item = value.toObject();
            const QString synced = item.value("syncedLyrics").toString();
            const QString source = synced.isEmpty() ? item.value("plainLyrics").toString() : synced;
            if (source.isEmpty())
                continue;
            labels.append(QStringLiteral("%1. %2 — %3 (%4)")
                .arg(labels.size() + 1).arg(item.value("artistName").toString(),
                    item.value("trackName").toString(), synced.isEmpty() ? QStringLiteral("plain") : QStringLiteral("synced")));
            sources.append(source);
        }
        if (sources.isEmpty()) {
            notice->setText(QStringLiteral("No lyrics found."));
            return;
        }
        int index = 0;
        if (!automatic && sources.size() > 1) {
            bool accepted = false;
            const QUrl currentTrack = track;
            const QString choice = QInputDialog::getItem(this, QStringLiteral("Lyrics results"),
                QStringLiteral("Choose lyrics"), labels, 0, false, &accepted);
            if (!accepted || currentTrack != track) {
                if (currentTrack == track)
                    notice->setText(QStringLiteral("Lyrics selection canceled."));
                return;
            }
            index = labels.indexOf(choice);
        }
        if (index >= 0)
            display(sources.at(index), QStringLiteral("LRCLIB"));
    });
}

void LyricsPanel::save()
{
    const QString path = sidecar();
    if (path.isEmpty() || lines.isEmpty())
        return;
    if (QFileInfo::exists(path) && QMessageBox::question(this, QStringLiteral("Replace lyrics"),
        QStringLiteral("Replace the existing LRC beside this track?")) != QMessageBox::Yes)
        return;
    if (path != sidecar() || lines.isEmpty())
        return;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        notice->setText(file.errorString());
        return;
    }
    QTextStream output(&file);
    for (const auto &line : lines) {
        if (line.milliseconds >= 0) {
            const qint64 time = qMax(0LL, line.milliseconds + qRound64(delay->value() * 1000));
            output << QStringLiteral("[%1:%2.%3]")
                          .arg(time / 60000, 2, 10, QChar(u'0'))
                          .arg((time / 1000) % 60, 2, 10, QChar(u'0'))
                          .arg(time % 1000, 3, 10, QChar(u'0'));
        }
        output << line.text << '\n';
    }
    output.flush();
    if (output.status() == QTextStream::Ok && file.commit())
        notice->setText(QStringLiteral("Saved beside the media file."));
    else
        notice->setText(file.errorString());
}
