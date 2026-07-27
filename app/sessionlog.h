#ifndef SESSIONLOG_H
#define SESSIONLOG_H

#include <QFile>
#include <QString>

#include <atomic>
#include <memory>
#include <thread>

// Captures this process's stdout and stderr into a file for the run. Everything
// the app, the core and every module child writes shares those two descriptors,
// so the file is the run's whole account of itself.
//
// Both streams travel one pipe, which keeps their interleaving intact, and the
// bytes are mirrored to the original stdout so an attached terminal still sees
// them. A caller that reads the two streams apart therefore has to merge them.
//
// POSIX only: elsewhere start() reports failure and the streams are untouched.
class SessionLog
{
public:
    static SessionLog& instance();

    // Captures into <logsDir>/<name>_<stamp>.log, rotating to <stamp>.NNN.log
    // every maxLinesPerFile lines and deleting all but the newest keepRuns
    // runs. False leaves the streams alone; calling it twice is a no-op.
    bool start(const QString& logsDir, const QString& name, int maxLinesPerFile = 10000,
               int keepRuns = 10);
    void stop();

    // The file being written, empty until start() has succeeded.
    QString filePath() const;

    SessionLog(const SessionLog&) = delete;
    SessionLog& operator=(const SessionLog&) = delete;

private:
    SessionLog() = default;
    ~SessionLog();

    void openNewFile();
    void readerLoop();
    void pruneOlderRuns();

    QString m_logsDir;
    QString m_name;
    QString m_sessionStamp;
    int m_maxLinesPerFile = 10000;
    int m_keepRuns = 10;
    int m_rotationIndex = 0;
    int m_linesInCurrentFile = 0;
    std::unique_ptr<QFile> m_currentFile;
    // The path the run started on, which stays addressable across rotations
    // while m_currentFile moves on.
    QString m_filePath;
    int m_originalStdout = -1;
    int m_originalStderr = -1;
    int m_readFd = -1;
    std::atomic_bool m_running{false};
    std::thread m_readerThread;
    bool m_started = false;
};

#endif // SESSIONLOG_H
