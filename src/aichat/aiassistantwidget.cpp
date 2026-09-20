/*
    SPDX-FileCopyrightText: 2026 Lorelei Noble <lorelei@arynwood.com>
    SPDX-License-Identifier: GPL-3.0-only
*/

#include "aiassistantwidget.h"
#include "aitoolutils.h"
#include "kdenlivesettings.h"

#include <KColorScheme>
#include <KLocalizedString>

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QImage>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QShowEvent>
#include <QSpinBox>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QToolButton>
#include <QVBoxLayout>

namespace AiChat {

namespace {

constexpr int s_previewChars = 160;
constexpr int s_argumentChars = 80;
constexpr int s_renderIntervalMs = 50;

enum class Reach { Local, Network, Remote };

/** How far from this machine the model runs, which is what the user needs to know before sending it project details. */
Reach reachOf(const QUrl &url)
{
    const QString host = url.host().toLower();
    if (host.isEmpty() || host == QLatin1String("localhost")) {
        return Reach::Local;
    }
    QHostAddress address;
    if (address.setAddress(host)) {
        if (address.isLoopback()) {
            return Reach::Local;
        }
        static const QStringList privateRanges = {QStringLiteral("10.0.0.0/8"),     QStringLiteral("172.16.0.0/12"), QStringLiteral("192.168.0.0/16"),
                                                  QStringLiteral("169.254.0.0/16"), QStringLiteral("fc00::/7"),      QStringLiteral("fe80::/10")};
        for (const QString &range : privateRanges) {
            if (address.isInSubnet(QHostAddress::parseSubnet(range))) {
                return Reach::Network;
            }
        }
        return Reach::Remote;
    }
    return host.endsWith(QLatin1String(".local")) || !host.contains(QLatin1Char('.')) ? Reach::Network : Reach::Remote;
}

QString elided(QString text, int maxChars)
{
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return text.size() > maxChars ? text.left(maxChars - 1) + QStringLiteral("…") : text;
}

QString argumentsText(const QJsonObject &arguments)
{
    return elided(QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact)), s_argumentChars);
}

QToolButton *iconButton(const QString &icon, const QString &tooltip, QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setIcon(QIcon::fromTheme(icon));
    button->setToolTip(tooltip);
    button->setAutoRaise(true);
    return button;
}

} // namespace

ChatInput::ChatInput(QWidget *parent)
    : QPlainTextEdit(parent)
{
}

void ChatInput::keyPressEvent(QKeyEvent *event)
{
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && !(event->modifiers() & Qt::ShiftModifier)) {
        event->accept();
        Q_EMIT sendRequested();
        return;
    }
    QPlainTextEdit::keyPressEvent(event);
}

