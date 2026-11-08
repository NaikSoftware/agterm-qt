// The dashboard: up to nine live panes re-hosted in a view-only grid.
#include "mainwindow.h"
#include "sidebar.h"
#include "store.h"

#include <KParts/ReadOnlyPart>

#include <QFrame>
#include <QGridLayout>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QVBoxLayout>

bool MainWindow::openDashboard(const QStringList &cells, const QString &fontMode, double fontSize, QString *error)
{
    if (m_dashboard)
        closeDashboard();
    if (m_zoomed)
        toggleZoom(); // the two views exclude each other
    m_dashboard = new QWidget;
    m_dashboard->setFocusPolicy(Qt::StrongFocus);
    auto *grid = new QGridLayout(m_dashboard);
    grid->setContentsMargins(6, 6, 6, 6);
    grid->setSpacing(6);
    const auto [cols, rows] = dashboardGrid(int(cells.size()));
    for (const QString &ref : cells) {
        const QString id = ref.section(QLatin1Char(':'), 0, 0), pane = ref.section(QLatin1Char(':'), 1);
        QWidget *w = paneWidget(id, pane);
        const Panes p = m_panes.value(id);
        if (!w || !p.split)
            continue;
        DashCell c{ref, w, p.split, p.split->indexOf(w), !w->isHidden(), new QFrame, new QLabel, 0};
        if (QWidget *d = terminalDisplay(pane == QLatin1String("right") ? p.right : p.main)) {
            c.font = d->font().pointSizeF();
            const double base = d->property("agtermBaseFont").toDouble() > 0 ? d->property("agtermBaseFont").toDouble() : c.font;
            if (fontMode == QLatin1String("auto"))
                zoomFontTo(d, qMax(6.0, std::round(base * dashboardFontFactor(cols, rows))));
            else if (fontMode == QLatin1String("fixed"))
                zoomFontTo(d, fontSize);
        }
        c.frame->setObjectName(QStringLiteral("dashCell"));
        auto *box = new QVBoxLayout(c.frame);
        box->setContentsMargins(2, 2, 2, 2);
        box->setSpacing(2);
        c.chip->setContentsMargins(8, 2, 8, 2);
        box->addWidget(c.chip, 0, Qt::AlignLeft);
        box->addWidget(w, 1); // the live terminal itself, taken out of its splitter until the grid closes
        w->show();
        const int i = int(m_dashCells.size());
        grid->addWidget(c.frame, i / cols, i % cols);
        m_dashCells << c;
    }
    for (int r = 0; r < rows; ++r)
        grid->setRowStretch(r, 1);
    for (int col = 0; col < cols; ++col)
        grid->setColumnStretch(col, 1);
    if (m_dashCells.isEmpty()) {
        delete std::exchange(m_dashboard, nullptr);
        return *error = QStringLiteral("no dashboard pane is realized"), false;
    }
    m_dashFontMode = fontMode;
    m_dashFontSize = fontMode == QLatin1String("fixed") ? fontSize : 0;
    m_dashHighlight = 0;
    m_stack->addWidget(m_dashboard);
    m_stack->setCurrentWidget(m_dashboard);
    m_wash->hide(); // the grid holds many sessions
    m_dashboard->setFocus();
    setWindowTitle(tr("Dashboard"));
    m_dashBlink.start();
    refreshDashboard();
    syncDashboardButton();
    return true;
}

void MainWindow::closeDashboard()
{
    if (!m_dashboard)
        return;
    m_dashBlink.stop();
    QList<DashCell> cells = std::exchange(m_dashCells, {});
    // back in splitter order, so both panes of one session land where they were
    std::sort(cells.begin(), cells.end(), [](const DashCell &a, const DashCell &b) { return a.index < b.index; });
    QSet<QString> ids;
    for (const DashCell &c : cells) {
        if (!c.widget || !c.home)
            continue;
        c.home->insertWidget(c.index, c.widget);
        c.widget->setVisible(c.wasVisible);
        if (m_dashFontMode != QLatin1String("untouched"))
            zoomFontTo(terminalDisplay(c.ref.endsWith(QLatin1String(":right")) ? m_panes.value(c.ref.section(QLatin1Char(':'), 0, 0)).right
                                                                                : m_panes.value(c.ref.section(QLatin1Char(':'), 0, 0)).main),
                       c.font); // the grid's font was transient
        ids << c.ref.section(QLatin1Char(':'), 0, 0);
    }
    m_dashboard->deleteLater();
    m_dashboard = nullptr;
    syncDashboardButton();
    for (const QString &id : ids)
        applyLayout(id);
    m_shownId.clear(); // focus returns to the selected session
    showSelected();
}

void MainWindow::activateDashboardCell(int i)
{
    const QString ref = m_dashCells.value(i).ref;
    if (ref.isEmpty())
        return;
    closeDashboard();
    const QString id = ref.section(QLatin1Char(':'), 0, 0);
    m_store.select(id);
    if (QWidget *w = paneWidget(id, ref.section(QLatin1Char(':'), 1)); w && isActiveWindow())
        w->setFocus(); // that exact pane
}

