#include "sidebar.h"

#include "store.h"

#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QFileInfo>
#include <QHash>
#include <QLineEdit>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QSvgRenderer>
#include <QtMath>

using namespace SidebarRole;

QColor statusColor(const Settings &settings, const QString &status)
{
    // agterm's defaults: active #DBD9E6, blocked system orange, completed system green
    const QHash<QString, std::pair<QString, QColor>> table{{QStringLiteral("active"), {settings.activeStatusColorHex, QColor(0xDB, 0xD9, 0xE6)}},
                                                           {QStringLiteral("blocked"), {settings.blockedStatusColorHex, QColor(0xFF, 0x9F, 0x0A)}},
                                                           {QStringLiteral("completed"), {settings.completedStatusColorHex, QColor(0x30, 0xD1, 0x58)}}};
    const auto [hex, fallback] = table.value(status);
    return QColor::isValidColorName(hex) ? QColor(hex) : fallback;
}

QString statusShape(const Settings &settings, const QString &status)
{
    const QString shape = status == QLatin1String("active")      ? settings.activeStatusShape
                          : status == QLatin1String("blocked")   ? settings.blockedStatusShape
                          : status == QLatin1String("completed") ? settings.completedStatusShape
                                                                 : QString();
    return shape.isEmpty() ? QStringLiteral("circle") : shape;
}

void drawStatusGlyph(QPainter *p, const QPointF &c, qreal r, const QString &shape)
{
    if (shape == QLatin1String("square")) {
        p->drawRoundedRect(QRectF(c.x() - r * 0.85, c.y() - r * 0.85, r * 1.7, r * 1.7), r * 0.3, r * 0.3);
    } else if (shape == QLatin1String("capsule")) {
        p->drawRoundedRect(QRectF(c.x() - r, c.y() - r * 0.6, r * 2, r * 1.2), r * 0.6, r * 0.6);
    } else if (shape == QLatin1String("triangle") || shape == QLatin1String("diamond") || shape == QLatin1String("star")) {
        QPolygonF poly;
        const int points = shape == QLatin1String("triangle") ? 3 : shape == QLatin1String("diamond") ? 4 : 10;
        for (int i = 0; i < points; ++i) {
            const qreal a = -M_PI / 2 + i * 2 * M_PI / points, radius = shape == QLatin1String("star") && i % 2 ? r * 0.45 : r * 1.1;
            poly << c + QPointF(radius * qCos(a), radius * qSin(a) + (points == 3 ? r * 0.2 : 0));
        }
        p->drawPolygon(poly);
    } else {
        p->drawEllipse(c, r, r);
    }
}

static int attentionRank(const QString &status)
{
    return status == QLatin1String("blocked") ? 0 : status == QLatin1String("active") ? 1 : 2;
}

static const char *sessionsMime = "application/x-agterm-sessions";
static const char *workspaceMime = "application/x-agterm-workspace";

