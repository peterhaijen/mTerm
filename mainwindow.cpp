#include "mainwindow.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMap>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QSet>
#include <QShortcut>
#include <QTabBar>
#include <QTabWidget>
#include <QTextStream>
#include <QVBoxLayout>
#include <QWidget>
#include <qtermwidget.h>

static QCheckBox *broadcastCheckBox(QTabWidget *tabs, int index);

class BroadcastCheckBox : public QCheckBox
{
public:
    explicit BroadcastCheckBox(QTabWidget *tabs)
        : QCheckBox(tabs)
        , tabs(tabs)
    {
    }

protected:
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        QCheckBox::mouseReleaseEvent(event);

        if (!(event->modifiers() & Qt::ControlModifier)) {
            focusTabWhenChecked();
            return;
        }

        for (int index = 0; index < tabs->count(); ++index) {
            auto *checkBox = broadcastCheckBox(tabs, index);
            if (checkBox) {
                checkBox->setChecked(isChecked());
            }
        }

        focusTabWhenChecked();
    }

private:
    void focusTabWhenChecked()
    {
        if (!isChecked()) {
            return;
        }

        for (int index = 0; index < tabs->count(); ++index) {
            if (broadcastCheckBox(tabs, index) == this) {
                tabs->setCurrentIndex(index);
                auto *widget = tabs->widget(index);
                if (widget) {
                    widget->setFocus();
                }
                return;
            }
        }
    }

    QTabWidget *tabs = nullptr;
};

struct SshHostEntry
{
    QString host;
    QString group;
};

static bool isConcreteSshHost(const QString &host)
{
    return !host.startsWith(QLatin1Char('!'))
        && !host.contains(QLatin1Char('*'))
        && !host.contains(QLatin1Char('?'));
}

