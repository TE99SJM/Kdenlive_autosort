#include <QApplication>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QCollator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QMimeData>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QSettings>
#include <QStatusBar>
#include <QUrl>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWidget>
#include <algorithm>
#include <functional>

namespace {
const QSet<QString> kVideoExtensions = {
    QStringLiteral("mp4"), QStringLiteral("mov"), QStringLiteral("m4v"),
    QStringLiteral("mkv"), QStringLiteral("avi"), QStringLiteral("mts"),
    QStringLiteral("m2ts"), QStringLiteral("mpg"), QStringLiteral("mpeg"),
    QStringLiteral("mxf"), QStringLiteral("webm"), QStringLiteral("360"),
    QStringLiteral("osv")
};

struct SortKey {
    int cameraGroup = 3;
    qint64 take = 0;
    qint64 chapter = 0;
    QString name;
};

QString normalizedPath(const QString &path)
{
    QString result = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#ifdef Q_OS_WIN
    result = result.toCaseFolded();
#endif
    return result;
}

SortKey sortKeyFor(const QString &path)
{
    const QString baseName = QFileInfo(path).completeBaseName();
    const QString upper = baseName.toUpper();
    SortKey key;
    key.name = baseName;

    static const QRegularExpression goPro(QStringLiteral("^(GOPR|GP|GX|GH|GL)(\\d+)$"));
    const auto goProMatch = goPro.match(upper);
    if (goProMatch.hasMatch()) {
        key.cameraGroup = 0;
        const QString prefix = goProMatch.captured(1);
        const QString digits = goProMatch.captured(2);
        if (prefix != QStringLiteral("GOPR") && digits.size() >= 6) {
            key.chapter = digits.left(2).toLongLong();
            key.take = digits.mid(2).toLongLong();
        } else {
            key.take = digits.toLongLong();
        }
        return key;
    }

    if (upper.startsWith(QStringLiteral("DJI"))) {
        key.cameraGroup = 1;
        return key;
    }

    static const QRegularExpression sony(QStringLiteral("^(C|S|M4V|DSC)_?\\d+$"));
    if (sony.match(upper).hasMatch()) {
        key.cameraGroup = 2;
    }
    return key;
}

bool naturalLess(const QString &left, const QString &right)
{
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    collator.setNumericMode(true);
    return collator.compare(left, right) < 0;
}

bool lessByCameraName(const QString &left, const QString &right)
{
    const SortKey a = sortKeyFor(left);
    const SortKey b = sortKeyFor(right);
    if (a.cameraGroup != b.cameraGroup) {
        return a.cameraGroup < b.cameraGroup;
    }
    if (a.cameraGroup == 0) {
        if (a.take != b.take) {
            return a.take < b.take;
        }
        if (a.chapter != b.chapter) {
            return a.chapter < b.chapter;
        }
    }
    const int byName = QCollator().compare(a.name, b.name);
    if (byName != 0) {
        return naturalLess(a.name, b.name);
    }
    return naturalLess(left, right);
}

QString cameraLabel(const QString &path)
{
    switch (sortKeyFor(path).cameraGroup) {
    case 0: return QStringLiteral("GoPro");
    case 1: return QStringLiteral("DJI");
    case 2: return QStringLiteral("Sony");
    default: return QStringLiteral("Other");
    }
}

QStringList ffprobeCandidates()
{
    QStringList paths;
    auto add = [&paths](const QString &path) {
        if (path.isEmpty() || !QFileInfo(path).isFile()) {
            return;
        }
        const QString canonical = QFileInfo(path).canonicalFilePath();
        const QString resolved = canonical.isEmpty() ? QFileInfo(path).absoluteFilePath() : canonical;
        if (!paths.contains(resolved, Qt::CaseInsensitive)) {
            paths << resolved;
        }
    };

    QSettings settings;
    add(settings.value(QStringLiteral("ffprobePath")).toString());
#ifdef Q_OS_WIN
    const QString executableName = QStringLiteral("ffprobe.exe");
#else
    const QString executableName = QStringLiteral("ffprobe");
#endif
    add(QDir(QCoreApplication::applicationDirPath()).filePath(executableName));
#ifdef Q_OS_WIN
    add(QDir(qEnvironmentVariable("ProgramFiles")).filePath(QStringLiteral("kdenlive/bin/ffprobe.exe")));
    add(QDir(qEnvironmentVariable("ProgramFiles(x86)")).filePath(QStringLiteral("kdenlive/bin/ffprobe.exe")));
#endif
    add(QStandardPaths::findExecutable(QStringLiteral("ffprobe")));
#ifdef Q_OS_WIN
    add(QStandardPaths::findExecutable(QStringLiteral("ffprobe.exe")));
#endif
    return paths;
}

QJsonObject rationalTime(double value, double rate)
{
    return {
        {QStringLiteral("OTIO_SCHEMA"), QStringLiteral("RationalTime.1")},
        {QStringLiteral("value"), value},
        {QStringLiteral("rate"), rate}
    };
}

struct MediaInfo {
    double durationSeconds = 0.0;
    double frameRate = 30.0;
};

bool parseRate(const QString &text, double *rate)
{
    const QStringList parts = text.split(QLatin1Char('/'));
    bool okNumerator = false;
    bool okDenominator = false;
    const double numerator = parts.value(0).toDouble(&okNumerator);
    const double denominator = parts.size() > 1 ? parts.value(1).toDouble(&okDenominator) : 1.0;
    if (!okNumerator || (parts.size() > 1 && !okDenominator) || denominator == 0.0 || numerator <= 0.0) {
        return false;
    }
    *rate = numerator / denominator;
    return true;
}

bool probeMedia(const QString &ffprobe, const QString &path, MediaInfo *info, QString *error)
{
    QProcess process;
    process.setProgram(ffprobe);
    process.setArguments({
        QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-select_streams"), QStringLiteral("v:0"),
        QStringLiteral("-show_entries"),
        QStringLiteral("stream=avg_frame_rate,r_frame_rate,duration:format=duration"),
        QStringLiteral("-of"), QStringLiteral("json"),
        path
    });
    process.start();
    if (!process.waitForStarted(3000) || !process.waitForFinished(20000)) {
        process.kill();
        process.waitForFinished();
        *error = QStringLiteral("FFprobe timed out or could not start.");
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        *error = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        if (error->isEmpty()) {
            *error = QStringLiteral("FFprobe could not read this file.");
        }
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(process.readAllStandardOutput(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *error = QStringLiteral("FFprobe returned invalid metadata.");
        return false;
    }
    const QJsonObject root = document.object();
    const QJsonArray streams = root.value(QStringLiteral("streams")).toArray();
    const QJsonObject stream = streams.isEmpty() ? QJsonObject{} : streams.first().toObject();
    const QJsonObject format = root.value(QStringLiteral("format")).toObject();

    info->durationSeconds = stream.value(QStringLiteral("duration")).toString().toDouble();
    if (info->durationSeconds <= 0.0) {
        info->durationSeconds = format.value(QStringLiteral("duration")).toString().toDouble();
    }
    if (info->durationSeconds <= 0.0) {
        *error = QStringLiteral("No usable video duration was found.");
        return false;
    }

    if (!parseRate(stream.value(QStringLiteral("avg_frame_rate")).toString(), &info->frameRate)) {
        parseRate(stream.value(QStringLiteral("r_frame_rate")).toString(), &info->frameRate);
    }
    if (info->frameRate <= 0.0 || info->frameRate > 1000.0) {
        info->frameRate = 30.0;
    }
    return true;
}

QJsonObject makeClip(const QString &path, const MediaInfo &media)
{
    const double frames = static_cast<double>(qMax<qint64>(1, qRound64(media.durationSeconds * media.frameRate)));
    const QJsonObject timeRange{
        {QStringLiteral("OTIO_SCHEMA"), QStringLiteral("TimeRange.1")},
        {QStringLiteral("start_time"), rationalTime(0.0, media.frameRate)},
        {QStringLiteral("duration"), rationalTime(frames, media.frameRate)}
    };
    const QJsonObject reference{
        {QStringLiteral("OTIO_SCHEMA"), QStringLiteral("ExternalReference.1")},
        {QStringLiteral("target_url"), QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toString(QUrl::FullyEncoded)},
        {QStringLiteral("available_range"), timeRange},
        {QStringLiteral("metadata"), QJsonObject{}}
    };
    return {
        {QStringLiteral("OTIO_SCHEMA"), QStringLiteral("Clip.2")},
        {QStringLiteral("name"), QFileInfo(path).fileName()},
        {QStringLiteral("source_range"), timeRange},
        {QStringLiteral("media_reference"), reference},
        {QStringLiteral("metadata"), QJsonObject{}}
    };
}

class ClipList final : public QListWidget {
public:
    explicit ClipList(QWidget *parent = nullptr) : QListWidget(parent)
    {
        setAcceptDrops(true);
        setDragDropMode(QAbstractItemView::DropOnly);
        setSelectionMode(QAbstractItemView::ExtendedSelection);
        setAlternatingRowColors(true);
    }

    std::function<void(const QStringList &)> filesDropped;

protected:
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event->mimeData()->hasUrls()) {
            event->acceptProposedAction();
        } else {
            QListWidget::dragEnterEvent(event);
        }
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (event->mimeData()->hasUrls()) {
            event->acceptProposedAction();
        } else {
            QListWidget::dragMoveEvent(event);
        }
    }

    void dropEvent(QDropEvent *event) override
    {
        if (!event->mimeData()->hasUrls()) {
            QListWidget::dropEvent(event);
            return;
        }
        QStringList paths;
        for (const QUrl &url : event->mimeData()->urls()) {
            if (url.isLocalFile()) {
                paths << url.toLocalFile();
            }
        }
        if (filesDropped) {
            filesDropped(paths);
        }
        event->acceptProposedAction();
    }
};

class MainWindow final : public QMainWindow {
public:
    MainWindow()
    {
        setWindowTitle(QStringLiteral("Kdenlive AutoSort"));
        resize(760, 540);

        auto *central = new QWidget(this);
        auto *layout = new QVBoxLayout(central);
        auto *description = new QLabel(
            QStringLiteral("Drop camera clips here. AutoSort orders GoPro, DJI and Sony names, then builds an OTIO timeline for Kdenlive."),
            central);
        description->setWordWrap(true);
        layout->addWidget(description);

        m_list = new ClipList(central);
        m_list->setToolTip(QStringLiteral("Drop multiple video files here."));
        layout->addWidget(m_list, 1);
        m_list->filesDropped = [this](const QStringList &paths) { addFiles(paths); };

        auto *buttons = new QHBoxLayout;
        auto *addButton = new QPushButton(QStringLiteral("Add clips…"), central);
        auto *removeButton = new QPushButton(QStringLiteral("Remove selected"), central);
        auto *clearButton = new QPushButton(QStringLiteral("Clear"), central);
        auto *exportButton = new QPushButton(QStringLiteral("Create Kdenlive timeline…"), central);
        buttons->addWidget(addButton);
        buttons->addWidget(removeButton);
        buttons->addWidget(clearButton);
        buttons->addStretch();
        buttons->addWidget(exportButton);
        layout->addLayout(buttons);
        setCentralWidget(central);
        statusBar()->showMessage(QStringLiteral("Drop or add multiple video files to begin."));

        connect(addButton, &QPushButton::clicked, this, [this] {
            const QStringList files = QFileDialog::getOpenFileNames(
                this, QStringLiteral("Select video clips"), QString(),
                QStringLiteral("Video files (*.mp4 *.mov *.m4v *.mkv *.avi *.mts *.m2ts *.mpg *.mpeg *.mxf *.webm *.360 *.osv);;All files (*)"));
            addFiles(files);
        });
        connect(removeButton, &QPushButton::clicked, this, [this] {
            const auto selected = m_list->selectedItems();
            for (QListWidgetItem *item : selected) {
                m_paths.remove(normalizedPath(item->data(Qt::UserRole).toString()));
                delete item;
            }
            sortRows();
        });
        connect(clearButton, &QPushButton::clicked, this, [this] {
            m_list->clear();
            m_paths.clear();
            statusBar()->showMessage(QStringLiteral("List cleared."));
        });
        connect(exportButton, &QPushButton::clicked, this, [this] { exportTimeline(); });
    }

private:
    ClipList *m_list = nullptr;
    QSet<QString> m_paths;

