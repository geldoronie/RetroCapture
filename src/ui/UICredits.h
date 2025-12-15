#pragma once

// Forward declarations
class UIManager;

/**
 * Janela de créditos
 */
class UICredits
{
public:
    UICredits(UIManager *uiManager);
    ~UICredits();

    void render();
    void setVisible(bool visible);
    bool isVisible() const { return m_visible; }

private:
    UIManager *m_uiManager = nullptr;
    bool m_visible = false;
};
