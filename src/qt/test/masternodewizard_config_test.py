#!/usr/bin/env python3
"""Isolated real-writer regression; no node/wallet or shared build required.

Run: python3 src/qt/test/masternodewizard_config_test.py --output /scratch/path
Requires C++20 and Qt5Widgets (pkg-config; QT_PREFIX optional).
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--output', required=True, type=Path)
p.add_argument('--source-ref', help='Read the writer from a git revision for baseline reproduction')
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
repo = Path(__file__).resolve().parents[3]
cpp = (subprocess.check_output(['git', 'show', a.source_ref + ':src/qt/masternodelist.cpp'], cwd=repo, text=True, encoding='utf8')
       if a.source_ref else (repo / 'src/qt/masternodelist.cpp').read_text())
start = cpp.index('bool MasternodeSetupWizard::saveOperatorSecretToConfig')
save = cpp[start:cpp.index('bool MasternodeSetupWizard::registerMasternode', start)]
source = r'''
#include <QtWidgets>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>
#include <atomic>
#include <thread>
#ifndef _WIN32
#include <sys/resource.h>
#include <signal.h>
#endif
#ifdef __APPLE__
#include <sys/stat.h>
#endif
namespace fs = std::filesystem;
static fs::path config;
struct Args {
 fs::path GetPathArg(const char*,const char*) {return config;}
 std::string GetArg(const char*,const char*) {return {};}
 bool IsArgSet(const char*) {return false;}
} gArgs;
struct Chain {std::string NetworkIDString() const {return "main";}};
Chain Params() {return {};}
constexpr const char* BITCOIN_CONF_FILENAME="korsh.conf";
fs::path GetConfigFile(fs::path p) {return p;}
bool TryCreateDirectories(fs::path p) {return fs::create_directories(p);}
bool RenameOver(fs::path a,fs::path b) {std::error_code e;fs::rename(a,b,e);return !e;}
namespace GUIUtil {QString PathToQString(fs::path p) {return QString::fromStdString(p.string());}}
std::string_view TrimLeft(std::string_view s) {auto n=s.find_first_not_of(" \t");return n==s.npos?std::string_view{}:s.substr(n);}
#if __has_include(<qt/masternodewizardconfig.h>)
#include <qt/masternodewizardconfig.h>
#endif
class MasternodeSetupWizard:public QObject {public:
 QLineEdit* m_bls_secret; bool m_restart_required=false;
 bool saveOperatorSecretToConfig(QString&);
};
''' + save + r'''
int main(int argc,char**argv) {
 QApplication app(argc,argv);
 config=fs::path(argv[1])/"fixture.conf";
 QLineEdit secret;secret.setText("SYNTHETIC_NEW");
 MasternodeSetupWizard w;w.m_bls_secret=&secret;QString error;
 const std::vector<std::string> identities={
 "\xEF\xBB\xBFmasternodeblsprivkey=SYNTHETIC_OLD\n",
 "masternodeblsprivkey = SYNTHETIC_OLD # existing operator\n",
 "[main]\nmasternodeblsprivkey=SYNTHETIC_OLD\n",
 "main.masternodeblsprivkey=SYNTHETIC_OLD\n"};
 int failed=0;
 // Admission must never install an identity, even though Prepare computes it.
 fs::remove(config);
 if(!MasternodeWizardConfig::Preflight(GUIUtil::PathToQString(config),"main","SYNTHETIC_NEW",error) || fs::exists(config)) {
  std::cerr<<"FAIL: successful preflight must leave absent config absent\n";++failed;
 }
 const std::string untouched="# NONSECRET preflight\n";
 {std::ofstream f(config);f<<untouched;}
 if(!MasternodeWizardConfig::Preflight(GUIUtil::PathToQString(config),"main","SYNTHETIC_NEW",error)) {
  std::cerr<<"FAIL: existing regular config preflight\n";++failed;
 }
 {std::ifstream f(config);std::string bytes((std::istreambuf_iterator<char>(f)),{});
  if(bytes!=untouched) {std::cerr<<"FAIL: preflight changed config bytes\n";++failed;}}
 {
  QLockFile held(GUIUtil::PathToQString(config)+".mnsetup.lock");
  if(!held.tryLock(0))return 2;
  if(MasternodeWizardConfig::Preflight(GUIUtil::PathToQString(config),"main","SYNTHETIC_NEW",error)) {
   std::cerr<<"FAIL: preflight ignored config lock\n";++failed;
  }
 }
 // A successful preflight is not authority to overwrite a later identity.
 {std::ofstream f(config);f<<"masternodeblsprivkey=LATER_IDENTITY\n";}
 if(w.saveOperatorSecretToConfig(error)) {
  std::cerr<<"FAIL: save trusted stale preflight\n";++failed;
 }
 {std::ifstream f(config);std::string bytes((std::istreambuf_iterator<char>(f)),{});
  if(bytes!="masternodeblsprivkey=LATER_IDENTITY\n") {std::cerr<<"FAIL: save changed intervening identity\n";++failed;}}
 for(const auto& original:identities) {
  {std::ofstream f(config);f<<original;}
  bool ok=w.saveOperatorSecretToConfig(error);
  std::ifstream f(config);std::string after((std::istreambuf_iterator<char>(f)),{});
  if(ok || original!=after) {std::cerr<<"FAIL: existing operator overwritten or shadowed\n";++failed;}
 }
 {std::ofstream f(config);f<<"# preserve\n[test]\nmasternodeblsprivkey=OTHER_NETWORK\n";}
 if(!w.saveOperatorSecretToConfig(error)) {std::cerr<<error.toStdString()<<"\n";++failed;}
 std::ifstream f(config);std::string after((std::istreambuf_iterator<char>(f)),{});
 if(after.find("OTHER_NETWORK")==std::string::npos || after.find("[main]\nmasternodeblsprivkey=SYNTHETIC_NEW")==std::string::npos) {
  std::cerr<<"FAIL: wrong network section\n";++failed;
 }
 // A byte-for-byte no-op must still remove preexisting public readability.
 const std::string same="[main]\nmasternodeblsprivkey=SYNTHETIC_NEW\n";
 {std::ofstream f(config);f<<same;}
 fs::permissions(config,fs::perms::owner_read|fs::perms::owner_write|fs::perms::group_read|fs::perms::others_read);
 bool changed=true;error.clear();
#ifdef __APPLE__
 // Actual kernel EPERM, not a mocked chmod: immutable scratch fixture owned
 // by this process. Restore the flag even when the assertion fails.
 if(chflags(config.c_str(),UF_IMMUTABLE)!=0) return 2;
 bool denied_ok=MasternodeWizardConfig::Save(GUIUtil::PathToQString(config),"main","SYNTHETIC_NEW",changed,error);
 if(chflags(config.c_str(),0)!=0) return 2;
 std::ifstream denied_file(config);std::string denied_after((std::istreambuf_iterator<char>(denied_file)),{});
 if(denied_ok || changed || error.isEmpty() || denied_after!=same) {
  std::cerr<<"FAIL: no-op permission error must fail closed and preserve bytes\n";++failed;
 }
 error.clear();changed=true;
#endif
 bool same_ok=MasternodeWizardConfig::Save(GUIUtil::PathToQString(config),"main","SYNTHETIC_NEW",changed,error);
 std::ifstream same_file(config);std::string same_after((std::istreambuf_iterator<char>(same_file)),{});
 if(!same_ok || changed || same_after!=same ||
    (fs::status(config).permissions()&(fs::perms::group_all|fs::perms::others_all))!=fs::perms::none) {
  std::cerr<<"FAIL: no-op must preserve bytes and harden permissions\n";++failed;
 }
 // Safe failure on a real short write: constrain only this child process,
 // never fill the user's filesystem. The original must survive byte-for-byte.
#ifndef _WIN32
 const std::string original="#"+std::string(50,'x')+"\n";
 {std::ofstream f(config);f<<original;}
 struct rlimit before;getrlimit(RLIMIT_FSIZE,&before);
 struct rlimit limited=before;limited.rlim_cur=64;
 signal(SIGXFSZ,SIG_IGN);setrlimit(RLIMIT_FSIZE,&limited);
 bool short_ok=w.saveOperatorSecretToConfig(error);
 setrlimit(RLIMIT_FSIZE,&before);
 std::ifstream short_file(config);std::string preserved((std::istreambuf_iterator<char>(short_file)),{});
 if(short_ok || preserved!=original) {std::cerr<<"FAIL: short write replaced original config\n";++failed;}
 const std::string large_original="#"+std::string(8192,'x')+"\n";
 {std::ofstream large(config);large<<large_original;}
 // Leave room for QLockFile metadata, but not the actual buffered config.
 struct rlimit buffered_limited=before;buffered_limited.rlim_cur=1024;
 setrlimit(RLIMIT_FSIZE,&buffered_limited);error.clear();
 const bool large_ok=w.saveOperatorSecretToConfig(error);
 setrlimit(RLIMIT_FSIZE,&before);
 std::ifstream large_file(config);std::string large_after((std::istreambuf_iterator<char>(large_file)),{});
 if(large_ok || error.isEmpty() || large_after!=large_original) {
  std::cerr<<"FAIL: buffered short config write must fail and preserve original\n";++failed;
 }
 fs::path link=config.parent_path()/"linked.conf";
 fs::remove(link);fs::create_symlink(config,link);
 const fs::path actual=config;config=link;
 if(w.saveOperatorSecretToConfig(error)) {std::cerr<<"FAIL: followed symlink\n";++failed;}
 config=actual;
 const auto permissions=fs::status(config).permissions();
 if((permissions & (fs::perms::group_all | fs::perms::others_all)) != fs::perms::none) {
  std::cerr<<"FAIL: operator config is accessible by other users\n";++failed;
 }
#endif
 // Competing publishers must have exactly one winner; observers can see only
 // a complete immutable payload, never a partially copied/truncated journal.
 const QString recovery=GUIUtil::PathToQString(config)+QString::fromUtf8(".recovery-\xc3\xb1.json");
 QFile::remove(recovery);
 std::atomic<int> ready{0},done{0},wins{0};
 std::atomic<bool> bad_error{false};
 std::vector<std::thread> publishers;
 std::atomic<int> winner_index{-1};
 for(int i=0;i<8;++i) publishers.emplace_back([&,i] {
  const QByteArray payload(131072,'A'+i);QString why;
  ++ready;while(ready.load()!=8) std::this_thread::yield();
  if(MasternodeWizardConfig::SaveRecovery(recovery,payload,why)) {winner_index=i;++wins;}
  else if(why.isEmpty()) bad_error=true;
  ++done;
 });
 QByteArray observed;bool partial=false;
 while(done.load()!=8) {
  QFile published(recovery);
  if(published.open(QIODevice::ReadOnly)) {
   const QByteArray bytes=published.readAll();
   if(bytes.size()!=131072 || bytes!=QByteArray(131072,bytes.isEmpty()?'?':bytes[0]) ||
      (!observed.isEmpty() && observed!=bytes)) partial=true;
   observed=bytes;
  }
  std::this_thread::yield();
 }
 for(auto& thread:publishers) thread.join();
 QFile published(recovery);published.open(QIODevice::ReadOnly);const QByteArray final=published.readAll();published.close();
 if(wins!=1 || bad_error || partial || final!=QByteArray(131072,'A'+winner_index.load())) {
  std::cerr<<"FAIL: journal publication must be exclusive and complete\n";++failed;
 }
 error.clear();
 if(MasternodeWizardConfig::SaveRecovery(recovery,"REPLACEMENT",error) || error.isEmpty()) {
  std::cerr<<"FAIL: existing journal must refuse replacement\n";++failed;
 }
 published.open(QIODevice::ReadOnly);
 if(published.readAll()!=final) {std::cerr<<"FAIL: journal overwritten\n";++failed;}
 published.close();QFile::remove(recovery);
#ifndef _WIN32
 const auto dangling=config.parent_path()/"absent-recovery-target";fs::remove(dangling);
 const auto link_path=fs::path(QFile::encodeName(recovery).constData());
 fs::create_symlink(dangling,link_path);error.clear();
 if(MasternodeWizardConfig::SaveRecovery(recovery,"REPLACEMENT",error) || error.isEmpty() ||
    !fs::is_symlink(link_path) || fs::exists(dangling)) {
  std::cerr<<"FAIL: journal publication must preserve dangling symlink\n";++failed;
 }
 fs::remove(link_path);
 struct rlimit journal_before;getrlimit(RLIMIT_FSIZE,&journal_before);
 struct rlimit journal_limited=journal_before;journal_limited.rlim_cur=64;
 setrlimit(RLIMIT_FSIZE,&journal_limited);error.clear();
 const bool journal_short=MasternodeWizardConfig::SaveRecovery(recovery,QByteArray(8192,'S'),error);
 setrlimit(RLIMIT_FSIZE,&journal_before);
 if(journal_short || error.isEmpty() || QFileInfo::exists(recovery)) {
  std::cerr<<"FAIL: short journal write must never publish (success="<<journal_short
           <<", error_empty="<<error.isEmpty()<<", published="<<QFileInfo::exists(recovery)<<")\n";++failed;
 }
#endif
 if(!QDir(QString::fromStdString(config.parent_path().string())).entryList({"*.staging-*"},QDir::Dirs|QDir::NoDotAndDotDot).isEmpty()) {
  std::cerr<<"FAIL: leaked recovery staging directory\n";++failed;
 }
 std::cout<<"config regressions failures="<<failed<<"\n";
 return failed?1:0;
}
'''
(a.output / 'probe.cpp').write_text(source)
env = dict(os.environ)
qt = env.get('QT_PREFIX', '/opt/homebrew/opt/qt@5')
env['PKG_CONFIG_PATH'] = qt + '/lib/pkgconfig:' + env.get('PKG_CONFIG_PATH', '')
flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'Qt5Widgets'], env=env, text=True, encoding='utf8'))
subprocess.run(['clang++', '-std=c++20', str(a.output/'probe.cpp'), '-I'+str(repo/'src'), *flags, '-o', str(a.output/'probe')], check=True)
env['QT_QPA_PLATFORM'] = 'minimal'
env['HOME'] = str(a.output)
env['XDG_CONFIG_HOME'] = str(a.output)
run = subprocess.run([str(a.output/'probe'), str(a.output)], env=env, text=True, encoding='utf8', stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
print(run.stdout)
(a.output/'results.log').write_text(run.stdout)
raise SystemExit(run.returncode)
