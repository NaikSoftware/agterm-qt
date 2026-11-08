#include "mainwindow.h"

#include "filepanel.h"
#include "help.h"
#include "hooks.h"
#include "launcher.h"
#include "library.h"
#include "livehost.h"
#include "palette.h"
#include "restore.h"
#include "paths.h"
#include "sidebar.h"
#include "store.h"

#include <KActionCollection>
#include <KNotification>
#include <KParts/PartLoader>
#include <KParts/ReadOnlyPart>
#include <kde_terminal_interface.h>

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDBusConnection>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QSet>
#include <QPushButton>
#include <QSaveFile>
#include <QSplitter>
#include <QScreen>
#include <QSplitterHandle>
#include <QStackedLayout>
#include <QStackedWidget>
#include <QWheelEvent>
#include <QWindow>
#include <QUuid>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

// A live pane's shell runs under its daemon, behind the attach client Konsole started; Konsole sees only the client.
// A very light wash of the shown session's background tint over the terminal, so panels tell apart by colour.
// Not in the stack's layout: it covers the whole stack and lets every mouse event through to the terminal.
class TintWash : public QWidget
{
public:
    explicit TintWash(QWidget *over)
        : QWidget(over)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
        over->installEventFilter(this);
        hide();
    }
    void setColor(QColor c)
    {
        if (c.isValid())
            c.setAlpha(7);
        if (c != m_color)
            update();
        m_color = c;
        setVisible(c.isValid());
        raise(); // above a page the stack just added or showed
    }

protected:
    bool eventFilter(QObject *, QEvent *e) override
    {
        if (e->type() == QEvent::Resize)
            setGeometry(parentWidget()->rect());
        return false;
    }
    void paintEvent(QPaintEvent *) override { QPainter(this).fillRect(rect(), m_color); }

private:
    QColor m_color;
};

static int shellPid(KParts::ReadOnlyPart *part)
{
    auto *t = qobject_cast<TerminalInterface *>(part);
    if (!t)
        return -1;
    const QString live = part->property("agtermLive").toString();
    return live.isEmpty() ? t->terminalProcessId() : Live::shellPid(live);
}

// Konsole's foregroundProcessId lags a command starting; the shell's tty says who is in front right now
static int freshForegroundPid(KParts::ReadOnlyPart *part)
{
    return Live::foregroundPid(shellPid(part));
}

static int foregroundPid(KParts::ReadOnlyPart *part)
{
    auto *t = qobject_cast<TerminalInterface *>(part);
    if (!t)
        return -1;
    const QString live = part->property("agtermLive").toString();
    return live.isEmpty() ? t->foregroundProcessId() : Live::foregroundPid(Live::shellPid(live));
}

QWidget *terminalDisplay(KParts::ReadOnlyPart *part)
{
    if (part)
        for (QWidget *w : part->widget()->findChildren<QWidget *>())
            if (w->inherits("Konsole::TerminalDisplay"))
                return w;
    return nullptr;
}

// The part exposes no font API and its zoom actions live only in Konsole's main window, so this drives
// the display's own Ctrl+wheel zoom, one point per notch.
// shortcut: a profile with MouseWheelZoomEnabled=false makes this a no-op; upgrade if the part grows a font API
void zoomFontTo(QWidget *display, double points)
{
    for (int guard = 0; display && guard < 200; ++guard) {
        const double now = display->font().pointSizeF();
        if (qAbs(now - points) < 0.5)
            return;
        const QPointF at(4, 4);
        QWheelEvent wheel(at, display->mapToGlobal(at), {}, QPoint(0, now < points ? 120 : -120), Qt::NoButton, Qt::ControlModifier,
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(display, &wheel);
        if (display->font().pointSizeF() == now)
            return; // at Konsole's limit, or zoom is off in the profile
    }
}

PartBridge::PartBridge(MainWindow *window, Store &store, const QString &id, QObject *parent)
    : QObject(parent), m_window(window), m_store(store), m_id(id)
{
}

void PartBridge::currentDirectoryChanged(const QString &dir)
{
    m_store.setCwd(m_id, dir);
}

void PartBridge::overrideShortcut(QKeyEvent *event, bool &override)
{
    // Konsole's part presets override = true (the terminal keeps every key); false hands ours to the window
    if (m_window->isOwnShortcut(event))
        override = false;
}

MainWindow::MainWindow(Store &store, const QString &statePath)
    : m_store(store), m_statePath(statePath)
{
    m_tree = new Sidebar(store);

    // footer: new session, new workspace, and the flagged view toggle, as agterm's sidebar bar
    auto footerButton = [](const QString &icon, const QString &tip) {
        auto *b = new QToolButton;
        b->setIcon(QIcon::fromTheme(icon));
        b->setToolTip(tip);
        b->setAccessibleName(tip);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        return b;
    };
    m_events = new EventSource(store, this);
    m_hooks = new HookRunner(this);
    m_hooks->socket = socketPath();
    m_hooks->pathPrefix = QCoreApplication::applicationDirPath();
    connect(m_events, &EventSource::event, m_hooks, &HookRunner::fire);
    connect(m_hooks, &HookRunner::failed, this, [this](const QString &hook, const QString &message) { notifyFailure(tr("Hook failed"), hook + QStringLiteral(": ") + message); });
    auto *newSessionButton = footerButton(QStringLiteral("tab-new"), tr("New Session"));
    connect(newSessionButton, &QToolButton::clicked, this, [this] { newSession(currentWorkspace()); });
    auto *newWorkspaceButton = footerButton(QStringLiteral("list-add"), tr("New Workspace"));
    connect(newWorkspaceButton, &QToolButton::clicked, this, &MainWindow::newWorkspace);
    m_flagButton = footerButton(QStringLiteral("flag"), tr("Flagged Sessions"));
    m_flagButton->setCheckable(true);
    connect(m_flagButton, &QToolButton::toggled, this, [this](bool on) {
        m_tree->setFlaggedView(on);
        m_actions.value(QStringLiteral("toggle_flagged_view"))->setChecked(on);
        m_saveTimer.start();
    });
    auto *footer = new QHBoxLayout;
    footer->setContentsMargins(8, 4, 8, 6);
    footer->setSpacing(2);
    footer->addWidget(newSessionButton);
    footer->addWidget(newWorkspaceButton);
    footer->addStretch(1);
    m_filterButton = footerButton({}, tr("Toggle Workspace Filter"));
    connect(m_filterButton, &QToolButton::clicked, this, [this] { m_store.setFilter(!m_store.focusEnabled); });
    footer->addWidget(m_filterButton);
    footer->addWidget(m_flagButton);
    m_footer = {{QStringLiteral("newSession"), newSessionButton}, {QStringLiteral("newWorkspace"), newWorkspaceButton},
                {QStringLiteral("focusFilter"), m_filterButton}, {QStringLiteral("flaggedView"), m_flagButton}};

    m_sidebarPanel = new QWidget;
    auto *side = new QVBoxLayout(m_sidebarPanel);
    side->setContentsMargins(0, 6, 0, 0);
    side->setSpacing(0);
    side->addWidget(m_tree, 1);
    side->addLayout(footer);

    m_stack = new QStackedWidget;
    m_empty = new QWidget;
    m_stack->addWidget(m_empty);
    m_wash = new TintWash(m_stack);

    m_split = new QSplitter;
    m_split->setChildrenCollapsible(false);
    m_filePanel = new FilePanel;
    m_filePanel->setMinimumWidth(160);
    m_filePanel->setMaximumWidth(560);
    m_filePanel->hide();
    connect(m_filePanel, &FilePanel::companionRequested, this, [this](const QString &dir) {
        if (m_store.session(m_store.selected))
            m_store.select(m_store.addCompanion(m_store.selected, {}, dir));
    });
    m_split->addWidget(m_sidebarPanel);
    m_split->addWidget(m_filePanel); // agterm's file panel sits right of the sidebar
    m_split->addWidget(m_stack);
    m_split->setStretchFactor(2, 1);
    m_split->setSizes({230, 240, 970});
    connect(m_split, &QSplitter::splitterMoved, this, [this] {
        if (!m_filePanel->isHidden() && m_split->sizes().value(1) > 0) {
            m_filePanelWidth = m_split->sizes().value(1); // a dragged divider is the new width
            m_saveTimer.start();
        }
    });
    m_sidebarPanel->setMinimumWidth(160);
    setCentralWidget(m_split);
    resize(1200, 760);
    // messages only: an idle status bar would be chrome agterm doesn't have
    statusBar()->hide();
    connect(statusBar(), &QStatusBar::messageChanged, statusBar(), [this](const QString &m) { statusBar()->setVisible(!m.isEmpty()); });

    createMenus();

    // title strip: sidebar toggle and the session name, as agterm's title bar
    auto *strip = new QToolBar;
    strip->setObjectName(QStringLiteral("titleStrip"));
    strip->setMovable(false);
    strip->setFloatable(false);
    strip->setIconSize(QSize(16, 16));
    strip->toggleViewAction()->setVisible(false);
    // a plain button, not the checkable menu action: agterm's toggle never renders pressed
    const QColor ink = palette().color(QPalette::WindowText);
    m_chrome.insert(QStringLiteral("filePanel"), strip->addAction(chromeIcon(QStringLiteral("filepanel"), ink), tr("Toggle File Panel"),
                                                                  m_actions.value(QStringLiteral("toggle_file_panel")), &QAction::trigger));
    m_chrome.insert(QStringLiteral("sidebarToggle"), strip->addAction(chromeIcon(QStringLiteral("sidebar"), ink), tr("Toggle Sidebar"), m_sidebarAction, &QAction::trigger));
    m_workspaceLabel = new QLabel;
    m_workspaceLabel->setContentsMargins(6, 0, 0, 0);
    m_workspaceLabel->setForegroundRole(QPalette::PlaceholderText);
    m_chrome.insert(QStringLiteral("workspaceName"), strip->addWidget(m_workspaceLabel));
    m_title = new QLabel;
    QFont tf = m_title->font();
    tf.setWeight(QFont::DemiBold);
    m_title->setFont(tf);
    m_title->setContentsMargins(6, 0, 0, 0);
    m_chrome.insert(QStringLiteral("sessionName"), strip->addWidget(m_title));
    m_context = new QLabel;
    m_context->setContentsMargins(10, 0, 0, 0);
    m_context->setForegroundRole(QPalette::PlaceholderText);
    m_chrome.insert(QStringLiteral("sessionContext"), strip->addWidget(m_context));
    auto *spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    strip->addWidget(spacer);
    // the mouse form of the Ctrl-Tab switcher, dimmed while there is nowhere else to go
    m_recentAction = strip->addAction(chromeIcon(QStringLiteral("clock"), ink), tr("Recent sessions (Ctrl+Tab)"), this, &MainWindow::recentMenu);
    m_recentAction->setEnabled(false);
    m_chrome.insert(QStringLiteral("recentSessions"), m_recentAction);
    m_chrome.insert(QStringLiteral("customCommands"), strip->addAction(QIcon::fromTheme(QStringLiteral("overflow-menu")), tr("Custom Commands"),
                                                                       this, &MainWindow::customCommandPalette));
    m_chrome.insert(QStringLiteral("split"),
                    strip->addAction(chromeIcon(QStringLiteral("split"), ink), tr("Split"), m_actions.value(QStringLiteral("toggle_split")), &QAction::trigger));
    m_chrome.insert(QStringLiteral("dashboard"), strip->addAction(chromeIcon(QStringLiteral("grid"), ink), tr("Dashboard"), m_actions.value(QStringLiteral("dashboard")),
                                                                  &QAction::trigger));
    addToolBar(Qt::TopToolBarArea, strip);
    m_strip = strip;

    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(500);
    connect(&m_saveTimer, &QTimer::timeout, this, &MainWindow::save);
    m_agentTimer.setInterval(2000);
    connect(&m_agentTimer, &QTimer::timeout, this, &MainWindow::sampleAgents);
    m_agentTimer.start();
    m_dashBlink.setInterval(600);
    connect(&m_dashBlink, &QTimer::timeout, this, [this] {
        m_dashBlinkOn = !m_dashBlinkOn;
        refreshDashboard();
    });

    connect(&m_store, &Store::changed, this, [this] {
        syncChrome();
        m_tree->rebuild();
        showSelected();
        m_saveTimer.start();
    });
    connect(&m_store, &Store::titleChanged, this, [this] {
        m_tree->rebuild();
        showSelected();
    });
    connect(&m_store, &Store::sessionAdded, this, &MainWindow::createTerminal);
    connect(&m_store, &Store::sessionRemoved, this, [this](const QString &id) {
        m_sessions.remove(id);
        forgetOverlays(id);
        dashboardForget(id); // its panes go back home first, so they die with the view
        // taken first, so the parts dying with their view are not mistaken for shells that exited
        if (Panes p = m_panes.take(id); p.view)
            p.view->deleteLater(); // closes the ptys, which hangs up the shells
        for (const char *pane : {"main", "right"}) // a deleted session's daemons go with it; a closed window's stay
            if (const QString live = Live::socket(stateDir(), id, QLatin1String(pane)); QFile::exists(live))
                Live::kill(live);
    });
    connect(&m_store, &Store::selectionChanged, this, &MainWindow::showSelected);
    connect(&m_store, &Store::paneLayoutChanged, this, &MainWindow::applyLayout);
    connect(&m_store, &Store::notified, this, [this](const QString &id, const QString &title, const QString &body) {
        const Session *s = m_store.session(id);
        if (!s || (isActiveWindow() && id == m_store.selected))
            return; // already in front of you
        m_store.setUnseen(id, s->unseen + 1); // counted whatever the badge setting: tree read-back keeps it
        const Settings &st = m_store.settings;
        if (st.dockBounce != QLatin1String("off"))
            QApplication::alert(this, st.dockBounce == QLatin1String("once") ? 1500 : 0);
        if (!st.notificationsEnabled)
            return;
        playSound(st.notificationSoundName);
        auto *n = new KNotification(QStringLiteral("notification"), KNotification::CloseOnTimeout);
        n->setComponentName(QStringLiteral("plasma_workspace")); // KNotification::event's standard notification, without a notifyrc of our own
        n->setTitle(title.isEmpty() ? s->displayName() : title);
        n->setText(body);
        n->setIconName(QStringLiteral("agterm-qt"));
        connect(n->addDefaultAction(tr("Show")), &KNotificationAction::activated, this, [this, id] {
            m_store.select(id); // the banner brings the window forward on the session that raised it
            show();
            raise();
            activateWindow();
        });
        n->sendEvent();
    });
    connect(&m_store, &Store::swapRequested, this, &MainWindow::swapPanes);
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (item && item->data(0, SidebarRole::Kind).toInt() == SidebarRole::SessionRow)
            m_store.select(item->data(0, SidebarRole::Id).toString());
    });
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &MainWindow::sidebarMenu);
    connect(m_tree, &Sidebar::launcherMenuRequested, this, &MainWindow::launcherMenu);
    connect(m_tree, &Sidebar::repeatRequested, this, [this](const QString &ws) { launch(m_store.settings.lastLauncher, ws); });
    connect(m_tree, &Sidebar::renameRequested, this, [this](const QString &kind, const QString &id, const QString &name) {
        kind == QLatin1String("session") ? m_store.renameSession(id, name) : m_store.renameWorkspace(id, name);
        m_tree->rebuild();
    });
    connect(m_tree, &Sidebar::foldersDropped, this, [this](const QString &ws, const QStringList &dirs) {
        const QString target = m_store.workspace(ws) ? ws : currentWorkspace();
        QString last;
        for (const auto &d : dirs)
            last = m_store.addSession(target, {}, d);
        m_store.select(last);
    });
    connect(&m_store, &Store::settingsChanged, this, &MainWindow::applySettings);
    m_idle.start();
    m_followTimer.setInterval(1000);
    connect(&m_followTimer, &QTimer::timeout, this, [this] { autoFollowTick(m_idle.elapsed()); });
    m_followTimer.start();
    connect(m_tree, &Sidebar::editFlaggedRequested, this, &MainWindow::editFlaggedSessions);
    connect(m_tree, &Sidebar::closeRequested, this, [this](const QString &id) { closeSessions({id}); });
    connect(&m_store, &Store::selectionChanged, this, [this](const QString &id) {
        if (const Session *s = m_store.session(id); s && s->status == QLatin1String("blocked"))
            m_followShown.insert(id);
    });
    connect(&m_store, &Store::statusChanged, this, [this](const QString &id, const QString &status) {
        trackBlocked(id, status);
        if (status == QLatin1String("blocked"))
            playSound(m_store.settings.blockedStatusSoundName);
    });
    for (auto it = m_actions.begin(); it != m_actions.end(); ++it) {
        QStringList keys;
        for (const QKeySequence &k : it.value()->shortcuts())
            keys << k.toString(QKeySequence::PortableText);
        m_defaultKeys.insert(it.key(), keys);
    }
    qApp->installEventFilter(this);
    detectLaunchers();
    applySettings();
}

