#include "control.h"

#include "paths.h"
#include "restore.h"
#include "store.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>

#include <cmath>

static QJsonObject ok(const QJsonObject &result = {})
{
    return {{"ok", true}, {"result", result}};
}

static QJsonObject fail(const QString &error)
{
    return {{"ok", false}, {"error", error}};
}

static QJsonObject withTint(QJsonObject node, const Tint &t)
{
    if (!t.color.isEmpty()) {
        node.insert("tint", t.color);
        node.insert("tintOn", QJsonArray::fromStringList(t.on));
    }
    return node;
}

static QJsonObject sessionNode(const Session &s, const QString &selected)
{
    return withTint({{"id", s.id},         {"name", s.displayName()}, {"customName", !s.name.isEmpty()}, {"cwd", s.cwd},
                     {"status", s.status}, {"blink", s.blink},         {"flagged", s.flagged},            {"agent", s.agent},
                     {"title", s.title},   {"active", s.id == selected},
                     {"hasSplit", s.hasSplit}, {"split", s.splitVisible}, {"splitAxis", s.splitAxis}, {"splitRatio", s.splitRatio},
                     {"unseen", s.unseen}, {"parent", s.parentId}, {"fontSize", s.fontSize}, {"splitFontSize", s.splitFontSize}},
                    s.tint);
}

static QJsonObject withContext(QJsonObject node, const Session &s, const Store &store)
{
    if (!s.context.isEmpty())
        node.insert("context", s.context);
    if (store.collapsed.contains(s.id) && !const_cast<Store &>(store).companionsOf(s.id).isEmpty())
        node.insert("collapsed", true); // a parent with its companions folded
    return node;
}

static QJsonObject withRestore(QJsonObject node, const Session &s)
{
    if (s.restorePin)
        node.insert("restoreCommand", *s.restorePin);
    if (s.splitRestorePin)
        node.insert("splitRestoreCommand", *s.splitRestorePin);
    return node;
}

int captureForeground(Store &store, Terminals &terminals)
{
    const QString shell = qEnvironmentVariable("SHELL");
    int count = 0;
    for (Session *s : store.allSessions()) {
        s->foreground = Restore::captured(terminals.foregroundArgv(s->id, QStringLiteral("main")), shell);
        s->splitForeground = s->splitVisible ? Restore::captured(terminals.foregroundArgv(s->id, QStringLiteral("right")), shell) : QStringList();
        count += !s->foreground.isEmpty() + !s->splitForeground.isEmpty();
    }
    return count;
}

QStringList dashboardCells(Store &store, const QStringList &targets, bool mru, QString *notes, QString *error)
{
    static const QStringList left{"primary", "left", "top"}, right{"split", "right", "bottom"};
    QList<std::pair<QString, QString>> wanted; // session target, "" | left | right
    if (mru) {
        for (const QString &id : store.recent)
            if (store.session(id) && wanted.size() < 9)
                wanted.append({id, {}});
        if (wanted.isEmpty())
            return *error = QStringLiteral("no recent sessions"), QStringList();
    }
    for (const QString &t : targets) {
        const qsizetype colon = t.indexOf(QLatin1Char(':'));
        const QString suffix = colon < 0 ? QString() : t.mid(colon + 1).toLower();
        if (colon == 0 || (colon > 0 && !left.contains(suffix) && !right.contains(suffix)))
            return *error = QStringLiteral("dashboard: invalid session id '%1' — use <id>, <id>:left, or <id>:right").arg(t), QStringList();
        wanted.append({colon < 0 ? t : t.left(colon), suffix.isEmpty() ? QString() : left.contains(suffix) ? QStringLiteral("left") : QStringLiteral("right")});
    }
    QStringList cells, dropped;
    for (const auto &[target, pane] : wanted) {
        QString e;
        const QString id = store.resolveSession(target, &e);
        if (id.isEmpty() || (pane == QLatin1String("right") && !store.session(id)->hasSplit)) {
            dropped << (id.isEmpty() ? e : QStringLiteral("%1 has no split").arg(target));
            continue;
        }
        QStringList panes{pane.isEmpty() ? QStringLiteral("left") : pane};
        if (pane.isEmpty() && store.session(id)->hasSplit)
            panes << QStringLiteral("right");
        for (const QString &p : panes)
            if (!cells.contains(id + QLatin1Char(':') + p))
                cells << id + QLatin1Char(':') + p;
    }
    QStringList parts;
    if (!dropped.isEmpty())
        parts << QStringLiteral("dropped %1").arg(dropped.join(QStringLiteral(", ")));
    if (cells.size() > 9) {
        parts << QStringLiteral("dropped %1 pane(s) beyond the 9-cell limit").arg(cells.size() - 9);
        cells = cells.mid(0, 9);
    }
    *notes = parts.join(QStringLiteral("; "));
    return cells;
}

