#pragma once

// #159 — UICallbackWiring: the UIManager callback registration initUI() did
// (the five wire*Callbacks groups from #153), moved out of Application.
// Reference-based: holds an Application& (friend); the registered lambdas reach
// Application directly. A persistent Application member declared BEFORE m_ui so
// it outlives the UIManager that stores the lambdas.

class Application;

class UICallbackWiring
{
public:
    explicit UICallbackWiring(Application &app);

    // Register every UIManager callback group. Call once, after m_ui is created.
    void wireAll();

private:
    void wireVisualCallbacks();
    void wireStreamingCallbacks();
    void wireRecordingCallbacks();
    void wireWebPortalCallbacks();
    void wireSourceAndMiscCallbacks();

    Application &m_app;
};