QAction *MainWindow::action(const QString &name, const QString &text, const QString &icon, const QKeySequence &key, std::function<void()> slot)
{
    auto *a = new QAction(QIcon::fromTheme(icon), text, this);
    a->setShortcut(key);
    a->setShortcutContext(Qt::WindowShortcut); // each window has its own copy; application-wide copies would be ambiguous
    connect(a, &QAction::triggered, this, slot);
    addAction(a); // shortcuts work with the menu bar hidden
    m_actions.insert(name, a);
    return a;
}

void MainWindow::createMenus()
{
    // agterm's ⌘ is Ctrl+Shift here (Konsole's convention), ⌘⇧ and ⌘⌃ are Ctrl+Alt+Shift, ⌥ is Alt, ⌃ is Ctrl
    auto *file = menuBar()->addMenu(tr("&File"));
    file->addAction(action(QStringLiteral("new_session"), tr("New Session"), QStringLiteral("tab-new"), QKeySequence(QStringLiteral("Ctrl+Shift+N")),
                           [this] { newSession(currentWorkspace()); }));
    file->addAction(action(QStringLiteral("new_workspace"), tr("New Workspace…"), QStringLiteral("list-add"),
                           QKeySequence(QStringLiteral("Ctrl+Alt+Shift+N")), [this] { newWorkspace(); }));
    file->addAction(action(QStringLiteral("open_directory"), tr("Open Directory…"), QStringLiteral("folder-open"),
                           QKeySequence(QStringLiteral("Ctrl+Shift+O")), [this] { openDirectory(); }));
    file->addSeparator();
    // ⌥⌘N would be Ctrl+Alt+Shift+N, which is ⌘⇧N (New Workspace) here, so New Window takes Ctrl+Alt+N
    file->addAction(action(QStringLiteral("new_window"), tr("New Window"), QStringLiteral("window-new"), QKeySequence(QStringLiteral("Ctrl+Alt+N")), [this] {
        if (library)
            library->newWindow({});
    }));
    QMenu *openWindow = file->addMenu(QIcon::fromTheme(QStringLiteral("window")), tr("Open Window"));
    connect(openWindow, &QMenu::aboutToShow, this, [this, openWindow] {
        openWindow->clear();
        if (library)
            for (const auto &e : library->entries()) {
                QAction *a = openWindow->addAction(e.name, this, [this, id = e.id] { library->select(id); });
                a->setCheckable(true);
                a->setChecked(e.open);
            }
    });
    file->addAction(action(QStringLiteral("rename_window"), tr("Rename Window…"), QStringLiteral("edit-rename"), {}, [this] {
        bool ok = false;
        const QString name = library ? askName(tr("Rename Window"), &ok) : QString();
        if (ok && !name.trimmed().isEmpty())
            library->rename(windowId, name.trimmed());
    }));
    file->addAction(action(QStringLiteral("delete_window"), tr("Delete Window"), QStringLiteral("edit-delete"), {}, [this] {
        QString error;
        const int n = int(m_store.allSessions().size());
        if (!library
            || (n > 0 && QMessageBox::question(this, tr("Delete Window"), tr("Delete this window and close its %n session(s)?", nullptr, n)) != QMessageBox::Yes))
            return;
        if (!library->remove(windowId, &error))
            statusBar()->showMessage(error, 3000);
    }));
    file->addSeparator();
    file->addAction(action(QStringLiteral("close_session"), tr("Close Session"), QStringLiteral("tab-close"), QKeySequence(QStringLiteral("Ctrl+Shift+W")),
                           [this] { m_zoomed ? toggleZoom() : closeSessions({m_store.selected}); })); // ⌘W leaves zoom first
    // ⌘Z: the newest close inside its grace comes back with its shells; enabled only then, so Redo keeps the chord otherwise
    QAction *undo = action(QStringLiteral("undo_close"), tr("Reopen Closed Item"), QStringLiteral("edit-undo"),
                           QKeySequence(QStringLiteral("Ctrl+Shift+Z")), [this] { m_store.undoClose(); });
    undo->setEnabled(false);
    file->addAction(undo);
    file->addAction(action(QStringLiteral("reopen_recent"), tr("Reopen Last Closed Item"), QStringLiteral("edit-undo"),
                           QKeySequence(QStringLiteral("Ctrl+Alt+Shift+T")), [this] {
                               if (m_store.reopenClosed().isEmpty())
                                   statusBar()->showMessage(tr("Nothing to reopen"), 2000);
                           }));
    file->addSeparator();
    file->addAction(action(QStringLiteral("edit_keymap"), tr("Edit Keymap…"), QStringLiteral("document-edit"), {},
                           [this] { editConfigFile(configDir() + QStringLiteral("/keymap.conf")); }));
    file->addAction(action(QStringLiteral("reload_keymap"), tr("Reload Keymap"), QStringLiteral("view-refresh"), {}, [this] { reloadKeymap(); }));
    file->addAction(action(QStringLiteral("edit_hooks"), tr("Edit Hooks…"), QStringLiteral("document-edit"), {},
                           [this] { editConfigFile(configDir() + QStringLiteral("/hooks.conf")); }));
    file->addAction(action(QStringLiteral("reload_hooks"), tr("Reload Hooks"), QStringLiteral("view-refresh"), {}, [this] { reloadHooks(); }));
    file->addSeparator();
    file->addAction(action(QStringLiteral("quit"), tr("Quit"), QStringLiteral("application-exit"), QKeySequence(QStringLiteral("Ctrl+Shift+Q")),
                           [this] { library ? library->quit() : void(close()); }));

    auto *session = menuBar()->addMenu(tr("&Session"));
    session->addAction(action(QStringLiteral("rename_session"), tr("Rename Session"), QStringLiteral("edit-rename"), {}, [this] {
        if (QTreeWidgetItem *item = m_tree->currentItem())
            m_tree->editItem(item);
    }));
    session->addAction(action(QStringLiteral("duplicate_session"), tr("Duplicate Session"), QStringLiteral("edit-copy"), {}, [this] {
        if (m_store.session(m_store.selected))
            m_store.select(m_store.duplicateSession(m_store.selected, liveCwd(m_store.selected)));
    }));
    session->addAction(action(QStringLiteral("new_companion"), tr("New Companion"), QStringLiteral("list-add"), {},
                              [this] { newCompanion(m_store.selected); }));
    session->addAction(action(QStringLiteral("duplicate_companion"), tr("Duplicate as Companion"), QStringLiteral("edit-copy"), {}, [this] {
        if (m_store.session(m_store.selected))
            duplicateCompanion(m_store.selected);
    }));
    session->addAction(action(QStringLiteral("clear_status"), tr("Clear Status"), QStringLiteral("edit-clear"), {},
                              [this] { m_store.setStatus(m_store.selected, QStringLiteral("idle")); }));
    session->addAction(action(QStringLiteral("toggle_flag"), tr("Flag"), QStringLiteral("flag"), QKeySequence(QStringLiteral("Ctrl+Alt+Shift+F")), [this] {
        if (const Session *s = m_store.session(m_store.selected))
            m_store.setFlag(s->id, !s->flagged);
    }));
    // palette-only, as in agterm: "Tint Session: Red" … "Tint Session: None" for the active session
    for (const QString &c : Store::tintColors + QStringList{QString()})
        action(QStringLiteral("tint_session_") + (c.isEmpty() ? QStringLiteral("none") : c),
               tr("Tint Session: %1").arg(c.isEmpty() ? tr("None") : c.left(1).toUpper() + c.mid(1)), {}, {},
               [this, c] { m_store.setTint(m_store.selected, c); });

    auto *view = menuBar()->addMenu(tr("&View"));
    m_sidebarAction = action(QStringLiteral("toggle_sidebar"), tr("Show Sidebar"), QStringLiteral("sidebar-show-symbolic"),
                             QKeySequence(QStringLiteral("Ctrl+Alt+Shift+S")), [this] {
                                 m_sidebarPanel->setVisible(m_sidebarAction->isChecked());
                                 m_saveTimer.start();
                             });
    m_sidebarAction->setCheckable(true);
    m_sidebarAction->setChecked(true);
    m_sidebarAction->setToolTip(tr("Toggle Sidebar"));
    view->addAction(m_sidebarAction);
    view->addAction(action(QStringLiteral("toggle_file_panel"), tr("Show File Panel"), QStringLiteral("folder"),
                           QKeySequence(QStringLiteral("Ctrl+Alt+Shift+E")), [this] { QString e; filePanel(QStringLiteral("toggle"), 0, &e); }));
    auto *flagged = action(QStringLiteral("toggle_flagged_view"), tr("Flagged Sessions"), QStringLiteral("flag"), {},
                           [this] { m_flagButton->setChecked(m_actions.value(QStringLiteral("toggle_flagged_view"))->isChecked()); });
    flagged->setCheckable(true);
    view->addAction(flagged);
    view->addAction(action(QStringLiteral("focus_workspace"), tr("Focus Workspace"), {}, {}, [this] {
        QString e;
        if (const QString ws = m_store.resolveWorkspace({}, &e); !ws.isEmpty())
            m_store.focusWorkspace(ws, QStringLiteral("toggle"));
    }));
    view->addAction(action(QStringLiteral("toggle_workspace_filter"), tr("Toggle Workspace Filter"), {}, {},
                           [this] { m_store.setFilter(!m_store.focusEnabled); }));
    view->addAction(action(QStringLiteral("clear_focus"), tr("Clear Focus"), {}, {}, [this] {
        for (const QString &id : QStringList(m_store.focused))
            m_store.focusWorkspace(id, QStringLiteral("off"));
    }));
    view->addAction(action(QStringLiteral("edit_flagged"), tr("Edit Flagged Sessions…"), {}, {}, [this] { editFlaggedSessions(); }));
    view->addAction(action(QStringLiteral("edit_focused"), tr("Edit Focused Workspaces…"), {}, {}, [this] { editFocusedWorkspaces(); }));
    view->addAction(action(QStringLiteral("toggle_menubar"), tr("Show Menu Bar"), QStringLiteral("show-menu"), QKeySequence(QStringLiteral("Ctrl+Shift+M")),
                           [this] { menuBar()->setVisible(!menuBar()->isVisible()); }));
    view->addAction(action(QStringLiteral("toggle_overlay"), tr("Hide Overlays"), {}, QKeySequence(QStringLiteral("Ctrl+Alt+Shift+J")),
                           [this] { toggleOverlays(); }));
    view->addSeparator();
    view->addAction(action(QStringLiteral("toggle_split"), tr("Split"), {}, QKeySequence(QStringLiteral("Ctrl+Shift+D")),
                           [this] { m_store.toggleSplit(m_store.selected, QStringLiteral("vertical")); }));
    view->addAction(action(QStringLiteral("toggle_horizontal_split"), tr("Split Horizontally"), {}, QKeySequence(QStringLiteral("Ctrl+Alt+Shift+D")),
                           [this] { m_store.toggleSplit(m_store.selected, QStringLiteral("horizontal")); }));
    view->addAction(action(QStringLiteral("swap_panes"), tr("Swap Panes"), {}, {}, [this] { swapPanes(m_store.selected); }));
    view->addAction(action(QStringLiteral("close_split"), tr("Close Split"), {}, {}, [this] {
        if (const Session *s = m_store.session(m_store.selected); s && s->hasSplit)
            m_store.setSplit(s->id, false, false, s->splitAxis);
    }));
    view->addAction(action(QStringLiteral("focus_left_pane"), tr("Focus Left Pane"), {}, QKeySequence(QStringLiteral("Ctrl+Alt+Shift+Left")),
                           [this] { focusPane(false); }));
    view->addAction(action(QStringLiteral("focus_right_pane"), tr("Focus Right Pane"), {}, QKeySequence(QStringLiteral("Ctrl+Alt+Shift+Right")),
                           [this] { focusPane(true); }));
    view->addSeparator();
    // ⌘+ ⌘− ⌘0; the shifted forms are what Ctrl+Shift with = - 0 produce on most layouts
    auto fontAction = [this, view](const QString &name, const QString &text, const QString &op, QList<QKeySequence> keys) {
        QAction *a = action(name, text, {}, keys.first(), [this, op] {
            QString e;
            if (m_store.session(m_store.selected))
                font(m_store.selected, QStringLiteral("main"), op, &e);
        });
        a->setShortcuts(keys);
        view->addAction(a);
    };
    fontAction(QStringLiteral("increase_font_size"), tr("Increase Font Size"), QStringLiteral("inc"),
               {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Plus), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Equal), QKeySequence(Qt::CTRL | Qt::Key_Plus)});
    fontAction(QStringLiteral("decrease_font_size"), tr("Decrease Font Size"), QStringLiteral("dec"),
               {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Minus), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Underscore),
                QKeySequence(Qt::CTRL | Qt::Key_Underscore)});
    fontAction(QStringLiteral("reset_font_size"), tr("Reset Font Size"), QStringLiteral("reset"),
               {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_0), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_ParenRight),
                QKeySequence(Qt::CTRL | Qt::Key_ParenRight)});
    view->addSeparator();
    view->addAction(action(QStringLiteral("toggle_terminal_zoom"), tr("Terminal Zoom"), QStringLiteral("zoom-fit-best"),
                           QKeySequence(QStringLiteral("Ctrl+Alt+Shift+Return")), [this] { toggleZoom(); }));
    view->addAction(action(QStringLiteral("select_theme"), tr("Select Theme…"), QStringLiteral("color-management"), {}, [this] { selectTheme(); }));
    view->addAction(action(QStringLiteral("toggle_fullscreen"), tr("Full Screen"), QStringLiteral("view-fullscreen"), QKeySequence(QStringLiteral("F11")),
                           [this] { setWindowState(windowState() ^ Qt::WindowFullScreen); }));

    auto *nav = menuBar()->addMenu(tr("&Navigate"));
    m_palette = new Palette(this);
    auto sessionItems = [this](bool byRecency) {
        QList<Palette::Item> items;
        QList<Session *> all = m_store.navigableSessions(); // the palettes stay inside the flagged view or the filter
        if (byRecency) {
            std::stable_sort(all.begin(), all.end(), [this](Session *a, Session *b) {
                const qsizetype ia = m_store.recent.indexOf(a->id), ib = m_store.recent.indexOf(b->id);
                return (ia < 0 ? 1 << 30 : ia) < (ib < 0 ? 1 << 30 : ib);
            });
        } else {
            std::stable_sort(all.begin(), all.end(), [](Session *a, Session *b) { return a->displayName().compare(b->displayName(), Qt::CaseInsensitive) < 0; });
        }
        for (Session *s : all)
            items.append({s->displayName(), m_store.workspaceOf(s->id)->name + QStringLiteral(" · ") + s->cwd, [this, id = s->id] { m_store.select(id); }});
        return items;
    };
    nav->addAction(action(QStringLiteral("session_palette"), tr("Go to Session…"), QStringLiteral("search"), QKeySequence(QStringLiteral("Ctrl+P")),
                          [this, sessionItems] { m_palette->open(tr("Go to session…"), sessionItems(false)); }));
    nav->addAction(action(QStringLiteral("command_palette"), tr("Action Palette…"), QStringLiteral("run-build"), QKeySequence(QStringLiteral("Ctrl+Shift+P")), [this] {
        QList<Palette::Item> items;
        for (QAction *a : actions())
            if (!a->text().isEmpty() && a != m_actions.value(QStringLiteral("command_palette")))
                items.append({a->text().remove(QLatin1Char('&')).remove(QStringLiteral("…")),
                              (a->property("agtermCustom").toBool() ? tr("custom") + QStringLiteral("  ") : QString()) + a->shortcut().toString(QKeySequence::NativeText),
                              [a] { a->trigger(); }, a->isEnabled()});
        m_palette->open(tr("Run an action…"), items);
    }));
    // ⌃⇧O; Ctrl+Shift+O is ⌘O (Open Directory) here, so the Shift becomes Alt
    nav->addAction(action(QStringLiteral("custom_command_palette"), tr("Custom Commands…"), QStringLiteral("system-run"), QKeySequence(QStringLiteral("Ctrl+Alt+O")),
                          [this] { customCommandPalette(); }));
    nav->addAction(action(QStringLiteral("mru_switcher"), tr("Switch to Recent Session"), {}, QKeySequence(QStringLiteral("Ctrl+Tab")), [this, sessionItems] {
        m_palette->switching() ? m_palette->step(1) : m_palette->open({}, sessionItems(true), true);
    }));
    nav->addAction(action(QStringLiteral("mru_switcher_back"), tr("Switch to Recent Session (Reverse)"), {}, QKeySequence(QStringLiteral("Ctrl+Shift+Backtab")),
                          [this, sessionItems] {
                              if (!m_palette->switching())
                                  m_palette->open({}, sessionItems(true), true);
                              m_palette->step(-1);
                          }));
    nav->addAction(action(QStringLiteral("dashboard"), tr("Dashboard"), QStringLiteral("view-grid"), QKeySequence(QStringLiteral("Ctrl+Alt+Shift+G")),
                          [this] { toggleDashboard(); }));
    nav->addSeparator();
    nav->addAction(action(QStringLiteral("previous_session"), tr("Previous Session"), QStringLiteral("go-up"),
                          QKeySequence(QStringLiteral("Ctrl+Alt+Shift+Up")), [this] { go(QStringLiteral("prev")); }));
    nav->addAction(action(QStringLiteral("next_session"), tr("Next Session"), QStringLiteral("go-down"),
                          QKeySequence(QStringLiteral("Ctrl+Alt+Shift+Down")), [this] { go(QStringLiteral("next")); }));
    nav->addAction(action(QStringLiteral("previous_attention_session"), tr("Previous Attention Session"), QStringLiteral("go-up-skip"),
                          QKeySequence(QStringLiteral("Ctrl+Alt+Up")), [this] { go(QStringLiteral("prev-attention")); }));
    nav->addAction(action(QStringLiteral("next_attention_session"), tr("Next Attention Session"), QStringLiteral("go-down-skip"),
                          QKeySequence(QStringLiteral("Ctrl+Alt+Down")), [this] { go(QStringLiteral("next-attention")); }));
    nav->addAction(action(QStringLiteral("previous_window"), tr("Previous Window"), {}, {}, [this] {
        QString e;
        if (library && !library->go(-1, &e))
            statusBar()->showMessage(e, 2000);
    }));
    nav->addAction(action(QStringLiteral("next_window"), tr("Next Window"), {}, {}, [this] {
        QString e;
        if (library && !library->go(1, &e))
            statusBar()->showMessage(e, 2000);
    }));
    nav->addAction(action(QStringLiteral("first_session"), tr("First Session"), QStringLiteral("go-top"), {}, [this] { go(QStringLiteral("first")); }));
    nav->addAction(action(QStringLiteral("last_session"), tr("Last Session"), QStringLiteral("go-bottom"), {}, [this] { go(QStringLiteral("last")); }));

    auto *settings = menuBar()->addMenu(tr("Se&ttings"));
    settings->addAction(action(QStringLiteral("settings"), tr("Configure agterm…"), QStringLiteral("configure"), QKeySequence(QStringLiteral("Ctrl+Shift+,")),
                               [this] { showSettings(); }));
    settings->addAction(action(QStringLiteral("edit_profile"), tr("Edit Current Profile…"), QStringLiteral("document-edit"), {},
                               [this] { profileDialog("showEditCurrentProfileDialog"); }));
    settings->addAction(action(QStringLiteral("manage_profiles"), tr("Manage Profiles…"), QStringLiteral("configure-shortcuts"), {},
                               [this] { profileDialog("showManageProfilesDialog"); }));
    addHelpMenu(menuBar()->addMenu(tr("&Help")), this);
}