AssistantWidget::AssistantWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    auto *header = new QHBoxLayout;
    m_modelCombo = new QComboBox(this);
    m_modelCombo->setEditable(true);
    m_modelCombo->setInsertPolicy(QComboBox::NoInsert);
    m_modelCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_modelCombo->setMinimumContentsLength(12);
    m_modelCombo->setToolTip(i18n("The model the assistant talks to"));
    m_modelCombo->lineEdit()->setPlaceholderText(i18n("Model"));
    m_newButton = iconButton(QStringLiteral("edit-clear-history"), i18n("Start a new conversation"), this);
    m_configureButton = iconButton(QStringLiteral("configure"), i18n("Assistant settings"), this);
    header->addWidget(m_modelCombo, 1);
    header->addWidget(m_newButton);
    header->addWidget(m_configureButton);
    layout->addLayout(header);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setTextFormat(Qt::RichText);
    m_statusLabel->setWordWrap(true);
    QFont small = font();
    small.setPointSizeF(small.pointSizeF() * 0.9);
    m_statusLabel->setFont(small);
    layout->addWidget(m_statusLabel);

    m_transcript = new QTextBrowser(this);
    m_transcript->setOpenLinks(false);
    m_transcript->setFrameShape(QFrame::NoFrame);
    layout->addWidget(m_transcript, 1);
    // Keep the newest text in view. The scrollbar's range only grows once the document has been laid out, which is after the text went
    // in, so the view follows the range instead of jumping to a maximum that is already out of date. Scrolling by hand decides whether
    // following goes on: at the bottom it does, anywhere else it stops.
    QScrollBar *transcriptBar = m_transcript->verticalScrollBar();
    connect(transcriptBar, &QScrollBar::rangeChanged, this, [this](int, int maximum) {
        // The welcome text is read from the top: its logo and heading come first.
        if (m_followEnd && !m_showingWelcome) {
            m_transcript->verticalScrollBar()->setValue(maximum);
        }
    });
    connect(transcriptBar, &QAbstractSlider::actionTriggered, this, [this]() {
        // The action is reported before it is applied.
        QTimer::singleShot(0, this, [this]() { m_followEnd = isScrolledToEnd(); });
    });

    m_approvalFrame = new QFrame(this);
    m_approvalFrame->setFrameShape(QFrame::StyledPanel);
    auto *approvalLayout = new QHBoxLayout(m_approvalFrame);
    m_approvalLabel = new QLabel(m_approvalFrame);
    m_approvalLabel->setTextFormat(Qt::RichText);
    m_approvalLabel->setWordWrap(true);
    auto *allow = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-ok-apply")), i18n("Allow"), m_approvalFrame);
    auto *deny = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-cancel")), i18n("Deny"), m_approvalFrame);
    approvalLayout->addWidget(m_approvalLabel, 1);
    approvalLayout->addWidget(allow);
    approvalLayout->addWidget(deny);
    m_approvalFrame->hide();
    layout->addWidget(m_approvalFrame);

    auto *inputRow = new QHBoxLayout;
    m_input = new ChatInput(this);
    m_input->setPlaceholderText(i18n("Ask the assistant to edit your project…  (Enter to send, Shift+Enter for a new line)"));
    m_input->setFixedHeight(m_input->fontMetrics().lineSpacing() * 3 + 16);
    m_sendButton = iconButton(QStringLiteral("document-send"), QString(), this);
    m_sendButton->setAutoRaise(false);
    m_sendButton->setIconSize(QSize(24, 24));
    inputRow->addWidget(m_input, 1);
    inputRow->addWidget(m_sendButton, 0, Qt::AlignBottom);
    layout->addLayout(inputRow);

    m_renderTimer.setSingleShot(true);
    m_renderTimer.setInterval(s_renderIntervalMs);
    connect(&m_renderTimer, &QTimer::timeout, this, &AssistantWidget::renderAssistant);

    connect(m_input, &ChatInput::sendRequested, this, &AssistantWidget::slotSend);
    connect(m_sendButton, &QToolButton::clicked, this, &AssistantWidget::slotSendOrStop);
    connect(m_newButton, &QToolButton::clicked, this, &AssistantWidget::slotNewConversation);
    connect(m_configureButton, &QToolButton::clicked, this, &AssistantWidget::slotConfigure);
    connect(m_modelCombo, QOverload<int>::of(&QComboBox::activated), this, &AssistantWidget::slotModelChosen);
    connect(m_modelCombo->lineEdit(), &QLineEdit::editingFinished, this, &AssistantWidget::slotModelChosen);
    connect(m_transcript, &QTextBrowser::anchorClicked, this, &AssistantWidget::slotExampleClicked);
    connect(allow, &QPushButton::clicked, this, [this]() {
        m_approvalFrame->hide();
        m_agent.resolveApproval(true);
    });
    connect(deny, &QPushButton::clicked, this, [this]() {
        m_approvalFrame->hide();
        m_agent.resolveApproval(false);
    });

    connect(&m_agent, &Agent::busyChanged, this, &AssistantWidget::slotBusyChanged);
    connect(&m_agent, &Agent::assistantStarted, this, &AssistantWidget::slotAssistantStarted);
    connect(&m_agent, &Agent::assistantDelta, this, &AssistantWidget::slotAssistantDelta);
    connect(&m_agent, &Agent::assistantFinished, this, &AssistantWidget::slotAssistantFinished);
    connect(&m_agent, &Agent::toolStarted, this, &AssistantWidget::slotToolStarted);
    connect(&m_agent, &Agent::toolFinished, this, &AssistantWidget::slotToolFinished);
    connect(&m_agent, &Agent::approvalRequested, this, &AssistantWidget::slotApprovalRequested);
    connect(&m_agent, &Agent::errorOccurred, this, &AssistantWidget::slotError);

    m_agent.setSettings(currentSettings());
    showWelcome();
    updateStatus();
    updateSendButton();
}