void MainWindow::refreshDashboard()
{
    const bool pulse = m_dashBlinkOn;
    const QColor accent = palette().color(QPalette::Highlight), mid = palette().color(QPalette::Mid);
    for (int i = 0; i < m_dashCells.size(); ++i) {
        const DashCell &c = m_dashCells[i];
        const Session *s = m_store.session(c.ref.section(QLatin1Char(':'), 0, 0));
        if (!s)
            continue;
        c.frame->setStyleSheet(QStringLiteral("QFrame#dashCell{border:%1px solid %2;border-radius:6px;}")
                                   .arg(i == m_dashHighlight ? 2 : 1)
                                   .arg((i == m_dashHighlight ? accent : mid).name()));
        // the name chip carries the agent status colour, pulsing while the status blinks
        QColor fill = s->status == QLatin1String("idle") ? palette().color(QPalette::AlternateBase) : statusColor(m_store.settings, s->status);
        if (s->blink && pulse)
            fill.setAlphaF(0.45);
        const QColor ink = qGray(fill.rgb()) > 140 && fill.alphaF() > 0.6 ? Qt::black : palette().color(QPalette::WindowText);
        c.chip->setText(s->displayName() + (c.ref.endsWith(QLatin1String(":right")) ? tr(" · right") : QString()));
        c.chip->setStyleSheet(QStringLiteral("QLabel{background:%1;color:%2;border-radius:8px;}").arg(fill.name(QColor::HexArgb), ink.name()));
    }
}

void MainWindow::dashboardForget(const QString &id)
{
    // a member's session or pane went away: rebuild from the survivors, or close when none remain
    bool member = false;
    for (const DashCell &c : std::as_const(m_dashCells))
        member |= c.ref.startsWith(id + QLatin1Char(':'));
    if (!m_dashboard || !member)
        return;
    QStringList refs;
    for (const DashCell &c : std::as_const(m_dashCells))
        refs << c.ref;
    const QString mode = m_dashFontMode;
    const double size = m_dashFontSize;
    closeDashboard();
    QTimer::singleShot(0, this, [this, refs, mode, size] {
        QStringList alive;
        for (const QString &r : refs)
            if (paneWidget(r.section(QLatin1Char(':'), 0, 0), r.section(QLatin1Char(':'), 1)) && !alive.contains(r))
                alive << r;
        QString error;
        if (!alive.isEmpty())
            openDashboard(alive, mode, size, &error);
    });
}

bool MainWindow::dashboardEvent(QObject *watched, QEvent *event)
{
    auto *w = qobject_cast<QWidget *>(watched);
    if (!m_dashboard || !w || (w != m_dashboard && !m_dashboard->isAncestorOf(w)))
        return false;
    switch (event->type()) {
    case QEvent::MouseButtonPress:
        for (int i = 0; i < m_dashCells.size(); ++i)
            if (m_dashCells[i].frame == w || m_dashCells[i].frame->isAncestorOf(w)) {
                QTimer::singleShot(0, this, [this, i] { activateDashboardCell(i); }); // after the press, not inside it
                break;
            }
        return true;
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove:
    case QEvent::Wheel:
        return true; // view-only
    case QEvent::KeyPress: {
        auto *key = static_cast<QKeyEvent *>(event);
        if (isOwnShortcut(key))
            return false;
        const int n = int(m_dashCells.size()), cols = dashboardGrid(n).first;
        int to = m_dashHighlight;
        switch (key->key()) {
        case Qt::Key_Escape:
            closeDashboard();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            activateDashboardCell(m_dashHighlight);
            return true;
        case Qt::Key_Left:
            to = m_dashHighlight % cols ? m_dashHighlight - 1 : m_dashHighlight;
            break;
        case Qt::Key_Right:
            to = (m_dashHighlight + 1) % cols ? m_dashHighlight + 1 : m_dashHighlight;
            break;
        case Qt::Key_Up:
            to = m_dashHighlight - cols;
            break;
        case Qt::Key_Down:
            to = m_dashHighlight + cols;
            break;
        default:
            return true;
        }
        if (to >= 0 && to < n) { // no wrap; an edge or a ragged-row gap keeps the highlight
            m_dashHighlight = to;
            refreshDashboard();
        }
        return true;
    }
    default:
        return false;
    }
}

bool MainWindow::dashboardCommand(const QJsonObject &args, QJsonObject *result, QString *error)
{
    if (args.value("close").toBool()) {
        if (!m_dashboard)
            return *error = QStringLiteral("no dashboard"), false;
        closeDashboard();
        return true;
    }
    QStringList cells;
    for (const auto &v : args.value("cells").toArray())
        cells << v.toString();
    if (!openDashboard(cells, args.value("fontMode").toString(), args.value("fontSize").toDouble(), error))
        return false;
    result->insert("members", dashboardState().value("dashboardMembers"));
    return true;
}

QJsonObject MainWindow::dashboardState() const
{
    if (!m_dashboard)
        return {};
    QJsonArray members;
    for (const DashCell &c : m_dashCells)
        members << c.ref;
    QJsonObject out{{"dashboardMembers", members}, {"dashboardHighlighted", m_dashCells.value(m_dashHighlight).ref}, {"dashboardFontMode", m_dashFontMode}};
    if (m_dashFontMode == QLatin1String("fixed"))
        out.insert("dashboardFontSize", m_dashFontSize);
    return out;
}

void MainWindow::toggleDashboard()
{
    if (m_dashboard)
        return closeDashboard();
    QString notes, error;
    const QStringList cells = dashboardCells(m_store, {}, true, &notes, &error);
    if (cells.isEmpty() || !openDashboard(cells, QStringLiteral("auto"), 0, &error))
        statusBar()->showMessage(error, 2000);
}

void MainWindow::syncDashboardButton()
{
    if (QAction *a = m_chrome.value(QStringLiteral("dashboard")))
        a->setIcon(chromeIcon(m_dashboard ? QStringLiteral("gridFilled") : QStringLiteral("grid"), palette().color(QPalette::WindowText)));
}
