// Copyright (c) 2016-2025 The Korsh Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/masternodelist.h>
#include <qt/forms/ui_masternodelist.h>
#include <qt/korshfeatures.h>
#include <qt/masternodewizardconfig.h>

#include <coins.h>
#include <evo/deterministicmns.h>
#include <evo/dmn_types.h>
#include <fs.h>
#include <index/txindex.h>
#include <governance/governance.h>
#include <net.h>
#include <net_processing.h>
#include <rpc/client.h>
#include <saltedhasher.h>
#include <util/system.h>

#include <qt/clientmodel.h>
#include <qt/descriptiondialog.h>
#include <qt/guiutil.h>
#include <qt/guiutil_font.h>
#include <qt/walletmodel.h>

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHostAddress>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>

#include <QScrollArea>
#include <QPushButton>
#include <QRegularExpression>
#include <QStringList>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>
#include <QWizard>
#include <QWizardPage>

#include <univalue.h>


#include <set>

#include <vector>

namespace {
constexpr int MASTERNODELIST_UPDATE_SECONDS{3};

QString ExtractRpcError(const UniValue& err)
{
    if (!err.isObject()) {
        return QString::fromStdString(err.write());
    }
    const UniValue& message = err.find_value("message");
    if (message.isStr()) {
        return QString::fromStdString(message.get_str());
    }
    return QString::fromStdString(err.write());
}

class MasternodeSetupWizard final : public QWizard
{
public:
    explicit MasternodeSetupWizard(QWidget* parent, WalletModel* wallet_model);

protected:
    void accept() override;
    void reject() override;

private:
    enum class MnType {
        Regular,
        Evo,
    };

    WalletModel* m_wallet_model{nullptr};
    QComboBox* m_mn_type{nullptr};
    QLabel* m_collateral_label{nullptr};
    QLineEdit* m_ip{nullptr};
    QLineEdit* m_port{nullptr};
    QLineEdit* m_collateral_address{nullptr};
    QLineEdit* m_owner_address{nullptr};
    QLineEdit* m_voting_address{nullptr};
    QLineEdit* m_payout_address{nullptr};
    QLineEdit* m_fee_address{nullptr};
    QGroupBox* m_evo_group{nullptr};
    QLineEdit* m_evo_platform_node_id{nullptr};
    QLineEdit* m_evo_platform_p2p_addrs{nullptr};
    QLineEdit* m_evo_platform_https_addrs{nullptr};
    QLineEdit* m_bls_secret{nullptr};
    QLineEdit* m_bls_public{nullptr};
    QPlainTextEdit* m_summary{nullptr};
    bool m_restart_required{false};
    bool m_sent{false};
    bool m_busy{false};
    QString m_raw_transaction;
    QString m_txid;
    QString m_recovery_path;
    int m_review_page_id{-1};

    [[nodiscard]] MnType currentType() const
    {
        if (!m_mn_type) {
            return MnType::Regular;
        }
        const QVariant current_data{m_mn_type->currentData()};
        if (current_data.isValid()) {
            const int type_value{current_data.toInt()};
            if (type_value == static_cast<int>(MnType::Evo)) {
                return MnType::Evo;
            }
        }
        return MnType::Regular;
    }

    [[nodiscard]] std::string walletUri() const
    {
        if (!m_wallet_model) {
            return {};
        }
        const QByteArray encoded{QUrl::toPercentEncoding(m_wallet_model->getWalletName())};
        return "/wallet/" + std::string(encoded.constData(), encoded.length());
    }

    [[nodiscard]] QString serviceAddress() const
    {
        QString host = m_ip->text().trimmed();
        if (host.contains(':') && !host.startsWith('[')) host = '[' + host + ']';
        return QString("%1:%2").arg(host, m_port->text().trimmed());
    }