void AssistantWidget::focusInput()
{
    m_input->setFocus(Qt::OtherFocusReason);
}

void AssistantWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // Checked when first shown rather than at startup, so a closed panel never touches the network.
    if (m_modelServerState == CheckState::Unknown && m_toolServerState == CheckState::Unknown) {
        slotRefreshStatus();
    }
}

AgentSettings AssistantWidget::currentSettings() const
{
    AgentSettings settings;
    settings.provider = KdenliveSettings::aiProvider() == 1 ? Provider::OpenAiCompatible : Provider::Ollama;
    const bool ollama = settings.provider == Provider::Ollama;
    settings.llmUrl = QUrl::fromUserInput(ollama ? KdenliveSettings::aiOllamaUrl() : KdenliveSettings::aiOpenAiUrl());
    settings.apiKey = ollama ? QString() : KdenliveSettings::aiApiKey();
    settings.model = savedModel();
    settings.mcpUrl = QUrl::fromUserInput(KdenliveSettings::aiMcpUrl());
    settings.contextSize = KdenliveSettings::aiContextSize();
    settings.maxToolRounds = KdenliveSettings::aiMaxToolRounds();
    settings.relevantToolsOnly = KdenliveSettings::aiRelevantTools();
    settings.approval = ApprovalMode(qBound(0, KdenliveSettings::aiApproval(), 2));
    return settings;
}

QString AssistantWidget::savedModel() const
{
    return KdenliveSettings::aiProvider() == 1 ? KdenliveSettings::aiOpenAiModel() : KdenliveSettings::aiOllamaModel();
}

void AssistantWidget::showWelcome()
{
    const QString muted = palette().color(QPalette::Disabled, QPalette::Text).name();
    const QString accent = KColorScheme(QPalette::Active, KColorScheme::View).decoration(KColorScheme::FocusColor).color().name();
    const QStringList examples = {
        i18n("Show me a summary of the timeline"),
        i18n("List the clips in my project bin"),
        i18n("Add cross-dissolves between all the clips on the first video track"),
    };
    // The tree mark, from the application's resources (absent in a bare test build, in which case no image is drawn).
    const QImage logo(QStringLiteral(":/pics/arynwood-logo.png"));
    QString logoHtml;
    if (!logo.isNull()) {
        m_transcript->document()->addResource(QTextDocument::ImageResource, QUrl(QStringLiteral("cutroom-logo")), logo);
        logoHtml = QStringLiteral("<p><img src='cutroom-logo' width='72' height='72'></p>");
    }
    QString html =
        logoHtml + QStringLiteral("<h3 style='color:%1'>%2</h3><p>%3</p><p>%4</p><ul>")
                       .arg(accent, i18n("Cutroom Assistant").toHtmlEscaped(),
                            i18n("Describe an edit in plain language and the assistant carries it out on your open project, using the same tools as the "
                                 "Cutroom MCP server. It edits the project directly, so save a copy of anything important first.")
                                .toHtmlEscaped(),
                            i18n("Try:").toHtmlEscaped());
    for (const QString &example : examples) {
        html += QStringLiteral("<li><a href='prompt:%1'>%2</a></li>").arg(QString::fromLatin1(QUrl::toPercentEncoding(example)), example.toHtmlEscaped());
    }
    html += QStringLiteral("</ul><p style='color:%1'>%2</p>")
                .arg(muted, i18n("Arynwood Cutroom · part of the Arynwood open toolkit for local AI tools").toHtmlEscaped());
    m_showingWelcome = true;
    m_transcript->setHtml(html);
    m_transcript->verticalScrollBar()->setValue(0);
}