bool MainWindow::isOwnShortcut(QKeyEvent *event) const
{
    static const QSet<int> modifierKeys{Qt::Key_Control, Qt::Key_Shift, Qt::Key_Alt, Qt::Key_Meta, Qt::Key_AltGr, Qt::Key_Super_L, Qt::Key_Super_R};
    if (modifierKeys.contains(event->key()))
        return false;
    // a leader sequence (ctrl+a>g): the window claims every key while some binding still partially matches
    QList<QKeyCombination> chords = m_chordPrefix;
    chords << event->keyCombination();
    const auto none = QKeyCombination::fromCombined(0);
    const QKeySequence seq(chords[0], chords.value(1, none), chords.value(2, none), chords.value(3, none));
    bool partial = false;
    for (const QAction *a : actions())
        if (a->isEnabled())
            for (const QKeySequence &k : a->shortcuts()) {
                const auto m = seq.matches(k); // Partial: seq is a prefix of k
                if (m == QKeySequence::ExactMatch) {
                    m_chordPrefix.clear();
                    return true;
                }
                partial |= m == QKeySequence::PartialMatch;
            }
    if (partial && chords.size() < 4) {
        m_chordPrefix = chords;
        return true;
    }
    if (!std::exchange(m_chordPrefix, {}).isEmpty())
        return isOwnShortcut(event); // the sequence broke off: this key on its own
    return false;
}