std::pair<int, int> dashboardGrid(int count)
{
    const int n = qBound(1, count, 9);
    const int cols = int(std::ceil(std::sqrt(double(n))));
    return {cols, (n + cols - 1) / cols};
}

double dashboardFontFactor(int cols, int rows)
{
    if (cols == 1 && rows == 1)
        return 1.0;
    if (cols == 2 && rows == 1)
        return 0.85;
    if (cols == 2 && rows == 2)
        return 0.75;
    if (cols == 3 && rows == 2)
        return 0.62;
    return 0.55;
}

static QJsonObject dashboard(Store &store, Terminals &terminals, const QJsonObject &args)
{
    QStringList targets;
    for (const auto &v : args.value("targets").toArray())
        targets << v.toString();
    const bool mru = args.value("mru").toBool(), autoSize = args.value("autoSize").toBool(), sized = args.contains("fontSize");
    QString error, notes;
    QJsonObject result;
    if (args.value("close").toBool()) {
        if (!targets.isEmpty() || mru || sized || autoSize)
            return fail(QStringLiteral("dashboard --close takes no ids, --mru, or font options"));
        return terminals.appCommand(QStringLiteral("dashboard"), {}, {{"close", true}}, &result, &error) ? ok(result) : fail(error);
    }
    if (sized && autoSize)
        return fail(QStringLiteral("dashboard: --font-size is mutually exclusive with --auto-size"));
    const double size = args.value("fontSize").toDouble();
    if (sized && !(size > 0 && std::isfinite(size)))
        return fail(QStringLiteral("dashboard --font-size must be a positive number"));
    if (mru && !targets.isEmpty())
        return fail(QStringLiteral("dashboard --mru cannot be combined with explicit session ids"));
    if (!mru && targets.isEmpty())
        return fail(QStringLiteral("dashboard requires at least one session id"));
    const QStringList cells = dashboardCells(store, targets, mru, &notes, &error);
    if (!error.isEmpty())
        return fail(error);
    if (cells.isEmpty())
        return fail(notes.isEmpty() ? QStringLiteral("dashboard requires at least one session id") : notes);
    QJsonObject app{{"cells", QJsonArray::fromStringList(cells)}, {"fontMode", sized ? "fixed" : autoSize ? "auto" : "untouched"}};
    if (sized)
        app.insert("fontSize", size);
    if (!terminals.appCommand(QStringLiteral("dashboard"), {}, app, &result, &error))
        return fail(error);
    if (!notes.isEmpty())
        result.insert("text", notes);
    return ok(result);
}

static QJsonObject restoreCommand(Store &store, Terminals &terminals, const QString &cmd, const QJsonObject &args)
{
    static const QStringList modes{"none", "rerun", "live"};
    QString error;
    if (cmd == QLatin1String("restore.mode")) {
        if (args.contains("mode")) {
            const QString mode = args.value("mode").toString();
            if (!modes.contains(mode))
                return fail(QStringLiteral("invalid restore mode: %1").arg(mode));
            Settings s = store.settings;
            s.restoreMode = mode;
            store.setSettings(s);
            if (!terminals.saveNow(&error))
                return fail(error);
        }
        const QString configured = store.settings.restoreMode, requested = store.restoreModeAtLaunch;
        QJsonObject r{{"configured", configured},
                      {"requestedAtLaunch", requested},
                      {"active", requested},
                      {"restartRequired", configured != requested}};
        return ok({{"restore", r}});
    }
    if (cmd == QLatin1String("restore.capture")) {
        if (store.settings.restoreMode != QLatin1String("rerun"))
            return fail(QStringLiteral("restore.capture requires rerun mode; configured restore mode is %1").arg(store.settings.restoreMode));
        const int count = captureForeground(store, terminals);
        if (!terminals.saveNow(&error))
            return fail(error);
        return ok({{"count", count}, {"text", QStringLiteral("captured %1 command(s)").arg(count)}});
    }
    if (cmd == QLatin1String("restore.clear")) {
        for (Session *s : store.allSessions())
            s->foreground.clear(), s->splitForeground.clear();
        if (!terminals.saveNow(&error))
            return fail(error);
        return ok();
    }
    return fail(QStringLiteral("unknown command '%1'").arg(cmd));
}