    bool execWalletRpc(const std::string& method, const std::vector<std::string>& args, UniValue& out, QString& error) const;
    bool execRpc(const std::string& method, const UniValue& params, UniValue& out, QString& error) const;
    bool validateInput(QString& error) const;
    bool autoFillAddresses();
    bool generateBls();
    bool deriveBlsPublic(bool legacy_bls, QString& public_key, QString& error) const;
    bool saveOperatorSecretToConfig(QString& error);
    bool registerMasternode(QString& txid, QString& registered_operator_pubkey, QString& error);
    void updateTypeUi();
    void updateSummary();
};

MasternodeSetupWizard::MasternodeSetupWizard(QWidget* parent, WalletModel* wallet_model) :
    QWizard(parent),
    m_wallet_model(wallet_model)
{
    setWindowTitle(tr("Masternode Setup Wizard"));
    setWizardStyle(QWizard::ModernStyle);
    setMinimumWidth(860);

    auto* intro_page = new QWizardPage();
    intro_page->setTitle(tr("Welcome"));
    auto* intro_layout = new QVBoxLayout(intro_page);
    auto* intro_text = new QLabel(tr("This wizard helps you create and register a masternode without typing long manual commands.\n\n"
                                     "Flow:\n"
                                     "1. Set network and payout addresses\n"
                                     "2. Generate BLS operator key\n"
                                     "3. Register masternode transaction (ProTx)\n"
                                     "4. Save operator key into korsh.conf\n\n"
                                     "Tip: Use 'Auto-fill from wallet' to generate all required addresses."));
    intro_text->setWordWrap(true);
    intro_layout->addWidget(intro_text);
    addPage(intro_page);

    auto* details_page = new QWizardPage();
    details_page->setTitle(tr("Network and Addresses"));
    auto* details_layout = new QVBoxLayout(details_page);
    auto* details_form = new QFormLayout();
    m_mn_type = new QComboBox(details_page);
    m_mn_type->addItem(tr("Regular (1,500 KSH)"), static_cast<int>(MnType::Regular));
    // Korsh: Evo masternodes require the Platform quorum types, so only offer
    // them once the network can actually register one.
    if (KorshFeatures::EvoEnabled()) {
        m_mn_type->addItem(tr("Evo (7,500 KSH)"), static_cast<int>(MnType::Evo));
    }
    details_form->addRow(tr("Masternode type"), m_mn_type);
    m_collateral_label = new QLabel(details_page);
    details_form->addRow(tr("Required collateral"), m_collateral_label);

    m_ip = new QLineEdit(details_page);
    m_ip->setPlaceholderText(tr("Public IP (example: 203.0.113.10)"));
    details_form->addRow(tr("Public IP"), m_ip);

    m_port = new QLineEdit(details_page);
    m_port->setText(QString::number(Params().GetDefaultPort()));
    details_form->addRow(tr("Core P2P port"), m_port);

    m_collateral_address = new QLineEdit(details_page);
    details_form->addRow(tr("Collateral address"), m_collateral_address);
    m_owner_address = new QLineEdit(details_page);
    details_form->addRow(tr("Owner address"), m_owner_address);
    m_voting_address = new QLineEdit(details_page);
    details_form->addRow(tr("Voting address"), m_voting_address);
    m_payout_address = new QLineEdit(details_page);
    details_form->addRow(tr("Payout address"), m_payout_address);
    m_fee_address = new QLineEdit(details_page);
    m_fee_address->setPlaceholderText(tr("Automatic (wallet)"));
    m_fee_address->setReadOnly(true);
    m_fee_address->setEnabled(false);
    details_form->addRow(tr("Fee source"), m_fee_address);

    auto* auto_fill = new QPushButton(tr("Auto-fill from wallet"), details_page);
    details_form->addRow(QString{}, auto_fill);

    m_evo_group = new QGroupBox(tr("Evo extras"), details_page);
    auto* evo_form = new QFormLayout(m_evo_group);
    m_evo_platform_node_id = new QLineEdit(m_evo_group);
    m_evo_platform_node_id->setPlaceholderText(tr("Platform Node ID (hex)"));
    evo_form->addRow(tr("Platform Node ID"), m_evo_platform_node_id);
    m_evo_platform_p2p_addrs = new QLineEdit(m_evo_group);
    m_evo_platform_p2p_addrs->setText(QString::number(Params().GetDefaultPlatformP2PPort()));
    evo_form->addRow(tr("Platform P2P"), m_evo_platform_p2p_addrs);
    m_evo_platform_https_addrs = new QLineEdit(m_evo_group);
    m_evo_platform_https_addrs->setText(QString::number(Params().GetDefaultPlatformHTTPPort()));
    evo_form->addRow(tr("Platform HTTPS"), m_evo_platform_https_addrs);

    auto* details_inner = new QWidget();
    auto* details_inner_layout = new QVBoxLayout(details_inner);
    details_inner_layout->setContentsMargins(0, 0, 0, 0);
    details_inner_layout->addLayout(details_form);
    details_inner_layout->addWidget(m_evo_group);

    auto* details_scroll = new QScrollArea();
    details_scroll->setWidgetResizable(true);
    details_scroll->setFrameShape(QFrame::NoFrame);
    details_scroll->setWidget(details_inner);
    details_layout->addWidget(details_scroll);
    addPage(details_page);

    auto* bls_page = new QWizardPage();
    bls_page->setTitle(tr("Operator BLS Key"));
    auto* bls_layout = new QVBoxLayout(bls_page);
    auto* bls_text = new QLabel(tr("Generate operator key pair. The secret key will be saved to korsh.conf as masternodeblsprivkey after the ProTx registration succeeds."));
    bls_text->setWordWrap(true);
    bls_layout->addWidget(bls_text);
    auto* bls_form = new QFormLayout();
    m_bls_secret = new QLineEdit(bls_page);
    m_bls_secret->setReadOnly(true);
    m_bls_public = new QLineEdit(bls_page);
    m_bls_public->setReadOnly(true);
    bls_form->addRow(tr("BLS secret"), m_bls_secret);
    bls_form->addRow(tr("BLS public"), m_bls_public);
    bls_layout->addLayout(bls_form);
    auto* generate_bls_btn = new QPushButton(tr("Generate BLS key"), bls_page);
    bls_layout->addWidget(generate_bls_btn, 0, Qt::AlignLeft);
    bls_layout->addStretch(1);
    addPage(bls_page);

    auto* review_page = new QWizardPage();
    review_page->setTitle(tr("Review and Create"));
    auto* review_layout = new QVBoxLayout(review_page);
    auto* review_text = new QLabel(tr("Review values below, then click Finish to create and register the masternode."));
    review_text->setWordWrap(true);
    review_layout->addWidget(review_text);
    m_summary = new QPlainTextEdit(review_page);
    m_summary->setReadOnly(true);
    review_layout->addWidget(m_summary);
    m_review_page_id = addPage(review_page);

    connect(m_mn_type, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { updateTypeUi(); });
    connect(auto_fill, &QPushButton::clicked, this, [this] {
        if (!autoFillAddresses()) {
            return;
        }
        updateSummary();
    });
    connect(generate_bls_btn, &QPushButton::clicked, this, [this] {
        if (!generateBls()) {
            return;
        }
        updateSummary();
    });
    connect(this, &QWizard::currentIdChanged, this, [this](int id) {
        if (id == m_review_page_id) {
            updateSummary();
        }
    });

    updateTypeUi();
}

bool MasternodeSetupWizard::execWalletRpc(const std::string& method, const std::vector<std::string>& args, UniValue& out, QString& error) const
{
    if (!m_wallet_model) {
        error = tr("Wallet is not loaded.");
        return false;
    }

    try {
        return execRpc(method, RPCConvertValues(method, args), out, error);
    } catch (const std::exception& e) {
        error = QString::fromStdString(e.what());
        return false;
    }
}

bool MasternodeSetupWizard::execRpc(const std::string& method, const UniValue& params, UniValue& out, QString& error) const
{
    try {
        out = m_wallet_model->node().executeRpc(method, params, walletUri());
        return true;
    } catch (const UniValue& rpc_error) {
        error = ExtractRpcError(rpc_error);
    } catch (const std::exception& e) {
        error = QString::fromStdString(e.what());
    }
    return false;
}

bool MasternodeSetupWizard::validateInput(QString& error) const
{
    if (!m_wallet_model) {
        error = tr("Wallet is not loaded.");
        return false;
    }

    if ((!gArgs.GetBoolArg("-listen", DEFAULT_LISTEN) && Params().RequireRoutableExternalIP()) ||
        !gArgs.GetBoolArg("-txindex", DEFAULT_TXINDEX) ||
        !gArgs.GetBoolArg("-peerbloomfilters", DEFAULT_PEERBLOOMFILTERS) ||
        gArgs.GetIntArg("-prune", 0) > 0 ||
        gArgs.GetIntArg("-maxconnections", DEFAULT_MAX_PEER_CONNECTIONS) < DEFAULT_MAX_PEER_CONNECTIONS ||
        gArgs.GetBoolArg("-disablegovernance", !DEFAULT_GOVERNANCE_ENABLE)) {
        error = tr("This node cannot restart as a masternode. Enable listen, txindex, peerbloomfilters and governance; disable pruning and allow at least %1 connections. Restart with those settings before registering.")
                    .arg(DEFAULT_MAX_PEER_CONNECTIONS);
        return false;
    }
    bool ok_port{false};
    const int port = m_port->text().trimmed().toInt(&ok_port);
    if (!ok_port || port < 1 || port > 65535) {
        error = tr("Invalid P2P port. Use a value between 1 and 65535.");
        return false;
    }
    // The RPC preparation below applies activation-aware endpoint rules.
    if (Params().NetworkIDString() == "main" && port != Params().GetDefaultPort()) {
        error = tr("The masternode service port must be %1, the Korsh P2P port for this network. "
                   "Port 8383 belongs to the old chain and will not connect.")
                    .arg(Params().GetDefaultPort());
        return false;
    }

    QString host = m_ip->text().trimmed();
    if (host.startsWith('[') && host.endsWith(']')) host = host.mid(1, host.size() - 2);
    if (QHostAddress(host).isNull()) {
        error = tr("A numeric public IP address is required.");
        return false;
    }

    auto require_valid_address = [this, &error](const QLineEdit* field, const QString& label) {
        const QString value{field->text().trimmed()};
        if (value.isEmpty()) {
            error = tr("%1 is required.").arg(label);
            return false;
        }
        if (!m_wallet_model->validateAddress(value)) {
            error = tr("%1 is not a valid Korsh address.").arg(label);
            return false;
        }
        return true;
    };

    if (!require_valid_address(m_collateral_address, tr("Collateral address"))) return false;
    if (!require_valid_address(m_owner_address, tr("Owner address"))) return false;
    if (!require_valid_address(m_voting_address, tr("Voting address"))) return false;
    if (!require_valid_address(m_payout_address, tr("Payout address"))) return false;
    if (m_bls_secret->text().trimmed().isEmpty() || m_bls_public->text().trimmed().isEmpty()) {
        error = tr("Generate BLS key pair before finishing.");
        return false;
    }

    if (currentType() == MnType::Evo) {
        if (m_evo_platform_node_id->text().trimmed().isEmpty()) {
            error = tr("Platform Node ID is required for Evo nodes.");
            return false;
        }
        if (m_evo_platform_p2p_addrs->text().trimmed().isEmpty()) {
            error = tr("Platform P2P value is required for Evo nodes.");
            return false;
        }
        if (m_evo_platform_https_addrs->text().trimmed().isEmpty()) {
            error = tr("Platform HTTPS value is required for Evo nodes.");
            return false;
        }
    }

    return true;
}

bool MasternodeSetupWizard::autoFillAddresses()
{
    if (!m_wallet_model || !m_raw_transaction.isEmpty()) return false;
    auto unlock_context{m_wallet_model->requestUnlock(false)};
    if (!unlock_context.isValid()) return false;
    QString error;
    QStringList addresses;
    for (const std::string label : {"mn_collateral", "mn_owner", "mn_voting", "mn_payout"}) {
        UniValue result;
        if (!execWalletRpc("getnewaddress", {label}, result, error) || !result.isStr()) {
            QMessageBox::warning(this, tr("MN Setup Wizard"), tr("Failed to auto-fill addresses: %1").arg(error));
            return false;
        }
        addresses << QString::fromStdString(result.get_str());
    }
    // Wallet address allocation cannot be rolled back, but never partially
    // replace the user's fields when a later keypool request fails.
    m_collateral_address->setText(addresses[0]);
    m_owner_address->setText(addresses[1]);
    m_voting_address->setText(addresses[2]);
    m_payout_address->setText(addresses[3]);
    m_fee_address->setText(tr("(auto)"));
    return true;
}

bool MasternodeSetupWizard::generateBls()
{
    if (!m_raw_transaction.isEmpty()) return false;
    QString error;
    UniValue result;
    if (!execWalletRpc("bls", {"generate"}, result, error)) {
        QMessageBox::warning(this, tr("MN Setup Wizard"), tr("Failed to generate BLS key: %1").arg(error));
        return false;
    }
    if (!result.isObject()) {
        QMessageBox::warning(this, tr("MN Setup Wizard"), tr("Unexpected RPC response for bls generate."));
        return false;
    }

    const UniValue& secret = result.find_value("secret");
    const UniValue& pub = result.find_value("public");
    if (!secret.isStr() || !pub.isStr()) {
        QMessageBox::warning(this, tr("MN Setup Wizard"), tr("RPC response does not include secret/public key."));
        return false;
    }

    m_bls_secret->setText(QString::fromStdString(secret.get_str()));
    m_bls_public->setText(QString::fromStdString(pub.get_str()));
    return true;
}

bool MasternodeSetupWizard::deriveBlsPublic(bool legacy_bls, QString& public_key, QString& error) const
{
    const QString secret{m_bls_secret->text().trimmed()};
    if (secret.isEmpty()) {
        error = tr("BLS secret key is empty.");
        return false;
    }

    UniValue result;
    if (!execWalletRpc("bls", {"fromsecret", secret.toStdString(), legacy_bls ? "true" : "false"}, result, error)) {
        return false;
    }
    if (!result.isObject()) {
        error = tr("Unexpected RPC response for bls fromsecret.");
        return false;
    }
    const UniValue& pub = result.find_value("public");
    if (!pub.isStr()) {
        error = tr("BLS public key is missing in RPC response.");
        return false;
    }

    public_key = QString::fromStdString(pub.get_str());
    return true;
}

bool MasternodeSetupWizard::saveOperatorSecretToConfig(QString& error)
{
    try {
        const fs::path path{GetConfigFile(gArgs.GetPathArg("-conf", BITCOIN_CONF_FILENAME))};
        const QString secret = m_bls_secret->text().trimmed();
        const std::string configured = gArgs.GetArg("-masternodeblsprivkey", "");
        // An explicit empty CLI/runtime setting also shadows the config file.
        if (gArgs.IsArgSet("-masternodeblsprivkey") && configured != secret.toStdString()) {
            error = tr("An operator key setting (possibly empty) overrides the intended key. Remove the conflicting setting before continuing; the wizard will not replace it.");
            return false;
        }
        bool changed = false;
        if (!MasternodeWizardConfig::Save(GUIUtil::PathToQString(path),
                QString::fromStdString(Params().NetworkIDString()), secret, changed, error)) return false;
        m_restart_required |= changed;
        return true;
    } catch (const std::exception& e) {
        error = QString::fromStdString(e.what());
        return false;
    }
}

bool MasternodeSetupWizard::registerMasternode(QString& txid, QString& registered_operator_pubkey, QString& error)
{
    const std::string service{serviceAddress().trimmed().toStdString()};
    auto send_protx = [&](bool legacy_bls, QString& out_error) -> bool {
        std::vector<std::string> args;
        QString operator_pubkey;
        if (!deriveBlsPublic(legacy_bls, operator_pubkey, out_error)) {
            return false;
        }

        if (currentType() == MnType::Regular) {
            args = {
                legacy_bls ? "register_fund_legacy" : "register_fund",
                m_collateral_address->text().trimmed().toStdString(),
                service,
                m_owner_address->text().trimmed().toStdString(),
                operator_pubkey.toStdString(),
                m_voting_address->text().trimmed().toStdString(),
                "0",
                m_payout_address->text().trimmed().toStdString(),
            };
        } else {
            args = {
                "register_fund_evo",
                m_collateral_address->text().trimmed().toStdString(),
                service,
                m_owner_address->text().trimmed().toStdString(),
                operator_pubkey.toStdString(),
                m_voting_address->text().trimmed().toStdString(),
                "0",
                m_payout_address->text().trimmed().toStdString(),
                m_evo_platform_node_id->text().trimmed().toStdString(),
                m_evo_platform_p2p_addrs->text().trimmed().toStdString(),
                m_evo_platform_https_addrs->text().trimmed().toStdString(),
            };
        }

        UniValue result;
        UniValue params;
        try {
            params = RPCConvertValues("protx", args);
        } catch (const std::exception& e) {
            out_error = QString::fromStdString(e.what());
            return false;
        }
        params.push_back(UniValue()); // Automatic wallet funding (JSON null).
        params.push_back(false); // Prepare/sign only. Never broadcast before fee approval.
        if (!execRpc("protx", params, result, out_error)) {
            return false;
        }
        if (!result.isStr()) {
            out_error = tr("Unexpected RPC response for ProTx registration.");
            return false;
        }

        UniValue decoded;
        if (!execWalletRpc("decoderawtransaction", {result.get_str()}, decoded, out_error) ||
            !decoded.isObject() || !decoded.find_value("proRegTx").isObject() ||
            !decoded["proRegTx"].find_value("pubKeyOperator").isStr()) {
            out_error = tr("Could not verify the prepared registration payload. Nothing sent.");
            return false;
        }
        txid = QString::fromStdString(result.get_str());
        registered_operator_pubkey = QString::fromStdString(decoded["proRegTx"]["pubKeyOperator"].get_str());
        return true;
    };

    QString first_error;
    if (send_protx(/*legacy_bls=*/false, first_error)) {
        return true;
    }

    if (first_error.toLower().contains("bad-protx-version")) {
        if (currentType() == MnType::Regular) {
            QString legacy_error;
            if (send_protx(/*legacy_bls=*/true, legacy_error)) {
                return true;
            }
            error = tr("%1 (legacy fallback failed: %2)").arg(first_error, legacy_error);
        } else {
            error = tr("Evo masternodes require V19 activation on this chain. "
                        "Please upgrade all nodes. (%1)").arg(first_error);
        }
        return false;
    }

    error = first_error;
    return false;
}

void MasternodeSetupWizard::updateTypeUi()
{
    const bool evo{currentType() == MnType::Evo};
    m_collateral_label->setText(evo ? tr("7500 KSH") : tr("1500 KSH"));
    m_evo_group->setVisible(evo);

    // The Platform Node ID must come from the node's actual P2P identity;
    // an arbitrary random identifier cannot configure a working Platform node.

}

void MasternodeSetupWizard::updateSummary()
{
    QStringList lines;
    const bool evo{currentType() == MnType::Evo};

    lines << tr("Type: %1").arg(evo ? tr("Evo") : tr("Regular"));
    lines << tr("Collateral: %1").arg(evo ? tr("7500 KSH") : tr("1500 KSH"));
    lines << tr("Core service: %1").arg(serviceAddress());
    lines << tr("Collateral address: %1").arg(m_collateral_address->text().trimmed());
    lines << tr("Owner address: %1").arg(m_owner_address->text().trimmed());
    lines << tr("Voting address: %1").arg(m_voting_address->text().trimmed());
    lines << tr("Payout address: %1").arg(m_payout_address->text().trimmed());
    lines << tr("Fee source: %1").arg(tr("(auto from wallet)"));
    lines << tr("BLS public key: %1").arg(m_bls_public->text().trimmed());

    if (evo) {
        lines << tr("Platform Node ID: %1").arg(m_evo_platform_node_id->text().trimmed());
        lines << tr("Platform P2P: %1").arg(m_evo_platform_p2p_addrs->text().trimmed());
        lines << tr("Platform HTTPS: %1").arg(m_evo_platform_https_addrs->text().trimmed());
    }

    const fs::path config_path{GetConfigFile(gArgs.GetPathArg("-conf", BITCOIN_CONF_FILENAME))};
    lines << tr("Config file: %1").arg(GUIUtil::PathToQString(config_path));
    lines << tr("Action on Finish: prepare NEW collateral, confirm the exact fee, save private recovery data, then broadcast and save masternodeblsprivkey.");
    m_summary->setPlainText(lines.join('\n'));
}

void MasternodeSetupWizard::reject()
{
    if (m_busy) return;
    if (!m_recovery_path.isEmpty()) {
        QMessageBox::warning(this, tr("MN Setup Wizard"),
            tr("Keep the private recovery file safe: %1\nIt contains the operator secret, signed transaction and transaction ID. Check the transaction status before starting another registration.")
                .arg(m_recovery_path));
    }
    QWizard::reject();
}

void MasternodeSetupWizard::accept()
{
    if (m_busy) return;
    m_busy = true;
    struct BusyReset { bool& busy; ~BusyReset() { busy = false; } } reset{m_busy};
    QString error;
    const QString config_file = GUIUtil::PathToQString(GetConfigFile(gArgs.GetPathArg("-conf", BITCOIN_CONF_FILENAME)));
    const QString network = QString::fromStdString(Params().NetworkIDString());
    const QString recovery = config_file + ".mnsetup-" + network + ".json";
    QLockFile recovery_lock(recovery + ".lock");
    if (!recovery_lock.tryLock(0)) {
        QMessageBox::warning(this, tr("MN Setup Wizard"), tr("Another registration is in progress."));
        return;
    }

    const auto config_preflight = [&]() {
        const QString configured = QString::fromStdString(gArgs.GetArg("-masternodeblsprivkey", ""));
        if (gArgs.IsArgSet("-masternodeblsprivkey") && configured != m_bls_secret->text().trimmed()) {
            error = tr("The effective operator key conflicts with this registration.");
            return false;
        }
        return MasternodeWizardConfig::Preflight(config_file, network, m_bls_secret->text().trimmed(), error);
    };
    if (m_raw_transaction.isEmpty()) {
        if (QFileInfo::exists(recovery) || QFileInfo(recovery).isSymLink()) {
            QMessageBox::warning(this, tr("MN Setup Wizard"),
                tr("A previous registration requires recovery. No new transaction was created. Keep this private file and reconcile its transaction ID before removing it: %1").arg(recovery));
            return;
        }
        if (!validateInput(error)) {
            QMessageBox::warning(this, tr("MN Setup Wizard"), error);
            return;
        }
        if (!config_preflight()) {
            QMessageBox::warning(this, tr("MN Setup Wizard"),
                tr("Configuration preflight failed. No transaction prepared or sent. %1\nBack up the configuration and use a regular, single-link file owned by your wallet user in a supported local data directory. Resolve file-access problems before retrying; do not loosen permissions.").arg(error));
            return;
        }
        auto unlock_context{m_wallet_model->requestUnlock(false)};
        if (!unlock_context.isValid()) return;
        QString raw, public_key;
        if (!registerMasternode(raw, public_key, error)) {
            QMessageBox::warning(this, tr("MN Setup Wizard"), error);
            return;
        }
        UniValue raw_array(UniValue::VARR), params(UniValue::VARR), tested;
        raw_array.push_back(raw.toStdString());
        params.push_back(raw_array);
        if (!execRpc("testmempoolaccept", params, tested, error) || !tested.isArray() || tested.size() != 1 ||
            !tested[0].find_value("allowed").isBool() || !tested[0]["allowed"].get_bool() ||
            !tested[0].find_value("txid").isStr() || !tested[0].find_value("fees").isObject() ||
            !tested[0]["fees"].find_value("base").isNum()) {
            QMessageBox::warning(this, tr("MN Setup Wizard"), tr("Transaction preflight failed; nothing sent. %1").arg(error));
            return;
        }
        const QString txid = QString::fromStdString(tested[0]["txid"].get_str());
        const QString fee = QString::fromStdString(tested[0]["fees"]["base"].getValStr());
        if (QMessageBox::question(this, tr("Confirm masternode registration"),
                tr("Create NEW collateral of %1 KSH, plus a transaction fee of %2 KSH?\nThis does not reuse an existing collateral output.\n\nThe operator secret and signed transaction will be saved privately to %3 before broadcast.")
                    .arg(currentType() == MnType::Evo ? "7500" : "1500", fee, recovery),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;

        UniValue journal(UniValue::VOBJ);
        journal.pushKV("network", network.toStdString());
        journal.pushKV("wallet", m_wallet_model->getWalletName().toStdString());
        journal.pushKV("config", config_file.toStdString());
        journal.pushKV("txid", txid.toStdString());
        journal.pushKV("raw", raw.toStdString());
        journal.pushKV("operator_secret", m_bls_secret->text().trimmed().toStdString());
        journal.pushKV("operator_public", public_key.toStdString());
        const QByteArray bytes = QByteArray::fromStdString(journal.write());
        if (!MasternodeWizardConfig::SaveRecovery(recovery, bytes, error)) {
            QMessageBox::critical(this, tr("MN Setup Wizard"), tr("Could not persist recovery data; nothing sent. %1").arg(error));
            return;
        }
        m_raw_transaction = raw;
        m_txid = txid;
        m_recovery_path = recovery;
        m_bls_public->setText(public_key);
        // After approval, all retries refer to this exact signed transaction.
        // Back/Generate cannot discard or substitute its operator identity.
        for (int id : pageIds()) page(id)->setEnabled(false);
        button(QWizard::BackButton)->setEnabled(false);
    }

    if (!m_sent) {
        // Approval and retries can outlive the first check. Do not submit a
        // transaction when config admission is already known to fail. Preserve
        // the exact signed intent/journal; a prior send may be ambiguous.
        if (!config_preflight()) {
            QMessageBox::warning(this, tr("MN Setup Wizard"),
                tr("Configuration preflight failed; this attempt did not submit the transaction. %1\nKeep the private recovery file: %2\nResolve the configuration access problem without loosening permissions, then retry this same transaction. Check its status before starting any other registration.").arg(error, m_recovery_path));
            return;
        }
        UniValue result;
        if (!execWalletRpc("sendrawtransaction", {m_raw_transaction.toStdString()}, result, error)) {
            // A response can be lost after submission. Resolve the exact txid,
            // never build another registration on an ambiguous failure.
            UniValue known;
            if (!execWalletRpc("getrawtransaction", {m_txid.toStdString()}, known, error) ||
                !known.isStr() || known.get_str() != m_raw_transaction.toStdString()) {
                QMessageBox::warning(this, tr("MN Setup Wizard"),
                    tr("Broadcast not confirmed. Retry sends only the same transaction.\nTxID: %1\nPrivate recovery: %2\n%3").arg(m_txid, m_recovery_path, error));
                return;
            }
        } else if (!result.isStr() || QString::fromStdString(result.get_str()) != m_txid) {
            QMessageBox::warning(this, tr("MN Setup Wizard"), tr("Unexpected broadcast response. Keep the recovery file and verify transaction %1.").arg(m_txid));
            return;
        }
        m_sent = true;
    }
    if (!saveOperatorSecretToConfig(error)) {
        QMessageBox::critical(this, tr("MN Setup Wizard"),
            tr("Transaction sent: %1\nOperator config could not be saved: %2\nPrivate recovery: %3\nFinish retries saving only; it will not register again.").arg(m_txid, error, m_recovery_path));
        return;
    }
    // The config now contains the key. Keep the recovery file as a private
    // backup; requiring explicit reconciliation prevents a reopened wizard
    // from silently registering again after an uncertain process exit.
    QMessageBox::information(this, tr("MN Setup Wizard"),
        tr("Registration transaction sent: %1\nOperator key saved to %2.\nPrivate recovery backup: %3\nRestart only after verifying masternode configuration. Registration still requires confirmation.")
            .arg(m_txid, config_file, m_recovery_path));
    QWizard::accept();
}
} // anonymous namespace

bool MasternodeListSortFilterProxyModel::filterAcceptsRow(int source_row, const QModelIndex& source_parent) const
{
    // "Type" filter
    if (m_type_filter != TypeFilter::All) {
        QModelIndex idx = sourceModel()->index(source_row, MasternodeModel::TYPE, source_parent);
        int type = sourceModel()->data(idx, Qt::EditRole).toInt();
        if (m_type_filter == TypeFilter::Regular && type != static_cast<int>(MnType::Regular)) {
            return false;
        }
        if (m_type_filter == TypeFilter::Evo && type != static_cast<int>(MnType::Evo)) {
            return false;
        }
    }

    // Banned filter
    if (m_hide_banned) {
        QModelIndex idx = sourceModel()->index(source_row, MasternodeModel::STATUS, source_parent);
        int banned = sourceModel()->data(idx, Qt::EditRole).toInt();
        if (banned != 0) {
            return false;
        }
    }

    // Text-matching filter
    if (const auto& regex = filterRegularExpression(); !regex.pattern().isEmpty()) {
        QModelIndex idx = sourceModel()->index(source_row, 0, source_parent);
        QString searchText = sourceModel()->data(idx, Qt::UserRole).toString();
        if (!searchText.contains(regex)) {
            return false;
        }
    }

    // "Owned" filter
    if (m_show_owned_only) {
        QModelIndex idx = sourceModel()->index(source_row, MasternodeModel::PROTX_HASH, source_parent);
        QString proTxHash = sourceModel()->data(idx, Qt::DisplayRole).toString();
        if (!m_owned_mns.contains(proTxHash)) {
            return false;
        }
    }

    return true;
}

MasternodeList::MasternodeList(QWidget* parent) :
    QWidget(parent),
    ui(new Ui::MasternodeList),
    m_proxy_model(new MasternodeListSortFilterProxyModel(this)),
    m_model(new MasternodeModel(this)),
    m_worker(new QObject),
    m_thread{new QThread(this)},
    m_timer{new QTimer(this)}
{
    ui->setupUi(this);

    GUIUtil::setFont({ui->label_count, ui->countLabel}, {GUIUtil::FontWeight::Bold, 14});

    // Set up proxy model
    m_proxy_model->setSourceModel(m_model);
    m_proxy_model->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_proxy_model->setSortRole(Qt::EditRole);

    // Set up table view
    ui->tableViewMasternodes->setModel(m_proxy_model);
    ui->tableViewMasternodes->setContextMenuPolicy(Qt::CustomContextMenu);
    ui->tableViewMasternodes->verticalHeader()->setVisible(false);

    // Set column widths
    auto* header = ui->tableViewMasternodes->horizontalHeader();
    header->setStretchLastSection(false);
    for (int col = 0; col < MasternodeModel::COUNT; ++col) {
        if (col == MasternodeModel::SERVICE) {
            header->setSectionResizeMode(col, QHeaderView::Stretch);
        } else {
            header->setSectionResizeMode(col, QHeaderView::ResizeToContents);
        }
    }

    // Hide ProTx Hash column (used for internal lookup)
    ui->tableViewMasternodes->setColumnHidden(MasternodeModel::PROTX_HASH, true);

    // Hide PoSe column by default (since "Hide banned" is checked by default)
    ui->tableViewMasternodes->setColumnHidden(MasternodeModel::POSE, true);

    ui->checkBoxOwned->setEnabled(false);
    ui->mnSetupWizardButton->setEnabled(false);

    contextMenuDIP3 = new QMenu(this);
    contextMenuDIP3->addAction(tr("Copy ProTx Hash"), this, &MasternodeList::copyProTxHash_clicked);
    contextMenuDIP3->addAction(tr("Copy Collateral Outpoint"), this, &MasternodeList::copyCollateralOutpoint_clicked);
    contextMenuDIP3->addSeparator();
    m_updateServicePortAction = contextMenuDIP3->addAction(
        tr("Update service to the current network port"), this, &MasternodeList::updateServicePort);
    m_updateServicePortAction->setVisible(false);

    QMenu* filterMenu = contextMenuDIP3->addMenu(tr("Filter by"));
    filterMenu->addAction(tr("Collateral Address"), this, &MasternodeList::filterByCollateralAddress);
    filterMenu->addAction(tr("Payout Address"), this, &MasternodeList::filterByPayoutAddress);
    filterMenu->addAction(tr("Owner Address"), this, &MasternodeList::filterByOwnerAddress);
    filterMenu->addAction(tr("Voting Address"), this, &MasternodeList::filterByVotingAddress);

    connect(ui->tableViewMasternodes, &QTableView::customContextMenuRequested, this, &MasternodeList::showContextMenuDIP3);
    connect(ui->tableViewMasternodes, &QTableView::doubleClicked, this, &MasternodeList::extraInfoDIP3_clicked);
    connect(m_proxy_model, &QSortFilterProxyModel::rowsInserted, this, &MasternodeList::updateFilteredCount);
    connect(m_proxy_model, &QSortFilterProxyModel::rowsRemoved, this, &MasternodeList::updateFilteredCount);
    connect(m_proxy_model, &QSortFilterProxyModel::modelReset, this, &MasternodeList::updateFilteredCount);
    connect(m_proxy_model, &QSortFilterProxyModel::layoutChanged, this, &MasternodeList::updateFilteredCount);

    GUIUtil::updateFonts();

    // Background thread for calculating masternode list
    m_worker->moveToThread(m_thread);
    // Make sure executor object is deleted in its own thread
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread->start();

    // Debounce timer to apply masternode list changes
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &MasternodeList::updateDIP3ListScheduled);
}

MasternodeList::~MasternodeList()
{
    m_timer->stop();
    m_thread->quit();
    m_thread->wait();
    delete ui;
}

void MasternodeList::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::StyleChange) {
        QTimer::singleShot(0, m_model, &MasternodeModel::refreshIcons);
    }
}

