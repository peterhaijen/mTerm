#include "mainwindow.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
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
#include <QTabBar>
#include <QTabWidget>
#include <QTextStream>
#include <QThread>
#include <QVBoxLayout>
#include <QWidget>
#include <qtermwidget.h>

#include "version.h"

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
    ViewMode viewMode = ViewMode::Tabs;
    std::function<void()> renderCurrentView;
    std::function<void(TerminalSession *)> setActiveSession;
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
    auto *emptyLabel = new QLabel(QStringLiteral("No terminals open"), central);
    auto *tileHeaders = new QMap<TerminalSession *, QWidget *>;
    auto *tileTitles = new QMap<TerminalSession *, QLabel *>;

    emptyLabel->setAlignment(Qt::AlignCenter);
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
                auto *checkBox = new BroadcastCheckBox(session, tabs);
                checkBox->setAllChecked = [state, setAllBroadcast](bool checked) {
                    setAllBroadcast(checked);
                    state->renderCurrentView();
                };
                checkBox->focusSession = [focusSession, session]() {
                    focusSession(session);
                };
                tabs->tabBar()->setTabButton(tabIndex, QTabBar::LeftSide, checkBox);
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

    std::function<void(TerminalSession *)> closeSession = [state](TerminalSession *session) {
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
    connect(closeCurrentTabShortcut, &QShortcut::activated, this, [currentSession, closeSession]() {
        closeSession(currentSession());
    });

    QFont terminalFont = QApplication::font();
    terminalFont.setFamily(QStringLiteral("Monospace"));
    terminalFont.setPointSize(10);

    auto createTerminal = [this, state, terminalFont, closeSession, updateSessionTitle](const QString &tabName = QStringLiteral("Terminal"),
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

        connect(terminal, &QTermWidget::finished, this, [session, closeSession]() {
            closeSession(session);
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
    fileMenu->addAction(QStringLiteral("Exit"), this, &QWidget::close);

    auto *viewMenu = menuBar()->addMenu(QStringLiteral("View"));
    auto *tabsViewAction = viewMenu->addAction(QStringLiteral("Tabs"));
    tabsViewAction->setCheckable(true);
    tabsViewAction->setChecked(true);
    auto *tileViewAction = viewMenu->addAction(QStringLiteral("Tile All"));
    tileViewAction->setCheckable(true);

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

    auto *hostsMenu = menuBar()->addMenu(QStringLiteral("Hosts"));
    const QList<SshHostEntry> hosts = readSshConfigHosts();

    QMap<QString, QMenu *> groupMenus;
    QMap<QString, QStringList> groupHosts;
    for (const SshHostEntry &entry : hosts) {
        for (const QString &group : entry.groups) {
            groupHosts[group].append(entry.host);
        }
    }

    auto openHost = [createTerminal](const QString &host) {
        createTerminal(host, QStringLiteral("ssh"), QStringList{host});
    };

    hostsMenu->addAction(QStringLiteral("localhost"), this, [createTerminal]() {
        createTerminal(QStringLiteral("localhost"));
    });

    if (!hosts.isEmpty()) {
        hostsMenu->addSeparator();
    }

    auto addHostAction = [this, openHost](QMenu *menu, const QString &host) {
        menu->addAction(host, this, [host, openHost]() {
            openHost(host);
        });
    };

    for (const SshHostEntry &entry : hosts) {
        if (entry.groups.isEmpty()) {
            addHostAction(hostsMenu, entry.host);
            continue;
        }

        for (const QString &entryGroup : entry.groups) {
            QMenu *groupMenu = groupMenus.value(entryGroup, nullptr);
            if (!groupMenu) {
                groupMenu = hostsMenu->addMenu(entryGroup);
                groupMenus.insert(entryGroup, groupMenu);

                const QString group = entryGroup;
                auto openGroup = [group, groupHosts, openHost]() {
                    const QStringList hosts = groupHosts.value(group);
                    for (const QString &host : hosts) {
                        openHost(host);
                    }
                };

                groupMenu->addAction(QStringLiteral("Open All"), this, openGroup);
                groupMenu->addSeparator();
            }

            addHostAction(groupMenu, entry.host);
        }
    }

    auto *helpMenu = menuBar()->addMenu(QStringLiteral("Help"));
    helpMenu->addAction(QStringLiteral("About"), this, [this]() {
        QMessageBox::about(
            this,
            QStringLiteral("About mTerm"),
            QStringLiteral("<h3>mTerm</h3>"
                           "<p>Version: %1</p>"
                           "<p>A Qt/QTermWidget terminal broadcaster for running the same commands across multiple terminal sessions, including SSH connections.</p>")
                .arg(QStringLiteral(MTERM_VERSION)));
    });

    helpMenu->addAction(QStringLiteral("Shortcuts"), this, [this]() {
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

    updateEmptyState();

    setWindowTitle(QStringLiteral("mTerm"));
    setCentralWidget(central);
    resize(800, 600);
}
