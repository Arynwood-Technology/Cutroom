/*
    SPDX-FileCopyrightText: 2026 Lorelei Noble <lorelei@arynwood.com>
    SPDX-License-Identifier: GPL-3.0-only
*/

#pragma once

#include "aiagent.h"

#include <QPlainTextEdit>
#include <QTimer>
#include <QWidget>

class QComboBox;
class QFrame;
class QLabel;
class QTextBrowser;
class QToolButton;

namespace AiChat {

/** @class ChatInput
 *  @brief Multi-line message box where Enter sends and Shift+Enter starts a new line. */
class ChatInput : public QPlainTextEdit
{
    Q_OBJECT
public:
    explicit ChatInput(QWidget *parent = nullptr);

Q_SIGNALS:
    void sendRequested();

protected:
    void keyPressEvent(QKeyEvent *event) override;
};

/** @class AssistantWidget
 *  @brief The Cutroom assistant panel: a chat with a local Ollama model or an OpenAI-compatible endpoint that edits the open
 *  project by calling the mcp-kdenlive tools. The conversation logic lives in Agent; this class is only the view and the settings. */
class AssistantWidget : public QWidget
{
    Q_OBJECT
public:
    explicit AssistantWidget(QWidget *parent = nullptr);

    /** @brief Put the keyboard cursor in the message box, so someone who has just launched the panel can start typing. */
    void focusInput();

protected:
    void showEvent(QShowEvent *event) override;

private Q_SLOTS:
    void slotSend();
    void slotSendOrStop();
    void slotNewConversation();
    void slotConfigure();
    void slotRefreshStatus();
    void slotModelChosen();
    void slotExampleClicked(const QUrl &link);
    void slotBusyChanged(bool busy);
    void slotAssistantStarted();
    void slotAssistantDelta(const QString &text);
    void slotAssistantFinished();
    void slotToolStarted(const QString &name, const QJsonObject &arguments);
    void slotToolFinished(const QString &name, bool ok, const QString &result);
    void slotApprovalRequested(const QString &name, const QJsonObject &arguments);
    void slotError(const QString &message);
    void renderAssistant();

private:
    enum class CheckState { Unknown, Ok, Failed };

    AgentSettings currentSettings() const;
    QString savedModel() const;
    void showWelcome();
    void updateStatus();
    void updateSendButton();
    /** @brief Add a labelled block to the transcript and return the position where its body starts. */
    int beginMessage(const QString &label, const QColor &labelColor);
    void appendNotice(const QString &text, bool isError);
    void setToolLine(const QString &text, bool isError);
    bool isScrolledToEnd() const;
    void scrollToEnd();

    Agent m_agent;
    QComboBox *m_modelCombo;
    QToolButton *m_newButton;
    QToolButton *m_configureButton;
    QLabel *m_statusLabel;
    QTextBrowser *m_transcript;
    QFrame *m_approvalFrame;
    QLabel *m_approvalLabel;
    ChatInput *m_input;
    QToolButton *m_sendButton;

    QTimer m_renderTimer;
    QString m_assistantText;
    int m_assistantStart{0};
    int m_toolStart{0};
    QString m_toolName;
    QString m_toolArguments;
    bool m_toolRunning{false};
    bool m_showingWelcome{false};
    /** @brief Whether new text scrolls into view; off while the user has scrolled up to read something older. */
    bool m_followEnd{true};
    bool m_remoteNoticeShown{false};

    CheckState m_modelServerState{CheckState::Unknown};
    CheckState m_toolServerState{CheckState::Unknown};
    QString m_modelServerError;
    QString m_toolServerError;
    int m_toolCount{0};
    /** @brief Bumped for every status check so that a slow, outdated answer is ignored. */
    int m_statusCheck{0};
};

} // namespace AiChat