static void addSshHostEntries(const QStringList &hosts,
                              const QString &group,
                              QSet<QString> *seen,
                              QList<SshHostEntry> *entries)
{
    for (const QString &host : hosts) {
        if (!isConcreteSshHost(host) || seen->contains(host)) {
            continue;
        }

        seen->insert(host);
        entries->append({host, group});
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
    QString currentGroup;
    QSet<QString> seen;
    QTextStream stream(&config);
    while (!stream.atEnd()) {
        QString line = stream.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }

        if (line.startsWith(QStringLiteral("#mGroup"))) {
            currentGroup = line.mid(QStringLiteral("#mGroup").size()).trimmed();
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

        addSshHostEntries(currentHosts, currentGroup, &seen, &entries);
        currentHosts = parts.mid(1);
        currentGroup.clear();
    }

    addSshHostEntries(currentHosts, currentGroup, &seen, &entries);

    return entries;
}

static QCheckBox *broadcastCheckBox(QTabWidget *tabs, int index)
{
    return qobject_cast<QCheckBox *>(tabs->tabBar()->tabButton(index, QTabBar::LeftSide));
}

static bool shouldReceiveBroadcast(QTabWidget *tabs, int index)
{
    auto *checkBox = broadcastCheckBox(tabs, index);
    return checkBox && checkBox->isChecked();
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    auto *tabs = new QTabWidget(central);
    auto *emptyLabel = new QLabel(QStringLiteral("No terminals open"), central);
    emptyLabel->setAlignment(Qt::AlignCenter);

    auto updateEmptyState = [tabs, emptyLabel]() {
        const bool hasTabs = tabs->count() > 0;
        tabs->setVisible(hasTabs);
        emptyLabel->setVisible(!hasTabs);
    };

    auto *nextTabShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Right), this);
    connect(nextTabShortcut, &QShortcut::activated, this, [tabs]() {
        if (tabs->count() > 0) {
            tabs->setCurrentIndex((tabs->currentIndex() + 1) % tabs->count());
        }
    });

    auto *previousTabShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Left), this);
    connect(previousTabShortcut, &QShortcut::activated, this, [tabs]() {
        if (tabs->count() > 0) {
            tabs->setCurrentIndex((tabs->currentIndex() - 1 + tabs->count()) % tabs->count());
        }
    });

    auto *toggleCurrentBroadcastShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Space), this);
    connect(toggleCurrentBroadcastShortcut, &QShortcut::activated, this, [tabs]() {
        auto *checkBox = broadcastCheckBox(tabs, tabs->currentIndex());
        if (checkBox) {
            checkBox->setChecked(!checkBox->isChecked());
        }
    });

    auto *closeCurrentTabShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Delete), this);
    connect(closeCurrentTabShortcut, &QShortcut::activated, this, [tabs, updateEmptyState]() {
        const int index = tabs->currentIndex();
        if (index == -1) {
            return;
        }

        QWidget *widget = tabs->widget(index);
        tabs->removeTab(index);
        updateEmptyState();
        if (widget) {
            widget->deleteLater();
        }
    });

    QFont terminalFont = QApplication::font();
    terminalFont.setFamily(QStringLiteral("Monospace"));
    terminalFont.setPointSize(10);

    auto createTerminal = [this, tabs, terminalFont, updateEmptyState](const QString &tabName = QStringLiteral("Terminal"),
                                                                      const QString &program = QString(),
                                                                      const QStringList &args = QStringList()) {
        const bool useCustomProgram = !program.isEmpty();
        auto *terminal = new QTermWidget(useCustomProgram ? 0 : 1, tabs);
        QFont font = terminalFont;
        terminal->setTerminalFont(font);
        terminal->setColorScheme(QStringLiteral("WhiteOnBlack"));
        terminal->setScrollBarPosition(QTermWidget::ScrollBarRight);
        terminal->setAutoClose(true);

        const int tabIndex = tabs->addTab(terminal, tabName);
        auto *broadcastCheckBox = new BroadcastCheckBox(tabs);
        broadcastCheckBox->setChecked(true);
        broadcastCheckBox->setToolTip(QStringLiteral("Receive broadcast input"));
        tabs->tabBar()->setTabButton(tabIndex, QTabBar::LeftSide, broadcastCheckBox);
        tabs->setCurrentIndex(tabIndex);
        updateEmptyState();

        connect(terminal, &QTermWidget::finished, this, [tabs, terminal, updateEmptyState]() {
            const int tabIndex = tabs->indexOf(terminal);
            if (tabIndex != -1) {
                tabs->removeTab(tabIndex);
            }
            updateEmptyState();
            terminal->deleteLater();
        });

        connect(terminal, &QTermWidget::termKeyPressed, this, [tabs, terminal](QKeyEvent *event) {
            static bool broadcasting = false;
            if (broadcasting) {
                return;
            }

            const int sourceIndex = tabs->indexOf(terminal);
            if (sourceIndex == -1 || !shouldReceiveBroadcast(tabs, sourceIndex)) {
                return;
            }

            broadcasting = true;
            for (int index = 0; index < tabs->count(); ++index) {
                auto *targetTerminal = qobject_cast<QTermWidget *>(tabs->widget(index));
                if (!targetTerminal || targetTerminal == terminal || !shouldReceiveBroadcast(tabs, index)) {
                    continue;
                }

                QKeyEvent forwardedEvent(event->type(),
                                         event->key(),
                                         event->modifiers(),
                                         event->text(),
                                         event->isAutoRepeat(),
                                         event->count());
                targetTerminal->sendKeyEvent(&forwardedEvent);
            }
            broadcasting = false;
        });

        if (useCustomProgram) {
            terminal->setShellProgram(program);
            terminal->setArgs(args);
            terminal->startShellProgram();
        }

        return terminal;
    };

    auto *terminalMenu = menuBar()->addMenu(QStringLiteral("Terminal"));
    terminalMenu->addAction(QStringLiteral("New"), this, [createTerminal]() {
        createTerminal();
    });

    auto *hostsMenu = menuBar()->addMenu(QStringLiteral("Hosts"));
    const QList<SshHostEntry> hosts = readSshConfigHosts();
    if (hosts.isEmpty()) {
        QAction *emptyAction = hostsMenu->addAction(QStringLiteral("No hosts found"));
        emptyAction->setEnabled(false);
    }

    QMap<QString, QMenu *> groupMenus;
    QMap<QString, QStringList> groupHosts;
    for (const SshHostEntry &entry : hosts) {
        if (!entry.group.isEmpty()) {
            groupHosts[entry.group].append(entry.host);
        }
    }

    auto openHost = [createTerminal](const QString &host) {
        createTerminal(host, QStringLiteral("ssh"), QStringList{host});
    };

    auto addHostAction = [this, openHost](QMenu *menu, const QString &host) {
        menu->addAction(host, this, [host, openHost]() {
            openHost(host);
        });
    };

    for (const SshHostEntry &entry : hosts) {
        if (entry.group.isEmpty()) {
            addHostAction(hostsMenu, entry.host);
            continue;
        }

        QMenu *groupMenu = groupMenus.value(entry.group, nullptr);
        if (!groupMenu) {
            groupMenu = hostsMenu->addMenu(entry.group);
            groupMenus.insert(entry.group, groupMenu);

            const QString group = entry.group;
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

    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(tabs);
    layout->addWidget(emptyLabel);
    updateEmptyState();

    setWindowTitle(QStringLiteral("mTerm"));
    setCentralWidget(central);
    resize(800, 600);
}
