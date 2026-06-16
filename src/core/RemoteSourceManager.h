#pragma once

#include <string>
#include "../streaming/RemoteMetaSync.h"

// #158 — RemoteSourceManager: owns the remote /meta worker wiring and the
// per-frame pending-meta drain, lifted out of Application (behavior-preserving,
// reference-based). Application declares it a friend and keeps the m_pendingRemote*
// fields; the manager just consolidates the logic. It must outlive m_remoteMetaSync
// (declared before it in Application) so the worker thread is joined while the
// manager is still alive.

class Application;

class RemoteSourceManager
{
public:
    explicit RemoteSourceManager(Application &app);

    // (Re)start the host /meta poller against devicePath; sets the auth token
    // first when authToken != nullptr (the source-switch path supplies one).
    void startWorker(const std::string &devicePath, const std::string *authToken);

    // Drain staged remote meta onto the GL thread; cheap no-op when nothing pending.
    void applyPendingRemoteMeta();

private:
    // Stage one /meta snapshot under m_pendingRemoteMutex (worker-thread callback).
    void stageSnapshot(const RemoteMetaSync::Snapshot &snap);

    Application &m_app;
};