namespace {
QColor textColor()
{
    return QApplication::palette().color(QPalette::WindowText);
}

void drawTerminalGlyph(QPainter *p, const QRectF &r, const QColor &c, bool filled)
{
    const QRectF box = r.adjusted(0.5, r.height() * 0.15, -0.5, -r.height() * 0.15);
    p->setPen(QPen(c, 1.2));
    p->setBrush(filled ? QBrush(c) : Qt::NoBrush);
    p->drawRoundedRect(box, 2.5, 2.5);
    const QColor ink = filled ? QApplication::palette().color(QPalette::Window) : c;
    QPainterPath prompt;
    const qreal x = box.left() + box.width() * 0.22, y = box.center().y();
    prompt.moveTo(x, y - 2.2);
    prompt.lineTo(x + 2.2, y);
    prompt.lineTo(x, y + 2.2);
    prompt.moveTo(x + 3.6, y + 2.4);
    prompt.lineTo(x + 6.6, y + 2.4);
    p->setPen(QPen(ink, 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p->setBrush(Qt::NoBrush);
    p->drawPath(prompt);
}

void drawGridGlyph(QPainter *p, const QRectF &r, const QColor &c, bool heavy = false)
{
    // heavy: agterm draws a workspace in the focus set at black weight
    const qreal g = 2, s = (r.width() - g) / 2;
    p->setPen(QPen(c, heavy ? 2.2 : 1.1));
    p->setBrush(Qt::NoBrush);
    for (int i = 0; i < 4; ++i)
        p->drawRoundedRect(QRectF(r.left() + (i % 2) * (s + g), r.top() + (i / 2) * (s + g), s, s).adjusted(0.5, 0.5, -0.5, -0.5), 1.5, 1.5);
}

QImage tintedLogo(const QString &id, const QColor &color, int px)
{
    static QHash<QString, QImage> cache;
    const QString key = QStringLiteral("%1/%2/%3").arg(id, color.name(QColor::HexArgb)).arg(px);
    if (auto it = cache.constFind(key); it != cache.cend())
        return *it;
    QImage img(px, px, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QSvgRenderer svg(QStringLiteral(":/launchers/%1.svg").arg(id));
    QPainter p(&img);
    svg.render(&p, QRectF(0, 0, px, px));
    p.setCompositionMode(QPainter::CompositionMode_SourceIn); // template image: keep alpha, take the colour
    p.fillRect(img.rect(), color);
    p.end();
    cache.insert(key, img);
    return img;
}

void drawLauncher(QPainter *p, const QRectF &r, const QString &id, const QColor &c, bool filled)
{
    if (id.isEmpty() || id == QLatin1String("terminal")) {
        drawTerminalGlyph(p, r, c, filled);
        return;
    }
    const qreal dpr = p->device()->devicePixelRatioF();
    QImage logo = tintedLogo(id, c, qRound(r.width() * dpr));
    logo.setDevicePixelRatio(dpr);
    p->drawImage(r.topLeft(), logo);
}
} // namespace

QColor tintColor(const QString &name)
{
    // systemRed..systemGray, light and dark appearance
    static const QHash<QString, std::pair<QRgb, QRgb>> colors{
        {QStringLiteral("red"), {0xFF3B30, 0xFF453A}},    {QStringLiteral("orange"), {0xFF9500, 0xFF9F0A}},
        {QStringLiteral("yellow"), {0xFFCC00, 0xFFD60A}}, {QStringLiteral("green"), {0x34C759, 0x30D158}},
        {QStringLiteral("blue"), {0x007AFF, 0x0A84FF}},   {QStringLiteral("purple"), {0xAF52DE, 0xBF5AF2}},
        {QStringLiteral("gray"), {0x8E8E93, 0x98989D}}};
    const auto it = colors.constFind(name);
    if (it == colors.cend())
        return {};
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    return QColor(dark ? it->second : it->first);
}

QIcon launcherIcon(const QString &launcherId, const QColor &color)
{
    QPixmap pm(QSize(32, 32));
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(2, 2);
    drawLauncher(&p, QRectF(0, 0, 16, 16), launcherId, color, false);
    p.end();
    pm.setDevicePixelRatio(2);
    return QIcon(pm);
}

QIcon chromeIcon(const QString &name, const QColor &color)
{
    QPixmap pm(QSize(40, 40));
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(2, 2);
    p.setPen(QPen(color, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (name == QLatin1String("clock")) { // the title bar's recent sessions
        p.drawEllipse(QRectF(3, 3, 14, 14));
        p.drawPolyline(QList<QPointF>{{10, 6}, {10, 10}, {13, 12}});
        p.end();
        pm.setDevicePixelRatio(2);
        return QIcon(pm);
    }
    if (name.startsWith(QLatin1String("grid"))) {
        // the footer's workspace filter: square.grid.2x2, filled while the filter applies
        if (name == QLatin1String("gridFilled"))
            for (int i = 0; i < 4; ++i)
                p.fillRect(QRectF(3.5 + (i % 2) * 7, 3.5 + (i / 2) * 7, 6, 6), color);
        else
            drawGridGlyph(&p, QRectF(3, 3, 14, 14), color, false);
        p.end();
        pm.setDevicePixelRatio(2);
        return QIcon(pm);
    }
    if (name == QLatin1String("filepanel")) {
        // agterm's "folder" symbol: a tab on the left of the lid
        QPainterPath folder;
        folder.moveTo(2.5, 15);
        folder.lineTo(2.5, 5.5);
        folder.quadTo(2.5, 4.5, 3.5, 4.5);
        folder.lineTo(7.5, 4.5);
        folder.lineTo(9, 6.5);
        folder.lineTo(16.5, 6.5);
        folder.quadTo(17.5, 6.5, 17.5, 7.5);
        folder.lineTo(17.5, 15);
        folder.quadTo(17.5, 16, 16.5, 16);
        folder.lineTo(3.5, 16);
        folder.quadTo(2.5, 16, 2.5, 15);
        p.drawPath(folder);
        p.drawLine(QPointF(2.5, 8.5), QPointF(17.5, 8.5));
        p.end();
        pm.setDevicePixelRatio(2);
        return QIcon(pm);
    }
    const QRectF box(2.5, 4.5, 15, 11);
    p.drawRoundedRect(box, 2.5, 2.5);
    if (name == QLatin1String("sidebar"))
        p.drawLine(QPointF(7.5, 4.5), QPointF(7.5, 15.5));
    else if (name == QLatin1String("split"))
        p.drawLine(QPointF(10, 4.5), QPointF(10, 15.5));
    else if (name == QLatin1String("splitH"))
        p.drawLine(QPointF(2.5, 10), QPointF(17.5, 10));
    p.end();
    pm.setDevicePixelRatio(2);
    return QIcon(pm);
}

SidebarDelegate::SidebarDelegate(Sidebar *sidebar)
    : QStyledItemDelegate(sidebar), m_sidebar(sidebar)
{
}

void SidebarDelegate::paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &index) const
{
    // QApplication's palette, not opt.palette: a stylesheet background would make the view's palette transparent
    const QPalette pal = QApplication::palette();
    const QColor text = pal.color(QPalette::WindowText);
    QColor dim = text;
    dim.setAlpha(170);
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(opt.rect).adjusted(6, 1, -6, -1);
    const bool session = index.data(Kind).toInt() == SessionRow;
    const bool flat = m_sidebar->flatList();
    // a row tint colours the parts it lists, selected or not
    const QColor tint = tintColor(index.data(TintColor).toString());
    const QStringList tintOn = index.data(TintOn).toStringList();
    const QColor iconColor = tint.isValid() && tintOn.contains(QLatin1String("icon")) ? tint : dim;
    const QColor nameColor = tint.isValid() && tintOn.contains(QLatin1String("name")) ? tint : text;

    // "background": a light wash; a workspace's also covers its open sessions, drawn as one block
    auto wash = [&](const QModelIndex &i) {
        return i.data(TintOn).toStringList().contains(QLatin1String("background")) ? tintColor(i.data(TintColor).toString()) : QColor();
    };
    auto block = [&](QColor c, bool openTop, bool openBottom) {
        c.setAlpha(22);
        p->save();
        p->setClipRect(opt.rect); // an open end runs past the row so stacked rows join seamlessly
        p->setPen(Qt::NoPen);
        p->setBrush(c);
        p->drawRoundedRect(QRectF(opt.rect).adjusted(6, openTop ? -8 : 1, -6, openBottom ? 8 : -1), 6, 6);
        p->restore();
    };
    const QColor own = wash(index), group = session ? wash(index.parent()) : QColor();
    if (!session && own.isValid())
        block(own, false, index.data(Expanded).toBool() && index.model()->rowCount(index) > 0);
    else if (group.isValid())
        block(group, true, index.row() < index.model()->rowCount(index.parent()) - 1);
    if (session && own.isValid())
        block(own, false, false);

    if (const QColor washed = own.isValid() ? own : group; washed.isValid() && (opt.state & QStyle::State_Selected)) {
        // on a wash a highlight fill muddies the colour: outline the row in the wash's own colour instead
        p->setPen(QPen(washed, m_sidebar->hasFocus() ? 1.6 : 1.2));
        p->setBrush(Qt::NoBrush);
        p->drawRoundedRect(r.adjusted(2, 1, -2, -1), 5, 5);
    } else if (opt.state & QStyle::State_Selected) {
        QColor sel = pal.color(QPalette::Highlight);
        sel.setAlpha(m_sidebar->hasFocus() ? 110 : 80);
        p->setPen(Qt::NoPen);
        p->setBrush(sel);
        p->drawRoundedRect(r, 6, 6);
    } else if (opt.state & QStyle::State_MouseOver) {
        QColor hover = text;
        hover.setAlpha(16);
        p->setPen(Qt::NoPen);
        p->setBrush(hover);
        p->drawRoundedRect(r, 6, 6);
    }

    qreal x = r.left() + 6;
    const qreal cy = r.center().y();
    if (!session) {
        QPainterPath chevron;
        if (index.data(Expanded).toBool()) {
            chevron.moveTo(x + 1, cy - 1.8);
            chevron.lineTo(x + 4.5, cy + 1.8);
            chevron.lineTo(x + 8, cy - 1.8);
        } else {
            chevron.moveTo(x + 2.7, cy - 3.5);
            chevron.lineTo(x + 6.3, cy);
            chevron.lineTo(x + 2.7, cy + 3.5);
        }
        p->setPen(QPen(dim, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p->drawPath(chevron);
        x += 14;
        if (m_sidebar->settings().shows(QStringLiteral("workspaceIcons"))) {
            drawGridGlyph(p, QRectF(x, cy - 7, 14, 14), iconColor, index.data(Focused).toBool());
            x += 21;
        } else {
            x += 4;
        }
    } else {
        x += flat ? 2 : 22;
        if (index.data(Companion).toBool() && !flat)
            x += 18; // indented under its parent
        if (index.data(HasCompanions).toBool() && !flat) {
            // the parent's own disclosure chevron folds its companions away
            QPainterPath chevron;
            const qreal cx = x - 13;
            if (index.data(Expanded).toBool()) {
                chevron.moveTo(cx - 3.5, cy - 1.8);
                chevron.lineTo(cx, cy + 1.8);
                chevron.lineTo(cx + 3.5, cy - 1.8);
            } else {
                chevron.moveTo(cx - 1.8, cy - 3.5);
                chevron.lineTo(cx + 1.8, cy);
                chevron.lineTo(cx - 1.8, cy + 3.5);
            }
            p->setPen(QPen(dim, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p->setBrush(Qt::NoBrush);
            p->drawPath(chevron);
            if (index.data(FoldedAttention).toBool()) {
                p->setPen(Qt::NoPen);
                p->setBrush(statusColor(m_sidebar->settings(), QStringLiteral("blocked")));
                p->drawEllipse(QPointF(x + 15, cy - 6), 3, 3); // a hidden companion needs you
            }
        }
        if (m_sidebar->settings().shows(QStringLiteral("sessionIcons"))) {
            drawLauncher(p, QRectF(x, cy - 8, 16, 16), index.data(Agent).toString(), iconColor, index.data(Flagged).toBool());
            x += 23;
        }
    }

    qreal right = r.right() - 8;
    // hover-only: the launcher menu "+" and, left of it, a repeat of the last choice
    const bool buttons = !session && m_sidebar->hovered() == index && m_sidebar->settings().shows(QStringLiteral("workspaceAddSession"));
    // a collapsed workspace rolls up its sessions' status and badge, as master does; an open one leaves them to its rows
    if (session || (!buttons && !index.data(Expanded).toBool())) {
        const QString status = index.data(Status).toString();
        if (session && m_sidebar->hovered() == index && m_sidebar->closable(index)) {
            // hover-only: a finished session's glyph turns into its close button
            const QPointF c(right - 6, cy); // the status glyph's own centre; QRect::center() rounds up a pixel
            p->setPen(QPen(dim, 1.4, Qt::SolidLine, Qt::RoundCap));
            p->drawLine(c + QPointF(-4, -4), c + QPointF(4, 4));
            p->drawLine(c + QPointF(-4, 4), c + QPointF(4, -4));
            right -= 20;
        } else if (status != QLatin1String("idle")) {
            QColor c = statusColor(m_sidebar->settings(), status);
            if (index.data(Blink).toBool())
                c.setAlpha(m_sidebar->blinkAlpha());
            p->setPen(Qt::NoPen);
            p->setBrush(c);
            drawStatusGlyph(p, QPointF(right - 6, cy), 6, statusShape(m_sidebar->settings(), status));
            right -= 20;
        }
        if (const int unseen = index.data(Unseen).toInt(); unseen > 0 && m_sidebar->settings().notificationBadgeEnabled) {
            QFont f = opt.font;
            f.setPointSizeF(f.pointSizeF() * 0.8);
            f.setWeight(QFont::DemiBold);
            const QString n = unseen > 99 ? QStringLiteral("99+") : QString::number(unseen);
            const qreal w = qMax<qreal>(18, QFontMetricsF(f).horizontalAdvance(n) + 10);
            const QRectF pill(right - w, cy - 9, w, 18);
            p->setPen(Qt::NoPen);
            p->setBrush(QColor(0xFF, 0x45, 0x3A)); // the system red of agterm's count badge
            p->drawRoundedRect(pill, 9, 9);
            p->setPen(Qt::white);
            p->setFont(f);
            p->drawText(pill, Qt::AlignCenter, n);
            right -= w + 6;
        }
    } else if (buttons) {
        const QRect add = Sidebar::addButtonRect(opt.rect), rep = Sidebar::repeatButtonRect(opt.rect);
        p->setPen(QPen(dim, 1.4, Qt::SolidLine, Qt::RoundCap));
        const QPointF c = QRectF(add).center();
        p->drawLine(QPointF(c.x() - 5, c.y()), QPointF(c.x() + 5, c.y()));
        p->drawLine(QPointF(c.x(), c.y() - 5), QPointF(c.x(), c.y() + 5));
        drawLauncher(p, QRectF(rep).adjusted(3, 3, -3, -3), m_sidebar->lastLauncher(), dim, false);
        right = rep.left() - 4;
    }

    p->setPen(nameColor);
    p->setFont(opt.font);
    const QRectF label(x, r.top(), right - x, r.height());
    const QString suffix = index.data(WorkspaceLabel).toString().isEmpty() ? QString() : QStringLiteral(" : ") + index.data(WorkspaceLabel).toString();
    const QString name = opt.fontMetrics.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, int(label.width()));
    p->drawText(label, Qt::AlignVCenter | Qt::AlignLeft, name);
    if (!suffix.isEmpty()) { // flat layout: "session : workspace", the workspace part dimmed
        p->setPen(dim);
        const QRectF rest = label.adjusted(opt.fontMetrics.horizontalAdvance(name), 0, 0, 0);
        p->drawText(rest, Qt::AlignVCenter | Qt::AlignLeft, opt.fontMetrics.elidedText(suffix, Qt::ElideRight, int(rest.width())));
    }
    p->restore();
}

QSize SidebarDelegate::sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &) const
{
    return QSize(200, opt.fontMetrics.height() + 16);
}

void SidebarDelegate::updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &opt, const QModelIndex &index) const
{
    const Settings &st = m_sidebar->settings();
    const int left = index.data(Kind).toInt() == SessionRow ? (m_sidebar->flatList() ? 37 : 57) - (st.shows(QStringLiteral("sessionIcons")) ? 0 : 23)
                                                            : 47 - (st.shows(QStringLiteral("workspaceIcons")) ? 0 : 17);
    editor->setGeometry(opt.rect.adjusted(left, 3, -12, -3));
}

void SidebarDelegate::setEditorData(QWidget *editor, const QModelIndex &index) const
{
    auto *line = qobject_cast<QLineEdit *>(editor);
    line->setText(index.data(Qt::DisplayRole).toString());
    line->selectAll();
}

Sidebar::Sidebar(Store &store, QWidget *parent)
    : QTreeWidget(parent), m_store(store)
{
    Q_INIT_RESOURCE(resources); // the logos live in a static library, which the linker would otherwise drop
    setHeaderHidden(true);
    setRootIsDecorated(false);
    setIndentation(0);
    setItemDelegate(new SidebarDelegate(this));
    setMouseTracking(true);
    setFrameShape(QFrame::NoFrame);
    setExpandsOnDoubleClick(false);
    setEditTriggers(NoEditTriggers);
    setSelectionMode(ExtendedSelection);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setFocusPolicy(Qt::NoFocus); // the keyboard stays in the terminal
    setVerticalScrollMode(ScrollPerPixel);
    setAnimated(false); // a style (Breeze) may animate expansion, which a rebuild would replay
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(false);
    setDragDropMode(DragDrop);
    viewport()->setAcceptDrops(true);
    viewport()->setAttribute(Qt::WA_Hover);

    connect(this, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item) {
        if (m_rebuilding)
            return;
        const bool isSession = item->data(0, Kind).toInt() == SessionRow;
        Q_EMIT renameRequested(isSession ? QStringLiteral("session") : QStringLiteral("workspace"), item->data(0, Id).toString(),
                               item->text(0));
    });
    m_blinkTimer.setInterval(33);
    connect(&m_blinkTimer, &QTimer::timeout, this, [this] {
        // master's glyph: opacity 1.0 -> 0.2 over 0.5 s, autoreversed (a 1 s cycle)
        m_blinkPhase = (m_blinkPhase + 1) % 30;
        m_blinkAlpha = 51 + int(204 * (0.5 + 0.5 * qCos(m_blinkPhase * M_PI / 15)));
        viewport()->update();
    });
}

QString Sidebar::lastLauncher() const
{
    return m_store.settings.lastLauncher;
}

const Settings &Sidebar::settings() const
{
    return m_store.settings;
}

QRect Sidebar::addButtonRect(const QRect &row)
{
    return QRect(row.right() - 34, row.center().y() - 11, 22, 22);
}

QRect Sidebar::closeButtonRect(const QRect &row)
{
    return QRect(row.right() - 31, row.center().y() - 11, 22, 22); // centred on the status glyph it replaces
}

QRect Sidebar::repeatButtonRect(const QRect &row)
{
    return QRect(row.right() - 62, row.center().y() - 11, 22, 22);
}

void Sidebar::setFlaggedView(bool on)
{
    m_store.flaggedView = on;
    rebuild();
}

bool Sidebar::flaggedView() const
{
    return m_store.flaggedView;
}

bool Sidebar::flatList() const
{
    return m_store.flaggedView && m_store.settings.flaggedViewLayout != QLatin1String("tree");
}

QStringList Sidebar::selectedSessionIds() const
{
    QStringList ids;
    for (auto *item : selectedItems())
        if (item->data(0, Kind).toInt() == SessionRow)
            ids << item->data(0, Id).toString();
    return ids;
}

QStringList Sidebar::structure() const
{
    // the rows and their order; anything else about a row is updated in place
    QStringList rows{m_store.flaggedView ? QStringLiteral("flagged:") + m_store.settings.flaggedViewLayout : QStringLiteral("tree")};
    for (const auto &w : m_store.workspaces) {
        if (m_store.flaggedView) {
            QStringList flagged;
            for (const auto &s : w.sessions)
                if (s.flagged)
                    flagged << s.id;
            if (!flatList() && !flagged.isEmpty()) // the tree layout leaves out workspaces holding nothing flagged
                rows << QLatin1Char('w') + w.id;
            rows << flagged;
            continue;
        }
        if (m_store.focusEnabled && !m_store.focused.contains(w.id))
            continue;
        rows << QLatin1Char('w') + w.id;
        for (const auto &s : w.sessions)
            if (s.parentId.isEmpty() || !m_store.collapsed.contains(s.parentId)) // a folded companion has no row
                rows << s.id;
    }
    return rows;
}

void Sidebar::rebuild()
{
    if (state() == EditingState)
        return; // a rename in progress keeps its row; the commit rebuilds
    m_rebuilding = true;
    const int scroll = verticalScrollBar()->value();
    // a multi-selection survives a rebuild only while it still contains the current session
    QStringList keepSelected = selectedSessionIds();
    if (!keepSelected.contains(m_store.selected))
        keepSelected.clear();
    // recreating the items on every change (a selection, a status) flickers and replays the expand animation
    const QStringList rows = structure();
    const bool reuse = rows == m_structure;
    m_structure = rows;
    if (!reuse) {
        setUpdatesEnabled(false);
        clear();
    }
    int top = 0;
    auto topItem = [&] { return reuse ? topLevelItem(top++) : new QTreeWidgetItem(this); };
    bool blinking = false;
    auto sessionItem = [&](QTreeWidgetItem *item, const ::Session &s) {
        item->setText(0, s.displayName());
        item->setData(0, Id, s.id);
        item->setData(0, Kind, SessionRow);
        item->setData(0, Status, s.status);
        item->setData(0, Blink, s.blink);
        item->setData(0, Agent, s.agent);
        item->setData(0, Flagged, s.flagged);
        item->setData(0, Unseen, s.unseen);
        item->setData(0, Busy, s.busy);
        item->setData(0, TintColor, s.tint.color);
        item->setData(0, TintOn, s.tint.on);
        item->setToolTip(0, s.title.isEmpty() ? s.cwd : s.title + QLatin1Char('\n') + s.cwd);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable | Qt::ItemIsDragEnabled);
        blinking |= s.blink;
        if (s.id == m_store.selected)
            setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
        item->setSelected(s.id == m_store.selected || keepSelected.contains(s.id));
    };
    for (const auto &w : m_store.workspaces) {
        if (flatList()) {
            for (const auto &s : w.sessions)
                if (s.flagged) {
                    QTreeWidgetItem *item = topItem();
                    sessionItem(item, s);
                    item->setData(0, WorkspaceLabel, m_store.settings.flaggedViewLayout == QLatin1String("plain") ? QString() : w.name);
                }
            continue;
        }
        const bool flaggedTree = m_store.flaggedView;
        if (flaggedTree && std::none_of(w.sessions.begin(), w.sessions.end(), [](const ::Session &s) { return s.flagged; }))
            continue;
        if (!flaggedTree && m_store.focusEnabled && !m_store.focused.contains(w.id))
            continue;
        QTreeWidgetItem *wi = topItem();
        int child = 0;
        // the most urgent rendered child's status (blocked > active > completed), the first on a tie
        const ::Session *urgent = nullptr;
        int unseen = 0;
        for (const auto &s : w.sessions)
            if (!flaggedTree || s.flagged) {
                unseen += s.unseen;
                if (s.status != QLatin1String("idle") && (!urgent || attentionRank(s.status) < attentionRank(urgent->status)))
                    urgent = &s;
            }
        wi->setData(0, Status, urgent ? urgent->status : QStringLiteral("idle"));
        wi->setData(0, Blink, urgent && urgent->blink);
        wi->setData(0, Unseen, unseen);
        wi->setText(0, w.name);
        wi->setData(0, Focused, m_store.focused.contains(w.id));
        wi->setData(0, Id, w.id);
        wi->setData(0, Kind, WorkspaceRow);
        wi->setData(0, TintColor, w.tint.color);
        wi->setData(0, TintOn, w.tint.on);
        wi->setToolTip(0, w.folder);
        wi->setFlags(Qt::ItemIsEnabled | Qt::ItemIsEditable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
        for (const auto &s : w.sessions) {
            if (flaggedTree ? !s.flagged : !s.parentId.isEmpty() && m_store.collapsed.contains(s.parentId))
                continue; // not flagged, or folded under its parent
            QTreeWidgetItem *item = reuse ? wi->child(child++) : new QTreeWidgetItem(wi);
            sessionItem(item, s);
            item->setData(0, Companion, !s.parentId.isEmpty());
            bool hasCompanions = false, attention = false;
            for (const auto &c : w.sessions)
                if (c.parentId == s.id) {
                    hasCompanions = true;
                    attention |= c.unseen > 0 || c.status == QLatin1String("blocked") || c.status == QLatin1String("completed");
                }
            item->setData(0, HasCompanions, hasCompanions && !flaggedTree); // the flagged tree lists companions on their own
            item->setData(0, Expanded, !m_store.collapsed.contains(s.id));
            item->setData(0, FoldedAttention, attention && m_store.collapsed.contains(s.id));
        }
        wi->setExpanded(!m_store.collapsed.contains(w.id) || m_store.isSoleFocus(w.id)); // a zoomed-to workspace is always open
        wi->setData(0, Expanded, wi->isExpanded());
    }
    if (!reuse) {
        verticalScrollBar()->setValue(scroll);
        setUpdatesEnabled(true);
    }
    viewport()->update();
    // start() on a running timer restarts it, so rebuilds faster than the interval would freeze the fade
    if (!blinking)
        m_blinkTimer.stop(), m_blinkAlpha = 255;
    else if (!m_blinkTimer.isActive())
        m_blinkTimer.start();
    m_rebuilding = false;
}

void Sidebar::drawRow(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    // the base drawRow is what normally adds per-row selection and hover state
    QStyleOptionViewItem o = option;
    o.state.setFlag(QStyle::State_Selected, selectionModel()->isSelected(index));
    o.state.setFlag(QStyle::State_MouseOver, index == m_hovered);
    itemDelegateForIndex(index)->paint(painter, o, index);
}

void Sidebar::mouseMoveEvent(QMouseEvent *event)
{
    const QModelIndex at = indexAt(event->position().toPoint());
    if (at != m_hovered) {
        m_hovered = at;
        viewport()->update();
    }
    QTreeWidget::mouseMoveEvent(event);
}

void Sidebar::leaveEvent(QEvent *event)
{
    m_hovered = {};
    viewport()->update();
    QTreeWidget::leaveEvent(event);
}

void Sidebar::mousePressEvent(QMouseEvent *event)
{
    const QPoint pos = event->position().toPoint();
    QTreeWidgetItem *item = itemAt(pos);
    if (!item && m_store.flaggedView && !topLevelItemCount() && event->button() == Qt::LeftButton)
        return Q_EMIT editFlaggedRequested();
    if (!item)
        return; // a click on empty space would clear the selection; the shown session always stays selected
    if (item && item->data(0, HasCompanions).toBool() && !m_store.flaggedView && event->button() == Qt::LeftButton
        && pos.x() < visualItemRect(item).left() + 34) {
        const QString id = item->data(0, Id).toString(); // the parent's chevron folds its companions
        m_store.collapsed.contains(id) ? m_store.collapsed.remove(id) : (m_store.collapsed.insert(id), true);
        rebuild();
        return;
    }
    if (isCloseButton(item, pos) && event->button() == Qt::LeftButton)
        return Q_EMIT closeRequested(item->data(0, Id).toString());
    if (item && item->data(0, Kind).toInt() == WorkspaceRow && event->button() == Qt::LeftButton) {
        const QRect row = visualItemRect(item);
        const QString id = item->data(0, Id).toString();
        const bool buttons = m_store.settings.shows(QStringLiteral("workspaceAddSession"));
        if (buttons && addButtonRect(row).contains(pos)) {
            Q_EMIT launcherMenuRequested(id, viewport()->mapToGlobal(addButtonRect(row).bottomLeft() + QPoint(0, 4)));
            return;
        }
        if (buttons && repeatButtonRect(row).contains(pos)) {
            Q_EMIT repeatRequested(id);
            return;
        }
        // a click on the row folds or unfolds it, as agterm's default does; with that off only the chevron does
        if (!m_store.settings.workspaceRowClickExpands && pos.x() > row.left() + 24)
            return QTreeWidget::mousePressEvent(event);
        m_store.collapsed.contains(id) ? m_store.collapsed.remove(id) : (m_store.collapsed.insert(id), true);
        item->setExpanded(!m_store.collapsed.contains(id));
        item->setData(0, Expanded, item->isExpanded());
    }
    QTreeWidget::mousePressEvent(event);
}

bool Sidebar::isCloseButton(QTreeWidgetItem *item, const QPoint &pos) const
{
    return item && item->data(0, Kind).toInt() == SessionRow && closable(indexFromItem(item))
        && closeButtonRect(visualItemRect(item)).contains(pos);
}

// the close X shows only on a quiet row: no attention mark, and no plain command running (an agent's own status speaks for it)
bool Sidebar::closable(const QModelIndex &i) const
{
    const QString status = i.data(Status).toString();
    if (status == QLatin1String("active") || status == QLatin1String("blocked") || i.data(FoldedAttention).toBool()
        || (i.data(Unseen).toInt() > 0 && settings().notificationBadgeEnabled))
        return false;
    return !i.data(Agent).toString().isEmpty() || !i.data(Busy).toBool();
}

QItemSelectionModel::SelectionFlags Sidebar::selectionCommand(const QModelIndex &index, const QEvent *event) const
{
    // the shown session always stays selected: a workspace row or a Ctrl-click on it would deselect it
    const QItemSelectionModel::SelectionFlags command = QTreeWidget::selectionCommand(index, event);
    if (!(index.flags() & Qt::ItemIsSelectable)
        || (index.data(Id).toString() == m_store.selected && command & (QItemSelectionModel::Toggle | QItemSelectionModel::Deselect)))
        return QItemSelectionModel::NoUpdate;
    return command;
}

void Sidebar::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (QTreeWidgetItem *item = itemAt(event->position().toPoint())) {
        if (item->data(0, Kind).toInt() == WorkspaceRow && (addButtonRect(visualItemRect(item)).contains(event->position().toPoint())
                                                         || repeatButtonRect(visualItemRect(item)).contains(event->position().toPoint())))
            return;
        if (isCloseButton(item, event->position().toPoint()))
            return;
        editItem(item); // inline rename, as in agterm
        return;
    }
    QTreeWidget::mouseDoubleClickEvent(event);
}

void Sidebar::startDrag(Qt::DropActions)
{
    auto *mime = new QMimeData;
    QTreeWidgetItem *item = currentItem();
    if (item && item->data(0, Kind).toInt() == WorkspaceRow)
        mime->setData(workspaceMime, item->data(0, Id).toString().toUtf8());
    else if (const QStringList ids = selectedSessionIds(); !ids.isEmpty())
        mime->setData(sessionsMime, ids.join(QLatin1Char(',')).toUtf8());
    else
        return delete mime;
    auto *drag = new QDrag(this);
    drag->setMimeData(mime);
    drag->exec(Qt::MoveAction);
    m_dropLine = {};
    viewport()->update();
}

Sidebar::Drop Sidebar::dropTarget(const QPoint &pos, bool workspaceDrag) const
{
    Drop d;
    QTreeWidgetItem *item = itemAt(pos);
    const int w = viewport()->width();
    if (workspaceDrag) {
        if (!item) {
            d.workspaceIndex = -1;
            d.line = QRect(8, visualItemRect(topLevelItem(topLevelItemCount() - 1)).bottom(), w - 16, 2);
            return d;
        }
        QTreeWidgetItem *top = item->parent() ? item->parent() : item;
        const QRect r = visualItemRect(top);
        const bool after = item->parent() || pos.y() > r.center().y();
        d.workspaceIndex = indexOfTopLevelItem(top) + (after ? 1 : 0);
        const QRect edge = after && top->isExpanded() && top->childCount() ? visualItemRect(top->child(top->childCount() - 1)) : r;
        d.line = QRect(8, after ? edge.bottom() : r.top(), w - 16, 2);
        return d;
    }
    if (!item) {
        if (m_store.flaggedView || !topLevelItemCount())
            return d;
        QTreeWidgetItem *last = topLevelItem(topLevelItemCount() - 1);
        d.workspaceId = last->data(0, Id).toString();
        return d;
    }
    if (item->data(0, Kind).toInt() == WorkspaceRow) {
        d.workspaceId = item->data(0, Id).toString();
        d.index = pos.y() > visualItemRect(item).center().y() ? 0 : -1;
        d.line = d.index == 0 ? QRect(28, visualItemRect(item).bottom(), w - 36, 2) : QRect();
        return d;
    }
    const QString sid = item->data(0, Id).toString();
    auto *ws = const_cast<Store &>(m_store).workspaceOf(sid);
    if (!ws)
        return d;
    int idx = 0;
    while (ws->sessions[idx].id != sid)
        ++idx;
    const QRect r = visualItemRect(item);
    const bool after = pos.y() > r.center().y();
    d.workspaceId = ws->id;
    d.index = idx + (after ? 1 : 0);
    d.line = QRect(28, after ? r.bottom() : r.top(), w - 36, 2);
    return d;
}

static QStringList localDirs(const QMimeData *mime)
{
    QStringList dirs;
    for (const QUrl &u : mime->urls())
        if (u.isLocalFile() && QFileInfo(u.toLocalFile()).isDir() && dirs.size() < 20)
            dirs << u.toLocalFile();
    return dirs;
}

void Sidebar::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasFormat(sessionsMime) || event->mimeData()->hasFormat(workspaceMime) || !localDirs(event->mimeData()).isEmpty())
        event->acceptProposedAction();
    else
        event->ignore();
}

void Sidebar::dragMoveEvent(QDragMoveEvent *event)
{
    const bool internal = event->mimeData()->hasFormat(sessionsMime) || event->mimeData()->hasFormat(workspaceMime);
    if (!internal && localDirs(event->mimeData()).isEmpty())
        return event->ignore();
    const Drop d = dropTarget(event->position().toPoint(), event->mimeData()->hasFormat(workspaceMime));
    m_dropLine = internal ? d.line : QRect();
    if (QTreeWidgetItem *item = itemAt(event->position().toPoint());
        item && item->data(0, Kind).toInt() == WorkspaceRow && !item->isExpanded() && !event->mimeData()->hasFormat(workspaceMime))
        item->setExpanded(true); // a collapsed workspace springs open under a drag
    viewport()->update();
    event->acceptProposedAction();
}

void Sidebar::dragLeaveEvent(QDragLeaveEvent *event)
{
    m_dropLine = {};
    viewport()->update();
    QTreeWidget::dragLeaveEvent(event);
}

void Sidebar::dropEvent(QDropEvent *event)
{
    m_dropLine = {};
    const QMimeData *mime = event->mimeData();
    const QPoint pos = event->position().toPoint();
    if (mime->hasFormat(workspaceMime)) {
        m_store.moveWorkspace(QString::fromUtf8(mime->data(workspaceMime)), dropTarget(pos, true).workspaceIndex);
    } else if (mime->hasFormat(sessionsMime)) {
        Drop d = dropTarget(pos, false);
        if (d.workspaceId.isEmpty())
            return event->ignore();
        for (const QString &id : QString::fromUtf8(mime->data(sessionsMime)).split(QLatin1Char(','))) {
            m_store.moveSession(id, d.workspaceId, d.index);
            if (d.index >= 0)
                ++d.index; // a batch keeps its order
        }
    } else {
        QString ws;
        if (QTreeWidgetItem *item = itemAt(pos))
            ws = (item->parent() ? item->parent() : item)->data(0, Id).toString();
        Q_EMIT foldersDropped(ws, localDirs(mime));
    }
    event->setDropAction(Qt::IgnoreAction); // the store moved the rows; don't let the view move items too
    event->accept();
    rebuild();
}

void Sidebar::paintEvent(QPaintEvent *event)
{
    QTreeWidget::paintEvent(event);
    if (m_store.flaggedView && !topLevelItemCount()) {
        QPainter p(viewport());
        p.setPen(palette().color(QPalette::PlaceholderText));
        const QRect r = viewport()->rect().adjusted(12, 24, -12, 0);
        p.drawText(r, Qt::AlignHCenter | Qt::AlignTop, tr("No flagged sessions"));
        p.setPen(palette().color(QPalette::Link));
        p.drawText(r.adjusted(0, fontMetrics().height() + 6, 0, 0), Qt::AlignHCenter | Qt::AlignTop, tr("Edit Flagged Sessions…"));
    }
    if (m_dropLine.isNull())
        return;
    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QApplication::palette().color(QPalette::Highlight));
    p.drawRoundedRect(m_dropLine, 1, 1);
}