void MainWindow::setSocketStatus(bool ok, const QString &error)
{
    if (!ok)
        statusBar()->showMessage(tr("Control socket unavailable: %1").arg(error));
}

void MainWindow::restore(const QJsonObject &window)
{
    // read once at launch, as master; the starter lists the multiplexers a re-run cannot bring back
    const QString denylist = configDir() + QStringLiteral("/restore-denylist.conf");
    if (!QFile::exists(denylist) && QDir().mkpath(configDir()))
        if (QFile f(denylist); f.open(QIODevice::WriteOnly))
            f.write(Restore::starterDenylist);
    if (QFile f(denylist); f.open(QIODevice::ReadOnly))
        m_denylist = Restore::parseDenylist(QString::fromUtf8(f.readAll()));
    for (auto *s : m_store.allSessions())
        createTerminal(s->id);
    restoreGeometry(QByteArray::fromBase64(window.value("geometry").toString().toLatin1()));
    m_split->restoreState(QByteArray::fromBase64(window.value("splitter").toString().toLatin1()));
    for (const auto &v : window.value("collapsed").toArray())
        m_store.collapsed.insert(v.toString());
    m_sidebarAction->setChecked(window.value("sidebar").toBool(true));
    m_sidebarPanel->setVisible(m_sidebarAction->isChecked());
    m_flagButton->setChecked(window.value("flaggedView").toBool(false));
    m_filePanelWidth = qBound(160, window.value("filePanelWidth").toInt(240), 560);
    m_filePanel->setVisible(window.value("filePanel").toBool(false));
    applyFilePanelWidth();
    syncChrome();
    m_tree->rebuild();
    showSelected();
}

KParts::ReadOnlyPart *MainWindow::spawnPart(const QString &id, const QString &pane, const QString &cwd, const QString &command, QWidget *parent,
                                            const QStringList &env)
{
    auto result = KParts::PartLoader::instantiatePart<KParts::ReadOnlyPart>(KPluginMetaData(QStringLiteral("kf6/parts/konsolepart")), parent, this);
    if (!result) {
        statusBar()->showMessage(tr("Cannot load the Konsole part: %1").arg(result.errorString));
        return nullptr;
    }
    KParts::ReadOnlyPart *part = result.plugin;
    auto *bridge = new PartBridge(this, m_store, id, part);
    connect(part, SIGNAL(currentDirectoryChanged(QString)), bridge, SLOT(currentDirectoryChanged(QString)));
    connect(part, SIGNAL(overrideShortcut(QKeyEvent *, bool &)), bridge, SLOT(overrideShortcut(QKeyEvent *, bool &)));
    if (pane == QLatin1String("main"))
        // the OSC itself names the session; the caption is the fallback for a Konsole whose emulation we cannot reach
        connect(part, &KParts::Part::setWindowCaption, this, [this, id, part](const QString &title) { paneTitle(id, part, title); });
    // the part deletes itself when its shell exits
    connect(part, &QObject::destroyed, this, [this, id, part] {
        if (!m_quitting)
            paneExited(id, part);
    });

    const QString dir = QFileInfo(cwd).isDir() ? cwd : QDir::homePath();
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString shell = qEnvironmentVariable("SHELL", QStringLiteral("/bin/bash"));
    QStringList argv{QStringLiteral("/usr/bin/env"),
                     QStringLiteral("-C"),
                     dir,
                     QStringLiteral("AGTERM_SESSION_ID=") + id,
                     QStringLiteral("AGTERM_WORKSPACE_ID=") + m_store.workspaceOf(id)->id,
                     QStringLiteral("AGTERM_PANE=") + pane,
                     QStringLiteral("AGTERM_SOCKET=") + socketPath(),
                     QStringLiteral("AGTERMCTL=") + appDir + QStringLiteral("/agtermctl"),
                     QStringLiteral("PATH=") + appDir + QLatin1Char(':') + qEnvironmentVariable("PATH")};
    if (!windowId.isEmpty())
        argv << QStringLiteral("AGTERM_WINDOW_ID=") + windowId;
    QString live;
    // without the host binary the pane would exit at once and take its session with it, so it gets a plain shell
    if ((pane == QLatin1String("main") || pane == QLatin1String("right")) && m_store.restoreModeAtLaunch == QLatin1String("live")
        && QFile::exists(appDir + QStringLiteral("/agterm-session-host")))
        live = Live::socket(stateDir(), id, pane), Live::makeDir(stateDir());
    const bool reattach = !live.isEmpty() && Live::alive(live);
    if (pane == QLatin1String("main") || pane == QLatin1String("right")) { // msg.* addresses a pane by it; a respawn gets a new one
        // a reattached shell keeps the id it was started with
        const QString paneId = reattach && !Live::paneId(live).isEmpty() ? Live::paneId(live) : QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!live.isEmpty() && !reattach)
            Live::setPaneId(live, paneId);
        part->setProperty("agtermPaneId", paneId);
        argv << QStringLiteral("AGTERM_PANE_ID=") + paneId;
    }
    argv << env;
    if (!live.isEmpty()) { // a surviving daemon ignores the rest of argv: its shell already runs
        argv << appDir + QStringLiteral("/agterm-session-host") << live << QStringLiteral("--");
        part->setProperty("agtermLive", live);
        part->setProperty("agtermReattached", reattach);
    }
    argv << shell;
    if (!command.isEmpty())
        argv << QStringLiteral("-c") << command;
    qobject_cast<TerminalInterface *>(part)->startProgram(argv.first(), argv);
    if (QWidget *display = terminalDisplay(part)) { // the profile's font, what Settings and reset return to
        display->setProperty("agtermProfileFont", display->font());
        display->setProperty("agtermBaseFont", display->font().pointSizeF());
    }
    applyAppearance(part);
    watchTerminalNotifications(part, bridge);
    stripPartShortcuts();
    return part;
}

