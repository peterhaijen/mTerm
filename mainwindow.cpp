#include "mainwindow.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMap>
#include <QMessageBox>
#include <QMouseEvent>
#include <QProcess>
#include <QRect>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QSet>
#include <QShortcut>
#include <QStatusBar>
#include <QStandardPaths>
#include <QTabBar>
#include <QTabWidget>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QVariant>
#include <QVector>
#include <QVBoxLayout>
#include <QWidget>
#include <qtermwidget.h>

#include "version.h"

#include <algorithm>
#include <cerrno>
#include <functional>
#include <memory>
#include <signal.h>
#include <utility>

struct TerminalSession
{
    QTermWidget *terminal = nullptr;
    QString title;
    bool broadcastEnabled = true;
    bool broadcastLocked = false;
    bool isAiTerminal = false;
    bool hasFocus = false;
    bool isPrompt = false;
    bool persistentProcess = false;
};

enum class ViewMode { Tabs, Tile };

static constexpr auto settingsViewModeKey = "ui/viewMode";
static constexpr auto settingsViewModeTabs = "tabs";
static constexpr auto settingsViewModeTile = "tile";
static constexpr auto settingsUseScreenKey = "terminal/useScreen";
static constexpr auto settingsTasksDirectoryKey = "tasks/directory";
static constexpr auto settingsAiBinaryPathKey = "ai/binaryPath";

struct TerminalUiState
{
    QList<TerminalSession *> sessions;
    ViewMode viewMode = ViewMode::Tile;
    bool useScreen = false;
    QString tasksDirectory;
    QString aiBinaryPath;
    std::function<void()> renderCurrentView;
    std::function<void(TerminalSession *)> setActiveSession;
    std::function<void(TerminalSession *)> closeSession;
};

static TerminalSession *sessionForTerminal(TerminalUiState *state, QTermWidget *terminal);

static QString viewModeSettingValue(ViewMode viewMode)
{
    return QString::fromLatin1(viewMode == ViewMode::Tabs
                                   ? settingsViewModeTabs
                                   : settingsViewModeTile);
}

static ViewMode viewModeFromSettingValue(const QVariant &value)
{
    if (value.toString() == QString::fromLatin1(settingsViewModeTabs)) {
        return ViewMode::Tabs;
    }

    return ViewMode::Tile;
}

class BroadcastCheckBox : public QCheckBox
{
public:
    explicit BroadcastCheckBox(TerminalSession *session, QWidget *parent = nullptr)
        : QCheckBox(parent)
        , session(session)
    {
        setChecked(session->broadcastEnabled);
        setEnabled(!session->broadcastLocked);
        setToolTip(session->broadcastLocked
                       ? QStringLiteral("Broadcast is disabled for this terminal")
                       : QStringLiteral("Receive broadcast input"));
    }

    std::function<void(bool)> setAllChecked;
    std::function<void()> focusSession;

protected:
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (session->broadcastLocked) {
            return;
        }

        QCheckBox::mouseReleaseEvent(event);
        session->broadcastEnabled = isChecked();

        if ((event->modifiers() & Qt::AltModifier)
            && (event->modifiers() & Qt::ShiftModifier)
            && setAllChecked) {
            setAllChecked(isChecked());
        }

        if (isChecked() && focusSession) {
            focusSession();
        }
    }

private:
    TerminalSession *session = nullptr;
};

class TerminalFocusFilter : public QObject
{
public:
    TerminalFocusFilter(TerminalUiState *state, QObject *parent = nullptr)
        : QObject(parent)
        , state(state)
    {
    }

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() != QEvent::FocusIn
            && event->type() != QEvent::MouseButtonPress
            && event->type() != QEvent::KeyPress) {
            return QObject::eventFilter(object, event);
        }

        TerminalSession *focusedSession = nullptr;
        QObject *currentObject = object;
        while (currentObject && !focusedSession) {
            if (auto *terminal = qobject_cast<QTermWidget *>(currentObject)) {
                focusedSession = sessionForTerminal(state, terminal);
            }
            currentObject = currentObject->parent();
        }

        if (!focusedSession) {
            return QObject::eventFilter(object, event);
        }

        if (state->setActiveSession) {
            state->setActiveSession(focusedSession);
        }

        return QObject::eventFilter(object, event);
    }

private:
    TerminalUiState *state = nullptr;
};

class TerminalShortcutFilter : public QObject
{
public:
    TerminalShortcutFilter(TerminalUiState *state,
                           const QFont &defaultTerminalFont,
                           std::function<void(TerminalSession *)> focusSession,
                           std::function<void(const QString &, int)> showStatusMessage,
                           QObject *parent = nullptr)
        : QObject(parent)
        , state(state)
        , defaultTerminalFont(defaultTerminalFont)
        , focusSession(std::move(focusSession))
        , showStatusMessage(std::move(showStatusMessage))
    {
    }

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() != QEvent::KeyPress) {
            return QObject::eventFilter(object, event);
        }

        QTermWidget *terminal = nullptr;
        QObject *currentObject = object;
        while (currentObject && !terminal) {
            terminal = qobject_cast<QTermWidget *>(currentObject);
            currentObject = currentObject->parent();
        }

        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (!terminal || !keyEvent) {
            return QObject::eventFilter(object, event);
        }

        const Qt::KeyboardModifiers modifiers = keyEvent->modifiers();
        const int key = keyEvent->key();
        const bool ctrl = modifiers.testFlag(Qt::ControlModifier);
        const bool shift = modifiers.testFlag(Qt::ShiftModifier);
        const bool alt = modifiers.testFlag(Qt::AltModifier);
        const bool meta = modifiers.testFlag(Qt::MetaModifier);

        if (ctrl && !shift && !alt && !meta && key == Qt::Key_Insert) {
            terminal->copyClipboard();
            keyEvent->accept();
            return true;
        }

        if (shift && alt && !ctrl && !meta && key == Qt::Key_Insert) {
            terminal->copyClipboard();
            const QString selectedText = QApplication::clipboard()->text();
            TerminalSession *sourceSession = sessionForTerminal(state, terminal);
            TerminalSession *targetSession = nullptr;
            for (int i = state->sessions.count() - 1; i >= 0; --i) {
                TerminalSession *candidate = state->sessions.at(i);
                if (candidate->isAiTerminal && candidate != sourceSession) {
                    targetSession = candidate;
                    break;
                }
            }

            if (!targetSession) {
                if (showStatusMessage) {
                    showStatusMessage(QStringLiteral("No AI terminal open"), 4000);
                }
            } else if (selectedText.isEmpty()) {
                if (showStatusMessage) {
                    showStatusMessage(QStringLiteral("No selected text to send to AI terminal"), 4000);
                }
            } else {
                targetSession->terminal->sendText(selectedText);
                if (focusSession) {
                    focusSession(targetSession);
                }
            }

            keyEvent->accept();
            return true;
        }

        if (shift && !ctrl && !alt && !meta && key == Qt::Key_Insert) {
            terminal->pasteClipboard();
            keyEvent->accept();
            return true;
        }

        if (ctrl && shift && !alt && !meta && (key == Qt::Key_Plus || key == Qt::Key_Equal)) {
            terminal->zoomIn();
            keyEvent->accept();
            return true;
        }

        if (ctrl && !shift && !alt && !meta && key == Qt::Key_Minus) {
            terminal->zoomOut();
            keyEvent->accept();
            return true;
        }

        if (ctrl && !shift && !alt && !meta && key == Qt::Key_0) {
            QFont font = defaultTerminalFont;
            terminal->setTerminalFont(font);
            keyEvent->accept();
            return true;
        }

        return QObject::eventFilter(object, event);
    }

private:
    TerminalUiState *state = nullptr;
    QFont defaultTerminalFont;
    std::function<void(TerminalSession *)> focusSession;
    std::function<void(const QString &, int)> showStatusMessage;
};

struct SshHostEntry
{
    QString host;
    QStringList groups;
};

struct TaskFileEntry
{
    QString title;
    QString path;
};

struct TaskParameterEntry
{
    QString placeholder;
    QString label;
    QString hint;
};

struct HelpPageEntry
{
    QString title;
    QString path;
    QString content;
};