// a colour from the palette and an optional placement of known parts; "" when valid
static QString tintArgs(const QJsonObject &args, QString *color, QStringList *parts)
{
    *color = args.value("tint").toString().toLower();
    if (!Store::tintColors.contains(*color))
        return QStringLiteral("invalid tint: %1 (%2)").arg(args.value("tint").toString(), Store::tintColors.join(QLatin1Char('|')));
    if (args.contains("tintOn")) {
        QStringList raw;
        for (const auto &v : args.value("tintOn").toArray())
            raw << v.toString();
        if ((*parts = Store::tintParts(raw)).isEmpty())
            return QStringLiteral("invalid tint placement: %1 (icon,name,background)").arg(raw.join(QLatin1Char(',')));
    }
    return {};
}

// session.tint / workspace.tint: a clear refuses a colour or placement rather than ignoring it
static QString applyTint(Store &store, const QString &id, const QString &cmd, const QJsonObject &args)
{
    const QString mode = args.value("mode").toString();
    QString color;
    QStringList parts;
    if (mode == QLatin1String("clear")) {
        if (args.contains("tint") || args.contains("tintOn"))
            return QStringLiteral("%1 clear takes no color or placement").arg(cmd);
    } else if (mode == QLatin1String("set")) {
        if (!args.contains("tint"))
            return QStringLiteral("%1 set requires a color").arg(cmd);
        if (QString e = tintArgs(args, &color, &parts); !e.isEmpty())
            return e;
    } else {
        return QStringLiteral("invalid tint mode: %1 (set|clear)").arg(mode);
    }
    store.setTint(id, color, parts);
    return {};
}

// session.new / workspace.new: validated before anything is created
static QString creationTint(const QJsonObject &args, QString *color, QStringList *parts)
{
    if (!args.contains("tint"))
        return args.contains("tintOn") ? QStringLiteral("--tint-on requires --tint") : QString();
    return tintArgs(args, color, parts);
}

static QJsonObject merged(QJsonObject node, const QJsonObject &extra)
{
    for (auto it = extra.begin(); it != extra.end(); ++it)
        node.insert(it.key(), it.value());
    return node;
}

static bool isColor(const QString &c)
{
    static const QRegularExpression re(QStringLiteral("^#[0-9a-fA-F]{6}$"));
    return re.match(c).hasMatch();
}