void MainWindow::stripPartShortcuts()
{
    // Konsole's own actions (close-session on Ctrl+Shift+W, …) would make our chords ambiguous, and an
    // ambiguous shortcut fires neither, so the window's keys win
    QSet<QKeySequence> ours;
    for (const QAction *a : actions())
        for (const QKeySequence &k : a->shortcuts())
            ours << k;
    for (KActionCollection *col : KActionCollection::allCollections())
        for (QAction *a : col->actions()) {
            QList<QKeySequence> keep = a->shortcuts();
            if (keep.removeIf([&ours](const QKeySequence &k) { return ours.contains(k); }) > 0)
                a->setShortcuts(keep);
        }
}

void MainWindow::createTerminal(const QString &id)
{
    const Session *s = m_store.session(id);
    Panes p;
    p.view = new QWidget;
    p.layers = new QStackedLayout(p.view);
    p.split = new QSplitter;
    p.split->setChildrenCollapsible(false);
    p.layers->addWidget(p.split);
    p.main = spawnPart(id, QStringLiteral("main"), s->cwd, s->command, p.split);
    if (s->fontSize > 0)
        zoomFontTo(terminalDisplay(p.main), s->fontSize);
    if (!p.main) {
        delete p.view;
        return;
    }
    p.split->addWidget(p.main->widget());
    connect(p.split, &QSplitter::splitterMoved, this, [this, id] {
        Session *s = m_store.session(id);
        const auto &pp = m_panes[id];
        const QList<int> sizes = pp.split->sizes();
        if (s && sizes.size() == 2 && sizes[0] + sizes[1] > 0) {
            s->splitRatio = double(sizes[0]) / (sizes[0] + sizes[1]);
            m_saveTimer.start();
        }
    });
    m_panes.insert(id, p);
    replay(id, p.main, false);
    if (QObject *ks = konsoleSession(id))
        for (int context : {0, 1}) // local and remote: caption = the title the program set (%w), not "dir : process"
            QMetaObject::invokeMethod(ks, "setTabTitleFormat", Q_ARG(int, context), Q_ARG(QString, QStringLiteral("%w")));
    m_stack->addWidget(p.view);
    applyLayout(id);
    prime(p.view);
}

void MainWindow::applyLayout(const QString &id)
{
    Session *s = m_store.session(id);
    if (!s || !m_panes.contains(id))
        return;
    Panes &p = m_panes[id];
    if (s->hasSplit && !p.right) {
        const QString cwd = !s->splitCwd.isEmpty() ? s->splitCwd : liveCwd(id).isEmpty() ? s->cwd : liveCwd(id);
        p.right = spawnPart(id, QStringLiteral("right"), cwd, std::exchange(s->splitCommand, {}), p.split);
        s->splitCwd.clear();
        if (s->splitFontSize > 0)
            zoomFontTo(terminalDisplay(p.right), s->splitFontSize);
        if (p.right) {
            p.split->addWidget(p.right->widget());
            replay(id, p.right, true);
        }
        p.split->handle(1)->installEventFilter(this); // double-click resets an even split
        p.split->handle(1)->setProperty("agtermSession", id);
    } else if (!s->hasSplit && p.right) {
        auto *right = std::exchange(p.right, nullptr);
        const QString live = right->property("agtermLive").toString();
        delete right; // closes the split shell whatever it runs
        if (!live.isEmpty())
            Live::kill(live);
    }
    p.split->setOrientation(s->splitAxis == QLatin1String("horizontal") ? Qt::Vertical : Qt::Horizontal);
    if (p.right) {
        p.right->widget()->setVisible(s->splitVisible);
        const int total = 10000;
        p.split->setSizes({int(total * s->splitRatio), total - int(total * s->splitRatio)});
    }
    p.layers->setCurrentWidget(p.split);
    if (KParts::ReadOnlyPart *target = coverPart(id); target && id == m_store.selected && isActiveWindow())
        target->widget()->setFocus();
    layoutOverlays(id);
}

void MainWindow::paneExited(const QString &id, KParts::ReadOnlyPart *part)
{
    if (!m_panes.contains(id))
        return;
    Panes &p = m_panes[id];
    if (!m_store.session(id)) { // closed and inside its undo grace: an undo brings back what is left
        p.main = p.main == part ? nullptr : p.main;
        p.right = p.right == part ? nullptr : p.right;
        return;
    }
    if (part == p.right) {
        p.right = nullptr;
        m_store.setSplit(id, false, false, m_store.session(id)->splitAxis);
    } else if (part == p.main) {
        p.main = nullptr;
        m_sessions.remove(id);
        if (p.right) { // the split survivor becomes the session's only shell
            p.main = std::exchange(p.right, nullptr);
            p.main->widget()->show();
            if (const QString live = p.main->property("agtermLive").toString(); !live.isEmpty()) {
                const QString main = Live::socket(stateDir(), id, QStringLiteral("main"));
                Live::move(live, main);
                p.main->setProperty("agtermLive", main);
            }
            m_store.setSplit(id, false, false, m_store.session(id)->splitAxis);
        } else if (!m_store.companionsOf(id).isEmpty() && m_respawned.value(id).msecsTo(QDateTime::currentDateTime()) > 2000) {
            // a parent with companions gets a fresh shell; one that exits again within 2 s closes the group
            m_respawned.insert(id, QDateTime::currentDateTime());
            const Session *s = m_store.session(id);
            p.main = spawnPart(id, QStringLiteral("main"), s->cwd, {}, p.split);
            if (p.main) {
                p.split->insertWidget(0, p.main->widget());
                if (id == m_store.selected)
                    p.main->widget()->setFocus();
            }
        } else {
            m_store.removeSession(id);
        }
    }
    dashboardForget(id);
}

void MainWindow::newCompanion(const QString &parentId)
{
    if (m_store.session(parentId))
        m_store.select(m_store.addCompanion(parentId, {}, liveCwd(parentId)));
}

bool MainWindow::font(const QString &sessionId, const QString &pane, const QString &op, QString *error)
{
    const Panes p = m_panes.value(sessionId);
    QWidget *display = terminalDisplay(pane == QLatin1String("right") ? p.right : p.main);
    Session *s = m_store.session(sessionId);
    if (!display || !s) {
        *error = QStringLiteral("pane has no terminal");
        return false;
    }
    const double now = display->font().pointSizeF();
    zoomFontTo(display, op == QLatin1String("inc") ? now + 1 : op == QLatin1String("dec") ? now - 1 : display->property("agtermBaseFont").toDouble());
    recordFontSizes(s);
    m_saveTimer.start();
    return true;
}

void MainWindow::recordFontSizes(Session *s) const
{
    // 0 while a pane is at its profile's size, so a profile change still reaches it after a relaunch
    auto size = [](QWidget *d) {
        return !d || d->font().pointSizeF() == d->property("agtermBaseFont").toDouble() ? 0.0 : d->font().pointSizeF();
    };
    const Panes p = m_panes.value(s->id);
    if (p.main)
        s->fontSize = size(terminalDisplay(p.main));
    if (p.right)
        s->splitFontSize = size(terminalDisplay(p.right));
}

void MainWindow::duplicateCompanion(const QString &id)
{
    QString error;
    const QString copy = duplicateAsCompanion(m_store, *this, id, &error);
    copy.isEmpty() ? statusBar()->showMessage(error, 5000) : m_store.select(copy);
}

bool MainWindow::panePlan(const QString &sessionId, const QString &pane, PanePlan *out, QString *error)
{
    // a shell holding the pane gives a plain shell in its directory; anything else is run again as it was started
    const Panes p = m_panes.value(sessionId);
    auto *t = qobject_cast<TerminalInterface *>(pane == QLatin1String("right") ? p.right : p.main);
    const QString name = pane == QLatin1String("right") ? QStringLiteral("right") : QStringLiteral("left");
    KParts::ReadOnlyPart *part = pane == QLatin1String("right") ? p.right : p.main;
    if (!t || shellPid(part) <= 0) {
        *error = QStringLiteral("%1 pane is not realized; nothing was created").arg(name);
        return false;
    }
    const int shell = shellPid(part), fg = foregroundPid(part);
    const int pid = fg > 0 ? fg : shell;
    out->cwd = QFile::symLinkTarget(QStringLiteral("/proc/%1/cwd").arg(pid));
    out->command.clear();
    if (pid == shell)
        return !out->cwd.isEmpty() || (*error = QStringLiteral("%1 pane: its directory cannot be read; nothing was created").arg(name), false);
    QFile f(QStringLiteral("/proc/%1/cmdline").arg(pid));
    const QStringList argv = f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).split(QChar(0), Qt::SkipEmptyParts) : QStringList();
    if (argv.isEmpty() || out->cwd.isEmpty()) {
        *error = QStringLiteral("%1 pane: the foreground program cannot be read; nothing was created").arg(name);
        return false;
    }
    // shortcut: the source session's id in the arguments is not rebound to the copy's, as master does
    QStringList quoted;
    for (QString a : argv)
        quoted << QLatin1Char('\'') + a.replace(QLatin1Char('\''), QStringLiteral("'\\''")) + QLatin1Char('\'');
    out->command = QStringLiteral("exec ") + quoted.join(QLatin1Char(' '));
    return true;
}

void MainWindow::replay(const QString &id, KParts::ReadOnlyPart *part, bool split)
{
    // typed into the login shell rather than exec'd, so the shell remains when the program ends
    // a reattached live pane still runs it, so the capture is only the fallback for a daemon that is gone
    if (const QString line = m_store.takeReplay(id, split, m_denylist); !line.isEmpty() && part && !part->property("agtermReattached").toBool())
        qobject_cast<TerminalInterface *>(part)->sendInput(line + QLatin1Char('\n'));
}

QStringList MainWindow::foregroundArgv(const QString &sessionId, const QString &pane)
{
    const Panes p = m_panes.value(sessionId);
    KParts::ReadOnlyPart *part = pane == QLatin1String("right") ? p.right : p.main;
    if (!part)
        return {};
    // the group leader; a pane whose program is the terminal process itself reports no foreground
    const int pid = foregroundPid(part) > 0 ? foregroundPid(part) : shellPid(part);
    QFile f(QStringLiteral("/proc/%1/cmdline").arg(pid));
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).split(QChar(0), Qt::SkipEmptyParts) : QStringList();
}

bool MainWindow::saveNow(QString *error)
{
    m_saveTimer.stop();
    return save() || (*error = QStringLiteral("cannot write %1").arg(m_statePath), false);
}

void MainWindow::swapPanes(const QString &id)
{
    Panes &p = m_panes[id];
    if (!p.right)
        return;
    p.split->insertWidget(0, p.right->widget()); // physical positions and the primary role move together
    std::swap(p.main, p.right);
    const QString a = p.main->property("agtermLive").toString(), b = p.right->property("agtermLive").toString();
    if (!a.isEmpty() && !b.isEmpty()) { // each daemon is named by its role
        Live::swap(a, b);
        p.main->setProperty("agtermLive", b);
        p.right->setProperty("agtermLive", a);
    }
    m_sessions.remove(id);
}

