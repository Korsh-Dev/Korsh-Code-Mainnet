// Standalone Windows/NTFS regression for the production inline writer.
// Build/run: bash ci/korsh/test_windows_acl.sh <disposable-parent> core|privileged
#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <qt/masternodewizardconfig.h>
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <atomic>
#include <thread>
#include <vector>
#include <iostream>

static int failures=0;
static void check(bool ok,const char* label) { std::cout<<(ok?"PASS ":"FAIL ")<<label<<std::endl;if(!ok)++failures; }
static bool acl(const QString& path,const wchar_t* sddl,bool protect=true) {
 PSECURITY_DESCRIPTOR sd=nullptr; if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl,SDDL_REVISION_1,&sd,nullptr))return false;
 PACL dacl=nullptr;BOOL present,def;GetSecurityDescriptorDacl(sd,&present,&dacl,&def);
 DWORD rc=SetNamedSecurityInfoW(const_cast<wchar_t*>(path.toStdWString().c_str()),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION|(protect?PROTECTED_DACL_SECURITY_INFORMATION:UNPROTECTED_DACL_SECURITY_INFORMATION),nullptr,nullptr,dacl,nullptr);LocalFree(sd);return rc==ERROR_SUCCESS;
}
static bool inheritedEveryoneRead(const QString& path) {
 PSECURITY_DESCRIPTOR sd=nullptr;PACL dacl=nullptr;
 if(GetNamedSecurityInfoW(const_cast<wchar_t*>(path.toStdWString().c_str()),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&dacl,nullptr,&sd)!=ERROR_SUCCESS)return false;
 unsigned char world[SECURITY_MAX_SID_SIZE];DWORD size=sizeof(world);bool found=false;
 if(CreateWellKnownSid(WinWorldSid,nullptr,world,&size)&&dacl)for(DWORD i=0;i<dacl->AceCount;++i){void* raw=nullptr;if(GetAce(dacl,i,&raw)){auto* ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);if(ace->Header.AceType==ACCESS_ALLOWED_ACE_TYPE&&(ace->Header.AceFlags&INHERITED_ACE)&&(ace->Mask&FILE_READ_DATA)&&EqualSid(&ace->SidStart,world))found=true;}}
 LocalFree(sd);return found;
}
// Independent oracle; never call production inspect/trustedOwner.
enum class OwnerPolicy { TokenUserOnly, ExistingTokenDefaultOwner };
static bool privateDescriptor(PSECURITY_DESCRIPTOR sd, OwnerPolicy policy=OwnerPolicy::TokenUserOnly) {
 PSID owner=nullptr;PACL d=nullptr;BOOL present=FALSE,def=FALSE;
 SECURITY_DESCRIPTOR_CONTROL ctl{};DWORD rev=0;
 if(!IsValidSecurityDescriptor(sd)||!GetSecurityDescriptorOwner(sd,&owner,&def)||!owner||!IsValidSid(owner)||
    !GetSecurityDescriptorDacl(sd,&present,&d,&def)||!present||!d||!IsValidAcl(d)||d->AceCount!=1||
    !GetSecurityDescriptorControl(sd,&ctl,&rev)||!(ctl&SE_DACL_PROTECTED))return false;
 HANDLE token=nullptr;DWORD n=0;std::vector<char> user,defaultOwner;
 if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return false;
 GetTokenInformation(token,TokenUser,nullptr,0,&n);user.resize(n);
 bool ok=n&&GetTokenInformation(token,TokenUser,user.data(),n,&n);
 if(ok){
  PSID sid=reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid;
  ok=IsValidSid(sid);bool ownerOK=ok&&EqualSid(owner,sid);
  if(ok&&!ownerOK&&policy==OwnerPolicy::ExistingTokenDefaultOwner){
   GetTokenInformation(token,TokenOwner,nullptr,0,&n);defaultOwner.resize(n);
   unsigned char admins[SECURITY_MAX_SID_SIZE];DWORD size=sizeof(admins);
   ownerOK=n&&GetTokenInformation(token,TokenOwner,defaultOwner.data(),n,&n)&&
    CreateWellKnownSid(WinBuiltinAdministratorsSid,nullptr,admins,&size)&&EqualSid(owner,admins)&&
    EqualSid(owner,reinterpret_cast<TOKEN_OWNER*>(defaultOwner.data())->Owner);
  }
  void* raw=nullptr;ok=ok&&ownerOK&&GetAce(d,0,&raw);
  if(ok){auto* ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);
   ok=ace->Header.AceType==ACCESS_ALLOWED_ACE_TYPE&&ace->Header.AceFlags==0&&
      ace->Header.AceSize>=sizeof(ACCESS_ALLOWED_ACE)&&ace->Mask==FILE_ALL_ACCESS&&
      IsValidSid(&ace->SidStart)&&EqualSid(&ace->SidStart,sid);}
 }
 CloseHandle(token);return ok;
}
static bool privateAcl(const QString& path, OwnerPolicy policy=OwnerPolicy::TokenUserOnly) {
 PSECURITY_DESCRIPTOR sd=nullptr;
 HANDLE h=CreateFileW(path.toStdWString().c_str(),READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
 if(h==INVALID_HANDLE_VALUE)return false;
 DWORD rc=GetSecurityInfo(h,SE_FILE_OBJECT,DACL_SECURITY_INFORMATION|OWNER_SECURITY_INFORMATION,nullptr,nullptr,nullptr,nullptr,&sd);CloseHandle(h);
 if(rc!=ERROR_SUCCESS)return false;
 bool ok=privateDescriptor(sd,policy);LocalFree(sd);return ok;
}
static void oracleFixtures() {
 HANDLE token=nullptr;DWORD n=0;std::vector<char> user;
 if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)){check(false,"oracle token fixture");return;}
 GetTokenInformation(token,TokenUser,nullptr,0,&n);user.resize(n);
 bool got=n&&GetTokenInformation(token,TokenUser,user.data(),n,&n);CloseHandle(token);
 check(got,"oracle token fixture");if(!got)return;
 PSID sid=reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid;LPWSTR stringSid=nullptr;
 if(!ConvertSidToStringSidW(sid,&stringSid)){check(false,"oracle SID fixture");return;}
 const std::wstring who(stringSid);LocalFree(stringSid);
 auto test=[&](const std::wstring& owner,const wchar_t* flags,bool expected,const char* label){
  std::wstring s=L"O:"+owner+L"D:P(A;"+flags+L";FA;;;"+who+L")";PSECURITY_DESCRIPTOR sd=nullptr;
  bool made=ConvertStringSecurityDescriptorToSecurityDescriptorW(s.c_str(),SDDL_REVISION_1,&sd,nullptr);
  check(made&&privateDescriptor(sd)==expected,label);if(sd)LocalFree(sd);
 };
 test(who,L"",true,"oracle accepts exact private descriptor");
 test(L"WD",L"",false,"oracle rejects foreign owner with otherwise exact ACL");
 test(who,L"OI",false,"oracle rejects non-inherited OBJECT_INHERIT flag");
 test(who,L"CI",false,"oracle rejects non-inherited CONTAINER_INHERIT flag");
 test(who,L"ID",false,"oracle rejects inherited flag");
}
static QByteArray read(const QString& p) {QFile f(p);f.open(QIODevice::ReadOnly);return f.readAll();}
int main(int argc,char**argv) {
 QCoreApplication app(argc,argv);
 if(argc!=3||(QString(argv[2])!="core"&&QString(argv[2])!="privileged"))return 2;
 const bool privileged=QString(argv[2])=="privileged";
 std::cout<<"SUITE "<<(privileged?"privileged (includes core)":"core (privileged fixtures NOT REQUESTED)")<<std::endl;
 oracleFixtures();
 // OpenSSH's elevated service token has backup/restore privileges ENABLED.
 // Disable them only in this test process so denial exercises normal DAC,
 // not the intentional administrator backup/restore bypass.
 HANDLE token=nullptr;
 if(!OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&token))return 2;
 for(const wchar_t* name:{L"SeBackupPrivilege",L"SeRestorePrivilege",L"SeTakeOwnershipPrivilege"}) {
  TOKEN_PRIVILEGES tp{};tp.PrivilegeCount=1;tp.Privileges[0].Attributes=privileged?0:SE_PRIVILEGE_REMOVED;
  if(!LookupPrivilegeValueW(nullptr,name,&tp.Privileges[0].Luid)||!AdjustTokenPrivileges(token,FALSE,&tp,0,nullptr,nullptr))return 2;
 }
 DWORD privilegeBytes=0;GetTokenInformation(token,TokenPrivileges,nullptr,0,&privilegeBytes);
 std::vector<char> privilegeData(privilegeBytes);
 bool disabled=privilegeBytes&&GetTokenInformation(token,TokenPrivileges,privilegeData.data(),privilegeBytes,&privilegeBytes);
 if(disabled){auto* privileges=reinterpret_cast<TOKEN_PRIVILEGES*>(privilegeData.data());
  for(const wchar_t* name:{L"SeBackupPrivilege",L"SeRestorePrivilege",L"SeTakeOwnershipPrivilege"}){
   LUID id{};disabled=disabled&&LookupPrivilegeValueW(nullptr,name,&id);
   for(DWORD i=0;i<privileges->PrivilegeCount;++i){const auto& privilege=privileges->Privileges[i];
    if(privilege.Luid.LowPart==id.LowPart&&privilege.Luid.HighPart==id.HighPart&&
       (!privileged||(privilege.Attributes&SE_PRIVILEGE_ENABLED)))disabled=false;
   }
  }
 }
 CloseHandle(token);check(disabled,"backup restore take-ownership privileges disabled in this process");if(!disabled)return 2;
 QTemporaryDir root(QCoreApplication::arguments()[1]+"/acl-NONSECRET-XXXXXX");if(!root.isValid())return 2;
 std::cout<<"FIXTURE "<<root.path().toStdString()<<std::endl;
 wchar_t volume[MAX_PATH]{};wchar_t filesystem[32]{};DWORD volumeFlags=0;
 const bool ntfs=GetVolumePathNameW(root.path().toStdWString().c_str(),volume,MAX_PATH)&&
  GetVolumeInformationW(volume,nullptr,0,nullptr,nullptr,&volumeFlags,filesystem,32)&&
  wcscmp(filesystem,L"NTFS")==0&&(volumeFlags&FILE_PERSISTENT_ACLS);
 check(ntfs,"fixture volume is actual NTFS with persistent ACLs");if(!ntfs)return 2;
 check(acl(root.path(),L"D:P(A;OICI;FA;;;WD)"),"fixture Everyone inheritance installed");
 const QString config=root.path()+QString::fromUtf8("/config-\xc3\xb1-\xe6\xb5\x8b.conf");QString error;bool changed=false;
 check(MasternodeWizardConfig::Save(config,"regtest","NONSECRET",changed,error)&&changed,"Save succeeds Unicode");
 check(privateAcl(config),"config protected current-user-only DACL");
 check(acl(config,L"D:(A;;FA;;;WD)",false)&&inheritedEveryoneRead(config),"restore inherited Everyone read on imported config");
 auto original=read(config);changed=true;error.clear();
 check(MasternodeWizardConfig::Save(config,"regtest","NONSECRET",changed,error)&&!changed&&read(config)==original,"no-op succeeds preserves bytes");
 check(privateAcl(config),"no-op hardens broad DACL");
 // OWNER RIGHTS removes the owner's implicit WRITE_DAC; explicit deny of
 // Everyone then forces a real kernel enforcement failure, even as admin.
 check(acl(config,L"D:P(D;;WD;;;WD)(D;;WD;;;OW)(A;;FA;;;WD)"),"install real WRITE_DAC denial");
 changed=true;error.clear();bool denied=MasternodeWizardConfig::Save(config,"regtest","NONSECRET",changed,error);
 check(!denied&&!changed&&!error.isEmpty()&&read(config)==original,"no-op ACL denial fails closed preserves bytes");
 // Restore via ownership-independent handle obtained before denial is not
 // needed: the synthetic file remains deletable via its parent.
 QFile::remove(config);

 // Qt imports may be TokenUser or the token's BA default owner. This is
 // intentionally the only file assertion allowing that preexisting policy.
 const QString imported=root.path()+"/imported.conf";
 {QFile f(imported);check(f.open(QIODevice::WriteOnly)&&f.write(original)==original.size(),"imported no-op default-owner fixture");}
 changed=true;error.clear();
 check(MasternodeWizardConfig::Save(imported,"regtest","NONSECRET",changed,error)&&!changed&&read(imported)==original&&
       privateAcl(imported,OwnerPolicy::ExistingTokenDefaultOwner),"imported default-owner no-op exact private ACL preserves bytes");
 QFile::remove(imported);
 // A real replacement must retain content and publish the private source ACL.
 {QFile f(config);check(f.open(QIODevice::WriteOnly)&&f.write("# PRESERVE_NONSECRET\n")==21,"replacement fixture");}
 check(acl(config,L"D:P(A;;FA;;;WD)"),"replacement broad ACL fixture");
 changed=false;error.clear();check(MasternodeWizardConfig::Save(config,"regtest","NONSECRET",changed,error)&&changed&&read(config).startsWith("# PRESERVE_NONSECRET\n")&&privateAcl(config),"atomic replacement retains prefix and private ACL");
 original=read(config);changed=true;error.clear();check(!MasternodeWizardConfig::Save(config,"regtest","OTHER_NONSECRET",changed,error)&&!changed&&read(config)==original,"operator identity rotation rejected");
 const QString linked=root.path()+"/hardlink.conf";
 check(CreateHardLinkW(linked.toStdWString().c_str(),config.toStdWString().c_str(),nullptr),"create hardlink fixture");
 changed=true;error.clear();check(!MasternodeWizardConfig::Save(config,"regtest","NONSECRET",changed,error)&&!changed&&read(linked)==original,"multiply-linked config rejected without changing bytes");
 QFile::remove(linked);
 if(privileged) {
 HANDLE symlinkToken=nullptr;
 bool symlinkReady=OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&symlinkToken);
 TOKEN_PRIVILEGES symlinkPrivileges{};symlinkPrivileges.PrivilegeCount=1;symlinkPrivileges.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;
 symlinkReady=symlinkReady&&LookupPrivilegeValueW(nullptr,L"SeCreateSymbolicLinkPrivilege",&symlinkPrivileges.Privileges[0].Luid)&&
  AdjustTokenPrivileges(symlinkToken,FALSE,&symlinkPrivileges,0,nullptr,nullptr)&&GetLastError()==ERROR_SUCCESS;
 if(symlinkToken)CloseHandle(symlinkToken);
 check(symlinkReady,"privileged fixture requires process-local symlink privilege");
 const QString symbolic=root.path()+"/symlink.conf";
 check(CreateSymbolicLinkW(symbolic.toStdWString().c_str(),config.toStdWString().c_str(),0),"create file reparse fixture");
 changed=true;error.clear();check(!MasternodeWizardConfig::Save(symbolic,"regtest","NONSECRET",changed,error)&&!changed&&read(config)==original,"file reparse target rejected");
 QFile::remove(symbolic);
 const QString symdir=root.path()+"/symdir";
 check(CreateSymbolicLinkW(symdir.toStdWString().c_str(),root.path().toStdWString().c_str(),SYMBOLIC_LINK_FLAG_DIRECTORY),"create directory reparse fixture");
 changed=true;error.clear();check(!MasternodeWizardConfig::Save(symdir+"/escape.conf","regtest","NONSECRET",changed,error)&&!changed&&!QFileInfo::exists(root.path()+"/escape.conf"),"reparse parent config fails closed");
 error.clear();check(!MasternodeWizardConfig::SaveRecovery(symdir+"/escape.json","NONSECRET",error)&&!QFileInfo::exists(root.path()+"/escape.json"),"reparse parent journal fails closed");
 RemoveDirectoryW(symdir.toStdWString().c_str());
 // Synthetic file owned by Everyone must not be legitimized by our DACL.
 HANDLE ownerToken=nullptr;OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&ownerToken);
 TOKEN_PRIVILEGES tp{};tp.PrivilegeCount=1;LookupPrivilegeValueW(nullptr,L"SeRestorePrivilege",&tp.Privileges[0].Luid);tp.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;
 check(AdjustTokenPrivileges(ownerToken,FALSE,&tp,0,nullptr,nullptr)&&GetLastError()==ERROR_SUCCESS,"enable fixture-only owner assignment");
 unsigned char world[SECURITY_MAX_SID_SIZE];DWORD sidSize=sizeof(world);CreateWellKnownSid(WinWorldSid,nullptr,world,&sidSize);
 check(SetNamedSecurityInfoW(const_cast<wchar_t*>(config.toStdWString().c_str()),SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION,world,nullptr,nullptr,nullptr)==ERROR_SUCCESS,"foreign owner fixture");
 tp.Privileges[0].Attributes=0;AdjustTokenPrivileges(ownerToken,FALSE,&tp,0,nullptr,nullptr);CloseHandle(ownerToken);
 changed=true;error.clear();check(!MasternodeWizardConfig::Save(config,"regtest","NONSECRET",changed,error)&&!changed&&read(config)==original,"foreign owner rejected without changing bytes");
 QFile::remove(config);
 } // Explicit privileged suite; core does not claim these fixtures.

 check(ImpersonateSelf(SecurityImpersonation),"impersonation fixture");
 changed=true;error.clear();bool impersonatedSave=MasternodeWizardConfig::Save(root.path()+"/impersonated.conf","regtest","NONSECRET",changed,error);
 check(!impersonatedSave&&!changed&&!error.isEmpty()&&!QFileInfo::exists(root.path()+"/impersonated.conf"),"impersonated config fails closed");
 error.clear();bool impersonatedRecovery=MasternodeWizardConfig::SaveRecovery(root.path()+"/impersonated.json","NONSECRET",error);
 check(!impersonatedRecovery&&!error.isEmpty()&&!QFileInfo::exists(root.path()+"/impersonated.json"),"impersonated journal fails closed");
 check(RevertToSelf(),"restore test thread token");
 changed=true;error.clear();check(!MasternodeWizardConfig::Save(root.path()+"/ads.conf:stream","regtest","NONSECRET",changed,error)&&!changed&&!QFileInfo::exists(root.path()+"/ads.conf"),"alternate data stream config rejected");
 error.clear();check(!MasternodeWizardConfig::SaveRecovery(root.path()+"/ads.json:stream","NONSECRET",error)&&!QFileInfo::exists(root.path()+"/ads.json"),"alternate data stream journal rejected");
 // Hold the actual production Stage alive, independent of scheduling.
 QString stagedDirectory,stagedFile;
 {
  MasternodeWizardConfig::WindowsPrivate::Security security;
  MasternodeWizardConfig::WindowsPrivate::Stage stage;
  error.clear();const QByteArray payload("NONSECRET_STAGE");
  check(security.init(error)&&stage.write(root.path()+"/deterministic",payload,security,error),"production Stage writes deterministic fixture");
  stagedDirectory=stage.directory;stagedFile=stage.file;
  check(privateAcl(stage.directory),"live production Stage directory exact owner and ACE flags");
  check(privateAcl(stage.file)&&read(stage.file)==payload,"live production Stage payload exact owner flags and bytes");
 }
 check(!QFileInfo::exists(stagedDirectory)&&!QFileInfo::exists(stagedFile),"production Stage destructor cleans fixture");
 const QString journal=root.path()+QString::fromUtf8("/journal-\xc3\xb1.json");
 std::atomic<int> ready{0},done{0},wins{0},winner{-1};std::atomic<bool> bad{false};std::vector<std::thread> threads;
 for(int i=0;i<8;i++)threads.emplace_back([&,i]{++ready;while(ready!=8)std::this_thread::yield();QString e;if(MasternodeWizardConfig::SaveRecovery(journal,QByteArray(131072,'A'+i),e)){winner=i;++wins;}else if(e.isEmpty())bad=true;++done;});
 bool partial=false;while(done!=8){
if(QFileInfo::exists(journal)){auto b=read(journal);if(b.size()!=131072||b!=QByteArray(131072,b.isEmpty()?'?':b[0]))partial=true;}std::this_thread::yield();}
 for(auto& t:threads)t.join();
 check(wins==1&&!bad&&!partial&&read(journal)==QByteArray(131072,'A'+winner.load()),"concurrent journal single complete winner");
 check(privateAcl(journal),"journal protected current-user-only DACL");
 original=read(journal);error.clear();check(!MasternodeWizardConfig::SaveRecovery(journal,"NONSECRET_REPLACE",error)&&!error.isEmpty()&&read(journal)==original,"existing journal preserved");
 const QString deniedDir=root.path()+"/denied";QDir().mkdir(deniedDir);check(acl(deniedDir,L"D:P(D;;0x00000006;;;WD)(A;;FA;;;WD)"),"directory create denied fixture");
 changed=true;error.clear();check(!MasternodeWizardConfig::Save(deniedDir+"/new.conf","regtest","NONSECRET",changed,error)&&!changed&&!QFileInfo::exists(deniedDir+"/new.conf"),"denied new config fails closed");
 error.clear();const bool journalDenied=MasternodeWizardConfig::SaveRecovery(deniedDir+"/journal","NONSECRET",error);std::cout<<"DENIED_JOURNAL ok="<<journalDenied<<" exists="<<QFileInfo::exists(deniedDir+"/journal")<<" error="<<error.toStdString()<<std::endl;check(!journalDenied&&!QFileInfo::exists(deniedDir+"/journal"),"denied journal fails closed");
 acl(deniedDir,L"D:P(A;;FA;;;WD)");
 check(QDir(root.path()).entryList({"*.staging-*"},QDir::Dirs|QDir::NoDotAndDotDot).isEmpty(),"no staging leaked");
 std::cout<<"failures="<<failures<<std::endl;return failures?1:0;
}
