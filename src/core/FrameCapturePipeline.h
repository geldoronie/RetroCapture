#pragma once

// #157 — FrameCapturePipeline: the per-frame capture -> shader -> distribute
// pipeline extracted out of Application (behavior-preserving). It does not own
// any state; it references the Application and accesses its collaborators and
// per-frame buffers directly (Application declares it a friend). Moving the
// ~1340-line render body here shrinks the Application god-object without
// changing init order, ownership, or threading.

class Application;

class FrameCapturePipeline
{
public:
    explicit FrameCapturePipeline(Application &app);

    // Per-frame render + shader + capture/push. Returns true when the frame was
    // already presented (caller should skip the rest of its loop iteration).
    bool renderAndDistributeFrame();

private:
    Application &m_app;
};