class HelpDialog : public QDialog
{
public:
    HelpDialog(QWidget *parent, const QString &title, const QString &content)
        : QDialog(parent)
    {
        setWindowTitle(title);

        auto *dialogLayout = new QVBoxLayout(this);
        dialogLayout->setContentsMargins(16, 16, 16, 12);
        dialogLayout->setSpacing(12);

        auto *textLabel = new QLabel(content, this);
        textLabel->setTextFormat(Qt::RichText);
        textLabel->setWordWrap(true);
        textLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        textLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

        auto *scrollArea = new QScrollArea(this);
        scrollArea->setWidget(textLabel);
        scrollArea->setWidgetResizable(true);
        scrollArea->setFrameShape(QFrame::NoFrame);
        dialogLayout->addWidget(scrollArea);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok, this);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        dialogLayout->addWidget(buttons);

        const QScreen *targetScreen = parent ? parent->screen() : screen();
        const QRect availableGeometry = targetScreen ? targetScreen->availableGeometry() : QRect(0, 0, 1200, 800);
        const int maxDialogWidth = std::max(360, static_cast<int>(availableGeometry.width() * 0.9));
        const int minDialogWidth = std::min(700, maxDialogWidth);
        const int dialogWidth = std::clamp(static_cast<int>(availableGeometry.width() * 0.55),
                                           minDialogWidth,
                                           std::min(1100, maxDialogWidth));
        const int maxDialogHeight = static_cast<int>(availableGeometry.height() * 0.75);
        textLabel->setMinimumWidth(dialogWidth - 64);
        resize(dialogWidth, std::clamp(sizeHint().height(), std::min(260, maxDialogHeight), maxDialogHeight));
    }
};

static QString readTextFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    QTextStream stream(&file);
    return stream.readAll();
}

static QString htmlTitle(const QString &content, const QString &fallback)
{
    static const QRegularExpression titleRegex(QStringLiteral("<title>([^<]+)</title>"),
                                               QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = titleRegex.match(content);
    if (match.hasMatch()) {
        return match.captured(1).trimmed();
    }

    QString title = QFileInfo(fallback).completeBaseName();
    title.remove(QRegularExpression(QStringLiteral("^\\d+-")));
    title.replace(QLatin1Char('-'), QLatin1Char(' '));
    if (!title.isEmpty()) {
        title[0] = title[0].toUpper();
    }
    return title;
}

static QList<HelpPageEntry> readHelpPages()
{
    QDir helpDirectory(QStringLiteral(":/help"));
    const QStringList files = helpDirectory.entryList(QStringList{QStringLiteral("*.html")}, QDir::Files, QDir::Name);

    QList<HelpPageEntry> pages;
    for (const QString &fileName : files) {
        const QString path = helpDirectory.filePath(fileName);
        QString content = readTextFile(path);
        if (content.isEmpty()) {
            continue;
        }
        content.replace(QStringLiteral("{{MTERM_VERSION}}"), QStringLiteral(MTERM_VERSION));
        pages.append({htmlTitle(content, fileName), path, content});
    }
    return pages;
}

static bool isConcreteSshHost(const QString &host)
{
    return !host.startsWith(QLatin1Char('!'))
        && !host.contains(QLatin1Char('*'))
        && !host.contains(QLatin1Char('?'));
}

static void addSshHostEntries(const QStringList &hosts,
                              const QStringList &groups,
                              bool isMarkedForMTerm,
                              QSet<QString> *seen,
                              QList<SshHostEntry> *entries)
{
    if (!isMarkedForMTerm) {
        return;
    }

    for (const QString &host : hosts) {
        if (!isConcreteSshHost(host) || seen->contains(host)) {
            continue;
        }

        seen->insert(host);
        entries->append({host, groups});
    }
}

static QList<SshHostEntry> readSshConfigHosts()
{
    QFile config(QDir::home().filePath(QStringLiteral(".ssh/config")));
    if (!config.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    QList<SshHostEntry> entries;
    QStringList currentHosts;
    QStringList currentGroups;
    bool currentMarkedForMTerm = false;
    QSet<QString> seen;
    QTextStream stream(&config);
    while (!stream.atEnd()) {
        QString line = stream.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }

        static const QRegularExpression mtermRegex(QStringLiteral("^#\\s*mTerm(?:\\s+Groups\\s+(.+))?\\s*$"));
        const QRegularExpressionMatch mtermMatch = mtermRegex.match(line);
        if (mtermMatch.hasMatch()) {
            currentMarkedForMTerm = true;
            if (mtermMatch.capturedLength(1) > 0) {
                currentGroups.append(mtermMatch.captured(1).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts));
            }
            continue;
        }

        if (line.startsWith(QLatin1Char('#'))) {
            continue;
        }

        const int commentIndex = line.indexOf(QLatin1Char('#'));
        if (commentIndex != -1) {
            line = line.left(commentIndex).trimmed();
        }

        const QStringList parts = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (parts.size() < 2 || parts.first().compare(QStringLiteral("Host"), Qt::CaseInsensitive) != 0) {
            continue;
        }

        addSshHostEntries(currentHosts, currentGroups, currentMarkedForMTerm, &seen, &entries);
        currentHosts = parts.mid(1);
        currentGroups.clear();
        currentMarkedForMTerm = false;
    }

    addSshHostEntries(currentHosts, currentGroups, currentMarkedForMTerm, &seen, &entries);

    return entries;
}

static QString normalizedYamlValue(QString value)
{
    value = value.trimmed();
    if ((value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\'')))
        || (value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))) {
        value = value.mid(1, value.size() - 2);
    }
    return value.trimmed();
}

