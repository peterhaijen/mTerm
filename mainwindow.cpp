#include "mainwindow.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMap>
#include <QMessageBox>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSet>
#include <QShortcut>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QVector>
#include <QVBoxLayout>
#include <QWidget>
#include <qtermwidget.h>

#include "version.h"

#include <algorithm>
#include <cerrno>
#include <functional>
#include <signal.h>

struct TerminalSession
{
    QTermWidget *terminal = nullptr;
    QString title;
    bool broadcastEnabled = true;
    bool hasFocus = false;
    bool isPrompt = false;
};

enum class ViewMode { Tabs, Tile };

struct TerminalUiState
{
    QList<TerminalSession *> sessions;
    ViewMode viewMode = ViewMode::Tile;
    std::function<void()> renderCurrentView;
    std::function<void(TerminalSession *)> setActiveSession;
    std::function<void(TerminalSession *)> closeSession;
};

static TerminalSession *sessionForTerminal(TerminalUiState *state, QTermWidget *terminal);

class BroadcastCheckBox : public QCheckBox
{
public:
    explicit BroadcastCheckBox(TerminalSession *session, QWidget *parent = nullptr)
        : QCheckBox(parent)
        , session(session)
    {
        setChecked(session->broadcastEnabled);
        setToolTip(QStringLiteral("Receive broadcast input"));
    }

    std::function<void(bool)> setAllChecked;
    std::function<void()> focusSession;

protected:
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        QCheckBox::mouseReleaseEvent(event);
        session->broadcastEnabled = isChecked();