    void addFiles(const QStringList &paths)
    {
        int added = 0;
        for (const QString &path : paths) {
            const QFileInfo info(path);
            if (!info.exists() || !info.isFile() || !kVideoExtensions.contains(info.suffix().toLower())) {
                continue;
            }
            const QString key = normalizedPath(path);
            if (m_paths.contains(key)) {
                continue;
            }
            auto *item = new QListWidgetItem(m_list);
            item->setData(Qt::UserRole, info.absoluteFilePath());
            item->setText(QStringLiteral("%1    %2").arg(cameraLabel(path), info.fileName()));
            item->setToolTip(info.absoluteFilePath());
            m_paths.insert(key);
            ++added;
        }
        sortRows();
        statusBar()->showMessage(QStringLiteral("%1 clip(s) in sorted order.").arg(m_list->count()));
        if (added == 0 && !paths.isEmpty()) {
            statusBar()->showMessage(QStringLiteral("No new supported video clips were added."));
        }
    }

    void sortRows()
    {
        QStringList paths;
        for (int i = 0; i < m_list->count(); ++i) {
            paths << m_list->item(i)->data(Qt::UserRole).toString();
        }
        std::stable_sort(paths.begin(), paths.end(), lessByCameraName);
        m_list->clear();
        for (const QString &path : paths) {
            auto *item = new QListWidgetItem(m_list);
            item->setData(Qt::UserRole, path);
            item->setText(QStringLiteral("%1    %2").arg(cameraLabel(path), QFileInfo(path).fileName()));
            item->setToolTip(path);
        }
    }