void MainWindow::focusPane(bool right)
{
    const Panes p = m_panes.value(m_store.selected);
    KParts::ReadOnlyPart *target = right ? p.right : p.main;
    if (target && target->widget()->isVisible())
        target->widget()->setFocus();
}

void MainWindow::toggleZoom()
{
    // a view mode: everything but the terminal hides, and comes back exactly as it was
    if (m_dashboard && !m_zoomed)
        closeDashboard();
    m_zoomed = !m_zoomed;
    if (m_zoomed) {
        m_zoomRestoreSidebar = m_sidebarPanel->isVisible();
        m_zoomRestoreMenu = menuBar()->isVisible();
    }
    m_sidebarPanel->setVisible(!m_zoomed && m_zoomRestoreSidebar);
    menuBar()->setVisible(!m_zoomed && m_zoomRestoreMenu);
    m_strip->setVisible(!m_zoomed);
}

void MainWindow::applyFilePanelWidth()
{
    // the splitter would otherwise hand a newly shown panel whatever share it computes
    if (m_filePanel->isHidden())
        return;
    QList<int> sizes = m_split->sizes();
    sizes[2] = qMax(200, sizes[2] + sizes[1] - m_filePanelWidth);
    sizes[1] = m_filePanelWidth;
    m_split->setSizes(sizes);
}

bool MainWindow::filePanel(const QString &mode, int width, QString *)
{
    if (!mode.isEmpty())
        m_filePanel->setVisible(mode == QLatin1String("toggle") ? m_filePanel->isHidden() : mode == QLatin1String("show"));
    if (width > 0)
        m_filePanelWidth = qBound(160, width, 560); // agterm's clamp
    applyFilePanelWidth();
    m_filePanel->setRoot(liveCwd(m_store.selected));
    m_saveTimer.start();
    return true;
}

QJsonObject MainWindow::chromeState() const
{
    QJsonObject out{{"sidebarVisible", !m_sidebarPanel->isHidden()},
                    {"filePanel", QJsonObject{{"visible", !m_filePanel->isHidden()}, {"width", m_filePanelWidth}}},
                    {"idleMs", m_idle.elapsed()}};
    if (const qint64 follow = autoFollowMs())
        out.insert("autoFollowMs", follow);
    for (const QJsonObject &extra : {dashboardState(), controlChrome()})
        for (auto it = extra.begin(); it != extra.end(); ++it)
            out.insert(it.key(), it.value());
    return out;
}

TerminalInterface *MainWindow::terminal(const QString &id) const
{
    auto *part = m_panes.value(id).main;
    return part ? qobject_cast<TerminalInterface *>(part) : nullptr;
}

QString MainWindow::liveCwd(const QString &id) const
{
    // Konsole refreshes its own answer only for the shown pane
    const int pid = shellPid(m_panes.value(id).main);
    return pid > 0 ? QFile::symLinkTarget(QStringLiteral("/proc/%1/cwd").arg(pid)) : QString();
}

void MainWindow::prime(QWidget *page)
{
    // a never-shown terminal has no screen size, so it would read back empty; one synchronous show sizes it
    if (!isVisible() || page->property("primed").toBool())
        return;
    page->setProperty("primed", true);
    QWidget *current = m_stack->currentWidget();
    m_stack->setCurrentWidget(page);
    m_stack->setCurrentWidget(current);
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    for (const Panes &p : std::as_const(m_panes))
        prime(p.view);
}

void MainWindow::paneTitle(const QString &id, QObject *part, const QString &title)
{
    // a title set by a running program (an agent naming its task) renames the session; the one a shell prompt sets
    // on every line (bash's user@host: dir) does not. Konsole sends the caption lazily, often only once the next
    // command is in front, so the prompt's title is remembered to tell it apart.
    auto *p = qobject_cast<KParts::ReadOnlyPart *>(part);
    if (!p || p != m_panes.value(id).main)
        return;
    const int fg = freshForegroundPid(p);
    if (fg <= 0 || fg == shellPid(p))
        m_shellTitle.insert(id, title);
    else if (!title.trimmed().isEmpty() && title != m_shellTitle.value(id)) {
        m_store.setTitle(id, title);
        m_titlePid.insert(id, fg); // the sampler leaves a program's own title alone
        m_commandTitle.remove(id);
    }
}

QObject *MainWindow::konsoleSession(const QString &id)
{
    // Konsole::Session is owned by Konsole's session manager, not the part; it registers on the
    // in-process bus as /Sessions/N, and the shell pid ties it to our part.
    if (auto cached = m_sessions.value(id))
        return cached;
    auto *t = terminal(id);
    if (!t)
        return nullptr;
    const int pid = t->terminalProcessId();
    for (int n = 1; n < 4096; ++n) {
        QObject *o = QDBusConnection::sessionBus().objectRegisteredAt(QStringLiteral("/Sessions/%1").arg(n));
        int sessionPid = -1;
        if (o && QMetaObject::invokeMethod(o, "processId", Q_RETURN_ARG(int, sessionPid)) && sessionPid == pid) {
            m_sessions.insert(id, o);
            return o;
        }
    }
    return nullptr;
}

bool MainWindow::type(const QString &sessionId, const QString &text, QString *error)
{
    auto *t = terminal(sessionId);
    if (!t) {
        *error = QStringLiteral("session has no terminal");
        return false;
    }
    t->sendInput(text);
    if (text.contains(QLatin1Char('\n')) || text.contains(QLatin1Char('\r')))
        m_store.keyPressed(sessionId, Store::Key::Submit); // injected text carrying a newline is a submit
    return true;
}

QJsonObject MainWindow::info(const QString &sessionId)
{
    auto *t = terminal(sessionId);
    if (!t)
        return {};
    KParts::ReadOnlyPart *part = m_panes.value(sessionId).main;
    QFile comm(QStringLiteral("/proc/%1/comm").arg(foregroundPid(part)));
    const bool live = !part->property("agtermLive").toString().isEmpty();
    return {{"pid", shellPid(part)},
            {"foreground", live && comm.open(QIODevice::ReadOnly) ? QString::fromUtf8(comm.readAll()).trimmed() : t->foregroundProcessName()},
            {"foregroundPid", foregroundPid(part)},
            {"liveCwd", liveCwd(sessionId)},
            {"profile", t->currentProfileName()}};
}

void MainWindow::showSelected()
{
    if (m_dashboard) { // a status repaint keeps the grid; picking another session leaves it
        if (m_store.selected == m_shownId)
            return refreshDashboard();
        return closeDashboard();
    }
    const Session *s = m_store.session(m_store.selected);
    const Panes p = m_panes.value(m_store.selected);
    m_stack->setCurrentWidget(p.view ? p.view : m_empty);
    {
        // the session's own wash, else its workspace's
        const Workspace *w = s ? m_store.workspaceOf(s->id) : nullptr;
        auto wash = [](const Tint &t) { return t.on.contains(QLatin1String("background")) ? tintColor(t.color) : QColor(); };
        const QColor own = s ? wash(s->tint) : QColor();
        static_cast<TintWash *>(m_wash)->setColor(own.isValid() ? own : w ? wash(w->tint) : QColor());
    }
    // focus moves only with the selection, so a status repaint never pulls it out of the split pane
    if (p.view && m_shownId != m_store.selected && isActiveWindow() && !m_tree->editing()) {
        KParts::ReadOnlyPart *front = coverPart(m_store.selected);
        if (front)
            front->widget()->setFocus();
    }
    m_shownId = m_store.selected;
    if (!m_filePanel->isHidden())
        m_filePanel->setRoot(liveCwd(m_store.selected)); // no-op while the folder is unchanged
    const QString name = s ? s->displayName() : QString();
    m_title->setText(name);
    m_context->setText(s ? s->context : QString());
    m_workspaceLabel->setText(s ? m_store.workspaceOf(s->id)->name.left(24) + QStringLiteral("  /") : QString());
    setWindowTitle(name);
}

QString MainWindow::currentWorkspace()
{
    QString error;
    const QString ws = m_store.resolveWorkspace({}, &error);
    return ws.isEmpty() ? m_store.addWorkspace(QStringLiteral("main")) : ws;
}

void MainWindow::newSession(const QString &workspaceId)
{
    const QString ws = m_store.workspace(workspaceId) ? workspaceId : currentWorkspace();
    m_store.select(m_store.addSession(ws, {}, m_store.newSessionCwd(ws, liveCwd(m_store.selected))));
}

void MainWindow::launch(const QString &launcherId, const QString &workspaceId)
{
    const Launcher *l = launcher(launcherId);
    if (!l)
        return;
    Settings s = m_store.settings;
    s.lastLauncher = l->id;
    m_store.setSettings(s);
    newSession(workspaceId);
    if (!l->command.isEmpty()) {
        QString error;
        type(m_store.selected, l->command + QLatin1Char('\n'), &error); // typed into the shell, so the shell remains when it exits
    }
}

void MainWindow::launcherMenu(const QString &workspaceId, const QPoint &globalPos)
{
    detectLaunchers();
    QMenu menu;
    const QColor ink = palette().color(QPalette::WindowText);
    for (const QString &id : std::as_const(m_launchers))
        if (const Launcher *l = launcher(id))
            menu.addAction(launcherIcon(id, ink), l->title, this, [this, id, workspaceId] { launch(id, workspaceId); });
    menu.exec(globalPos);
}

void MainWindow::detectLaunchers()
{
    // agents install into directories only the user's shell startup adds to PATH, so ask a login shell
    if (m_launchersAt.isValid() && m_launchersAt.secsTo(QDateTime::currentDateTime()) < 300)
        return;
    m_launchersAt = QDateTime::currentDateTime();
    auto *proc = new QProcess(this);
    const QString marker = QStringLiteral("agterm-launcher-path");
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("AGTERM_SESSION_ID"));
    proc->setProcessEnvironment(env);
    proc->setStandardInputFile(QProcess::nullDevice());
    connect(proc, &QProcess::finished, this, [this, proc, marker] {
        const QString out = QString::fromUtf8(proc->readAllStandardOutput());
        const int at = out.lastIndexOf(marker + QLatin1Char('\n'));
        const QString shellPath = at < 0 ? QString() : out.mid(at + marker.size() + 1).trimmed();
        const QString path = QStringList{shellPath, qEnvironmentVariable("PATH"), QDir::homePath() + QStringLiteral("/.local/bin")}.join(QLatin1Char(':'));
        m_launchers = ::availableLaunchers(path, [](const QString &f) { return QFileInfo(f).isExecutable() && QFileInfo(f).isFile(); });
        proc->deleteLater();
    });
    QTimer::singleShot(3000, proc, [proc] { proc->kill(); });
    proc->start(qEnvironmentVariable("SHELL", QStringLiteral("/bin/bash")),
                {QStringLiteral("-l"), QStringLiteral("-i"), QStringLiteral("-c"), QStringLiteral("printf '\\n%s\\n' %1; printenv PATH").arg(marker)});
}