        if ((event->modifiers() & Qt::ControlModifier) && setAllChecked) {
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

static QList<TaskFileEntry> readVaultTaskFiles()
{
    const QString vaultPath = QDir::home().filePath(QStringLiteral("vault33"));
    QDir vaultDirectory(vaultPath);
    if (!vaultDirectory.exists()) {
        return {};
    }

    QList<TaskFileEntry> entries;
    QDirIterator iterator(vaultPath,
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

static QStringList readMarkdownCommandBlocks(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    QStringList commandBlocks;
    QStringList currentBlock;
    bool inCodeBlock = false;

    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine();
        if (line.trimmed().startsWith(QStringLiteral("```"))) {
            if (inCodeBlock) {
                commandBlocks.append(currentBlock.join(QLatin1Char('\n')));
                currentBlock.clear();
                inCodeBlock = false;
            } else {
                inCodeBlock = true;
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

static QString sshLauncherScript(const QString &host)
{
    const QString quotedHost = shellSingleQuote(host);
    return QStringLiteral("ssh ")
        + quotedHost
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

    auto *state = new TerminalUiState;
    qApp->installEventFilter(new TerminalFocusFilter(state, this));
    connect(qApp, &QCoreApplication::aboutToQuit, this, [state]() {
        for (TerminalSession *session : state->sessions) {
            if (session->terminal) {
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
        stopTerminalProcess(session->terminal);
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

    auto *nextTabShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Right), this);
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

    auto *previousTabShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Left), this);
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

    auto *toggleCurrentBroadcastShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Space), this);
    connect(toggleCurrentBroadcastShortcut, &QShortcut::activated, this, [currentSession, state]() {
        TerminalSession *session = currentSession();
        if (session) {
            session->broadcastEnabled = !session->broadcastEnabled;
            state->renderCurrentView();
        }
    });

    auto *closeCurrentTabShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Delete), this);
    connect(closeCurrentTabShortcut, &QShortcut::activated, this, [currentSession, state]() {
        if (state->closeSession) {
            state->closeSession(currentSession());
        }
    });

    QFont terminalFont = QApplication::font();
    terminalFont.setFamily(QStringLiteral("Monospace"));
    terminalFont.setPointSize(10);

    auto createTerminal = [this, state, terminalFont, updateSessionTitle](const QString &tabName = QStringLiteral("Terminal"),
                                                                                       const QString &program = QString(),
                                                                                       const QStringList &args = QStringList()) {
        const bool useCustomProgram = !program.isEmpty();
        auto *terminal = new QTermWidget(useCustomProgram ? 0 : 1, this);
        auto *session = new TerminalSession{terminal, tabName, true, true};
        for (TerminalSession *otherSession : state->sessions) {
            otherSession->hasFocus = false;
        }
        state->sessions.append(session);

        QFont font = terminalFont;
        terminal->setTerminalFont(font);
        terminal->setColorScheme(QStringLiteral("WhiteOnBlack"));
        terminal->setScrollBarPosition(QTermWidget::ScrollBarRight);
        terminal->setAutoClose(true);

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
            terminal->setShellProgram(program);
            terminal->setArgs(args);
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
    tabsViewAction->setChecked(false);
    tabsViewAction->setStatusTip(QStringLiteral("Show one terminal per tab"));
    auto *tileViewAction = viewMenu->addAction(QStringLiteral("Tile All"));
    tileViewAction->setCheckable(true);
    tileViewAction->setChecked(true);
    tileViewAction->setStatusTip(QStringLiteral("Show all terminals in a tiled layout"));

    connect(tabsViewAction, &QAction::triggered, this, [state, tabsViewAction, tileViewAction]() {
        state->viewMode = ViewMode::Tabs;
        tabsViewAction->setChecked(true);
        tileViewAction->setChecked(false);
        state->renderCurrentView();
    });

    connect(tileViewAction, &QAction::triggered, this, [state, tabsViewAction, tileViewAction]() {
        state->viewMode = ViewMode::Tile;
        tabsViewAction->setChecked(false);
        tileViewAction->setChecked(true);
        state->renderCurrentView();
    });

    auto openHost = [createTerminal](const QString &host) {
        createTerminal(host, QStringLiteral("/bin/bash"), QStringList{QStringLiteral("-lc"), sshLauncherScript(host)});
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
    tasksMenu->menuAction()->setStatusTip(QStringLiteral("Run mterm tasks from ~/vault33"));
    const QList<TaskFileEntry> taskFiles = readVaultTaskFiles();
    if (taskFiles.isEmpty()) {
        auto *emptyTasksAction = tasksMenu->addAction(QStringLiteral("No mterm tasks found"));
        emptyTasksAction->setEnabled(false);
        emptyTasksAction->setStatusTip(QStringLiteral("No markdown files tagged with mterm were found in ~/vault33"));
    } else {
        for (const TaskFileEntry &taskFile : taskFiles) {
            QAction *taskAction = tasksMenu->addAction(taskFile.title);
            taskAction->setToolTip(taskFile.path);
            taskAction->setStatusTip(QStringLiteral("Run commands from %1").arg(taskFile.path));
            connect(taskAction, &QAction::triggered, this, [this, state, currentSession, taskFile]() {
                const QStringList commandBlocks = readMarkdownCommandBlocks(taskFile.path);
                if (commandBlocks.isEmpty()) {
                    QMessageBox::warning(
                        this,
                        QStringLiteral("Task has no commands"),
                        QStringLiteral("No commands between ``` blocks were found in:\n%1").arg(taskFile.path));
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
    }

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
    auto *aboutAction = helpMenu->addAction(QStringLiteral("About"), this, [this]() {
        QMessageBox::about(
            this,
            QStringLiteral("About mTerm"),
            QStringLiteral("<h3>mTerm</h3>"
                           "<p>Version: %1</p>"
                           "<p>A Qt/QTermWidget terminal broadcaster for running the same commands across multiple terminal sessions, including SSH connections.</p>")
                .arg(QStringLiteral(MTERM_VERSION)));
    });
    aboutAction->setStatusTip(QStringLiteral("Show mTerm version and application information"));

    auto *addingHostsAction = helpMenu->addAction(QStringLiteral("Adding Hosts"), this, [this]() {
        QMessageBox::information(
            this,
            QStringLiteral("Adding Hosts"),
            QStringLiteral(
                "mTerm reads hosts from ~/.ssh/config.\n"
                "\n"
                "Only Host blocks marked with '# mTerm' are added to the Hosts menu.\n"
                "\n"
                "Example:\n"
                "Host webserver\n"
                "  # mTerm Groups Webserver\n"
                "  Hostname 10.0.0.11\n"
                "\n"
                "To place a host in multiple groups, add a line like:\n"
                "  # mTerm Groups VPS Webserver\n"
                "\n"
                "Groups are used to build nested submenus under Hosts.\n"
                "Wildcard entries (like 'Host *') are ignored."));
    });
    addingHostsAction->setStatusTip(QStringLiteral("Show how to add SSH hosts to the Hosts menu"));

    auto *shortcutsAction = helpMenu->addAction(QStringLiteral("Shortcuts"), this, [this]() {
        QMessageBox::information(
            this,
            QStringLiteral("Shortcuts"),
            QStringLiteral(
                "Ctrl+Right: next tab (tabs view) or next terminal (tile view)\n"
                "Ctrl+Left: previous tab (tabs view) or previous terminal (tile view)\n"
                "Ctrl+Space: toggle broadcast checkbox for the current terminal\n"
                "Ctrl+Delete: close current terminal\n"
                "Click terminal checkbox: toggle broadcast for that terminal\n"
                "Ctrl+Click terminal checkbox: set all checkboxes to the same state"));
    });
    shortcutsAction->setStatusTip(QStringLiteral("Show keyboard and mouse shortcuts"));

    updateEmptyState();

    setWindowTitle(QStringLiteral("mTerm"));
    setCentralWidget(central);
    resize(800, 600);
}