static QString markdownTaskTitle(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    QTextStream stream(&file);
    if (stream.atEnd() || stream.readLine().trimmed() != QStringLiteral("---")) {
        return {};
    }

    bool hasMTermTag = false;
    QString description;
    bool inTags = false;
    while (!stream.atEnd()) {
        const QString line = stream.readLine();
        const QString trimmed = line.trimmed();

        if (trimmed == QStringLiteral("---")) {
            if (hasMTermTag) {
                return description.isEmpty() ? QFileInfo(path).completeBaseName() : description;
            }
            return {};
        }

        static const QRegularExpression descriptionRegex(QStringLiteral("^description\\s*:\\s*(.*)$"),
                                                         QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch descriptionMatch = descriptionRegex.match(trimmed);
        if (descriptionMatch.hasMatch()) {
            description = normalizedYamlValue(descriptionMatch.captured(1));
            const int commentIndex = description.indexOf(QLatin1Char('#'));
            if (commentIndex != -1) {
                description = normalizedYamlValue(description.left(commentIndex));
            }
            continue;
        }

        if (trimmed == QStringLiteral("tags:")) {
            inTags = true;
            continue;
        }

        if (!inTags) {
            continue;
        }

        if (!line.isEmpty()
            && !line.at(0).isSpace()
            && !trimmed.startsWith(QLatin1Char('-'))) {
            inTags = false;
            continue;
        }

        if (!trimmed.startsWith(QLatin1Char('-'))) {
            continue;
        }

        QString value = normalizedYamlValue(trimmed.mid(1));
        const int commentIndex = value.indexOf(QLatin1Char('#'));
        if (commentIndex != -1) {
            value = normalizedYamlValue(value.left(commentIndex));
        }

        if (value.compare(QStringLiteral("mterm"), Qt::CaseInsensitive) == 0) {
            hasMTermTag = true;
        }
    }

    return {};
}

static QList<TaskFileEntry> readTaskFiles(const QString &tasksDirectoryPath)
{
    QDir tasksDirectory(tasksDirectoryPath);
    if (tasksDirectoryPath.isEmpty() || !tasksDirectory.exists()) {
        return {};
    }

    QList<TaskFileEntry> entries;
    QDirIterator iterator(tasksDirectory.absolutePath(),
                          QStringList{QStringLiteral("*.md")},
                          QDir::Files,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        const QString taskTitle = markdownTaskTitle(path);
        if (!taskTitle.isEmpty()) {
            const QFileInfo fileInfo(path);
            entries.append({taskTitle, fileInfo.absoluteFilePath()});
        }
    }

    std::sort(entries.begin(), entries.end(), [](const TaskFileEntry &left, const TaskFileEntry &right) {
        return QString::localeAwareCompare(left.title, right.title) < 0;
    });

    return entries;
}

static QList<TaskParameterEntry> readMarkdownTaskParameters(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    QTextStream stream(&file);
    if (stream.atEnd() || stream.readLine().trimmed() != QStringLiteral("---")) {
        return {};
    }

    QList<TaskParameterEntry> parameters;
    QSet<QString> seenPlaceholders;
    static const QRegularExpression parameterRegex(QStringLiteral("^<([^>]+)>\\s*:\\s*(.*)$"));

    while (!stream.atEnd()) {
        const QString line = stream.readLine();
        const QString trimmed = line.trimmed();

        if (trimmed == QStringLiteral("---")) {
            break;
        }

        const QRegularExpressionMatch match = parameterRegex.match(trimmed);
        if (!match.hasMatch()) {
            continue;
        }

        const QString label = match.captured(1).trimmed();
        if (label.isEmpty()) {
            continue;
        }

        const QString placeholder = QStringLiteral("<%1>").arg(label);
        if (seenPlaceholders.contains(placeholder)) {
            continue;
        }

        QString hint = normalizedYamlValue(match.captured(2));
        const int commentIndex = hint.indexOf(QLatin1Char('#'));
        if (commentIndex != -1) {
            hint = normalizedYamlValue(hint.left(commentIndex));
        }

        seenPlaceholders.insert(placeholder);
        parameters.append({placeholder, label, hint});
    }

    return parameters;
}

static bool promptTaskParameterValues(QWidget *parent,
                                      const QList<TaskParameterEntry> &parameters,
                                      QMap<QString, QString> *values)
{
    if (parameters.isEmpty()) {
        return true;
    }

    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Task Parameters"));

    auto *dialogLayout = new QVBoxLayout(&dialog);
    dialogLayout->setContentsMargins(16, 16, 16, 12);
    dialogLayout->setSpacing(12);

    auto *titleLabel = new QLabel(QStringLiteral("Enter task values"), &dialog);
    titleLabel->setWordWrap(true);
    dialogLayout->addWidget(titleLabel);

    auto *fieldsLayout = new QGridLayout();
    fieldsLayout->setColumnStretch(1, 1);
    fieldsLayout->setHorizontalSpacing(12);
    fieldsLayout->setVerticalSpacing(8);
    dialogLayout->addLayout(fieldsLayout);

    QVector<QLineEdit *> fields;
    fields.reserve(parameters.size());

    for (int i = 0; i < parameters.size(); ++i) {
        const TaskParameterEntry &parameter = parameters.at(i);

        auto *label = new QLabel(parameter.label, &dialog);
        auto *field = new QLineEdit(&dialog);
        field->setMinimumWidth(480);
        field->setPlaceholderText(parameter.hint);
        field->setClearButtonEnabled(true);
        label->setBuddy(field);

        fieldsLayout->addWidget(label, i, 0);
        fieldsLayout->addWidget(field, i, 1);
        fields.append(field);
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialogLayout->addWidget(buttons);

    for (int i = 0; i < fields.size(); ++i) {
        QLineEdit *field = fields.at(i);
        QObject::connect(field, &QLineEdit::returnPressed, &dialog, [i, fields, &dialog]() {
            if (i + 1 < fields.size()) {
                fields.at(i + 1)->setFocus();
                fields.at(i + 1)->selectAll();
                return;
            }
            dialog.accept();
        });
    }

    fields.first()->setFocus();
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }

    for (int i = 0; i < parameters.size(); ++i) {
        values->insert(parameters.at(i).placeholder, fields.at(i)->text());
    }

    return true;
}

static QStringList readMarkdownCommandBlocks(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    QStringList commandBlocks;
    QStringList currentBlock;
    bool inCodeBlock = false;
    bool currentBlockIsBash = false;

    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine();
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith(QStringLiteral("```"))) {
            if (inCodeBlock) {
                if (currentBlockIsBash) {
                    commandBlocks.append(currentBlock.join(QLatin1Char('\n')));
                }
                currentBlock.clear();
                inCodeBlock = false;
                currentBlockIsBash = false;
            } else {
                inCodeBlock = true;
                currentBlockIsBash = trimmed.compare(QStringLiteral("```bash"), Qt::CaseInsensitive) == 0;
            }
            continue;
        }

        if (inCodeBlock) {
            currentBlock.append(line);
        }
    }

    return commandBlocks;
}

static QString taskInjectionText(const QStringList &commandBlocks)
{
    const QString commands = commandBlocks.join(QStringLiteral("\n\n"));
    QString delimiter = QStringLiteral("MTERM_TASK_EOF");
    int suffix = 1;
    while (commands.split(QLatin1Char('\n')).contains(delimiter)) {
        delimiter = QStringLiteral("MTERM_TASK_EOF_%1").arg(suffix++);
    }

    QString injection;
    QTextStream stream(&injection);
    stream << "mterm_task=$(mktemp) || exit\n";
    stream << "cat > \"$mterm_task\" <<'" << delimiter << "'\n";
    stream << "trap 'rm -f \"$0\"' EXIT\n";
    stream << commands;
    if (!commands.endsWith(QLatin1Char('\n'))) {
        stream << "\n";
    }
    stream << "mterm_status=$?\n";
    stream << "printf '\\nmTerm task exited with status %s\\n' \"$mterm_status\"\n";
    stream << "exit \"$mterm_status\"\n";
    stream << delimiter << "\n";
    stream << "bash \"$mterm_task\"\n";

    return injection;
}

static TerminalSession *sessionForTerminal(TerminalUiState *state, QTermWidget *terminal)
{
    for (TerminalSession *session : state->sessions) {
        if (session->terminal == terminal) {
            return session;
        }
    }
    return nullptr;
}

static TerminalSession *sessionFromFocusedWidget(TerminalUiState *state)
{
    QObject *object = QApplication::focusWidget();
    while (object) {
        if (auto *terminal = qobject_cast<QTermWidget *>(object)) {
            return sessionForTerminal(state, terminal);
        }
        object = object->parent();
    }
    return nullptr;
}

static int tileColumns(int count)
{
    if (count <= 1) {
        return 1;
    }
    if (count <= 4) {
        return 2;
    }
    if (count <= 9) {
        return 3;
    }
    return 4;
}

static QString printableAscii(const QString &text)
{
    QString result;
    result.reserve(text.size());

    for (const QChar character : text) {
        const ushort value = character.unicode();
        if (value == '\r' || value == '\n' || value == '\t' || (value >= 0x20 && value <= 0x7e)) {
            result.append(QChar(value));
        }
    }

    return result;
}

static QString stripTerminalEscapes(const QString &text)
{
    QString result;
    result.reserve(text.size());

    for (int index = 0; index < text.size(); ++index) {
        const QChar character = text.at(index);
        if (character != QChar(0x1b)) {
            result.append(character);
            continue;
        }

        if (index + 1 >= text.size()) {
            break;
        }

        const QChar next = text.at(++index);
        if (next == QLatin1Char(']')) {
            while (index + 1 < text.size()) {
                ++index;
                if (text.at(index) == QChar(0x07)) {
                    break;
                }
                if (text.at(index) == QChar(0x1b) && index + 1 < text.size() && text.at(index + 1) == QLatin1Char('\\')) {
                    ++index;
                    break;
                }
            }
            continue;
        }

        if (next == QLatin1Char('[')) {
            while (index + 1 < text.size()) {
                ++index;
                const ushort value = text.at(index).unicode();
                if (value >= 0x40 && value <= 0x7e) {
                    break;
                }
            }
            continue;
        }
    }

    return result;
}

static QString detectedPromptTitle(const QString &text)
{
    static const QRegularExpression promptRegex(QStringLiteral("([A-Za-z0-9._-]+@[A-Za-z0-9._-]+):[^\\r\\n]*[#$] $"));
    static constexpr qsizetype minimumPromptLength = 7;
    const QString ascii = printableAscii(stripTerminalEscapes(text));

    if (ascii.size() < minimumPromptLength) {
        return {};
    }

    const QRegularExpressionMatch match = promptRegex.match(ascii);
    if (match.hasMatch()) {
        return match.captured(1);
    }

    return {};
}

static QString shellSingleQuote(QString text)
{
    text.replace(QStringLiteral("'"), QStringLiteral("'\"'\"'"));
    return QStringLiteral("'") + text + QStringLiteral("'");
}

