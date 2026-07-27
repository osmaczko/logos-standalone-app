#include "sessionlog.h"

#include <QDateTime>
#include <QDir>
#include <QRegularExpression>
#include <QSet>

#include <cerrno>
#include <cstdio>

#if !defined(Q_OS_WIN)
#  include <unistd.h>
#endif

SessionLog& SessionLog::instance()
{
    static SessionLog s;
    return s;
}

SessionLog::~SessionLog()
{
    stop();
}

QString SessionLog::filePath() const
{
    return m_filePath;
}

#if defined(Q_OS_WIN)

bool SessionLog::start(const QString&, const QString&, int, int) { return false; }
void SessionLog::stop() {}
void SessionLog::openNewFile() {}
void SessionLog::readerLoop() {}
void SessionLog::pruneOlderRuns() {}

#else

bool SessionLog::start(const QString& logsDir, const QString& name, int maxLinesPerFile,
                       int keepRuns)
{
    if (m_started)
        return true;

    m_logsDir = logsDir;
    m_name = name;
    m_maxLinesPerFile = maxLinesPerFile > 0 ? maxLinesPerFile : 10000;
    m_keepRuns = keepRuns > 0 ? keepRuns : 1;

    if (!QDir().mkpath(m_logsDir))
        return false;

    m_sessionStamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    m_rotationIndex = 0;
    m_linesInCurrentFile = 0;

    // Before this run adds its own, so the directory settles at keepRuns.
    pruneOlderRuns();

    auto cleanup = [this]() {
        if (m_originalStdout >= 0) { ::close(m_originalStdout); m_originalStdout = -1; }
        if (m_originalStderr >= 0) { ::close(m_originalStderr); m_originalStderr = -1; }
        if (m_currentFile) {
            m_currentFile->close();
            m_currentFile.reset();
        }
        m_filePath.clear();
    };

    openNewFile();
    if (!m_currentFile || !m_currentFile->isOpen()) {
        cleanup();
        return false;
    }

    m_originalStdout = ::dup(fileno(stdout));
    m_originalStderr = ::dup(fileno(stderr));
    if (m_originalStdout < 0 || m_originalStderr < 0) {
        cleanup();
        return false;
    }

    int fds[2];
    if (::pipe(fds) != 0) {
        cleanup();
        return false;
    }

    std::fflush(stdout);
    std::fflush(stderr);

    if (::dup2(fds[1], fileno(stdout)) == -1 || ::dup2(fds[1], fileno(stderr)) == -1) {
        ::close(fds[0]);
        ::close(fds[1]);
        cleanup();
        return false;
    }

    // Line-buffer so a line reaches the pipe when it is written rather than
    // when the buffer happens to fill, now that neither stream is a terminal.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IOLBF, 0);

    ::close(fds[1]);
    m_readFd = fds[0];

    m_running = true;
    m_readerThread = std::thread(&SessionLog::readerLoop, this);
    m_started = true;
    return true;
}

void SessionLog::stop()
{
    if (!m_started)
        return;
    m_running = false;

    std::fflush(stdout);
    std::fflush(stderr);

    // Restoring via dup2 closes the pipe's write end that stdout and stderr
    // hold, and once both are restored the reader sees EOF and returns. The
    // duplicated originals stay open until it has, since readerLoop() may still
    // be writing to one of them and a closed fd number gets reused.
    if (m_originalStdout >= 0)
        ::dup2(m_originalStdout, fileno(stdout));
    if (m_originalStderr >= 0)
        ::dup2(m_originalStderr, fileno(stderr));

    if (m_readerThread.joinable())
        m_readerThread.join();

    if (m_originalStdout >= 0) { ::close(m_originalStdout); m_originalStdout = -1; }
    if (m_originalStderr >= 0) { ::close(m_originalStderr); m_originalStderr = -1; }

    if (m_readFd >= 0) {
        ::close(m_readFd);
        m_readFd = -1;
    }
    if (m_currentFile) {
        m_currentFile->flush();
        m_currentFile->close();
        m_currentFile.reset();
    }
    m_started = false;
}

void SessionLog::openNewFile()
{
    const QString live =
        m_logsDir + QStringLiteral("/%1_%2.log").arg(m_name, m_sessionStamp);

    // The run announces one path, once, at the spawn that hands it to the view
    // module, so that path has to stay the file being written. Rotation moves
    // the full one aside under a numbered name and opens a fresh file back
    // under the announced one; a reader following the path sees it restart
    // rather than go quiet on a file nobody writes any more.
    if (m_rotationIndex > 0) {
        const QString filled = m_logsDir
            + QStringLiteral("/%1_%2.%3.log")
                  .arg(m_name, m_sessionStamp)
                  .arg(m_rotationIndex, 3, 10, QChar('0'));
        QFile::rename(live, filled);
    }

    auto f = std::make_unique<QFile>(live);
    if (!f->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        m_currentFile.reset();
        return;
    }
    m_filePath = f->fileName();
    m_currentFile = std::move(f);
    m_linesInCurrentFile = 0;
    ++m_rotationIndex;
}

void SessionLog::pruneOlderRuns()
{
    // A run is one stamp, however many rotations it produced, so the newest
    // keepRuns stamps are kept whole and everything older goes.
    const QRegularExpression runFile(
        QStringLiteral("^%1_(\\d{8}_\\d{6})(?:\\.\\d{3})?\\.log$").arg(QRegularExpression::escape(m_name)));

    const QStringList names = QDir(m_logsDir).entryList(QDir::Files, QDir::Name);
    QSet<QString> stamps;
    for (const QString& fileName : names) {
        const QRegularExpressionMatch match = runFile.match(fileName);
        if (match.hasMatch())
            stamps.insert(match.captured(1));
    }
    if (stamps.size() < m_keepRuns)
        return;

    QStringList ordered(stamps.begin(), stamps.end());
    ordered.sort();
    // Room for the run about to start.
    const QStringList doomed = ordered.mid(0, ordered.size() - (m_keepRuns - 1));

    QDir dir(m_logsDir);
    for (const QString& fileName : names) {
        const QRegularExpressionMatch match = runFile.match(fileName);
        if (match.hasMatch() && doomed.contains(match.captured(1)))
            dir.remove(fileName);
    }
}

void SessionLog::readerLoop()
{
    char buf[4096];
    while (true) {
        ssize_t n = ::read(m_readFd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break; // EOF: stdout and stderr have been restored.

        // Mirror to the original stdout so an attached terminal keeps seeing
        // output. Failures here are ignored (stdout may be /dev/null).
        if (m_originalStdout >= 0) {
            ssize_t off = 0;
            while (off < n) {
                ssize_t w = ::write(m_originalStdout, buf + off, n - off);
                if (w < 0) {
                    if (errno == EINTR)
                        continue;
                    break;
                }
                off += w;
            }
        }

        if (!m_currentFile)
            continue;

        ssize_t start = 0;
        for (ssize_t i = 0; i < n; ++i) {
            if (buf[i] != '\n')
                continue;
            m_currentFile->write(buf + start, i - start + 1);
            ++m_linesInCurrentFile;
            start = i + 1;
            if (m_linesInCurrentFile >= m_maxLinesPerFile) {
                m_currentFile->flush();
                m_currentFile->close();
                openNewFile();
                if (!m_currentFile)
                    return; // Rotation failed; nothing further can be written.
            }
        }
        if (start < n && m_currentFile)
            m_currentFile->write(buf + start, n - start);
        m_currentFile->flush();
    }

    if (m_currentFile)
        m_currentFile->flush();
}

#endif // Q_OS_WIN
