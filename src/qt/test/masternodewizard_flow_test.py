#!/usr/bin/env python3
"""Execute the production wizard with real Qt and scripted RPC boundaries.

No daemon, wallet keys or shared build. This is not an end-to-end RPC test.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--output', required=True, type=Path)
p.add_argument('--generate-only', action='store_true', help='Emit the same harness for isolated native Windows compilation')
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
repo = Path(__file__).resolve().parents[3]
cpp = (repo / 'src/qt/masternodelist.cpp').read_text()
body = cpp[cpp.index('class MasternodeSetupWizard'):cpp.index('} // anonymous namespace')]
body = body.replace('private:', 'public:').replace('protected:', 'public:')
slot = cpp[cpp.index('void MasternodeList::updateServicePort()'):cpp.index('void MasternodeList::handleMasternodeListChanged()')]
header = (repo/'src/qt/masternodelist.h').read_text()
helper = header[header.index('namespace MasternodeListUtils {'):header.index('QT_BEGIN_NAMESPACE')]
# Exercise real precedence, retaining the standalone harness filesystem adapter.
settings_header = (repo/'src/util/settings.h').read_text().replace('#include <fs.h>', '')
settings_cpp = (repo/'src/util/settings.cpp').read_text()
settings_body = (settings_cpp[settings_cpp.index('namespace util {'):settings_cpp.index('bool ReadSettings(')]
                 + settings_cpp[settings_cpp.index('SettingsValue GetSetting('):settings_cpp.index('std::vector<SettingsValue> GetSettingsList(')]
                 + settings_cpp[settings_cpp.index('SettingsSpan::SettingsSpan('):])
source = r'''
#include <QtWidgets>
#include <QHostAddress>
#include <univalue.h>
#include <qt/masternodewizardconfig.h>
#include <evo/dmn_types.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <set>
#include <functional>
#ifdef _WIN32
#include <sddl.h>
#endif
#ifndef _WIN32
#include <sys/resource.h>
#include <signal.h>
#endif
namespace fs = std::filesystem;
''' + settings_header + settings_body + r'''
static util::Settings settings;
static fs::path config;
static std::string disabled;
static bool service_test=false;
static bool decline_fee=false;
static bool saw_exact_fee=false;
static QString last_message;
static bool conversion_failure=false;
static bool create_journal_during_fee=false;
static std::function<void()> during_fee;
#ifndef _WIN32
static bool short_journal_during_fee=false;
static struct rlimit saved_file_limit;
#endif
constexpr const char* BITCOIN_CONF_FILENAME="korsh.conf";
constexpr bool DEFAULT_LISTEN=true, DEFAULT_TXINDEX=true, DEFAULT_PEERBLOOMFILTERS=true, DEFAULT_GOVERNANCE_ENABLE=true;
constexpr int DEFAULT_MAX_PEER_CONNECTIONS=125;
struct Args {
 fs::path GetPathArg(const char*,const char*) {return config;}
 UniValue GetSetting() {return util::GetSetting(settings,"main","masternodeblsprivkey",false,false,false);}
 bool IsArgSet(const char*) {return !GetSetting().isNull();}
 std::string GetArg(const char*,const char* fallback) {auto v=GetSetting();return service_test?"SYNTHETIC":v.isNull()?fallback:v.isFalse()?"0":v.get_str();}
 bool GetBoolArg(const char* key,bool v) {return disabled==key?!v:v;}
 int GetIntArg(const char*,int v) {return v;}
} gArgs;
struct Chain {
 std::string NetworkIDString() const {return "main";}
 int GetDefaultPort() const {return 9777;}
 int GetDefaultPlatformP2PPort() const {return 1;}
 int GetDefaultPlatformHTTPPort() const {return 2;}
 bool RequireRoutableExternalIP() const {return true;}
};
Chain Params() {return {};}
fs::path GetConfigFile(fs::path p) {return p;}
namespace GUIUtil {QString PathToQString(fs::path p) {return QString::fromStdString(p.string());}}
namespace KorshFeatures {bool EvoEnabled() {return false;}}
QString ExtractRpcError(const UniValue& e) {return QString::fromStdString(e.write());}
UniValue RPCConvertValues(const std::string& method,const std::vector<std::string>& args) {
 if(method=="protx" && conversion_failure) throw std::runtime_error("Malformed RPC parameter");
 UniValue a(UniValue::VARR);for(auto& s:args)a.push_back(s);return a;
}
struct Node {
 int address_calls=0, broadcasts=0, prepares=0;
 int fail_address=0;
 bool unlocked=false, break_config=false, recovery_before_send=false, unsafe_inputs=false, pending_collateral=false;
 void sent() {
  ++broadcasts;
  QFile journal(QString::fromStdString(config.string())+".mnsetup-main.json");
  UniValue recovered;
  recovery_before_send=journal.open(QIODevice::ReadOnly) && recovered.read(journal.readAll().toStdString()) &&
   recovered.find_value("operator_secret").get_str()=="SYNTHETIC" &&
   recovered.find_value("raw").get_str()=="RAW" && recovered.find_value("txid").get_str()=="TXID" &&
   recovered.find_value("operator_public").get_str()=="REGISTERED_SERIALIZATION" &&
   recovered.find_value("network").get_str()=="main" && recovered.find_value("wallet").get_str()=="fixture";
#ifndef _WIN32
  if(journal.isOpen()) recovery_before_send &=
   (fs::status(config.string()+".mnsetup-main.json").permissions()&(fs::perms::group_all|fs::perms::others_all))==fs::perms::none;
#endif
  if(break_config) {fs::remove(config);fs::create_directory(config);}
 }
 UniValue executeRpc(const std::string& method,const UniValue& args,const std::string&) {
  if(method=="getnewaddress") {
   ++address_calls;
   if(!unlocked || address_calls==fail_address) throw std::runtime_error("keypool exhausted");
   return UniValue("address"+std::to_string(address_calls));
  }
  if(method=="bls") {UniValue r(UniValue::VOBJ);r.pushKV("public","PUBLIC");return r;}
  if(method=="protx") {
   if(args[0].get_str()=="list") {UniValue r;r.read(R"([{"collateralHash":"COLLATERAL","collateralIndex":0}])");return r;}
   if(args[0].get_str()=="update_service") {
    if(args[5].get_str()=="A") throw std::runtime_error("Insufficient funds");
    if(args.size()!=7 || args[6].get_str()!="false") throw std::runtime_error("Must prepare only");
    ++prepares;return UniValue("RAW");
   }
   if(args.size()>8 && args[args.size()-1].isBool() && !args[args.size()-1].get_bool()) {++prepares; return UniValue("RAW");}
   sent(); return UniValue("TXID");
  }
  if(method=="testmempoolaccept") {UniValue r;r.read(R"([{"allowed":true,"txid":"TXID","fees":{"base":0.00001}}])");return r;}
  if(method=="listunspent") {UniValue r;r.read(R"([{"address":"A","txid":"A_TX","vout":0,"amount":0.00000001,"spendable":true},{"address":"B","txid":"B_TX","vout":0,"amount":1,"spendable":true}])");return r;}
  if(method=="decoderawtransaction") {UniValue r;r.read(unsafe_inputs?R"({"txid":"TXID","vin":[{"txid":"COLLATERAL","vout":0}]})":R"({"txid":"TXID","proRegTx":{"pubKeyOperator":"REGISTERED_SERIALIZATION"},"vin":[{"txid":"B_TX","vout":0}]})");return r;}
  if(method=="sendrawtransaction") {sent();return UniValue("TXID");}
  if(method=="gettxout") {UniValue r;r.read(pending_collateral?R"({"value":1500})":R"({"value":1})");return r;}
  throw std::runtime_error("Unexpected RPC: "+method);
 }
};
struct WalletModel {
 Node n; bool allow_unlock=true; int unlocks=0;
 struct Unlock {bool valid;bool isValid()const{return valid;}};
 Unlock requestUnlock(bool) {++unlocks;n.unlocked=allow_unlock;return {allow_unlock};}
 Node& node() {return n;}
 QString getWalletName() {return "fixture";}
 bool validateAddress(const QString&) {return true;}
};
''' + body + helper + r'''
struct Entry {
 bool evo=false;
 QString proTxHash()const{return "PROTX";}
 QString service()const{return "8.8.8.8:8383";}
 MnType type()const{return evo?MnType::Evo:MnType::Regular;}
};
class MasternodeList:public QWidget {public:
 WalletModel* walletModel; Entry entry;
 const Entry* GetSelectedEntry(){return &entry;}
 void updateServicePort();
};
''' + slot + r'''
int main(int argc,char**argv) {
 QApplication app(argc,argv);config=fs::path(argv[1])/"fixture.conf";
 QTimer closer; QObject::connect(&closer,&QTimer::timeout,[] {
  for(QWidget* widget:QApplication::topLevelWidgets()) if(auto* box=qobject_cast<QMessageBox*>(widget)) {
   last_message=box->text();
   if(box->text().contains("0.00001")) {
    saw_exact_fee=true;
    if(during_fee) {auto action=std::move(during_fee);during_fee={};action();}
    if(create_journal_during_fee) {
     std::ofstream f(config.string()+".mnsetup-main.json");f<<"SYNTHETIC_PREEXISTING_RECOVERY";
     create_journal_during_fee=false;
    }
#ifndef _WIN32
    if(short_journal_during_fee) {
     getrlimit(RLIMIT_FSIZE,&saved_file_limit);
     struct rlimit limited=saved_file_limit;limited.rlim_cur=64;
     signal(SIGXFSZ,SIG_IGN);setrlimit(RLIMIT_FSIZE,&limited);
     short_journal_during_fee=false;
    }
#endif
   }
   if(box->standardButtons() & QMessageBox::Yes) box->button(decline_fee?QMessageBox::No:QMessageBox::Yes)->click(); else box->accept();
  }
 });closer.start(1);
 int failures=0;
 auto check=[&](bool ok,const char* name) {std::cout<<(ok?"PASS: ":"FAIL: ")<<name<<"\n";if(!ok)++failures;};
 {
  WalletModel wallet;MasternodeSetupWizard wizard(nullptr,&wallet);
  check(wizard.autoFillAddresses() && wallet.unlocks==1,"autofill unlocks before keypool RPC");
 }
 {
  WalletModel wallet;wallet.n.unlocked=true;wallet.n.fail_address=3;
  MasternodeSetupWizard wizard(nullptr,&wallet);wizard.m_collateral_address->setText("ORIGINAL");
  check(!wizard.autoFillAddresses() && wizard.m_collateral_address->text()=="ORIGINAL","failed autofill leaves fields unchanged");
 }
 {
  std::ofstream f(config);f<<"masternodeblsprivkey=OLD\n";f.close();
  WalletModel wallet;MasternodeSetupWizard wizard(nullptr,&wallet);
  wizard.m_ip->setText("8.8.8.8");wizard.m_collateral_address->setText("C");
  wizard.m_owner_address->setText("O");wizard.m_voting_address->setText("V");wizard.m_payout_address->setText("P");
  wizard.m_bls_secret->setText("SYNTHETIC");wizard.m_bls_public->setText("PUBLIC");
  wizard.accept();
  check(wallet.n.broadcasts==0,"existing config blocks before broadcast");
 }
 {
  fs::remove(config);
  fs::remove(config.string()+".mnsetup-main.json");
  WalletModel wallet;wallet.n.break_config=true;
  MasternodeSetupWizard wizard(nullptr,&wallet);
  wizard.m_ip->setText("8.8.8.8");wizard.m_collateral_address->setText("C");
  wizard.m_owner_address->setText("O");wizard.m_voting_address->setText("V");wizard.m_payout_address->setText("P");
  wizard.m_bls_secret->setText("SYNTHETIC");wizard.m_bls_public->setText("PUBLIC");
  wizard.accept();
  check(wallet.n.prepares==1,"registration prepared without broadcasting");
  check(wallet.n.recovery_before_send,"secret and transaction recoverable before broadcast");
  check(saw_exact_fee,"exact prepared fee is visible before broadcast");
  check(wizard.m_bls_public->text()=="REGISTERED_SERIALIZATION","operator public key uses prepared payload serialization");
  check(!wizard.generateBls(),"operator identity cannot change after broadcast");
  fs::remove(config);
  wizard.accept();
  check(wallet.n.broadcasts==1,"retry after save failure does not register again");
  check(QFileInfo::exists(QString::fromStdString(config.string())),"retry saves operator config");
  MasternodeSetupWizard reopened(nullptr,&wallet);reopened.accept();
  check(wallet.n.broadcasts==1,"reopened wizard refuses unresolved recovery journal");
 }
 {
  WalletModel wallet;MasternodeSetupWizard wizard(nullptr,&wallet);
  wizard.m_ip->setText("8.8.8.8");wizard.m_collateral_address->setText("C");
  wizard.m_owner_address->setText("O");wizard.m_voting_address->setText("V");wizard.m_payout_address->setText("P");
  wizard.m_bls_secret->setText("SYNTHETIC");wizard.m_bls_public->setText("PUBLIC");
  QString error;
  disabled="-txindex";
  check(!wizard.validateInput(error),"txindex disabled rejected before preparation");
  disabled.clear();wizard.m_ip->setText("not-an-ip");
  check(!wizard.validateInput(error),"invalid IP rejected before preparation");
 }
 {
  service_test=true;
  WalletModel wallet;MasternodeList list;list.walletModel=&wallet;
  list.updateServicePort();
  check(wallet.n.broadcasts==1 && wallet.n.prepares==1,"service update tries a sufficient later fee source");
  WalletModel unsafe;unsafe.n.unsafe_inputs=true;list.walletModel=&unsafe;
  list.updateServicePort();
  check(unsafe.n.broadcasts==0,"service update rejects prepared collateral spend");
  WalletModel pending;pending.n.pending_collateral=true;list.walletModel=&pending;
  list.updateServicePort();
  check(pending.n.broadcasts==0,"service update protects unregistered collateral-sized outputs");
  WalletModel evo;list.walletModel=&evo;list.entry.evo=true;
  list.updateServicePort();
  check(evo.n.broadcasts==0,"regular service action rejects Evo");
  service_test=false;
 }
 {
  WalletModel wallet;MasternodeSetupWizard wizard(nullptr,&wallet);
  wizard.m_mn_type->addItem("Evo",1);wizard.m_mn_type->setCurrentIndex(1);
  check(wizard.m_evo_platform_node_id->text().isEmpty(),"Platform identity is supplied by actual Platform node");
 }
#ifndef _WIN32
 {
  fs::remove(config);fs::remove(config.string()+".mnsetup-main.json");
  const auto target=config.parent_path()/"must-not-create.json";fs::remove(target);
  fs::create_symlink(target,config.string()+".mnsetup-main.json");
  WalletModel wallet;MasternodeSetupWizard wizard(nullptr,&wallet);
  wizard.m_ip->setText("8.8.8.8");wizard.m_collateral_address->setText("C");
  wizard.m_owner_address->setText("O");wizard.m_voting_address->setText("V");wizard.m_payout_address->setText("P");
  wizard.m_bls_secret->setText("SYNTHETIC");wizard.m_bls_public->setText("PUBLIC");
  wizard.accept();
  check(wallet.n.broadcasts==0 && !fs::exists(target),"dangling recovery symlink fails closed");
  fs::remove(config.string()+".mnsetup-main.json");
 }
#endif
 {
  fs::remove(config);fs::remove(config.string()+".mnsetup-main.json");
  WalletModel wallet;MasternodeSetupWizard wizard(nullptr,&wallet);
  wizard.m_ip->setText("8.8.8.8");wizard.m_collateral_address->setText("C");
  wizard.m_owner_address->setText("O");wizard.m_voting_address->setText("V");wizard.m_payout_address->setText("P");
  wizard.m_bls_secret->setText("SYNTHETIC");wizard.m_bls_public->setText("PUBLIC");
  decline_fee=true;wizard.accept();decline_fee=false;
  check(wallet.n.broadcasts==0 && !fs::exists(config) && !fs::exists(config.string()+".mnsetup-main.json"),"declining fee never broadcasts or persists identity or intent");
  WalletModel locked;locked.allow_unlock=false;MasternodeSetupWizard locked_wizard(nullptr,&locked);
  check(!locked_wizard.autoFillAddresses() && locked.n.address_calls==0,"canceled unlock does not allocate addresses");
  conversion_failure=true;
  bool caught=false;QString raw,pub,error;
  try {caught=!wizard.registerMasternode(raw,pub,error);} catch(...) {}
  check(caught && !error.isEmpty(),"RPC conversion exceptions become controlled errors");
  conversion_failure=false;
 }
 // Explicit emptiness must not be confused with absence in any source.
 for(int origin=0;origin<5;++origin) {
  fs::remove(config);fs::remove(config.string()+".mnsetup-main.json");
  settings={};
  if(origin==0) settings.forced_settings["masternodeblsprivkey"]=UniValue("");
  if(origin==1) settings.command_line_options["masternodeblsprivkey"].push_back(UniValue(""));
  if(origin==2) settings.rw_settings["masternodeblsprivkey"]=UniValue("");
  if(origin==3) settings.ro_config["main"]["masternodeblsprivkey"].push_back(UniValue(""));
  if(origin==4) settings.ro_config[""]["masternodeblsprivkey"].push_back(UniValue(""));
  // Higher-priority empty values shadow an otherwise usable network key.
  if(origin<3) settings.ro_config["main"]["masternodeblsprivkey"].push_back(UniValue("SYNTHETIC"));
  WalletModel wallet;MasternodeSetupWizard wizard(nullptr,&wallet);
  wizard.m_ip->setText("8.8.8.8");wizard.m_collateral_address->setText("C");
  wizard.m_owner_address->setText("O");wizard.m_voting_address->setText("V");wizard.m_payout_address->setText("P");
  wizard.m_bls_secret->setText("SYNTHETIC");wizard.m_bls_public->setText("PUBLIC");
  wizard.accept();
  check(wallet.n.prepares==0 && wallet.n.broadcasts==0 && !fs::exists(config),"empty effective setting blocks before preparation and spending");
  QString error;
  check(!wizard.saveOperatorSecretToConfig(error) && !error.isEmpty() && !fs::exists(config),"save rejects explicit empty effective setting");
 }
 settings={};
 {
  fs::remove(config);fs::remove(config.string()+".mnsetup-main.json");
  WalletModel wallet;MasternodeSetupWizard wizard(nullptr,&wallet);
  wizard.m_ip->setText("8.8.8.8");wizard.m_collateral_address->setText("C");
  wizard.m_owner_address->setText("O");wizard.m_voting_address->setText("V");wizard.m_payout_address->setText("P");
  wizard.m_bls_secret->setText("SYNTHETIC");wizard.m_bls_public->setText("PUBLIC");
  create_journal_during_fee=true;wizard.accept();
  std::ifstream f(config.string()+".mnsetup-main.json");std::string preserved((std::istreambuf_iterator<char>(f)),{});
  check(wallet.n.broadcasts==0 && preserved=="SYNTHETIC_PREEXISTING_RECOVERY" && wizard.m_raw_transaction.isEmpty(),"noncooperating writer during fee approval wins without overwrite or broadcast");
 }
#ifndef _WIN32
 {
  fs::remove(config);fs::remove(config.string()+".mnsetup-main.json");
  WalletModel wallet;MasternodeSetupWizard wizard(nullptr,&wallet);
  wizard.m_ip->setText("8.8.8.8");wizard.m_collateral_address->setText("C");
  wizard.m_owner_address->setText("O");wizard.m_voting_address->setText("V");wizard.m_payout_address->setText("P");
  wizard.m_bls_secret->setText("SYNTHETIC");wizard.m_bls_public->setText("PUBLIC");
  getrlimit(RLIMIT_FSIZE,&saved_file_limit);
  short_journal_during_fee=true;wizard.accept();
  setrlimit(RLIMIT_FSIZE,&saved_file_limit);short_journal_during_fee=false;
  check(wallet.n.prepares==1 && wallet.n.broadcasts==0 && !fs::exists(config.string()+".mnsetup-main.json") && wizard.m_raw_transaction.isEmpty(),"journal short write fails before broadcast without partial recovery");
 }
#endif
 {
  fs::remove(config);fs::remove(config.string()+".mnsetup-main.json");
  WalletModel wallet;MasternodeSetupWizard wizard(nullptr,&wallet);
  wizard.m_ip->setText("8.8.8.8");wizard.m_collateral_address->setText("C");
  wizard.m_owner_address->setText("O");wizard.m_voting_address->setText("V");wizard.m_payout_address->setText("P");
  wizard.m_bls_secret->setText("SYNTHETIC");wizard.m_bls_public->setText("PUBLIC");
  during_fee=[] {fs::create_directory(config);};wizard.accept();
  check(wallet.n.prepares==1 && wallet.n.broadcasts==0,"changed config admission rechecked before submission");
  check(!wizard.m_raw_transaction.isEmpty() && fs::exists(config.string()+".mnsetup-main.json"),"submission rejection preserves approved recovery intent");
  fs::remove(config);wizard.accept();
  check(wallet.n.prepares==1 && wallet.n.broadcasts==1 && wallet.n.recovery_before_send,"corrected config retries same approved transaction without preparing again");
  fs::remove(config);fs::remove(config.string()+".mnsetup-main.json");
 }
#ifdef _WIN32
 // Real kernel fixtures, never mock the production config-security helper.
 auto acl=[](const fs::path& path,const wchar_t* text) {
  PSECURITY_DESCRIPTOR sd=nullptr;PACL dacl=nullptr;BOOL present,def;
  if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(text,SDDL_REVISION_1,&sd,nullptr))return false;
  GetSecurityDescriptorDacl(sd,&present,&dacl,&def);
  auto rc=SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,nullptr,nullptr,dacl,nullptr);
  LocalFree(sd);return rc==ERROR_SUCCESS;
 };
 HANDLE token=nullptr;
 if(!OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&token))return 2;
 for(const wchar_t* name:{L"SeBackupPrivilege",L"SeRestorePrivilege",L"SeTakeOwnershipPrivilege"}) {
  TOKEN_PRIVILEGES tp{};tp.PrivilegeCount=1;
  if(!LookupPrivilegeValueW(nullptr,name,&tp.Privileges[0].Luid)||!AdjustTokenPrivileges(token,FALSE,&tp,0,nullptr,nullptr))return 2;
 }
 const auto normal_config=config;
 for(int kind=0;kind<4;++kind) {
  if(kind==2 && qEnvironmentVariable("KORSH_ACL_PRIVILEGED_TESTS")!="1") {
   std::cout<<"SKIP: foreign-owner fixture requires explicit privileged-fixture opt-in\n";continue;
  }
  config=normal_config;
  if(kind==3) {
   const auto parent=config.parent_path();
   const int length=230-static_cast<int>(parent.string().size())-1;
   if(length<10)return 2;
   config=parent/std::string(length,'x');
  }
  fs::remove(config);fs::remove(config.string()+".mnsetup-main.json");
  {std::ofstream f(config,std::ios::binary);f<<"# NONSECRET original\n";}
  const auto linked=config.parent_path()/"preflight-linked.conf";
  fs::remove(linked);
  if(kind==0) check(acl(config,L"D:P(D;;WD;;;WD)(D;;WD;;;OW)(A;;FA;;;WD)"),"fixture WRITE_DAC denial installed");
  if(kind==1) check(CreateHardLinkW(linked.c_str(),config.c_str(),nullptr),"fixture multiple links installed");
  if(kind==2) {
   TOKEN_PRIVILEGES tp{};tp.PrivilegeCount=1;LookupPrivilegeValueW(nullptr,L"SeRestorePrivilege",&tp.Privileges[0].Luid);tp.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;
   check(AdjustTokenPrivileges(token,FALSE,&tp,0,nullptr,nullptr)&&GetLastError()==ERROR_SUCCESS,"fixture owner privilege enabled");
   unsigned char world[SECURITY_MAX_SID_SIZE];DWORD size=sizeof(world);CreateWellKnownSid(WinWorldSid,nullptr,world,&size);
   check(SetNamedSecurityInfoW(const_cast<wchar_t*>(config.c_str()),SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION,world,nullptr,nullptr,nullptr)==ERROR_SUCCESS,"fixture foreign owner installed");
   tp.Privileges[0].Attributes=0;AdjustTokenPrivileges(token,FALSE,&tp,0,nullptr,nullptr);
  }
  QString error;
  const QString probe=QString::fromStdString((config.parent_path()/"independent-recovery.json").string());
  QByteArray readable;
  check(MasternodeWizardConfig::Read(QString::fromStdString(config.string()),readable,error) && readable=="# NONSECRET original\n","rejected config is independently readable");
  check(MasternodeWizardConfig::SaveRecovery(probe,"NONSECRET",error),"private recovery independently creatable despite config refusal");
  QFile::remove(probe);
  WalletModel wallet;MasternodeSetupWizard wizard(nullptr,&wallet);
  wizard.m_ip->setText("8.8.8.8");wizard.m_collateral_address->setText("C");
  wizard.m_owner_address->setText("O");wizard.m_voting_address->setText("V");wizard.m_payout_address->setText("P");
  wizard.m_bls_secret->setText("SYNTHETIC");wizard.m_bls_public->setText("PUBLIC");
  saw_exact_fee=false;wizard.accept();
  std::cout<<"ADMISSION kind="<<kind<<" prepares="<<wallet.n.prepares<<" broadcasts="<<wallet.n.broadcasts<<" unlocks="<<wallet.unlocks<<"\n";
  const char* labels[]={"WRITE_DAC denial blocks before prepare and sendrawtransaction","multiple links block before prepare and sendrawtransaction","foreign owner blocks before prepare and sendrawtransaction","private staging capability fails before prepare and sendrawtransaction"};
  check(wallet.n.prepares==0 && wallet.n.broadcasts==0 && wallet.unlocks==0 && !saw_exact_fee,labels[kind]);
  if(kind==3) check(last_message.contains("create private staging directory"),"stage-specific capability failure detected (not merely readability or lock)");
  std::ifstream f(config);std::string preserved((std::istreambuf_iterator<char>(f)),{});f.close();
  check(preserved=="# NONSECRET original\n" && !fs::exists(config.string()+".mnsetup-main.json"),"rejection preserves config bytes without identity or recovery publication");
  check(QDir(QString::fromStdString(config.parent_path().string())).entryList({"*.staging-*"},QDir::Dirs|QDir::NoDotAndDotDot).isEmpty(),"preflight probe leaves no staging payload");
  fs::remove(linked);fs::remove(config);fs::remove(config.string()+".mnsetup-main.json");
 }
 config=normal_config;CloseHandle(token);
#endif
 std::cout<<"flow failures="<<failures<<"\n";return failures?1:0;
}
'''
(a.output/'flow.cpp').write_text(source)
if a.generate_only:
    raise SystemExit(0)
env = dict(os.environ)
qt = env.get('QT_PREFIX', '/opt/homebrew/opt/qt@5')
env['PKG_CONFIG_PATH'] = qt + '/lib/pkgconfig:' + env.get('PKG_CONFIG_PATH', '')
flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'Qt5Widgets', 'Qt5Network'], env=env, text=True, encoding='utf8'))
subprocess.run(['clang++', '-std=c++20', str(a.output/'flow.cpp'), '-I'+str(repo/'src'), '-I'+str(repo/'src/univalue/include'), *map(str, (repo/'src/univalue/lib').glob('*.cpp')), *flags, '-o', str(a.output/'flow')], check=True)
env.update(QT_QPA_PLATFORM='offscreen', HOME=str(a.output), XDG_CONFIG_HOME=str(a.output))
run = subprocess.run([str(a.output/'flow'), str(a.output)], env=env, text=True, encoding='utf8', stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
print(run.stdout)
(a.output/'results.log').write_text(run.stdout)
raise SystemExit(run.returncode)