static QString screenAttachScript()
{
    return QStringLiteral(
        "screen_session_name=mterm\n"
        "screen_list=$(\"$screen_program\" -ls 2>/dev/null)\n"
        "screen_attached_session=$(printf '%s\\n' \"$screen_list\" | sed -n 's/^[[:space:]]*\\([0-9][^[:space:]]*\\.mterm\\)[[:space:]].*(Attached).*/\\1/p' | head -n 1)\n"
        "if [ -n \"$screen_attached_session\" ]; then\n"
        "    if [ -n \"$SHELL\" ]; then\n"
        "        exec \"$SHELL\" -l\n"
        "    fi\n"
        "    exec /bin/sh -l\n"
        "fi\n"
        "screen_session=$(printf '%s\\n' \"$screen_list\" | sed -n 's/^[[:space:]]*\\([0-9][^[:space:]]*\\.mterm\\)[[:space:]].*/\\1/p' | head -n 1)\n"
        "if [ -n \"$screen_session\" ]; then\n"
        "    exec \"$screen_program\" -q -x \"$screen_session\"\n"
        "fi\n"
        "exec \"$screen_program\" -q -S \"$screen_session_name\" -xRR\n");
}

static QString localScreenLauncherScript(const QString &screenPath)
{
    return QStringLiteral("screen_program=")
        + shellSingleQuote(screenPath)
        + QStringLiteral("\n")
        + screenAttachScript();
}

static bool screenSessionIsAttached(const QString &screenPath)
{
    QProcess screenList;
    screenList.setProgram(screenPath);
    screenList.setArguments(QStringList{QStringLiteral("-ls")});
    screenList.setProcessChannelMode(QProcess::MergedChannels);
    screenList.start();
    if (!screenList.waitForFinished(1000)) {
        screenList.kill();
        screenList.waitForFinished();
        return false;
    }

    const QString output = QString::fromLocal8Bit(screenList.readAll());
    static const QRegularExpression attachedSessionRegex(
        QStringLiteral("(^|\\n)\\s*[0-9][^\\s]*\\.mterm\\s+.*\\(Attached\\)"));
    return attachedSessionRegex.match(output).hasMatch();
}

static QString sshLauncherScript(const QString &host, bool useScreen)
{
    const QString quotedHost = shellSingleQuote(host);
    QString sshCommand = QStringLiteral("ssh ") + quotedHost;
    QString remoteCommand;
    if (useScreen) {
        remoteCommand = QStringLiteral(
            "if screen_program=$(command -v screen 2>/dev/null); then\n")
            + screenAttachScript()
            + QStringLiteral(
                "fi\n"
                "if [ -n \"$SHELL\" ]; then\n"
                "    exec \"$SHELL\" -l\n"
                "fi\n"
                "exec /bin/sh -l\n");
        sshCommand = QStringLiteral("ssh -tt ")
            + quotedHost
            + QStringLiteral(" ")
            + shellSingleQuote(remoteCommand);
    }

    return sshCommand
        + QStringLiteral(
              "\n"
              "status=$?\n"
              "if [ \"$status\" -eq 0 ]; then\n"
              "    exit 0\n"
              "fi\n"
              "\n"
              "printf '\\nSSH exited with status %s. Press any key within 10 seconds to keep this tab open.\\n' \"$status\"\n"
              "remaining=10\n"
              "while [ \"$remaining\" -gt 0 ]; do\n"
              "    filled=$((11 - remaining))\n"
              "    empty=$((10 - filled))\n"
              "    bar=$(printf '%*s' \"$filled\" '' | tr ' ' '#')\n"
              "    spaces=$(printf '%*s' \"$empty\" '')\n"
              "    printf '\\rClosing in %2d seconds [%s%s] ' \"$remaining\" \"$bar\" \"$spaces\"\n"
              "    if IFS= read -r -s -n 1 -t 1 _keep_open; then\n"
              "        printf '\\nKeeping tab open.\\n'\n"
              "        exec /bin/bash -i\n"
              "    fi\n"
              "    remaining=$((remaining - 1))\n"
              "done\n"
              "printf '\\n'\n"
              "exit \"$status\"\n");
}

static QString screenProgramPath()
{
    return QStandardPaths::findExecutable(QStringLiteral("screen"));
}

static bool processExists(int pid)
{
    return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM);
}

static void signalTerminalProcess(QTermWidget *terminal, int signalNumber)
{
    const int pid = terminal ? terminal->getShellPID() : -1;
    if (pid <= 0) {
        return;
    }

    // QTermWidget starts the shell/ssh as the session process. Try the process
    // group first so children of the shell are also notified, then the process.
    kill(-pid, signalNumber);
    kill(pid, signalNumber);
}

