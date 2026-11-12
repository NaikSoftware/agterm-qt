#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <functional>

// What a workspace row's "+" starts: a plain shell, or a coding agent typed into one.
struct Launcher {
    QString id;      // terminal | claude | codex | opencode | antigravity; also the icon name
    QString title;
    QString command; // typed into the new shell; empty for a plain terminal
};

const QList<Launcher> &launchers();
const Launcher *launcher(const QString &id);

// The agent a foreground argv runs ("" for none); `node /path/claude` counts as claude.
QString agentForArgv(QStringList argv);
// The first agent among a foreground process group's argvs, leader first: a script shell does no job control,
// so an agent it starts shares the group without leading it.
QString agentForGroup(const QList<QStringList> &group);
// A short name for a foreground argv: the program, or the script/module an interpreter runs (`python3 -m http.server`
// is http.server); inline code (`sh -c ...`) names the interpreter.
// shortcut: wrappers name themselves (`sudo htop` is sudo, `npm run dev` is npm); unwrap them if that reads badly.
QString commandTitle(QStringList argv);
// The title agy (Antigravity) gave the conversation a process holds open: agy sets no terminal title, but locks
// <dataDir>/presence/<id>.lock and keeps each conversation's title in <dataDir>/conversation_summaries.db.
// `fdDir` is the process's /proc/<pid>/fd; "" when it is not agy or the model has not titled the conversation yet.
QString antigravityTitle(const QString &fdDir, const QString &dataDir);
// The line that resumes the conversation a claude process runs: Claude Code keeps <sessionsDir>/<pid>.json with
// its sessionId (it changes on /clear and /resume). "" when there is no such file or id.
QString claudeResumeLine(const QString &sessionsDir, int pid);
// Terminal plus every agent whose command is an executable in one of `path`'s directories.
QStringList availableLaunchers(const QString &path, const std::function<bool(const QString &)> &isExecutable);