void MainWindow::sampleAgents()
{
    // the agent icon follows whatever runs in the foreground, typed by hand or by a script
    QHash<int, QList<int>> groups; // pgid -> pids, leader first
    for (QDirIterator it(QStringLiteral("/proc"), QDir::Dirs | QDir::NoDotAndDotDot); it.hasNext();) {
        bool numeric = false;
        const int pid = it.nextFileInfo().fileName().toInt(&numeric);
        if (!numeric)
            continue;
        QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
        if (!stat.open(QIODevice::ReadOnly))
            continue;
        const QByteArray line = stat.readAll();
        const QList<QByteArray> fields = line.mid(line.lastIndexOf(')') + 2).split(' ');
        if (fields.size() < 3)
            continue;
        const int pgid = fields[2].toInt();
        pid == pgid ? groups[pgid].prepend(pid) : groups[pgid].append(pid);
    }
    for (auto *s : m_store.allSessions()) {
        KParts::ReadOnlyPart *part = m_panes.value(s->id).main;
        const int fg = foregroundPid(part);
        if (part && !part->property("agtermLive").toString().isEmpty())
            m_store.setCwd(s->id, liveCwd(s->id)); // Konsole tracks only its own child, the attach client
        QList<QStringList> argvs;
        for (int pid : groups.value(fg)) {
            QFile f(QStringLiteral("/proc/%1/cmdline").arg(pid));
            if (f.open(QIODevice::ReadOnly))
                argvs << QString::fromUtf8(f.readAll()).split(QChar(0), Qt::SkipEmptyParts);
        }
        const QString agent = agentForGroup(argvs);
        m_store.setAgent(s->id, agent);
        m_store.setBusy(s->id, fg > 0 && fg != shellPid(part));
        // a plain command names the session while it runs (an agent names itself); idle, the folder does
        const int shell = shellPid(part);
        if (fg > 0 && fg != shell && agent.isEmpty() && !argvs.isEmpty() && fg != m_titlePid.value(s->id)) {
            const QString name = commandTitle(argvs.first());
            m_store.setTitle(s->id, name);
            m_titlePid.insert(s->id, fg);
            m_commandTitle.insert(s->id, name);
        } else if ((fg <= 0 || fg == shell) && !s->title.isEmpty()
                   // after a restart only the argv captured at quit tells a command's name from a program's title
                   && (s->title == m_commandTitle.value(s->id) || s->title == commandTitle(s->foreground))) {
            m_store.setTitle(s->id, QString());
            m_commandTitle.remove(s->id);
        }
        if (agent == QLatin1String("antigravity")) { // agy sets no terminal title; its conversation title names the session
            static const QString agyData = QDir::homePath() + QStringLiteral("/.gemini/antigravity-cli");
            for (int pid : groups.value(fg))
                if (const QString t = antigravityTitle(QStringLiteral("/proc/%1/fd").arg(pid), agyData); !t.isEmpty()) {
                    m_store.setTitle(s->id, t);
                    m_titlePid.insert(s->id, fg);
                    break;
                }
        }
    }
    if (!m_filePanel->isHidden()) // a cd in the active session moves the file panel with it
        m_filePanel->setRoot(liveCwd(m_store.selected));
}

QString MainWindow::askName(const QString &title, bool *ok)
{
    // QInputDialog's SetMinAndMaxSize layout overrides a dialog minimum width, so widen the field instead
    QInputDialog dlg(this);
    dlg.setWindowTitle(title);
    dlg.setLabelText(tr("Name:"));
    dlg.setInputMode(QInputDialog::TextInput);
    if (auto *edit = dlg.findChild<QLineEdit *>())
        edit->setMinimumWidth(qMax(360, width() / 3));
    *ok = dlg.exec() == QDialog::Accepted;
    return dlg.textValue();
}

void MainWindow::newWorkspace()
{
    bool ok = false;
    const QString name = askName(tr("New Workspace"), &ok);
    if (ok && !name.trimmed().isEmpty())
        m_store.addWorkspace(name.trimmed());
}

void MainWindow::openDirectory()
{
    const Session *cur = m_store.session(m_store.selected);
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Open Directory"), cur ? liveCwd(cur->id) : QDir::homePath());
    if (dir.isEmpty())
        return;
    const QString remembering = m_store.workspaceRemembering(dir);
    m_store.select(m_store.addSession(remembering.isEmpty() ? currentWorkspace() : remembering, {}, dir));
}

void MainWindow::closeSessions(const QStringList &ids)
{
    QStringList live;
    for (const auto &id : ids)
        if (m_store.session(id))
            live << id;
    if (live.isEmpty())
        return;
    if (m_store.settings.confirmCloseSession
        && QMessageBox::question(this, tr("Close Session"),
                                 live.size() == 1 ? tr("Close “%1”?").arg(m_store.session(live.first())->displayName())
                                                  : tr("Close %1 sessions?").arg(live.size()))
               != QMessageBox::Yes)
        return;
    if (!m_store.settings.closeUndo || !m_store.softClose(live))
        for (const auto &id : live)
            m_store.removeSession(id);
}

void MainWindow::deleteWorkspace(const QString &id)
{
    const Workspace *w = m_store.workspace(id);
    if (!w)
        return;
    if (!w->sessions.isEmpty()
        && QMessageBox::question(this, tr("Delete Workspace"), tr("Close all %1 sessions in “%2”?").arg(w->sessions.size()).arg(w->name))
               != QMessageBox::Yes)
        return;
    if (!m_store.settings.closeUndo || !m_store.softRemoveWorkspace(id))
        m_store.removeWorkspace(id);
}

void MainWindow::fillMoveMenu(QMenu *menu, const QStringList &ids)
{
    const Workspace *from = m_store.workspaceOf(ids.value(0));
    for (const auto &w : m_store.workspaces) {
        auto *a = menu->addAction(w.name, this, [this, ids, wid = w.id] {
            for (const auto &id : ids)
                m_store.moveSession(id, wid, -1);
        });
        a->setEnabled(ids.size() > 1 || !from || from->id != w.id);
    }
    menu->addSeparator();
    menu->addAction(tr("New Workspace…"), this, [this, ids] {
        bool ok = false;
        const QString name = askName(tr("New Workspace"), &ok);
        if (!ok || name.trimmed().isEmpty())
            return;
        const QString ws = m_store.addWorkspace(name.trimmed());
        for (const auto &id : ids)
            m_store.moveSession(id, ws, -1);
    });
}

void MainWindow::recentMenu()
{
    QMenu menu;
    for (const QString &id : m_store.recentOthers()) {
        const Session *s = m_store.session(id);
        QIcon dot;
        if (s->status != QLatin1String("idle")) {
            QPixmap pm(16, 16);
            pm.fill(Qt::transparent);
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(Qt::NoPen);
            p.setBrush(statusColor(m_store.settings, s->status));
            drawStatusGlyph(&p, QPointF(8, 8), 5, statusShape(m_store.settings, s->status));
            p.end();
            dot = QIcon(pm);
        }
        // the second column carries the workspace, as the switcher row's subtitle
        menu.addAction(dot, s->displayName() + QLatin1Char('\t') + m_store.workspaceOf(id)->name, this, [this, id] { m_store.select(id); });
    }
    QWidget *button = qobject_cast<QToolBar *>(m_strip)->widgetForAction(m_recentAction);
    menu.exec(button ? button->mapToGlobal(QPoint(0, button->height())) : QCursor::pos());
}

void MainWindow::syncChrome()
{
    if (m_flagButton->isChecked() != m_store.flaggedView)
        m_flagButton->setChecked(m_store.flaggedView); // sidebar.mode changed it
    // the filter button is dimmed while nothing is marked: there is nothing to filter to
    const QColor ink = palette().color(QPalette::WindowText);
    m_filterButton->setIcon(chromeIcon(m_store.focusEnabled ? QStringLiteral("gridFilled") : QStringLiteral("grid"), ink));
    m_filterButton->setEnabled(!m_store.focused.isEmpty());
    m_filterButton->setToolTip(m_store.focusEnabled ? tr("Show all workspaces") : tr("Show only focused workspaces"));
    QString e;
    m_actions.value(QStringLiteral("focus_workspace"))->setText(m_store.isSoleFocus(m_store.resolveWorkspace({}, &e)) ? tr("Unfocus Workspace") : tr("Focus Workspace"));
    m_actions.value(QStringLiteral("toggle_workspace_filter"))->setEnabled(!m_store.focused.isEmpty());
    m_actions.value(QStringLiteral("clear_focus"))->setEnabled(!m_store.focused.isEmpty());
    m_actions.value(QStringLiteral("undo_close"))->setEnabled(!m_store.pending.isEmpty());
    m_recentAction->setEnabled(!m_store.recentOthers().isEmpty());
    bool shown = false;
    QAction *overlays = m_actions.value(QStringLiteral("toggle_overlay"));
    overlays->setEnabled(hasHideableOverlay(m_store.selected, &shown));
    overlays->setText(overlays->isEnabled() && !shown ? tr("Show Overlays") : tr("Hide Overlays"));
}

namespace {
// triggers a clicked (or Enter-pressed) action without closing its menu
class KeepMenuOpen : public QObject
{
public:
    using QObject::QObject;
    bool eventFilter(QObject *o, QEvent *e) override
    {
        auto *menu = static_cast<QMenu *>(o);
        QAction *a = nullptr;
        if (e->type() == QEvent::MouseButtonRelease)
            a = menu->actionAt(static_cast<QMouseEvent *>(e)->position().toPoint());
        else if (e->type() == QEvent::KeyPress && (static_cast<QKeyEvent *>(e)->key() == Qt::Key_Return || static_cast<QKeyEvent *>(e)->key() == Qt::Key_Enter))
            a = menu->activeAction();
        if (!a || !a->isEnabled() || a->isSeparator() || a->menu())
            return false;
        a->trigger();
        return true;
    }
};
}