void MasternodeList::setClientModel(ClientModel* model)
{
    this->clientModel = model;
    if (model) {
        connect(clientModel, &ClientModel::masternodeListChanged, this, &MasternodeList::handleMasternodeListChanged);
        m_timer->start(0);
    } else {
        m_timer->stop();
    }
}

void MasternodeList::setWalletModel(WalletModel* model)
{
    this->walletModel = model;
    ui->checkBoxOwned->setEnabled(model != nullptr);
    ui->mnSetupWizardButton->setEnabled(model != nullptr);
}

void MasternodeList::showContextMenuDIP3(const QPoint& point)
{
    QModelIndex index = ui->tableViewMasternodes->indexAt(point);
    if (index.isValid()) {
        ui->tableViewMasternodes->selectionModel()->setCurrentIndex(
            index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        const auto* entry = GetSelectedEntry();
        const QString updated_service = entry
            ? MasternodeListUtils::ServiceWithPort(entry->service(), Params().GetDefaultPort())
            : QString{};
        const bool needs_update = entry && !updated_service.isEmpty() && updated_service != entry->service();
        m_updateServicePortAction->setVisible(needs_update);
        m_updateServicePortAction->setEnabled(needs_update && walletModel);
        contextMenuDIP3->exec(QCursor::pos());
    }
}

void MasternodeList::updateServicePort()
{
    if (!walletModel) {
        QMessageBox::warning(this, tr("Masternode service"), tr("Load a wallet before updating a masternode service."));
        return;
    }

    const auto* entry = GetSelectedEntry();
    if (!entry) return;
    if (entry->type() != MnType::Regular) {
        QMessageBox::warning(this, tr("Masternode service"), tr("Evo service changes require the Platform endpoints and update_service_evo; this action is for regular masternodes only."));
        return;
    }

    // Copy model values before opening modal dialogs, which run a nested event loop
    // while the masternode list can refresh in the background.
    const QString protx_hash{entry->proTxHash()};
    const QString old_service{entry->service()};
    const QString new_service{MasternodeListUtils::ServiceWithPort(old_service, Params().GetDefaultPort())};
    if (new_service.isEmpty() || new_service == old_service) {
        return;
    }

    const std::string operator_key{gArgs.GetArg("-masternodeblsprivkey", "")};
    if (operator_key.empty()) {
        QMessageBox::warning(this, tr("Masternode service"),
                             tr("This node has no masternodeblsprivkey configured. Run Korsh on the masternode host with its operator key, then retry."));
        return;
    }

    const QString confirmation = tr("Submit an on-chain service update?\n\nCurrent: %1\nNew: %2\n\n"
                                    "The transaction fee will be paid from a spendable output in this wallet. "
                                    "This updates the registered endpoint only; it does not unlock or transfer masternode collateral.\n\n"
                                    "Make sure the masternode is listening on port %3 and that the port is reachable.")
                                     .arg(old_service, new_service)
                                     .arg(Params().GetDefaultPort());
    if (QMessageBox::question(this, tr("Masternode service"), confirmation,
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    auto unlock_context{walletModel->requestUnlock(false)};
    if (!unlock_context.isValid()) {
        QMessageBox::warning(this, tr("Masternode service"), tr("Wallet unlock was canceled or failed."));
        return;
    }

    const QByteArray encoded_wallet{QUrl::toPercentEncoding(walletModel->getWalletName())};
    const std::string wallet_uri{"/wallet/" + std::string(encoded_wallet.constData(), encoded_wallet.length())};
    auto execute_wallet_rpc = [this, &wallet_uri](const std::string& method,
                                                  const std::vector<std::string>& args,
                                                  UniValue& result,
                                                  QString& error) {
        try {
            result = walletModel->node().executeRpc(method, RPCConvertValues(method, args), wallet_uri);
            return true;
        } catch (const UniValue& rpc_error) {
            error = ExtractRpcError(rpc_error);
        } catch (const std::exception& exception) {
            error = QString::fromStdString(exception.what());
        }
        return false;
    };

    QString error;
    UniValue unspent;
    if (!execute_wallet_rpc("listunspent", {"1", "9999999"}, unspent, error) || !unspent.isArray()) {
        QMessageBox::warning(this, tr("Masternode service"), tr("Could not find a fee source: %1").arg(error));
        return;
    }
    std::set<std::string> candidates;
    for (const UniValue& output : unspent.getValues()) {
        const UniValue& spendable = output.find_value("spendable");
        const UniValue& address = output.find_value("address");
        const UniValue& amount = output.find_value("amount");
        if (spendable.isBool() && spendable.get_bool() && address.isStr() &&
            !address.get_str().empty() && amount.isNum() && amount.get_real() > 0) {
            candidates.insert(address.get_str());
        }
    }
    auto no_collateral_inputs = [&](const std::string& raw) {
        UniValue decoded, registered;
        if (!execute_wallet_rpc("decoderawtransaction", {raw}, decoded, error) ||
            !decoded.isObject() || !decoded.find_value("vin").isArray() ||
            !execute_wallet_rpc("protx", {"list", "registered", "true"}, registered, error) || !registered.isArray()) return false;
        std::set<std::pair<std::string, int>> collateral;
        for (const UniValue& mn : registered.getValues()) {
            const auto& hash = mn.find_value("collateralHash");
            const auto& index = mn.find_value("collateralIndex");
            if (!hash.isStr() || !index.isNum()) return false;
            collateral.emplace(hash.get_str(), index.getInt<int>());
        }
        for (const UniValue& input : decoded["vin"].getValues()) {
            const auto& hash = input.find_value("txid");
            const auto& index = input.find_value("vout");
            if (!hash.isStr() || !index.isNum() || collateral.count({hash.get_str(), index.getInt<int>()})) {
                error = tr("The prepared update would spend registered collateral; it was not sent.");
                return false;
            }
            // Also protect prepared/pending collateral not yet in the registry,
            // even if its wallet lock was manually removed. Conservatively
            // require a non-collateral-sized fee input for this guided action.
            UniValue coin;
            if (!execute_wallet_rpc("gettxout", {hash.get_str(), index.getValStr(), "true"}, coin, error) ||
                !coin.isObject() || !coin.find_value("value").isNum()) return false;
            const double value = coin["value"].get_real();
            if (value == static_cast<double>(dmn_types::Regular.collat_amount) / COIN ||
                value == static_cast<double>(dmn_types::Evo.collat_amount) / COIN) {
                error = tr("Use a separate non-collateral output to pay the service-update fee.");
                return false;
            }
        }
        return true;
    };
    UniValue prepared;
    bool funded = false;
    for (const std::string& address : candidates) {
        // Funding itself, rather than a guessed fee threshold or list order,
        // proves this address has sufficient eligible coins. No submit here.
        if (execute_wallet_rpc("protx", {"update_service", protx_hash.toStdString(), new_service.toStdString(),
                operator_key, "", address, "false"}, prepared, error) && prepared.isStr() &&
                no_collateral_inputs(prepared.get_str())) {
            funded = true;
            break;
        }
    }
    if (!funded) {
        QMessageBox::warning(this, tr("Masternode service"), tr("No eligible fee source could fund the update: %1").arg(error));
        return;
    }
    const std::string raw = prepared.get_str();
    UniValue raw_array(UniValue::VARR), tested;
    raw_array.push_back(raw);
    if (!execute_wallet_rpc("testmempoolaccept", {raw_array.write()}, tested, error) ||
        !tested.isArray() || tested.size() != 1 || !tested[0].find_value("allowed").isBool() ||
        !tested[0]["allowed"].get_bool() || !tested[0].find_value("fees").isObject() ||
        !tested[0]["fees"].find_value("base").isNum() || !tested[0].find_value("txid").isStr()) {
        QMessageBox::warning(this, tr("Masternode service"), tr("Update preflight failed; nothing sent. %1").arg(error));
        return;
    }
    const QString expected_txid = QString::fromStdString(tested[0]["txid"].get_str());
    if (QMessageBox::question(this, tr("Confirm service update fee"),
            tr("Pay exactly %1 KSH to update %2?\nTransaction: %3")
                .arg(QString::fromStdString(tested[0]["fees"]["base"].getValStr()), new_service, expected_txid),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    if (!no_collateral_inputs(raw)) {
        QMessageBox::warning(this, tr("Masternode service"), tr("Collateral safety recheck failed; nothing sent. %1").arg(error));
        return;
    }
    UniValue txid;
    if (!execute_wallet_rpc("sendrawtransaction", {raw}, txid, error) || !txid.isStr()) {
        QMessageBox::warning(this, tr("Masternode service"),
            tr("Broadcast was not confirmed. Check transaction %1 before retrying. %2").arg(expected_txid, error));
        return;
    }
    QMessageBox::information(this, tr("Masternode service"),
        tr("Service update submitted.\nTransaction ID: %1\nThis updates the endpoint only; wait for confirmation.")
            .arg(QString::fromStdString(txid.get_str())));
}

void MasternodeList::handleMasternodeListChanged()
{
    if (!clientModel || m_timer->isActive()) {
        // Too early or already processing, nothing to do
        return;
    }

    int delay{MASTERNODELIST_UPDATE_SECONDS * 1000};
    if (!clientModel->masternodeSync().isBlockchainSynced()) {
        // Currently syncing, reduce refreshes
        delay *= 10;
    }
    m_timer->start(delay);
}

void MasternodeList::updateDIP3ListScheduled()
{
    if (!clientModel || clientModel->node().shutdownRequested()) {
        return;
    }

    if (m_in_progress.exchange(true)) {
        // Already applying, re-arm for next attempt
        handleMasternodeListChanged();
        return;
    }

    QMetaObject::invokeMethod(m_worker, [this] {
        auto result = std::make_shared<CalcMnList>(calcMasternodeList());
        m_in_progress.store(false);
        QTimer::singleShot(0, this, [this, result] {
            if (result->m_valid) {
                setMasternodeList(std::move(*result));
            } else {
                // Something went wrong, try again
                handleMasternodeListChanged();
            }
        });
    });
}

MasternodeList::CalcMnList MasternodeList::calcMasternodeList() const
{
    CalcMnList ret;
    if (!clientModel || clientModel->node().shutdownRequested()) {
        return ret;
    }

    auto [mnList, pindex] = clientModel->getMasternodeList();
    if (!pindex) return ret;
    auto projectedPayees = mnList->getProjectedMNPayees(pindex);

    if (projectedPayees.empty() && mnList->getValidMNsCount() > 0) {
        // GetProjectedMNPayees failed to provide results for a list with valid mns.
        // Keep current list and let it try again later.
        return ret;
    }

    ret.m_list_height = mnList->getHeight();

    Uint256HashMap<CTxDestination> mapCollateralDests;
    mnList->forEachMN(/*only_valid=*/false, [&](const auto& dmn) {
        CTxDestination collateralDest;
        Coin coin;
        if (clientModel->node().getUnspentOutput(dmn.getCollateralOutpoint(), coin) &&
            ExtractDestination(coin.out.scriptPubKey, collateralDest)) {
            mapCollateralDests.emplace(dmn.getProTxHash(), collateralDest);
        }
    });

    Uint256HashMap<int> nextPayments;
    for (size_t i = 0; i < projectedPayees.size(); i++) {
        const auto& dmn = projectedPayees[i];
        nextPayments.emplace(dmn->getProTxHash(), ret.m_list_height + (int)i + 1);
    }

    mnList->forEachMN(/*only_valid=*/false, [&](const auto& dmn) {
        QString collateralStr = QObject::tr("UNKNOWN");
        auto collateralDestIt = mapCollateralDests.find(dmn.getProTxHash());
        if (collateralDestIt != mapCollateralDests.end()) {
            collateralStr = QString::fromStdString(EncodeDestination(collateralDestIt->second));
        }

        int nNextPayment = 0;
        auto nextPaymentIt = nextPayments.find(dmn.getProTxHash());
        if (nextPaymentIt != nextPayments.end()) {
            nNextPayment = nextPaymentIt->second;
        }

        ret.m_entries.push_back(std::make_unique<MasternodeEntry>(dmn, collateralStr, nNextPayment));
    });

    // Compute "owned" masternode hashes for the filter
    if (walletModel) {
        std::set<COutPoint> setOutpts;
        for (const auto& outpt : walletModel->wallet().listProTxCoins()) {
            setOutpts.emplace(outpt);
        }

        mnList->forEachMN(/*only_valid=*/false, [&](const auto& dmn) {
            bool fMyMasternode = setOutpts.count(dmn.getCollateralOutpoint()) ||
                                 walletModel->wallet().isSpendable(PKHash(dmn.getKeyIdOwner())) ||
                                 walletModel->wallet().isSpendable(PKHash(dmn.getKeyIdVoting())) ||
                                 walletModel->wallet().isSpendable(dmn.getScriptPayout()) ||
                                 walletModel->wallet().isSpendable(dmn.getScriptOperatorPayout());
            if (fMyMasternode) {
                ret.m_owned_mns.insert(QString::fromStdString(dmn.getProTxHash().ToString()));
            }
        });
    }

    ret.m_valid = true;
    return ret;
}

void MasternodeList::setMasternodeList(CalcMnList&& list)
{
    m_model->setCurrentHeight(list.m_list_height);
    m_model->reconcile(std::move(list.m_entries));

    if (walletModel) {
        m_proxy_model->setMyMasternodeHashes(std::move(list.m_owned_mns));
        if (ui->checkBoxOwned->isChecked()) {
            m_proxy_model->forceInvalidateFilter();
        }
    }

    updateFilteredCount();
}

void MasternodeList::updateFilteredCount()
{
    const int total = m_model->rowCount();
    int evoCount = 0;
    int regularCount = 0;
    for (int i = 0; i < total; ++i) {
        const auto* entry = m_model->getEntryAt(m_model->index(i, 0));
        if (entry) {
            if (entry->type() == MnType::Evo) {
                ++evoCount;
            } else {
                ++regularCount;
            }
        }
    }
    const int filtered = m_proxy_model->rowCount();
    const QString filter_suffix = filtered != total ? QString{" "} + tr("(showing %1)").arg(filtered) : QString();
    ui->countLabel->setText(
        tr("Total: %1 | Evo: %2 | Regular: %3").arg(total).arg(evoCount).arg(regularCount) +
        filter_suffix);
}

void MasternodeList::on_filterText_textChanged(const QString& strFilterIn)
{
    m_proxy_model->setFilterRegularExpression(
        QRegularExpression(QRegularExpression::escape(strFilterIn), QRegularExpression::CaseInsensitiveOption));
    updateFilteredCount();
}

void MasternodeList::on_mnSetupWizardButton_clicked()
{
    if (!walletModel) {
        QMessageBox::warning(this, tr("MN Setup Wizard"), tr("Load a wallet first to use the masternode setup wizard."));
        return;
    }

    MasternodeSetupWizard wizard(this, walletModel);
    wizard.exec();
}

void MasternodeList::on_comboBoxType_currentIndexChanged(int index)
{
    if (index < 0 || index >= static_cast<int>(MasternodeListSortFilterProxyModel::TypeFilter::COUNT)) {
        return;
    }
    const auto index_enum{static_cast<MasternodeListSortFilterProxyModel::TypeFilter>(index)};
    ui->tableViewMasternodes->setColumnHidden(MasternodeModel::TYPE, index_enum != MasternodeListSortFilterProxyModel::TypeFilter::All);
    m_proxy_model->setTypeFilter(index_enum);
    m_proxy_model->forceInvalidateFilter();
    updateFilteredCount();
}

void MasternodeList::on_checkBoxOwned_stateChanged(int state)
{
    m_proxy_model->setShowOwnedOnly(state == Qt::Checked);
    m_proxy_model->forceInvalidateFilter();
    updateFilteredCount();
}

void MasternodeList::on_checkBoxHideBanned_stateChanged(int state)
{
    const bool hide_banned{state == Qt::Checked};
    ui->tableViewMasternodes->setColumnHidden(MasternodeModel::POSE, hide_banned);
    m_proxy_model->setHideBanned(hide_banned);
    m_proxy_model->forceInvalidateFilter();
    updateFilteredCount();
}

const MasternodeEntry* MasternodeList::GetSelectedEntry()
{
    if (!m_model) {
        return nullptr;
    }

    QItemSelectionModel* selectionModel = ui->tableViewMasternodes->selectionModel();
    if (!selectionModel) {
        return nullptr;
    }

    QModelIndexList selected = selectionModel->selectedRows();
    if (selected.count() == 0) {
        return nullptr;
    }

    // Map from proxy to source model
    return m_model->getEntryAt(m_proxy_model->mapToSource(selected.at(0)));
}

void MasternodeList::extraInfoDIP3_clicked()
{
    const auto* entry = GetSelectedEntry();
    if (!entry) {
        return;
    }

    auto* dialog = new DescriptionDialog(tr("Details for Masternode %1").arg(entry->proTxHash()), entry->toHtml(), /*parent=*/this);
    dialog->resize(1000, 500);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void MasternodeList::copyProTxHash_clicked()
{
    const auto* entry = GetSelectedEntry();
    if (!entry) {
        return;
    }

    QApplication::clipboard()->setText(entry->proTxHash());
}

void MasternodeList::copyCollateralOutpoint_clicked()
{
    const auto* entry = GetSelectedEntry();
    if (!entry) {
        return;
    }

    QApplication::clipboard()->setText(entry->collateralOutpoint());
}

void MasternodeList::filterByCollateralAddress()
{
    const auto* entry = GetSelectedEntry();
    if (entry) {
        ui->filterText->setText(entry->collateralAddress());
    }
}

void MasternodeList::filterByPayoutAddress()
{
    const auto* entry = GetSelectedEntry();
    if (entry) {
        ui->filterText->setText(entry->payoutAddress());
    }
}

void MasternodeList::filterByOwnerAddress()
{
    const auto* entry = GetSelectedEntry();
    if (entry) {
        ui->filterText->setText(entry->ownerAddress());
    }
}

void MasternodeList::filterByVotingAddress()
{
    const auto* entry = GetSelectedEntry();
    if (entry) {
        ui->filterText->setText(entry->votingAddress());
    }
}