QString overlayArgsError(const QString &cmd, QJsonObject *args)
{
    QJsonObject &a = *args;
    const QString name = cmd;
    if (a.contains("pane")) {
        static const QStringList left{"left", "primary", "top"}, right{"right", "split", "bottom"};
        const QString p = a.value("pane").toString().toLower();
        if (!left.contains(p) && !right.contains(p))
            return QStringLiteral("--pane must be left or right");
        a.insert("pane", left.contains(p) ? QStringLiteral("left") : QStringLiteral("right"));
    }
    auto percent = [&]() -> QString {
        if (a.contains("sizePercent") && (a.value("sizePercent").toInt() < 1 || a.value("sizePercent").toInt() > 100))
            return QStringLiteral("%1: --size-percent must be 1...100").arg(name);
        return {};
    };
    auto points = [&]() -> QString {
        for (const char *k : {"width", "height"})
            if (a.contains(QLatin1String(k)) && a.value(QLatin1String(k)).toDouble() <= 0)
                return QStringLiteral("%1: --%2 must be a positive number of points").arg(name, QLatin1String(k));
        return {};
    };
    if (a.contains("color") && !isColor(a.value("color").toString()))
        return QStringLiteral("invalid color: %1 (#rrggbb)").arg(a.value("color").toString());
    if (cmd == QLatin1String("session.overlay.open")) {
        if (a.contains("html") || a.contains("url") || a.contains("image") || a.contains("markdownFile") || a.contains("textFile"))
            return QStringLiteral("page overlays need Qt WebEngine, which this build lacks (install qt6-webengine-dev and rebuild)");
        if (a.value("command").toString().trimmed().isEmpty())
            return QStringLiteral("session.overlay.open requires a command");
        if (a.contains("pane") && a.contains("sizePercent"))
            return QStringLiteral("session.overlay.open: --pane is mutually exclusive with --size-percent");
        if ((a.contains("width") || a.contains("height")) && !a.contains("sizePercent"))
            return QStringLiteral("session.overlay.open: --width/--height need --size-percent");
        if (QString e = percent(); !e.isEmpty())
            return e;
        return points();
    }
    if (cmd == QLatin1String("session.overlay.resize")) {
        if (a.contains("pane"))
            return QStringLiteral("session.overlay.resize: --pane is not supported (pane overlays are always full)");
        if (a.value("full").toBool() && a.contains("sizePercent"))
            return QStringLiteral("session.overlay.resize: --full is mutually exclusive with --size-percent");
        if (!a.value("full").toBool() && !a.contains("sizePercent") && !a.contains("width") && !a.contains("height"))
            return QStringLiteral("session.overlay.resize requires --size-percent, --width/--height or --full");
        if (QString e = percent(); !e.isEmpty())
            return e;
        return points();
    }
    if (cmd == QLatin1String("session.hud.open") || cmd == QLatin1String("session.hud.update")) {
        const QString message = a.value("message").toString(), detail = a.value("detail").toString();
        const bool markdown = a.value("markdown").toBool();
        if (message.isEmpty())
            return QStringLiteral("%1 requires a message").arg(cmd);
        auto controls = [](const QString &t, bool allowLines) {
            for (QChar ch : t)
                if ((ch.unicode() < 0x20 || ch.unicode() == 0x7f) && !(allowLines && (ch == QLatin1Char('\n') || ch == QLatin1Char('\t'))))
                    return true;
            return false;
        };
        if (controls(message, markdown) || controls(detail, false))
            return QStringLiteral("hud text must not contain control characters");
        if (message.size() > (markdown ? 4096 : 256))
            return QStringLiteral("hud message too long (max %1 characters)").arg(markdown ? 4096 : 256);
        if (detail.size() > 256)
            return QStringLiteral("hud detail too long (max 256 characters)");
        if (a.contains("textColor") && !isColor(a.value("textColor").toString()))
            return QStringLiteral("invalid text color: %1 (#rrggbb)").arg(a.value("textColor").toString());
        if (a.contains("fontSize")) {
            if (cmd == QLatin1String("session.hud.update"))
                return QStringLiteral("session.hud.update: --font-size is fixed at open; reopen the hud to change it");
            if (a.value("fontSize").toDouble() < 6 || a.value("fontSize").toDouble() > 72)
                return QStringLiteral("session.hud.open: --font-size must be 6...72 points");
        }
        if (cmd == QLatin1String("session.hud.update"))
            a.remove("color"); // fixed at open, ignored on update as master does
        if (QString e = percent(); !e.isEmpty())
            return e;
        if (a.contains("hideAfter") && (a.value("hideAfter").toDouble() < 0 || a.value("hideAfter").toDouble() > 86400))
            return QStringLiteral("%1: --hide-after must be 0...86400 seconds").arg(cmd);
        static const QStringList positions{"top-left", "top-center", "top-right", "center-left", "center", "center-right", "bottom-left", "bottom-center", "bottom-right"};
        QString pos = a.value("position").toString(QStringLiteral("center")).toLower();
        pos = pos == QLatin1String("top") ? QStringLiteral("top-center") : pos == QLatin1String("bottom") ? QStringLiteral("bottom-center") : pos;
        if (!positions.contains(pos))
            return QStringLiteral("invalid position: %1 (%2)").arg(a.value("position").toString(), positions.join(QLatin1Char('|')));
        a.insert("position", pos);
        static const QStringList spinners{"bar", "braille", "circle", "blocks", "dot", "none"};
        if (a.contains("spinner")) {
            const QString sp = a.value("spinner").isBool() ? (a.value("spinner").toBool() ? QStringLiteral("bar") : QStringLiteral("none")) : a.value("spinner").toString();
            if (!spinners.contains(sp))
                return QStringLiteral("invalid spinner: %1 (%2)").arg(sp, spinners.join(QLatin1Char('|')));
            a.insert("spinner", sp);
        }
    }
    return {};
}