void MainWindow::fillTintMenu(QMenu *menu, const QStringList &ids)
{
    // the menu stays open so a colour and its placement are picked in one go; refresh re-checks after each pick
    struct Entry {
        QAction *action;
        std::function<bool(const QSet<QString> &, const QSet<QStringList> &, bool)> checked;
        bool placement;
    };
    auto entries = std::make_shared<QList<Entry>>();
    auto refresh = [this, ids, entries] {
        // a choice is checked only when every target shares it; the placements act only on tinted rows
        QSet<QString> colors;
        QSet<QStringList> placements;
        bool allTinted = true;
        for (const QString &id : ids) {
            const Session *s = m_store.session(id);
            const Workspace *w = s ? nullptr : m_store.workspace(id);
            if (!s && !w)
                return; // the row closed while the menu was open
            const Tint t = s ? s->tint : w->tint;
            colors << t.color;
            placements << t.on;
            allTinted &= !t.color.isEmpty();
        }
        for (const Entry &e : *entries) {
            e.action->setChecked(e.checked(colors, placements, allTinted));
            if (e.placement)
                e.action->setEnabled(allTinted);
        }
    };
    auto add = [&](const QString &title, const QIcon &icon, bool placement, std::function<bool(const QSet<QString> &, const QSet<QStringList> &, bool)> checked,
                   std::function<void(const QString &)> apply) {
        auto *a = menu->addAction(icon, title, this, [ids, apply, refresh] {
            for (const QString &id : ids)
                apply(id);
            refresh();
        });
        a->setCheckable(true);
        entries->append({a, checked, placement});
    };
    add(tr("None"), {}, false, [](const auto &colors, const auto &, bool) { return colors == QSet<QString>{QString()}; },
        [this](const QString &id) { m_store.setTint(id, {}); });
    for (const QString &c : Store::tintColors) {
        QPixmap swatch(24, 24);
        swatch.fill(Qt::transparent);
        QPainter p(&swatch);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(tintColor(c));
        p.drawEllipse(QRectF(2, 2, 20, 20));
        p.end();
        add(c.left(1).toUpper() + c.mid(1), QIcon(swatch), false, [c](const auto &colors, const auto &, bool) { return colors == QSet<QString>{c}; },
            [this, c](const QString &id) { m_store.setTint(id, c); });
    }
    menu->addSection(tr("Show On"));
    const QList<std::pair<QString, QStringList>> options{{tr("Icon"), {QStringLiteral("icon")}},
                                                        {tr("Name"), {QStringLiteral("name")}},
                                                        {tr("Icon and Name"), {QStringLiteral("icon"), QStringLiteral("name")}},
                                                        {tr("Background"), {QStringLiteral("background")}},
                                                        {tr("Everything"), {QStringLiteral("icon"), QStringLiteral("name"), QStringLiteral("background")}}};
    for (const auto &[title, parts] : options)
        add(title, {}, true, [parts](const auto &, const auto &placements, bool allTinted) { return allTinted && placements == QSet<QStringList>{parts}; },
            [this, parts](const QString &id) { m_store.placeTint(id, parts); });
    menu->installEventFilter(new KeepMenuOpen(menu));
    refresh();
}

void MainWindow::sidebarMenu(const QPoint &pos)
{
    QMenu menu;
    QTreeWidgetItem *item = m_tree->itemAt(pos);
    if (!item) {
        menu.addAction(QIcon::fromTheme(QStringLiteral("list-add")), tr("New Workspace…"), this, &MainWindow::newWorkspace);
        menu.exec(m_tree->viewport()->mapToGlobal(pos));
        return;
    }
    const QString id = item->data(0, SidebarRole::Id).toString();
    if (item->data(0, SidebarRole::Kind).toInt() == SidebarRole::SessionRow) {
        // right-clicking inside the selection acts on all of it; outside narrows to the row
        QStringList ids = m_tree->selectedSessionIds();
        if (!ids.contains(id))
            ids = {id};
        const Session *s = m_store.session(id);
        if (ids.size() == 1) {
            auto *clear = menu.addAction(tr("Clear Status"), this, [this, id] { m_store.setStatus(id, QStringLiteral("idle")); });
            clear->setEnabled(s->status != QLatin1String("idle"));
            menu.addSeparator();
            menu.addAction(tr("Rename"), this, [this, item] { m_tree->editItem(item); });
            menu.addAction(tr("Duplicate Session"), this, [this, id] { m_store.select(m_store.duplicateSession(id, liveCwd(id))); });
            menu.addAction(tr("New Companion"), this, [this, id] { newCompanion(id); });
            menu.addAction(tr("Duplicate as Companion"), this, [this, id] { duplicateCompanion(id); });
        }
        fillMoveMenu(menu.addMenu(tr("Move to")), ids);
        bool allFlagged = true;
        for (const auto &sid : ids)
            allFlagged &= m_store.session(sid)->flagged;
        menu.addAction(allFlagged ? tr("Unflag") : tr("Flag"), this, [this, ids, allFlagged] {
            for (const auto &sid : ids)
                m_store.setFlag(sid, !allFlagged);
        });
        fillTintMenu(menu.addMenu(tr("Tint")), ids);
        menu.addAction(ids.size() == 1 ? tr("Close Session") : tr("Close %1 Sessions").arg(ids.size()), this, [this, ids] { closeSessions(ids); });
    } else {
        const Workspace *w = m_store.workspace(id);
        menu.addAction(QIcon::fromTheme(QStringLiteral("tab-new")), tr("New Session"), this, [this, id] { newSession(id); });
        menu.addSeparator();
        menu.addAction(m_store.isSoleFocus(id) ? tr("Unfocus") : tr("Focus"), this, [this, id] { m_store.focusWorkspace(id, QStringLiteral("toggle")); });
        const bool member = m_store.focused.contains(id);
        menu.addAction(member ? tr("Remove from Focus") : tr("Add to Focus"), this,
                       [this, id, member] { m_store.focusWorkspace(id, member ? QStringLiteral("off") : QStringLiteral("add")); });
        menu.addSeparator();
        menu.addAction(tr("Rename"), this, [this, item] { m_tree->editItem(item); });
        bool allFlagged = !w->sessions.isEmpty();
        for (const auto &s : w->sessions)
            allFlagged &= s.flagged;
        auto *flag = menu.addAction(allFlagged ? tr("Unflag All Sessions") : tr("Flag All Sessions"), this, [this, id, allFlagged] {
            QStringList ids;
            for (const auto &s : m_store.workspace(id)->sessions)
                ids << s.id;
            for (const auto &sid : ids)
                m_store.setFlag(sid, !allFlagged);
        });
        flag->setEnabled(!w->sessions.isEmpty());
        fillTintMenu(menu.addMenu(tr("Tint")), {id});
        menu.addSeparator();
        auto *del = menu.addAction(tr("Delete Workspace"), this, [this, id] { deleteWorkspace(id); });
        del->setEnabled(m_store.workspaces.size() > 1);
    }
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

void MainWindow::go(const QString &direction)
{
    QString error;
    if (m_store.go(direction, &error).isEmpty())
        statusBar()->showMessage(error, 2000);
}

void MainWindow::profileDialog(const char *slot)
{
    auto *part = m_panes.value(m_store.selected).main;
    if (!part || !QMetaObject::invokeMethod(part, slot, Q_ARG(QWidget *, this)))
        statusBar()->showMessage(tr("Open a session first"), 2000);
}

bool MainWindow::save()
{
    // Konsole signals directory changes only for a shown pane; /proc is authoritative for the rest
    for (auto *s : m_store.allSessions()) {
        if (const QString cwd = liveCwd(s->id); !cwd.isEmpty())
            m_store.workspaceOf(s->id)->folder = s->cwd = cwd;
        recordFontSizes(s); // a Ctrl+wheel zoom in the terminal persists too
    }
    QJsonObject root = m_store.toJson();
    QJsonArray collapsed;
    for (const auto &c : std::as_const(m_store.collapsed))
        collapsed << c;
    root.insert("window", QJsonObject{{"geometry", QString::fromLatin1(saveGeometry().toBase64())},
                                      {"splitter", QString::fromLatin1(m_split->saveState().toBase64())},
                                      {"sidebar", m_sidebarAction->isChecked()},
                                      {"flaggedView", m_tree->flaggedView()},
                                      {"filePanel", !m_filePanel->isHidden()},
                                      {"filePanelWidth", m_filePanelWidth},
                                      {"collapsed", collapsed}});
    QSaveFile f(m_statePath);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(QJsonDocument(root).toJson());
    return f.commit();
}

MainWindow::~MainWindow()
{
    // ~QWidget would delete the parts after our members are gone, while their destroyed handlers still run
    m_quitting = true;
    for (const Panes &p : std::as_const(m_panes))
        delete p.view; // its parts go with their widgets
    // a still-running PATH probe would emit finished() into our destroyed members when ~QObject kills it
    for (auto *proc : findChildren<QProcess *>()) {
        proc->disconnect(this);
        delete proc;
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // closing one of several windows keeps its bundle for File > Open Window; the last one quits the app
    if (library && event->spontaneous() && !library->quitting() && library->openWindows().size() > 1 && !m_store.allSessions().isEmpty()) {
        QString name;
        for (const auto &e : library->entries())
            if (e.window == this)
                name = e.name;
        const int n = int(m_store.allSessions().size());
        if (QMessageBox::question(this, tr("Close Window"),
                                  tr("Close “%1”?").arg(name) + QStringLiteral("\n\n")
                                      + tr("This ends %n running session(s). The window can be reopened from File ▸ Open Window.", nullptr, n))
            != QMessageBox::Yes)
            return event->ignore();
    }
    m_saveTimer.stop();
    m_store.finalizePending(); // a close still inside its grace is final at quit
    if (m_store.settings.restoreMode != QLatin1String("none")) {
        captureForeground(m_store, *this); // a quit or close captures; a crash or SIGKILL cannot
    } else {
        for (Session *s : m_store.allSessions())
            s->foreground.clear(), s->splitForeground.clear();
    }
    save();
    m_quitting = true; // parts die with the window from here on; their sessions must stay saved
    QMainWindow::closeEvent(event);
    if (library)
        library->windowClosed(this, event->spontaneous());
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonDblClick && watched->property("agtermSession").isValid()) {
        const QString id = watched->property("agtermSession").toString();
        if (Session *s = m_store.session(id)) {
            s->splitRatio = 0.5; // a double-click on the divider returns to an even split
            applyLayout(id);
            m_saveTimer.start();
        }
        return true;
    }
    if (dashboardEvent(watched, event))
        return true;
    if (event->type() == QEvent::FocusIn && watched->isWidgetType())
        for (auto it = m_panes.begin(); it != m_panes.end(); ++it)
            if (it->right && it->main) {
                if (it->right->widget()->isAncestorOf(static_cast<QWidget *>(watched)))
                    m_splitFocused.insert(it.key(), true);
                else if (it->main->widget()->isAncestorOf(static_cast<QWidget *>(watched)))
                    m_splitFocused.insert(it.key(), false);
            }
    if (event->type() == QEvent::Resize && watched->property("agtermOverlayHost").isValid())
        layoutOverlays(watched->property("agtermOverlayHost").toString());
    if ((event->type() == QEvent::KeyPress || event->type() == QEvent::MouseButtonPress || event->type() == QEvent::Wheel) && watched->isWidgetType()
        && static_cast<QWidget *>(watched)->window() == this)
        m_idle.restart(); // auto-follow waits for input idleness in this window
    if (event->type() == QEvent::KeyPress && !m_quitting) {
        auto *w = qobject_cast<QWidget *>(watched);
        auto *key = static_cast<QKeyEvent *>(event);
        if (w && m_stack->isAncestorOf(w) && !isOwnShortcut(key) && !key->isAutoRepeat()) {
            const bool interrupt = key->key() == Qt::Key_Escape || (key->key() == Qt::Key_C && key->modifiers() == Qt::ControlModifier);
            const bool submit = (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && !(key->modifiers() & ~Qt::KeypadModifier);
            m_store.keyPressed(m_store.selected, interrupt ? Store::Key::Interrupt : submit ? Store::Key::Submit : Store::Key::Plain);
        }
    }
    return QMainWindow::eventFilter(watched, event);
}