    void exportTimeline()
    {
        if (m_list->count() == 0) {
            QMessageBox::information(this, QStringLiteral("No clips"), QStringLiteral("Add at least one video clip first."));
            return;
        }
        const QString output = QFileDialog::getSaveFileName(
            this, QStringLiteral("Save Kdenlive timeline"), QStringLiteral("Camera-Sorted-Timeline.otio"),
            QStringLiteral("OpenTimelineIO (*.otio)"));
        if (output.isEmpty()) {
            return;
        }

        QStringList ffprobes = ffprobeCandidates();
        if (ffprobes.isEmpty()) {
#ifdef Q_OS_WIN
            const QString filter = QStringLiteral("FFprobe (ffprobe.exe);;All files (*)");
#else
            const QString filter = QStringLiteral("FFprobe (ffprobe);;All files (*)");
#endif
            const QString selected = QFileDialog::getOpenFileName(this, QStringLiteral("Locate FFprobe"), QString(), filter);
            if (selected.isEmpty()) {
                return;
            }
            ffprobes << selected;
        }

        QJsonArray clips;
        QString successfulFfprobe;
        for (int i = 0; i < m_list->count(); ++i) {
            const QString path = m_list->item(i)->data(Qt::UserRole).toString();
            MediaInfo media;
            QString error;
            bool readable = false;
            for (const QString &ffprobe : ffprobes) {
                if (probeMedia(ffprobe, path, &media, &error)) {
                    readable = true;
                    successfulFfprobe = ffprobe;
                    break;
                }
            }
            if (!readable) {
                QMessageBox::critical(this, QStringLiteral("Could not read clip"),
                                      QStringLiteral("%1\\n\\nTried the available FFprobe versions.\\n%2").arg(QFileInfo(path).fileName(), error));
                return;
            }
            clips.append(makeClip(path, media));
            statusBar()->showMessage(QStringLiteral("Reading clip %1 of %2…").arg(i + 1).arg(m_list->count()));
            QApplication::processEvents();
        }
        QSettings().setValue(QStringLiteral("ffprobePath"), successfulFfprobe);

        const QJsonObject track{
            {QStringLiteral("OTIO_SCHEMA"), QStringLiteral("Track.1")},
            {QStringLiteral("name"), QStringLiteral("V1")},
            {QStringLiteral("kind"), QStringLiteral("Video")},
            {QStringLiteral("children"), clips},
            {QStringLiteral("metadata"), QJsonObject{}}
        };
        const QJsonObject stack{
            {QStringLiteral("OTIO_SCHEMA"), QStringLiteral("Stack.1")},
            {QStringLiteral("name"), QStringLiteral("tracks")},
            {QStringLiteral("children"), QJsonArray{track}},
            {QStringLiteral("metadata"), QJsonObject{}}
        };
        const QJsonObject timeline{
            {QStringLiteral("OTIO_SCHEMA"), QStringLiteral("Timeline.1")},
            {QStringLiteral("name"), QStringLiteral("Camera Sorted Timeline")},
            {QStringLiteral("global_start_time"), QJsonValue::Null},
            {QStringLiteral("tracks"), stack},
            {QStringLiteral("metadata"), QJsonObject{}}
        };

        QString finalPath = output;
        if (!finalPath.endsWith(QStringLiteral(".otio"), Qt::CaseInsensitive)) {
            finalPath += QStringLiteral(".otio");
        }
        QFile file(finalPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QMessageBox::critical(this, QStringLiteral("Could not save timeline"), file.errorString());
            return;
        }
        file.write(QJsonDocument(timeline).toJson(QJsonDocument::Indented));
        file.close();
        statusBar()->showMessage(QStringLiteral("Saved %1 clip(s) to %2").arg(clips.size()).arg(QFileInfo(finalPath).fileName()));
        QMessageBox::information(this, QStringLiteral("Timeline created"),
                                 QStringLiteral("Created a timeline with %1 clips. In Kdenlive, use File → Import Timeline → OpenTimelineIO.").arg(clips.size()));
    }
};

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Kdenlive AutoSort"));
    QApplication::setOrganizationName(QStringLiteral("Mooresforge"));
    MainWindow window;
    window.show();
    return app.exec();
}