void AssistantWidget::updateStatus()
{
    const KColorScheme scheme(QPalette::Active, KColorScheme::View);
    auto dot = [&scheme](CheckState state) {
        const QColor color = state == CheckState::Ok       ? scheme.foreground(KColorScheme::PositiveText).color()
                             : state == CheckState::Failed ? scheme.foreground(KColorScheme::NegativeText).color()
                                                           : scheme.foreground(KColorScheme::InactiveText).color();
        return QStringLiteral("<span style='color:%1'>●</span>").arg(color.name());
    };
    const AgentSettings settings = currentSettings();
    const Reach reach = reachOf(settings.llmUrl);
    const QString where = reach == Reach::Local     ? i18n("Local model")
                          : reach == Reach::Network ? i18n("Local network: %1", settings.llmUrl.host())
                                                    : i18n("Remote: %1", settings.llmUrl.host());
    QString text = QStringLiteral("%1 %2 &nbsp;&nbsp; %3 %4")
                       .arg(dot(m_modelServerState), where.toHtmlEscaped(), dot(m_toolServerState),
                            m_toolServerState == CheckState::Ok ? i18np("%1 tool", "%1 tools", m_toolCount) : i18n("Tool server"));
    if (reach == Reach::Remote) {
        const QColor warning = scheme.foreground(KColorScheme::NeutralText).color();
        text += QStringLiteral("<br><span style='color:%1'>%2</span>").arg(warning.name(), i18n("Messages and tool results are sent to this service."));
    }
    m_statusLabel->setText(text);
    QStringList tips;
    if (!m_modelServerError.isEmpty()) {
        tips << i18n("Model server (%1): %2", settings.llmUrl.toString(), m_modelServerError);
    }
    if (!m_toolServerError.isEmpty()) {
        tips << i18n("Tool server (%1): %2", settings.mcpUrl.toString(), m_toolServerError);
    }
    m_statusLabel->setToolTip(tips.join(QLatin1Char('\n')));
}

void AssistantWidget::updateSendButton()
{
    const bool busy = m_agent.isBusy();
    m_sendButton->setIcon(QIcon::fromTheme(busy ? QStringLiteral("process-stop") : QStringLiteral("document-send")));
    m_sendButton->setToolTip(busy ? i18n("Stop") : i18n("Send"));
    m_configureButton->setEnabled(!busy);
    m_newButton->setEnabled(!busy);
    m_modelCombo->setEnabled(!busy);
}

void AssistantWidget::slotRefreshStatus()
{
    if (m_agent.isBusy()) {
        return;
    }
    m_agent.setSettings(currentSettings());
    m_modelServerState = CheckState::Unknown;
    m_toolServerState = CheckState::Unknown;
    m_modelServerError.clear();
    m_toolServerError.clear();
    updateStatus();
    const int check = ++m_statusCheck;

    m_agent.llm()->listModels([this, check](bool ok, const QStringList &models, const QString &error) {
        if (check != m_statusCheck) {
            return;
        }
        m_modelServerState = ok ? CheckState::Ok : CheckState::Failed;
        m_modelServerError = error;
        if (ok) {
            const QString saved = savedModel();
            QSignalBlocker blocker(m_modelCombo);
            m_modelCombo->clear();
            m_modelCombo->addItems(models);
            const int index = m_modelCombo->findText(saved);
            if (index >= 0) {
                m_modelCombo->setCurrentIndex(index);
            } else if (!saved.isEmpty()) {
                // Not on the server's list, but keep what the user chose; the server will say if it truly isn't there.
                m_modelCombo->setEditText(saved);
            } else if (KdenliveSettings::aiProvider() == 1) {
                // A hosted API lists dozens of unrelated models; there is no sensible one to pick for the user.
                m_modelCombo->setCurrentIndex(-1);
            } else if (!models.isEmpty()) {
                m_modelCombo->setCurrentIndex(m_modelCombo->findText(preferredModel(models)));
                slotModelChosen();
            }
        }
        updateStatus();
    });
    m_agent.refreshTools([this, check](bool ok, int count, const QString &error) {
        if (check != m_statusCheck) {
            return;
        }
        m_toolServerState = ok ? CheckState::Ok : CheckState::Failed;
        m_toolServerError = error;
        m_toolCount = count;
        updateStatus();
    });
}

void AssistantWidget::slotModelChosen()
{
    const QString name = m_modelCombo->currentText().trimmed();
    if (name.isEmpty() || name == savedModel()) {
        return;
    }
    if (KdenliveSettings::aiProvider() == 1) {
        KdenliveSettings::setAiOpenAiModel(name);
    } else {
        KdenliveSettings::setAiOllamaModel(name);
    }
    KdenliveSettings::self()->save();
}