static void stopTerminalProcess(QTermWidget *terminal)
{
    const int pid = terminal ? terminal->getShellPID() : -1;
    if (pid <= 0 || !processExists(pid)) {
        return;
    }

    signalTerminalProcess(terminal, SIGHUP);
    for (int i = 0; i < 20 && processExists(pid); ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(10);
    }

    if (!processExists(pid)) {
        return;
    }

    signalTerminalProcess(terminal, SIGTERM);
    for (int i = 0; i < 20 && processExists(pid); ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(10);
    }

    if (processExists(pid)) {
        signalTerminalProcess(terminal, SIGKILL);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    statusBar()->showMessage(QStringLiteral("Ready"));

    QSettings settings;
    auto *state = new TerminalUiState;
    state->viewMode = viewModeFromSettingValue(settings.value(settingsViewModeKey,
                                                              QString::fromLatin1(settingsViewModeTile)));
    state->useScreen = settings.value(settingsUseScreenKey, false).toBool();
    state->tasksDirectory = settings.value(settingsTasksDirectoryKey).toString();
    state->aiBinaryPath = settings.value(settingsAiBinaryPathKey).toString();
    qApp->installEventFilter(new TerminalFocusFilter(state, this));
    connect(qApp, &QCoreApplication::aboutToQuit, this, [state]() {
        for (TerminalSession *session : state->sessions) {
            if (session->terminal && !session->persistentProcess) {
                stopTerminalProcess(session->terminal);
            }
        }
    });
    connect(this, &QObject::destroyed, this, [state]() {
        for (TerminalSession *session : state->sessions) {
            delete session;
        }
        delete state;
    });

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    auto *tabs = new QTabWidget(central);
    auto *tileScroll = new QScrollArea(central);
    auto *tileContent = new QWidget(tileScroll);
    auto *tileLayout = new QGridLayout(tileContent);
    auto *emptyLabel = new QLabel(
        QStringLiteral(
            "<div style='text-align:center; line-height:1.35;'>"
            "<p><b>mTerm</b> broadcasts your keystrokes to multiple terminals at once.</p>"
            "<p>Open terminals from the <b>Hosts</b> menu, then use the checkboxes to choose which sessions receive shared input.</p>"
            "<p style='color:#c62828;'><b>Warning:</b> work carefully. One bad command can hit many systems very fast.</p>"
            "<p style='color:#c62828;'>I take no responsibility if something breaks; if it breaks, you get to keep both pieces :)</p>"
            "</div>"),
        central);
    auto *tileHeaders = new QMap<TerminalSession *, QWidget *>;
    auto *tileTitles = new QMap<TerminalSession *, QLabel *>;

    emptyLabel->setAlignment(Qt::AlignCenter);
    emptyLabel->setWordWrap(true);
    tileScroll->setWidget(tileContent);
    tileScroll->setWidgetResizable(true);
    tileLayout->setContentsMargins(6, 6, 6, 6);
    tileLayout->setSpacing(6);

    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(tabs);
    layout->addWidget(tileScroll);
    layout->addWidget(emptyLabel);

    auto setAllBroadcast = [state](bool checked) {
        for (TerminalSession *session : state->sessions) {
            if (session->broadcastLocked) {
                continue;
            }
            session->broadcastEnabled = checked;
        }
    };

    auto updateEmptyState = [state, tabs, tileScroll, emptyLabel]() {
        const bool hasSessions = !state->sessions.isEmpty();
        tabs->setVisible(hasSessions && state->viewMode == ViewMode::Tabs);
        tileScroll->setVisible(hasSessions && state->viewMode == ViewMode::Tile);
        emptyLabel->setVisible(!hasSessions);
    };

    auto updateTileHeaderStyles = [tileHeaders]() {
        for (auto it = tileHeaders->begin(); it != tileHeaders->end(); ++it) {
            TerminalSession *session = it.key();
            QWidget *header = it.value();
            header->setStyleSheet(session->hasFocus
                                      ? QStringLiteral("background-color: #2f6fed; color: white; padding: 3px;")
                                      : QStringLiteral("background-color: #3a3a3a; color: white; padding: 3px;"));
        }
    };

    state->setActiveSession = [state, updateTileHeaderStyles](TerminalSession *activeSession) {
        bool changed = false;
        for (TerminalSession *session : state->sessions) {
            const bool hasFocus = session == activeSession;
            if (session->hasFocus != hasFocus) {
                session->hasFocus = hasFocus;
                changed = true;
            }
        }

        if (changed) {
            updateTileHeaderStyles();
        }
    };

    auto clearTabs = [tabs]() {
        while (tabs->count() > 0) {
            QWidget *widget = tabs->widget(0);
            QWidget *button = tabs->tabBar()->tabButton(0, QTabBar::LeftSide);
            tabs->tabBar()->setTabButton(0, QTabBar::LeftSide, nullptr);
            if (button) {
                button->deleteLater();
            }
            tabs->removeTab(0);
            if (widget) {
                widget->setParent(nullptr);
            }
        }
    };

    auto clearTile = [state, tileLayout, tileHeaders, tileTitles]() {
        tileHeaders->clear();
        tileTitles->clear();

        for (TerminalSession *session : state->sessions) {
            session->terminal->setParent(nullptr);
        }

        while (QLayoutItem *item = tileLayout->takeAt(0)) {
            if (QWidget *widget = item->widget()) {
                widget->deleteLater();
            }
            delete item;
        }
    };

    auto focusSession = [state, tabs](TerminalSession *session) {
        if (!session) {
            return;
        }

        if (state->setActiveSession) {
            state->setActiveSession(session);
        }

        if (state->viewMode == ViewMode::Tabs) {
            const int index = tabs->indexOf(session->terminal);
            if (index != -1) {
                tabs->setCurrentIndex(index);
            }
        }

        session->terminal->setFocus();
    };

    state->renderCurrentView = [state, tabs, tileContent, tileLayout, tileHeaders, tileTitles, clearTabs, clearTile, updateEmptyState, setAllBroadcast, focusSession]() {
        clearTabs();
        clearTile();

        if (state->viewMode == ViewMode::Tabs) {
            for (TerminalSession *session : state->sessions) {
                const int tabIndex = tabs->addTab(session->terminal, session->title);
                session->terminal->setMinimumSize(0, 0);
                session->terminal->show();
                auto *tabHeader = new QWidget(tabs);
                auto *tabHeaderLayout = new QHBoxLayout(tabHeader);
                auto *checkBox = new BroadcastCheckBox(session, tabs);
                auto *closeButton = new QToolButton(tabs);

                tabHeaderLayout->setContentsMargins(0, 0, 0, 0);
                tabHeaderLayout->setSpacing(4);
                tabHeaderLayout->addWidget(checkBox);
                tabHeaderLayout->addWidget(closeButton);
                closeButton->setText(QStringLiteral("x"));
                closeButton->setAutoRaise(true);
                closeButton->setToolTip(QStringLiteral("Close terminal"));
                closeButton->setFixedSize(16, 16);

                checkBox->setAllChecked = [state, setAllBroadcast](bool checked) {
                    setAllBroadcast(checked);
                    state->renderCurrentView();
                };
                checkBox->focusSession = [focusSession, session]() {
                    focusSession(session);
                };
                QObject::connect(closeButton, &QToolButton::clicked, tabs, [state, session]() {
                    if (state->closeSession) {
                        state->closeSession(session);
                    }
                });

                tabs->tabBar()->setTabButton(tabIndex, QTabBar::LeftSide, tabHeader);
            }
        } else {
            const int columns = tileColumns(state->sessions.count());
            for (int index = 0; index < state->sessions.count(); ++index) {
                TerminalSession *session = state->sessions.at(index);
                auto *cell = new QWidget(tileContent);
                auto *cellLayout = new QVBoxLayout(cell);
                auto *header = new QWidget(cell);
                auto *headerLayout = new QHBoxLayout(header);
                auto *checkBox = new BroadcastCheckBox(session, header);
                auto *title = new QLabel(session->title, header);
                auto *closeButton = new QToolButton(header);

                header->setStyleSheet(session->hasFocus
                                          ? QStringLiteral("background-color: #2f6fed; color: white; padding: 3px;")
                                          : QStringLiteral("background-color: #3a3a3a; color: white; padding: 3px;"));
                tileHeaders->insert(session, header);
                tileTitles->insert(session, title);

                checkBox->setAllChecked = [state, setAllBroadcast](bool checked) {
                    setAllBroadcast(checked);
                    state->renderCurrentView();
                };
                checkBox->focusSession = [focusSession, session]() {
                    focusSession(session);
                };

                headerLayout->setContentsMargins(0, 0, 0, 0);
                headerLayout->addWidget(checkBox);
                headerLayout->addWidget(title);
                headerLayout->addStretch();
                headerLayout->addWidget(closeButton);
                closeButton->setText(QStringLiteral("x"));
                closeButton->setAutoRaise(true);
                closeButton->setToolTip(QStringLiteral("Close terminal"));
                closeButton->setFixedSize(16, 16);
                QObject::connect(closeButton, &QToolButton::clicked, header, [state, session]() {
                    if (state->closeSession) {
                        state->closeSession(session);
                    }
                });

                cellLayout->setContentsMargins(0, 0, 0, 0);
                cellLayout->addWidget(header);
                cellLayout->addWidget(session->terminal);
                cellLayout->setStretch(0, 0);
                cellLayout->setStretch(1, 1);

                session->terminal->setMinimumSize(320, 180);
                session->terminal->show();
                cell->setMinimumSize(340, 220);

                tileLayout->addWidget(cell, index / columns, index % columns);
            }
        }

        updateEmptyState();
    };

    state->closeSession = [state](TerminalSession *session) {
        if (!session || !state->sessions.contains(session)) {
            return;
        }

        const int closedIndex = state->sessions.indexOf(session);
        state->sessions.removeAll(session);
        QObject::disconnect(session->terminal, nullptr, nullptr, nullptr);
        if (!session->persistentProcess) {
            stopTerminalProcess(session->terminal);
        }
        session->terminal->setParent(nullptr);
        session->terminal->deleteLater();
        delete session;

        if (!state->sessions.isEmpty()) {
            const int nextIndex = closedIndex < state->sessions.count() ? closedIndex : state->sessions.count() - 1;
            TerminalSession *nextSession = state->sessions.at(nextIndex);
            if (state->setActiveSession) {
                state->setActiveSession(nextSession);
            } else {
                nextSession->hasFocus = true;
            }
        }

        state->renderCurrentView();
    };

    auto currentSession = [state, tabs]() -> TerminalSession * {
        if (state->viewMode == ViewMode::Tabs) {
            return sessionForTerminal(state, qobject_cast<QTermWidget *>(tabs->currentWidget()));
        }

        if (TerminalSession *session = sessionFromFocusedWidget(state)) {
            return session;
        }

        for (TerminalSession *session : state->sessions) {
            if (session->hasFocus) {
                return session;
            }
        }

        return nullptr;
    };

    auto aiSession = [state]() -> TerminalSession * {
        for (TerminalSession *session : state->sessions) {
            if (session->isAiTerminal) {
                return session;
            }
        }

        return nullptr;
    };

    auto updateSessionTitle = [tabs, tileTitles](TerminalSession *session, const QString &title) {
        if (!session || session->title == title) {
            return;
        }

        session->title = title;

        const int tabIndex = tabs->indexOf(session->terminal);
        if (tabIndex != -1) {
            tabs->setTabText(tabIndex, title);
        }

        if (auto *tileTitle = tileTitles->value(session, nullptr)) {
            tileTitle->setText(title);
        }
    };

    auto *nextTabShortcut = new QShortcut(QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_Right), this);
    connect(nextTabShortcut, &QShortcut::activated, this, [state, tabs, currentSession, focusSession]() {
        if (state->viewMode == ViewMode::Tabs && tabs->count() > 0) {
            tabs->setCurrentIndex((tabs->currentIndex() + 1) % tabs->count());
            return;
        }

        if (state->viewMode == ViewMode::Tile && !state->sessions.isEmpty()) {
            TerminalSession *session = currentSession();
            const int index = session ? state->sessions.indexOf(session) : -1;
            focusSession(state->sessions.at((index + 1 + state->sessions.count()) % state->sessions.count()));
        }
    });

    auto *previousTabShortcut = new QShortcut(QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_Left), this);
    connect(previousTabShortcut, &QShortcut::activated, this, [state, tabs, currentSession, focusSession]() {
        if (state->viewMode == ViewMode::Tabs && tabs->count() > 0) {
            tabs->setCurrentIndex((tabs->currentIndex() - 1 + tabs->count()) % tabs->count());
            return;
        }

        if (state->viewMode == ViewMode::Tile && !state->sessions.isEmpty()) {
            TerminalSession *session = currentSession();
            const int index = session ? state->sessions.indexOf(session) : 0;
            focusSession(state->sessions.at((index - 1 + state->sessions.count()) % state->sessions.count()));
        }
    });

    auto *toggleCurrentBroadcastShortcut = new QShortcut(QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_Space), this);
    connect(toggleCurrentBroadcastShortcut, &QShortcut::activated, this, [currentSession, state]() {
        TerminalSession *session = currentSession();
        if (session && !session->broadcastLocked) {
            session->broadcastEnabled = !session->broadcastEnabled;
            state->renderCurrentView();
        }
    });

    auto *closeCurrentTabShortcut = new QShortcut(QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_Delete), this);
    connect(closeCurrentTabShortcut, &QShortcut::activated, this, [currentSession, state]() {
        if (state->closeSession) {
            state->closeSession(currentSession());
        }
    });

    QFont terminalFont = QApplication::font();
    terminalFont.setFamily(QStringLiteral("Monospace"));
    terminalFont.setPointSize(10);
    auto activeStatusWarningToken = std::make_shared<int>(0);
    auto activeStatusWarningMessage = std::make_shared<QString>();
    connect(statusBar(), &QStatusBar::messageChanged, this, [this, activeStatusWarningMessage](const QString &currentMessage) {
        if (!activeStatusWarningMessage->isEmpty() && currentMessage != *activeStatusWarningMessage) {
            activeStatusWarningMessage->clear();
            statusBar()->setStyleSheet(QString());
        }
    });
    auto showStatusMessage = [this, activeStatusWarningToken, activeStatusWarningMessage](const QString &message, int timeoutMs) {
        ++(*activeStatusWarningToken);
        const int warningToken = *activeStatusWarningToken;
        *activeStatusWarningMessage = message;
        statusBar()->setStyleSheet(QStringLiteral("QStatusBar { background-color: #c62828; color: white; font-weight: 700; }"));
        statusBar()->showMessage(message, timeoutMs);
        QTimer::singleShot(timeoutMs, this, [this, activeStatusWarningToken, activeStatusWarningMessage, warningToken]() {
            if (*activeStatusWarningToken == warningToken) {
                activeStatusWarningMessage->clear();
                statusBar()->setStyleSheet(QString());
            }
        });
    };
    auto *terminalShortcutFilter = new TerminalShortcutFilter(state, terminalFont, focusSession, showStatusMessage, this);
    QApplication::instance()->installEventFilter(terminalShortcutFilter);

    auto createTerminal = [this, state, terminalFont, updateSessionTitle, terminalShortcutFilter](
                              const QString &tabName = QStringLiteral("Terminal"),
                              const QString &program = QString(),
                              const QStringList &args = QStringList(),
                              bool persistentProcess = false,
                              bool initialBroadcastEnabled = true,
                              bool broadcastLocked = false,
                              bool isAiTerminal = false) {
        QString terminalProgram = program;
        QStringList terminalArgs = args;
        if (terminalProgram.isEmpty() && state->useScreen) {
            const QString screenPath = screenProgramPath();
            if (!screenPath.isEmpty() && !screenSessionIsAttached(screenPath)) {
                terminalProgram = QStringLiteral("/bin/sh");
                terminalArgs = QStringList{QStringLiteral("-lc"), localScreenLauncherScript(screenPath)};
                persistentProcess = true;
            }
        }

        const bool useCustomProgram = !terminalProgram.isEmpty();
        auto *terminal = new QTermWidget(useCustomProgram ? 0 : 1, this);
        auto *session = new TerminalSession{
            terminal,
            tabName,
            broadcastLocked ? false : initialBroadcastEnabled,
            broadcastLocked,
            isAiTerminal,
            true,
            false,
            persistentProcess,
        };
        for (TerminalSession *otherSession : state->sessions) {
            otherSession->hasFocus = false;
        }
        state->sessions.append(session);

        QFont font = terminalFont;
        terminal->setTerminalFont(font);
        terminal->setColorScheme(QStringLiteral("WhiteOnBlack"));
        terminal->setScrollBarPosition(QTermWidget::ScrollBarRight);
        terminal->setAutoClose(true);
        terminal->installEventFilter(terminalShortcutFilter);

        connect(terminal, &QTermWidget::finished, this, [session, state]() {
            if (state->closeSession) {
                state->closeSession(session);
            }
        });

        connect(terminal, &QTermWidget::termKeyPressed, this, [state, session](QKeyEvent *event) {
            static bool broadcasting = false;
            if (broadcasting || !session->broadcastEnabled) {
                return;
            }

            broadcasting = true;
            for (TerminalSession *targetSession : state->sessions) {
                if (targetSession == session || !targetSession->broadcastEnabled) {
                    continue;
                }

                QKeyEvent forwardedEvent(event->type(),
                                         event->key(),
                                         event->modifiers(),
                                         event->text(),
                                         event->isAutoRepeat(),
                                         event->count());
                targetSession->terminal->sendKeyEvent(&forwardedEvent);
            }
            broadcasting = false;
        });

        connect(terminal, &QTermWidget::receivedData, this, [session, updateSessionTitle](const QString &text) {
            session->isPrompt = false;
            const QString promptTitle = detectedPromptTitle(text);
            if (!promptTitle.isEmpty()) {
                session->isPrompt = true;
                updateSessionTitle(session, promptTitle);
            }
        });

        state->renderCurrentView();
        terminal->setFocus();

        if (useCustomProgram) {
            terminal->setShellProgram(terminalProgram);
            terminal->setArgs(terminalArgs);
            terminal->startShellProgram();
        }

        return terminal;
    };

    auto *fileMenu = menuBar()->addMenu(QStringLiteral("File"));
    fileMenu->menuAction()->setStatusTip(QStringLiteral("Application actions"));
    auto *exitAction = fileMenu->addAction(QStringLiteral("Exit"), this, &QWidget::close);
    exitAction->setStatusTip(QStringLiteral("Close mTerm"));

    auto *viewMenu = menuBar()->addMenu(QStringLiteral("View"));
    viewMenu->menuAction()->setStatusTip(QStringLiteral("Change terminal layout"));
    auto *tabsViewAction = viewMenu->addAction(QStringLiteral("Tabs"));
    tabsViewAction->setCheckable(true);
    tabsViewAction->setChecked(state->viewMode == ViewMode::Tabs);
    tabsViewAction->setStatusTip(QStringLiteral("Show one terminal per tab"));
    auto *tileViewAction = viewMenu->addAction(QStringLiteral("Tile All"));
    tileViewAction->setCheckable(true);
    tileViewAction->setChecked(state->viewMode == ViewMode::Tile);
    tileViewAction->setStatusTip(QStringLiteral("Show all terminals in a tiled layout"));

    connect(tabsViewAction, &QAction::triggered, this, [state, tabsViewAction, tileViewAction]() {
        state->viewMode = ViewMode::Tabs;
        tabsViewAction->setChecked(true);
        tileViewAction->setChecked(false);
        QSettings().setValue(settingsViewModeKey, viewModeSettingValue(state->viewMode));
        state->renderCurrentView();
    });

    connect(tileViewAction, &QAction::triggered, this, [state, tabsViewAction, tileViewAction]() {
        state->viewMode = ViewMode::Tile;
        tabsViewAction->setChecked(false);
        tileViewAction->setChecked(true);
        QSettings().setValue(settingsViewModeKey, viewModeSettingValue(state->viewMode));
        state->renderCurrentView();
    });

    auto *terminalMenu = menuBar()->addMenu(QStringLiteral("Terminal"));
    terminalMenu->menuAction()->setStatusTip(QStringLiteral("Terminal behavior"));
    auto *useScreenAction = terminalMenu->addAction(QStringLiteral("Use screen sessions"));
    useScreenAction->setCheckable(true);
    useScreenAction->setChecked(state->useScreen);
    useScreenAction->setStatusTip(QStringLiteral("Use screen sessions for new local and SSH terminals"));
    connect(useScreenAction, &QAction::toggled, this, [state](bool checked) {
        state->useScreen = checked;
        QSettings().setValue(settingsUseScreenKey, checked);
    });

    auto *aiMenu = menuBar()->addMenu(QStringLiteral("AI"));
    aiMenu->menuAction()->setStatusTip(QStringLiteral("Start configured AI command-line tools"));
    auto *selectAiBinaryAction = aiMenu->addAction(QStringLiteral("Select AI Binary..."));
    selectAiBinaryAction->setStatusTip(QStringLiteral("Choose the AI command binary to launch"));
    auto *startAiAction = aiMenu->addAction(QStringLiteral("Start AI"));
    startAiAction->setStatusTip(QStringLiteral("Start the configured AI binary in a terminal"));
    auto *currentAiBinaryAction = aiMenu->addAction(QStringLiteral("Binary: Not set"));
    currentAiBinaryAction->setEnabled(false);

    auto updateAiActions = [state, startAiAction, currentAiBinaryAction]() {
        const QFileInfo binaryInfo(state->aiBinaryPath);
        const bool hasExecutableBinary = !state->aiBinaryPath.isEmpty()
            && binaryInfo.exists()
            && binaryInfo.isFile()
            && binaryInfo.isExecutable();

        startAiAction->setEnabled(hasExecutableBinary);
        currentAiBinaryAction->setText(
            state->aiBinaryPath.isEmpty()
                ? QStringLiteral("Binary: Not set")
                : QStringLiteral("Binary: %1").arg(QDir::toNativeSeparators(state->aiBinaryPath)));
        currentAiBinaryAction->setStatusTip(state->aiBinaryPath);
    };

    connect(selectAiBinaryAction, &QAction::triggered, this, [this, state, updateAiActions]() {
        const QString startDirectory = QFileInfo(state->aiBinaryPath).exists()
                                           ? QFileInfo(state->aiBinaryPath).absolutePath()
                                           : QDir::homePath();
        const QString selectedBinary = QFileDialog::getOpenFileName(
            this,
            QStringLiteral("Select AI Binary"),
            startDirectory);
        if (selectedBinary.isEmpty()) {
            return;
        }

        const QFileInfo binaryInfo(selectedBinary);
        if (!binaryInfo.exists() || !binaryInfo.isFile() || !binaryInfo.isExecutable()) {
            QMessageBox::warning(
                this,
                QStringLiteral("AI binary is not executable"),
                QStringLiteral("Choose an executable binary file."));
            return;
        }

        state->aiBinaryPath = binaryInfo.absoluteFilePath();
        QSettings().setValue(settingsAiBinaryPathKey, state->aiBinaryPath);
        updateAiActions();
    });

    connect(startAiAction, &QAction::triggered, this, [this, state, createTerminal, aiSession, focusSession]() {
        if (TerminalSession *session = aiSession()) {
            focusSession(session);
            return;
        }

        const QFileInfo binaryInfo(state->aiBinaryPath);
        if (state->aiBinaryPath.isEmpty()
            || !binaryInfo.exists()
            || !binaryInfo.isFile()
            || !binaryInfo.isExecutable()) {
            QMessageBox::warning(
                this,
                QStringLiteral("AI binary is not configured"),
                QStringLiteral("Choose an executable AI binary first."));
            return;
        }

        createTerminal(QStringLiteral("AI: %1").arg(binaryInfo.fileName()),
                       binaryInfo.absoluteFilePath(),
                       QStringList(),
                       false,
                       false,
                       true,
                       true);
    });

    updateAiActions();

    auto openHost = [state, createTerminal](const QString &host) {
        createTerminal(host, QStringLiteral("/bin/bash"), QStringList{QStringLiteral("-lc"), sshLauncherScript(host, state->useScreen)});
    };

    auto *hostsMenu = menuBar()->addMenu(QStringLiteral("Hosts"));
    hostsMenu->menuAction()->setStatusTip(QStringLiteral("Open local and SSH terminal sessions"));

    auto addHostAction = [this, openHost](QMenu *menu, const QString &host) {
        auto *action = menu->addAction(host, this, [host, openHost]() {
            openHost(host);
        });
        action->setStatusTip(QStringLiteral("Open terminal for %1").arg(host));
    };

    auto menuPathKey = [](const QStringList &parts) {
        return parts.join(QChar(0x1f));
    };

    auto rebuildHostsMenu = [this, hostsMenu, createTerminal, openHost, addHostAction, menuPathKey]() {
        hostsMenu->clear();

        const QList<SshHostEntry> hosts = readSshConfigHosts();
        auto *localhostAction = hostsMenu->addAction(QStringLiteral("localhost"), this, [createTerminal]() {
            createTerminal(QStringLiteral("localhost"));
        });
        localhostAction->setStatusTip(QStringLiteral("Open a local terminal"));

        if (!hosts.isEmpty()) {
            hostsMenu->addSeparator();
        }

        const QString rootPathKey;
        QMap<QString, QSet<QString>> hostsByPath;
        QMap<QString, QSet<QString>> directHostsByPath;
        QMap<QString, QSet<QString>> childGroupsByPath;

        for (const SshHostEntry &entry : hosts) {
            if (entry.groups.isEmpty()) {
                addHostAction(hostsMenu, entry.host);
                continue;
            }

            QStringList uniqueGroups;
            QSet<QString> seenGroups;
            for (const QString &group : entry.groups) {
                if (group.isEmpty() || seenGroups.contains(group)) {
                    continue;
                }
                seenGroups.insert(group);
                uniqueGroups.append(group);
            }

            if (uniqueGroups.isEmpty()) {
                addHostAction(hostsMenu, entry.host);
                continue;
            }

            QVector<bool> used(uniqueGroups.size(), false);
            QStringList permutation;

            std::function<void()> collectPermutations = [&]() {
                if (permutation.size() == uniqueGroups.size()) {
                    for (int depth = 1; depth <= permutation.size(); ++depth) {
                        const QStringList path = permutation.mid(0, depth);
                        const QString key = menuPathKey(path);
                        const QString parentKey = depth == 1
                                                      ? rootPathKey
                                                      : menuPathKey(permutation.mid(0, depth - 1));

                        hostsByPath[key].insert(entry.host);
                        childGroupsByPath[parentKey].insert(path.last());
                        if (depth == permutation.size()) {
                            directHostsByPath[key].insert(entry.host);
                        }
                    }
                    return;
                }

                QSet<QString> levelSeen;
                for (int index = 0; index < uniqueGroups.size(); ++index) {
                    if (used[index] || levelSeen.contains(uniqueGroups.at(index))) {
                        continue;
                    }

                    levelSeen.insert(uniqueGroups.at(index));
                    used[index] = true;
                    permutation.append(uniqueGroups.at(index));
                    collectPermutations();
                    permutation.removeLast();
                    used[index] = false;
                }
            };

            collectPermutations();
        }

        std::function<void(QMenu *, const QStringList &)> addGroupMenuTree = [&](QMenu *parentMenu, const QStringList &path) {
            if (path.isEmpty()) {
                return;
            }

            const QString key = menuPathKey(path);
            auto *menu = parentMenu->addMenu(path.last());
            menu->menuAction()->setStatusTip(QStringLiteral("Open hosts in %1").arg(path.join(QStringLiteral(" / "))));

            QStringList openAllHosts = hostsByPath.value(key).values();
            openAllHosts.sort(Qt::CaseInsensitive);
            auto *openAllAction = menu->addAction(QStringLiteral("Open All"), this, [openAllHosts, openHost]() {
                for (const QString &host : openAllHosts) {
                    openHost(host);
                }
            });
            openAllAction->setStatusTip(QStringLiteral("Open all hosts in %1").arg(path.join(QStringLiteral(" / "))));
            menu->addSeparator();

            QStringList directHosts = directHostsByPath.value(key).values();
            directHosts.sort(Qt::CaseInsensitive);
            for (const QString &host : directHosts) {
                addHostAction(menu, host);
            }

            QStringList childGroups = childGroupsByPath.value(key).values();
            childGroups.sort(Qt::CaseInsensitive);
            for (const QString &childGroup : childGroups) {
                QStringList childPath = path;
                childPath.append(childGroup);
                addGroupMenuTree(menu, childPath);
            }
        };

        QStringList topLevelGroups = childGroupsByPath.value(rootPathKey).values();
        topLevelGroups.sort(Qt::CaseInsensitive);
        for (const QString &group : topLevelGroups) {
            addGroupMenuTree(hostsMenu, QStringList{group});
        }
    };

    rebuildHostsMenu();

    auto *tasksMenu = menuBar()->addMenu(QStringLiteral("Tasks"));
    tasksMenu->menuAction()->setStatusTip(QStringLiteral("Run mterm tasks from the configured tasks directory"));

    auto rebuildTasksMenu = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> rebuildTasksMenuWeak = rebuildTasksMenu;
    *rebuildTasksMenu = [this, state, tasksMenu, currentSession, rebuildTasksMenuWeak]() {
        tasksMenu->clear();

        auto *selectTasksDirectoryAction = tasksMenu->addAction(QStringLiteral("Select Tasks Directory..."));
        selectTasksDirectoryAction->setStatusTip(QStringLiteral("Choose the folder scanned for mterm task markdown files"));
        if (const auto rebuildTasksMenu = rebuildTasksMenuWeak.lock()) {
            connect(selectTasksDirectoryAction, &QAction::triggered, this, [this, state, rebuildTasksMenu]() {
                const QString startDirectory = QDir(state->tasksDirectory).exists()
                                                   ? state->tasksDirectory
                                                   : QDir::homePath();
                const QString selectedDirectory = QFileDialog::getExistingDirectory(
                    this,
                    QStringLiteral("Select Tasks Directory"),
                    startDirectory);
                if (selectedDirectory.isEmpty()) {
                    return;
                }

                state->tasksDirectory = QDir(selectedDirectory).absolutePath();
                QSettings().setValue(settingsTasksDirectoryKey, state->tasksDirectory);
                (*rebuildTasksMenu)();
            });
        } else {
            selectTasksDirectoryAction->setEnabled(false);
            selectTasksDirectoryAction->setStatusTip(QStringLiteral("Tasks menu cannot be rebuilt"));
        }

        const QString currentDirectoryText = state->tasksDirectory.isEmpty()
                                                 ? QStringLiteral("Directory: Not set")
                                                 : QStringLiteral("Directory: %1").arg(QDir::toNativeSeparators(state->tasksDirectory));
        auto *currentDirectoryAction = tasksMenu->addAction(currentDirectoryText);
        currentDirectoryAction->setEnabled(false);
        currentDirectoryAction->setStatusTip(state->tasksDirectory);
        tasksMenu->addSeparator();

        if (state->tasksDirectory.isEmpty()) {
            auto *noDirectoryAction = tasksMenu->addAction(QStringLiteral("No tasks directory selected"));
            noDirectoryAction->setEnabled(false);
            noDirectoryAction->setStatusTip(QStringLiteral("Choose a tasks directory first"));
            return;
        }

        const QList<TaskFileEntry> taskFiles = readTaskFiles(state->tasksDirectory);
        if (taskFiles.isEmpty()) {
            auto *emptyTasksAction = tasksMenu->addAction(QStringLiteral("No mterm tasks found"));
            emptyTasksAction->setEnabled(false);
            emptyTasksAction->setStatusTip(QStringLiteral("No markdown files tagged with mterm were found in %1").arg(state->tasksDirectory));
            return;
        }

        for (const TaskFileEntry &taskFile : taskFiles) {
            QAction *taskAction = tasksMenu->addAction(taskFile.title);
            taskAction->setToolTip(taskFile.path);
            taskAction->setStatusTip(QStringLiteral("Run commands from %1").arg(taskFile.path));
            connect(taskAction, &QAction::triggered, this, [this, state, currentSession, taskFile]() {
                QStringList commandBlocks = readMarkdownCommandBlocks(taskFile.path);
                if (commandBlocks.isEmpty()) {
                    QMessageBox::warning(
                        this,
                        QStringLiteral("Task has no commands"),
                        QStringLiteral("No commands between ```bash and ``` blocks were found in:\n%1").arg(taskFile.path));
                    return;
                }

                TerminalSession *activeSession = currentSession();
                if (!activeSession) {
                    QMessageBox::warning(
                        this,
                        QStringLiteral("No active terminal"),
                        QStringLiteral("Open or focus a terminal before running a task."));
                    return;
                }

                const QList<TaskParameterEntry> parameters = readMarkdownTaskParameters(taskFile.path);
                QMap<QString, QString> parameterValues;
                if (!promptTaskParameterValues(this, parameters, &parameterValues)) {
                    return;
                }

                if (!parameterValues.isEmpty()) {
                    for (QString &commandBlock : commandBlocks) {
                        for (auto iterator = parameterValues.constBegin(); iterator != parameterValues.constEnd(); ++iterator) {
                            commandBlock.replace(iterator.key(), iterator.value());
                        }
                    }
                }

                QList<TerminalSession *> targetSessions{activeSession};
                QList<TerminalSession *> checkedSessions;
                for (TerminalSession *session : state->sessions) {
                    if (session->broadcastEnabled) {
                        checkedSessions.append(session);
                    }
                }

                if (checkedSessions.count() > 1) {
                    for (TerminalSession *session : checkedSessions) {
                        if (!targetSessions.contains(session)) {
                            targetSessions.append(session);
                        }
                    }
                }

                const QString commands = taskInjectionText(commandBlocks);
                for (TerminalSession *session : targetSessions) {
                    session->terminal->sendText(commands);
                }
            });
        }
    };
    (*rebuildTasksMenu)();

    const QString sshConfigPath = QDir::home().filePath(QStringLiteral(".ssh/config"));
    const QString sshDirectoryPath = QDir::home().filePath(QStringLiteral(".ssh"));
    auto *sshConfigWatcher = new QFileSystemWatcher(this);
    auto ensureSshConfigWatchPaths = [sshConfigWatcher, sshConfigPath, sshDirectoryPath]() {
        const QStringList watchedFiles = sshConfigWatcher->files();
        const QStringList watchedDirectories = sshConfigWatcher->directories();

        if (QFile::exists(sshConfigPath) && !watchedFiles.contains(sshConfigPath)) {
            sshConfigWatcher->addPath(sshConfigPath);
        }

        if (QDir(sshDirectoryPath).exists() && !watchedDirectories.contains(sshDirectoryPath)) {
            sshConfigWatcher->addPath(sshDirectoryPath);
        }
    };
    auto *sshConfigReloadTimer = new QTimer(this);
    sshConfigReloadTimer->setSingleShot(true);
    sshConfigReloadTimer->setInterval(100);

    connect(sshConfigReloadTimer, &QTimer::timeout, this, [rebuildHostsMenu, ensureSshConfigWatchPaths]() {
        ensureSshConfigWatchPaths();
        rebuildHostsMenu();
    });
    const auto scheduleSshConfigReload = [sshConfigReloadTimer]() {
        sshConfigReloadTimer->start();
    };
    connect(sshConfigWatcher, &QFileSystemWatcher::fileChanged, this, scheduleSshConfigReload);
    connect(sshConfigWatcher, &QFileSystemWatcher::directoryChanged, this, scheduleSshConfigReload);
    ensureSshConfigWatchPaths();

    auto *helpMenu = menuBar()->addMenu(QStringLiteral("Help"));
    helpMenu->menuAction()->setStatusTip(QStringLiteral("Help and application information"));
    const QList<HelpPageEntry> helpPages = readHelpPages();
    if (helpPages.isEmpty()) {
        auto *emptyHelpAction = helpMenu->addAction(QStringLiteral("No help pages found"));
        emptyHelpAction->setEnabled(false);
        emptyHelpAction->setStatusTip(QStringLiteral("No help resources were found"));
    } else {
        for (const HelpPageEntry &helpPage : helpPages) {
            QAction *helpAction = helpMenu->addAction(helpPage.title);
            helpAction->setStatusTip(QStringLiteral("Show %1 help").arg(helpPage.title));
            connect(helpAction, &QAction::triggered, this, [this, helpPage]() {
                HelpDialog(this, helpPage.title, helpPage.content).exec();
            });
        }
    }

    updateEmptyState();

    setWindowTitle(QStringLiteral("mTerm"));
    setCentralWidget(central);
    resize(800, 600);
}