static QJsonObject tree(Store &store, const Terminals &terminals)
{
    QJsonArray ws;
    for (const auto &w : store.workspaces) {
        QJsonArray ss;
        bool active = false;
        for (const auto &s : w.sessions) {
            ss << merged(withContext(withRestore(sessionNode(s, store.selected), s), s, store), terminals.sessionExtras(s.id));
            active |= s.id == store.selected;
        }
        QJsonObject node = withTint({{"id", w.id}, {"name", w.name}, {"rememberedFolder", w.folder}, {"active", active}, {"sessions", ss}}, w.tint);
        if (store.focused.contains(w.id))
            node.insert("focused", true); // membership, whether or not the filter applies (workspaceFilter)
        if (store.collapsed.contains(w.id))
            node.insert("collapsed", true);
        ws << node;
    }
    return {{"workspaces", ws},
            {"workspaceFilter", store.focusEnabled},
            {"sidebarMode", store.flaggedView ? "flagged" : "tree"},
            {"sidebarFlaggedLayout", store.settings.flaggedViewLayout}};
}

// on | off | toggle against the current state; "" means toggle
static bool resolveMode(const QString &mode, bool current, bool *out)
{
    if (mode.isEmpty() || mode == QLatin1String("toggle"))
        return *out = !current, true;
    if (mode != QLatin1String("on") && mode != QLatin1String("off"))
        return false;
    return *out = mode == QLatin1String("on"), true;
}

static QJsonObject sessionNew(Store &store, const QJsonObject &args)
{
    QString cwd = args.value("cwd").toString();
    if (cwd.startsWith(QLatin1Char('~')))
        cwd = QDir::homePath() + cwd.mid(1);
    if (!cwd.isEmpty() && !QFileInfo(cwd).isDir())
        return fail(QStringLiteral("not a directory: %1").arg(cwd));
    if (!cwd.isEmpty())
        cwd = QFileInfo(cwd).absoluteFilePath();

    QString error, wsId, tint;
    QStringList tintOn;
    if (error = creationTint(args, &tint, &tintOn); !error.isEmpty())
        return fail(error);
    if (args.contains("parent")) {
        const QString parent = store.resolveSession(args.value("parent").toString(), &error);
        if (parent.isEmpty())
            return fail(error);
        const QString id = store.addCompanion(parent, args.value("name").toString(), cwd, args.value("command").toString());
        store.setTint(id, tint, tintOn);
        if (!args.value("noSelect").toBool())
            store.select(id);
        return ok({{"id", id}});
    }
    const QString wsName = args.value("workspaceName").toString();
    if (!wsName.isEmpty()) {
        for (const auto &w : store.workspaces)
            if (w.name == wsName)
                wsId = w.id;
        if (wsId.isEmpty() && !args.value("createWorkspace").toBool())
            return fail(QStringLiteral("no workspace named '%1' (pass --create-workspace)").arg(wsName));
        if (wsId.isEmpty())
            wsId = store.addWorkspace(wsName);
    } else if (store.workspaces.isEmpty() && args.value("workspace").toString().isEmpty()) {
        wsId = store.addWorkspace(QStringLiteral("main"));
    } else if (!cwd.isEmpty() && args.value("workspace").toString().isEmpty() && !store.workspaceRemembering(cwd).isEmpty()) {
        wsId = store.workspaceRemembering(cwd); // a bare --cwd joins the workspace that lives there
    } else if (wsId = store.resolveWorkspace(args.value("workspace").toString(), &error); wsId.isEmpty()) {
        return fail(error);
    }
    if (cwd.isEmpty()) {
        const Session *cur = store.session(store.selected);
        cwd = store.newSessionCwd(wsId, cur ? cur->cwd : QString());
    }

    const QString id = store.addSession(wsId, args.value("name").toString(), cwd, args.value("command").toString());
    store.setTint(id, tint, tintOn);
    if (!args.value("noSelect").toBool())
        store.select(id);
    return ok({{"id", id}});
}

QString duplicateAsCompanion(Store &store, Terminals &terminals, const QString &id, QString *error)
{
    const Session *source = store.session(id);
    if (!source) {
        *error = QStringLiteral("no such session");
        return {};
    }
    PanePlan left, right;
    if (!terminals.panePlan(id, QStringLiteral("main"), &left, error)
        || (source->hasSplit && !terminals.panePlan(id, QStringLiteral("right"), &right, error)))
        return {};
    const bool split = source->hasSplit;
    const QString axis = source->splitAxis;
    const double ratio = source->splitRatio;
    const QString copy = store.addCompanion(id, {}, left.cwd, left.command);
    if (split) {
        Session *c = store.session(copy);
        c->splitCwd = right.cwd;
        c->splitCommand = right.command;
        c->splitRatio = ratio;
        store.setSplit(copy, true, true, axis);
    }
    return copy;
}