void AssistantWidget::slotConfigure()
{
    QDialog dialog(this);
    dialog.setWindowTitle(i18n("Cutroom Assistant Settings"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;

    auto *provider = new QComboBox(&dialog);
    provider->addItem(i18n("Ollama (local)"));
    provider->addItem(i18n("OpenAI-compatible API"));
    provider->setCurrentIndex(KdenliveSettings::aiProvider() == 1 ? 1 : 0);
    auto *ollamaUrl = new QLineEdit(KdenliveSettings::aiOllamaUrl(), &dialog);
    auto *openAiUrl = new QLineEdit(KdenliveSettings::aiOpenAiUrl(), &dialog);
    openAiUrl->setToolTip(i18n("The API root, for example https://api.openai.com/v1 or http://localhost:1234/v1"));
    auto *apiKey = new QLineEdit(KdenliveSettings::aiApiKey(), &dialog);
    apiKey->setEchoMode(QLineEdit::Password);
    apiKey->setToolTip(i18n("Stored in Kdenlive's configuration file, obfuscated but not encrypted"));
    auto *mcpUrl = new QLineEdit(KdenliveSettings::aiMcpUrl(), &dialog);
    mcpUrl->setToolTip(i18n("Where the mcp-kdenlive service listens (MCP_TRANSPORT=http)"));
    auto *contextSize = new QSpinBox(&dialog);
    contextSize->setRange(2048, 262144);
    contextSize->setSingleStep(1024);
    contextSize->setValue(KdenliveSettings::aiContextSize());
    contextSize->setToolTip(i18n("The context window requested from Ollama, and the budget for how much conversation is sent along"));
    auto *rounds = new QSpinBox(&dialog);
    rounds->setRange(1, 30);
    rounds->setValue(KdenliveSettings::aiMaxToolRounds());
    auto *toolMode = new QComboBox(&dialog);
    toolMode->addItem(i18n("Only the tools relevant to each request"));
    toolMode->addItem(i18n("All tools"));
    toolMode->setCurrentIndex(KdenliveSettings::aiRelevantTools() ? 0 : 1);
    toolMode->setToolTip(i18n("All tools is about 20,000 tokens of tool descriptions. Use it only with models that have a very large context window."));
    auto *approval = new QComboBox(&dialog);
    approval->addItem(i18n("Every change to the project"));
    approval->addItem(i18n("Only actions that can lose work or overwrite files"));
    approval->addItem(i18n("Never"));
    // Stored as the ApprovalMode value, but listed strictest first.
    static const int approvalOrder[] = {int(ApprovalMode::EveryChange), int(ApprovalMode::RiskyOnly), int(ApprovalMode::Never)};
    for (int i = 0; i < 3; ++i) {
        if (approvalOrder[i] == qBound(0, KdenliveSettings::aiApproval(), 2)) {
            approval->setCurrentIndex(i);
        }
    }
    approval->setToolTip(i18n("Small local models sometimes make mistakes or repeat actions. Reading and looking around never asks."));

    form->addRow(i18n("Provider:"), provider);
    form->addRow(i18n("Ollama address:"), ollamaUrl);
    form->addRow(i18n("API address:"), openAiUrl);
    form->addRow(i18n("API key:"), apiKey);
    form->addRow(i18n("Tool server:"), mcpUrl);
    form->addRow(i18n("Context size:"), contextSize);
    form->addRow(i18n("Tool rounds per request:"), rounds);
    form->addRow(i18n("Tools sent to the model:"), toolMode);
    form->addRow(i18n("Ask before running:"), approval);
    layout->addLayout(form);

    auto *privacy =
        new QLabel(i18n("The assistant sends your messages and the results of its tool calls, which include project, clip and file names, to the model "
                        "server. With Ollama on this machine nothing leaves it. With a remote API they are sent to that service."),
                   &dialog);
    privacy->setWordWrap(true);
    layout->addWidget(privacy);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    auto syncProvider = [=](int index) {
        ollamaUrl->setEnabled(index == 0);
        openAiUrl->setEnabled(index == 1);
        apiKey->setEnabled(index == 1);
    };
    syncProvider(provider->currentIndex());
    connect(provider, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, syncProvider);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    KdenliveSettings::setAiProvider(provider->currentIndex());
    KdenliveSettings::setAiOllamaUrl(ollamaUrl->text().trimmed());
    KdenliveSettings::setAiOpenAiUrl(openAiUrl->text().trimmed());
    KdenliveSettings::setAiApiKey(apiKey->text());
    KdenliveSettings::setAiMcpUrl(mcpUrl->text().trimmed());
    KdenliveSettings::setAiContextSize(contextSize->value());
    KdenliveSettings::setAiMaxToolRounds(rounds->value());
    KdenliveSettings::setAiRelevantTools(toolMode->currentIndex() == 0);
    KdenliveSettings::setAiApproval(approvalOrder[approval->currentIndex()]);
    KdenliveSettings::self()->save();
    m_remoteNoticeShown = false;
    slotRefreshStatus();
}

void AssistantWidget::slotSendOrStop()
{
    if (m_agent.isBusy()) {
        m_agent.stop();
    } else {
        slotSend();
    }
}

void AssistantWidget::slotSend()
{
    const QString text = m_input->toPlainText().trimmed();
    if (text.isEmpty() || m_agent.isBusy()) {
        return;
    }
    if (m_modelCombo->currentText().trimmed().isEmpty()) {
        appendNotice(i18n("Choose a model first. If the list is empty, the model server isn't reachable; hover over the status line to see why."), true);
        return;
    }
    slotModelChosen();
    m_agent.setSettings(currentSettings());
    m_followEnd = true;

    if (m_showingWelcome) {
        m_transcript->clear();
        m_showingWelcome = false;
    }
    const AgentSettings settings = m_agent.settings();
    if (!m_remoteNoticeShown && reachOf(settings.llmUrl) == Reach::Remote) {
        m_remoteNoticeShown = true;
        appendNotice(i18n("This conversation is sent to %1: your messages and the results of tool calls, including project, clip and file names.",
                          settings.llmUrl.host()),
                     false);
    }
    const int bodyStart = beginMessage(i18n("You"), palette().color(QPalette::Text));
    QTextCursor cursor(m_transcript->document());
    cursor.setPosition(bodyStart);
    cursor.insertText(text);
    if (m_followEnd) {
        scrollToEnd();
    }
    m_input->clear();
    m_agent.send(text);
}

void AssistantWidget::slotNewConversation()
{
    m_agent.reset();
    m_assistantText.clear();
    m_toolRunning = false;
    m_approvalFrame->hide();
    m_followEnd = true;
    m_transcript->clear();
    showWelcome();
}

void AssistantWidget::slotExampleClicked(const QUrl &link)
{
    if (link.scheme() == QLatin1String("prompt")) {
        m_input->setPlainText(link.path(QUrl::FullyDecoded));
        m_input->setFocus();
        m_input->moveCursor(QTextCursor::End);
    }
}

void AssistantWidget::slotBusyChanged(bool busy)
{
    if (!busy) {
        m_approvalFrame->hide();
        if (m_toolRunning) {
            // The tool was cut short by Stop; it never reported back.
            setToolLine(QStringLiteral("⏹ %1(%2) — %3").arg(m_toolName, m_toolArguments, i18n("stopped")), true);
            m_toolRunning = false;
        }
        m_input->setFocus();
    }
    updateSendButton();
}

int AssistantWidget::beginMessage(const QString &label, const QColor &labelColor)
{
    QTextDocument *document = m_transcript->document();
    QTextCursor cursor(document);
    cursor.movePosition(QTextCursor::End);
    QTextBlockFormat spaced;
    spaced.setTopMargin(10);
    if (document->isEmpty()) {
        cursor.setBlockFormat(spaced);
    } else {
        cursor.insertBlock(spaced, QTextCharFormat());
    }
    QTextCharFormat labelFormat;
    labelFormat.setFontWeight(QFont::Bold);
    labelFormat.setForeground(labelColor);
    cursor.insertText(label, labelFormat);
    cursor.insertBlock(QTextBlockFormat(), QTextCharFormat());
    return cursor.position();
}

void AssistantWidget::appendNotice(const QString &text, bool isError)
{
    if (m_showingWelcome) {
        m_transcript->clear();
        m_showingWelcome = false;
    }
    const KColorScheme scheme(QPalette::Active, KColorScheme::View);
    QTextCursor cursor(m_transcript->document());
    cursor.movePosition(QTextCursor::End);
    QTextBlockFormat spaced;
    spaced.setTopMargin(8);
    if (m_transcript->document()->isEmpty()) {
        cursor.setBlockFormat(spaced);
    } else {
        cursor.insertBlock(spaced, QTextCharFormat());
    }
    QTextCharFormat format;
    format.setForeground(isError ? scheme.foreground(KColorScheme::NegativeText) : scheme.foreground(KColorScheme::InactiveText));
    cursor.insertText(text, format);
    if (m_followEnd) {
        scrollToEnd();
    }
}

void AssistantWidget::slotAssistantStarted()
{
    m_assistantText.clear();
    m_assistantStart = beginMessage(i18n("Cutroom"), KColorScheme(QPalette::Active, KColorScheme::View).decoration(KColorScheme::FocusColor).color());
    if (m_followEnd) {
        scrollToEnd();
    }
}

void AssistantWidget::slotAssistantDelta(const QString &text)
{
    m_assistantText += text;
    if (!m_renderTimer.isActive()) {
        m_renderTimer.start();
    }
}

void AssistantWidget::slotAssistantFinished()
{
    m_renderTimer.stop();
    renderAssistant();
}

void AssistantWidget::renderAssistant()
{
    QTextDocument markdown;
    markdown.setMarkdown(m_assistantText);
    QTextCursor cursor(m_transcript->document());
    cursor.setPosition(m_assistantStart);
    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    cursor.insertFragment(QTextDocumentFragment(&markdown));
    if (m_followEnd) {
        scrollToEnd();
    }
}

void AssistantWidget::setToolLine(const QString &text, bool isError)
{
    const KColorScheme scheme(QPalette::Active, KColorScheme::View);
    QTextCursor cursor(m_transcript->document());
    cursor.setPosition(m_toolStart);
    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    QTextCharFormat format;
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSizeF(font().pointSizeF() * 0.9);
    format.setFont(mono);
    format.setForeground(isError ? scheme.foreground(KColorScheme::NegativeText) : scheme.foreground(KColorScheme::InactiveText));
    cursor.insertText(text, format);
}

void AssistantWidget::slotToolStarted(const QString &name, const QJsonObject &arguments)
{
    // A reply's own text is finished by now; anything that follows is a new block.
    QTextCursor cursor(m_transcript->document());
    cursor.movePosition(QTextCursor::End);
    QTextBlockFormat spaced;
    spaced.setTopMargin(4);
    cursor.insertBlock(spaced, QTextCharFormat());
    m_toolStart = cursor.position();
    m_toolName = name;
    m_toolArguments = argumentsText(arguments);
    m_toolRunning = true;
    setToolLine(QStringLiteral("▸ %1(%2)").arg(m_toolName, m_toolArguments), false);
    if (m_followEnd) {
        scrollToEnd();
    }
}

void AssistantWidget::slotToolFinished(const QString &name, bool ok, const QString &result)
{
    Q_UNUSED(name)
    m_toolRunning = false;
    setToolLine(
        QStringLiteral("%1 %2(%3) → %4").arg(ok ? QStringLiteral("✓") : QStringLiteral("✗"), m_toolName, m_toolArguments, elided(result, s_previewChars)), !ok);
    if (m_followEnd) {
        scrollToEnd();
    }
}

void AssistantWidget::slotApprovalRequested(const QString &name, const QJsonObject &arguments)
{
    m_approvalLabel->setText(i18n("The assistant wants to run <b>%1</b><br><tt>%2</tt>", name.toHtmlEscaped(), argumentsText(arguments).toHtmlEscaped()));
    m_approvalFrame->show();
}

void AssistantWidget::slotError(const QString &message)
{
    appendNotice(message, true);
}

bool AssistantWidget::isScrolledToEnd() const
{
    const QScrollBar *bar = m_transcript->verticalScrollBar();
    return bar->value() >= bar->maximum() - 24;
}

void AssistantWidget::scrollToEnd()
{
    QScrollBar *bar = m_transcript->verticalScrollBar();
    bar->setValue(bar->maximum());
}

} // namespace AiChat