QJsonObject dispatch(Store &store, Terminals &terminals, const QJsonObject &request)
{
    const QString cmd = request.value("cmd").toString();
    const QString target = request.value("target").toString();
    const QJsonObject args = request.value("args").toObject();
    QString error;

    if (cmd == QLatin1String("tree")) {
        QJsonObject t = tree(store, terminals);
        const QJsonObject w = terminals.chromeState();
        for (auto it = w.begin(); it != w.end(); ++it)
            t.insert(it.key(), it.value());
        return ok({{"tree", t}});
    }
    bool handled = false;
    if (QJsonObject r = moreCommands(store, terminals, cmd, target, args, &handled); handled)
        return r;
    if (const QString why = unsupportedReason(cmd); !why.isEmpty())
        return fail(why);
    if (cmd == QLatin1String("session.companion.new")) {
        for (const char *k : {"workspace", "workspaceName", "after", "before"})
            if (args.contains(QLatin1String(k)))
                return fail(QStringLiteral("a companion takes no workspace or placement: it lands under its parent"));
        if (args.value("createWorkspace").toBool())
            return fail(QStringLiteral("a companion takes no workspace or placement: it lands under its parent"));
        if (args.value("wait").toBool() && args.value("command").toString().isEmpty())
            return fail(QStringLiteral("--wait requires --command"));
        QJsonObject a = args;
        a.insert("parent", target);
        QJsonObject r = sessionNew(store, a);
        if (r.value("ok").toBool()) {
            QJsonObject result = r.value("result").toObject();
            result.insert("parent", store.session(result.value("id").toString())->parentId);
            r.insert("result", result);
        }
        return r;
    }
    if (cmd == QLatin1String("filepanel") || cmd == QLatin1String("filepanel.width")) {
        const QString mode = cmd == QLatin1String("filepanel") ? args.value("mode").toString(QStringLiteral("toggle")) : QString();
        const int width = args.value("width").toInt();
        if (cmd == QLatin1String("filepanel.width") && width < 120)
            return fail(QStringLiteral("width must be at least 120"));
        if (!mode.isEmpty() && mode != QLatin1String("show") && mode != QLatin1String("hide") && mode != QLatin1String("toggle"))
            return fail(QStringLiteral("mode must be show, hide or toggle"));
        if (!terminals.filePanel(mode, width, &error))
            return fail(error);
        return ok(terminals.chromeState());
    }
    if (cmd == QLatin1String("version"))
        return ok({{"text", QStringLiteral("agterm-qt %1").arg(QLatin1String(appVersion))}});
    if (cmd == QLatin1String("workspace.new")) {
        QString tint;
        QStringList tintOn;
        if (error = creationTint(args, &tint, &tintOn); !error.isEmpty())
            return fail(error);
        const QString id = store.addWorkspace(args.value("name").toString());
        store.setTint(id, tint, tintOn);
        return ok({{"id", id}});
    }
    if (cmd == QLatin1String("session.new"))
        return sessionNew(store, args);
    if (cmd.startsWith(QLatin1String("keymap.")) || cmd.startsWith(QLatin1String("hooks.")) || cmd.startsWith(QLatin1String("theme."))) {
        QString id;
        if (cmd == QLatin1String("keymap.run") && (id = store.resolveSession(target, &error)).isEmpty())
            return fail(error);
        QJsonObject result;
        if (!terminals.appCommand(cmd, id, args, &result, &error))
            return fail(error);
        return ok(result);
    }
    if (cmd == QLatin1String("dashboard"))
        return dashboard(store, terminals, args);
    if (cmd.startsWith(QLatin1String("restore.")))
        return restoreCommand(store, terminals, cmd, args);
    if (cmd == QLatin1String("session.go")) {
        const QString id = store.go(args.value("to").toString(), &error);
        return id.isEmpty() ? fail(error) : ok({{"id", id}});
    }

    if (cmd.startsWith(QLatin1String("font."))) {
        const QString id = store.resolveSession(target, &error);
        if (id.isEmpty())
            return fail(error);
        const QString op = cmd.mid(5), p = args.value("pane").toString(QStringLiteral("left"));
        static const QStringList left{"left", "main", "primary", "top"}, right{"right", "split", "bottom"};
        if (!QStringList{"inc", "dec", "reset"}.contains(op))
            return fail(QStringLiteral("unknown command '%1'").arg(cmd));
        if (!left.contains(p) && !right.contains(p))
            return fail(QStringLiteral("pane must be left or right"));
        const bool split = right.contains(p);
        if (split && !store.session(id)->hasSplit)
            return fail(QStringLiteral("session has no split"));
        if (!terminals.font(id, split ? QStringLiteral("right") : QStringLiteral("main"), op, &error))
            return fail(error);
        const Session *s = store.session(id);
        return ok({{"id", id}, {"fontSize", split ? s->splitFontSize : s->fontSize}});
    }
    if (cmd == QLatin1String("workspace.filter")) { // the window's filter, not one workspace
        bool on = false;
        if (!resolveMode(args.value("mode").toString(), store.focusEnabled, &on))
            return fail(QStringLiteral("mode must be on, off or toggle"));
        store.setFilter(on); // enabling an empty set is refused silently, as the footer button is disabled then
        return ok({{"workspaceFilter", store.focusEnabled}});
    }
    if (cmd.startsWith(QLatin1String("workspace."))) {
        const QString id = store.resolveWorkspace(target, &error);
        if (id.isEmpty())
            return fail(error);
        if (cmd == QLatin1String("workspace.rename")) {
            if (args.value("name").toString().isEmpty())
                return fail(QStringLiteral("name is required"));
            store.renameWorkspace(id, args.value("name").toString());
        } else if (cmd == QLatin1String("workspace.move")) {
            store.moveWorkspace(id, args.value("index").toInt());
        } else if (cmd == QLatin1String("workspace.focus")) {
            if (!store.focusWorkspace(id, args.value("mode").toString(QStringLiteral("toggle"))))
                return fail(QStringLiteral("mode must be on, off, toggle or add"));
        } else if (cmd == QLatin1String("workspace.tint")) {
            if (error = applyTint(store, id, cmd, args); !error.isEmpty())
                return fail(error);
        } else if (cmd == QLatin1String("workspace.delete")) {
            store.removeWorkspace(id);
        } else if (cmd == QLatin1String("workspace.select")) {
            const auto &ss = store.workspace(id)->sessions;
            if (ss.isEmpty())
                return fail(QStringLiteral("workspace has no sessions"));
            store.select(ss.first().id);
        } else {
            return fail(QStringLiteral("unknown command '%1'").arg(cmd));
        }
        return ok({{"id", id}});
    }

    if (cmd.startsWith(QLatin1String("session.overlay.")) || cmd.startsWith(QLatin1String("session.hud."))) {
        const QString id = store.resolveSession(target, &error);
        if (id.isEmpty())
            return fail(error);
        QJsonObject a = args;
        if (error = overlayArgsError(cmd, &a); !error.isEmpty())
            return fail(error);
        if (a.value("pane").toString() == QLatin1String("right") && !store.session(id)->hasSplit)
            return fail(QStringLiteral("session has no split"));
        QJsonObject result{{"id", id}};
        if (!terminals.appCommand(cmd, id, a, &result, &error))
            return fail(error);
        if (a.value("follow").toBool())
            store.select(id);
        return ok(result);
    }
    if (cmd.startsWith(QLatin1String("session."))) {
        const QString id = store.resolveSession(target, &error);
        if (id.isEmpty())
            return fail(error);
        if (cmd == QLatin1String("session.close")) {
            // immediate unless asked for the GUI's undo grace, and then only while that setting allows it
            if (!args.value("undoable").toBool()) {
                store.removeSession(id);
            } else {
                const bool soft = store.settings.closeUndo && store.softClose({id});
                if (!soft)
                    store.removeSession(id);
                return ok({{"id", id}, {"undoable", soft}});
            }
        } else if (cmd == QLatin1String("session.select")) {
            store.select(id);
        } else if (cmd == QLatin1String("session.rename")) {
            store.renameSession(id, args.value("name").toString()); // empty reverts to the directory name
        } else if (cmd == QLatin1String("session.status")) {
            const QString status = args.value("status").toString();
            if (!store.setStatus(id, status, args.value("autoReset").toBool(), args.value("blink").toBool()))
                return fail(QStringLiteral("status must be one of %1").arg(Store::statuses.join(QLatin1String(", "))));
        } else if (cmd == QLatin1String("session.flag")) {
            const QString mode = args.value("mode").toString(QStringLiteral("toggle"));
            if (mode != QLatin1String("on") && mode != QLatin1String("off") && mode != QLatin1String("toggle"))
                return fail(QStringLiteral("mode must be on, off or toggle"));
            store.setFlag(id, mode == QLatin1String("toggle") ? !store.session(id)->flagged : mode == QLatin1String("on"));
        } else if (cmd == QLatin1String("session.tint")) {
            if (error = applyTint(store, id, cmd, args); !error.isEmpty())
                return fail(error);
        } else if (cmd == QLatin1String("session.move")) {
            const QString ws = store.resolveWorkspace(args.value("workspace").toString(), &error);
            if (ws.isEmpty())
                return fail(error);
            store.moveSession(id, ws, args.contains("index") ? args.value("index").toInt() : -1);
        } else if (cmd == QLatin1String("session.split")) {
            const Session *s = store.session(id);
            const QString axis = args.value("axis").toString(s->splitAxis);
            bool on = false;
            if (!resolveMode(args.value("mode").toString(), s->splitVisible, &on) || (axis != QLatin1String("vertical") && axis != QLatin1String("horizontal")))
                return fail(QStringLiteral("mode must be on, off or toggle; axis vertical or horizontal"));
            store.setSplit(id, s->hasSplit || on, on, axis);
        } else if (cmd == QLatin1String("session.split.close")) {
            if (!store.session(id)->hasSplit)
                return fail(QStringLiteral("session has no split"));
            store.setSplit(id, false, false, store.session(id)->splitAxis);
        } else if (cmd == QLatin1String("session.swap")) {
            if (!store.session(id)->hasSplit)
                return fail(QStringLiteral("session has no split"));
            store.requestSwap(id);
        } else if (cmd == QLatin1String("session.restore")) {
            const QString mode = args.value("mode").toString(QStringLiteral("set")), pane = args.value("pane").toString(QStringLiteral("left"));
            Session *s = store.session(id);
            if (pane != QLatin1String("left") && pane != QLatin1String("right"))
                return fail(QStringLiteral("--pane must be left or right"));
            if (pane == QLatin1String("right") && !s->hasSplit)
                return fail(QStringLiteral("session has no split"));
            auto &pin = pane == QLatin1String("right") ? s->splitRestorePin : s->restorePin;
            if (mode == QLatin1String("set")) {
                if (error = Restore::pinError(args.value("command").toString()); !error.isEmpty())
                    return fail(error);
                pin = args.value("command").toString();
            } else if (mode == QLatin1String("none")) {
                pin = QString();
            } else if (mode == QLatin1String("clear")) {
                pin.reset();
            } else {
                return fail(QStringLiteral("invalid restore mode: %1 (set|none|clear)").arg(mode));
            }
            Q_EMIT store.changed();
            if (store.settings.restoreMode != QLatin1String("rerun"))
                return ok({{"id", id}, {"text", QStringLiteral("saved; it applies only in rerun restore mode")}});
        } else if (cmd == QLatin1String("session.seen")) {
            store.setUnseen(id, 0);
        } else if (cmd == QLatin1String("session.companion.duplicate")) {
            const QString copy = duplicateAsCompanion(store, terminals, id, &error);
            if (copy.isEmpty())
                return fail(error);
            if (!args.value("noSelect").toBool())
                store.select(copy);
            return ok({{"id", copy}, {"parent", store.session(copy)->parentId}});
        } else if (cmd == QLatin1String("session.duplicate")) {
            const QString copy = store.duplicateSession(id, {});
            if (!args.value("noSelect").toBool())
                store.select(copy);
            return ok({{"id", copy}});
        } else if (cmd == QLatin1String("session.type")) {
            if (!terminals.type(id, args.value("text").toString(), &error))
                return fail(error);
        } else if (cmd == QLatin1String("session.info")) {
            QJsonObject node = merged(withContext(withRestore(sessionNode(*store.session(id), store.selected), *store.session(id)), *store.session(id), store),
                                      terminals.sessionExtras(id));
            node.insert("workspace", store.workspaceOf(id)->id);
            const QJsonObject live = terminals.info(id);
            for (auto it = live.begin(); it != live.end(); ++it)
                node.insert(it.key(), it.value());
            return ok({{"id", id}, {"session", node}});
        } else {
            return fail(QStringLiteral("unknown command '%1'").arg(cmd));
        }
        return ok({{"id", id}});
    }

    return fail(QStringLiteral("unknown command '%1'").arg(cmd));
}
