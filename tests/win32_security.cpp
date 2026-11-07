// security_checks: portable RSA and invalid-input tests.
// security_checks <AMD DLL directory>: additionally verifies real file integrity,
// metadata, signer lookup and RSA key pin; corrupts a copy to check rejection.
#include "win32_security.h"
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#define MS __attribute__((ms_abi))
#define CHECK(expression) do { if (!(expression)) { std::fprintf(stderr,"%s:%d: check failed: %s\n",__FILE__,__LINE__,#expression); std::abort(); } } while (false)
namespace ffxvk {
std::string fromUtf16(const char16_t* p) { std::string s; for (;p && *p;++p) s += char(*p); return s; }
uint32_t& lastError() { static thread_local uint32_t e; return e; }
}
struct Blob { uint32_t size; unsigned char* data; };
struct Algorithm { const char* oid; Blob parameters; };
struct BitBlob { uint32_t size; unsigned char* data; uint32_t unused; };
struct CertInfo {
 uint32_t version; Blob serial; Algorithm signature; Blob issuer; uint64_t before,after;
 Blob subject; Algorithm publicAlgorithm; BitBlob publicKey,issuerUnique,subjectUnique;
 uint32_t count; void* extensions;
};
struct CertContext { uint32_t encoding; unsigned char* data; uint32_t size; CertInfo* info; void* store; };
struct SignerInfo { uint32_t version; Blob issuer,serial; };
struct FileInfo { uint32_t size = 32; const char16_t* path; void* handle = nullptr; void* subject = nullptr; };
struct TrustData {
 uint32_t size = 88; void* policy = nullptr; void* sip = nullptr; uint32_t ui = 2, revocation = 0, choice = 1;
 FileInfo* file; uint32_t action = 1; void* state = nullptr; void* url = nullptr; uint32_t flags = 0,context = 0;
 void* signatureSettings = nullptr;
};
static_assert(sizeof(TrustData)==88);
static_assert(offsetof(TrustData,file)==40);
template<class T> T function(const char* dll,const char* name) { auto f = ffxvk::securityStandIn(dll,name); CHECK(f); return reinterpret_cast<T>(f); }
int main(int argc,char** argv) {
 if(argc>2) return std::fprintf(stderr,"security_checks [DLL directory]\n"),2;
 auto verify=function<int32_t MS(*)(void*,const void*,void*)>("wintrust.dll","WinVerifyTrust");
 auto size=function<uint32_t MS(*)(const char16_t*,uint32_t*)>("version.dll","GetFileVersionInfoSizeW");
 auto version=function<int MS(*)(const char16_t*,uint32_t,uint32_t,void*)>("version.dll","GetFileVersionInfoW");
 auto value=function<int MS(*)(const void*,const char16_t*,void**,uint32_t*)>("version.dll","VerQueryValueW");
 auto query=function<int MS(*)(uint32_t,const void*,uint32_t,uint32_t,uint32_t,uint32_t*,uint32_t*,uint32_t*,void**,void**,const void**)>("crypt32.dll","CryptQueryObject");
 auto param=function<int MS(*)(void*,uint32_t,uint32_t,void*,uint32_t*)>("crypt32.dll","CryptMsgGetParam");
 auto find=function<const CertContext* MS(*)(void*,uint32_t,uint32_t,uint32_t,const CertInfo*,const void*)>("crypt32.dll","CertFindCertificateInStore");
 auto decode=function<int MS(*)(uint32_t,const char*,const unsigned char*,uint32_t,uint32_t,const void*,void*,uint32_t*)>("crypt32.dll","CryptDecodeObjectEx");
 const unsigned char action[] = {0x6b,0xc5,0xaa,0x00,0x44,0xcd,0xd0,0x11,0x8c,0xc2,0x00,0xc0,0x4f,0xc2,0x95,0xee};
 // These checks need no proprietary fixtures and are suitable for default CTest.
 CHECK(size(u"/does/not/exist.dll",nullptr)==0);
 CHECK(!query(1,u"/does/not/exist.dll",0x400,2,0,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr));
 CHECK(verify(nullptr,action,nullptr)!=0);
 const unsigned char rsa[] = {0x30,0x0b,0x02,0x04,0x00,0x80,0x01,0x02,0x02,0x03,0x01,0x00,0x01};
 uint32_t decodedSize=0;
 CHECK(decode(0x10001,reinterpret_cast<const char*>(0x48),rsa,sizeof rsa,0,nullptr,nullptr,&decodedSize));
 CHECK(decodedSize==30);
 std::vector<unsigned char> rsaBlob(decodedSize);
 CHECK(decode(0x10001,reinterpret_cast<const char*>(0x48),rsa,sizeof rsa,0,nullptr,rsaBlob.data(),&decodedSize));
 const uint32_t header[] = {0x31415352,24,3,3,0,0};
 const unsigned char keyBytes[]={1,0,1,0x80,1,2};
 CHECK(!std::memcmp(rsaBlob.data(),header,sizeof header));
 CHECK(!std::memcmp(rsaBlob.data()+24,keyBytes,sizeof keyBytes));
 uint32_t tooSmall=29;
 CHECK(!decode(0x10001,reinterpret_cast<const char*>(0x48),rsa,sizeof rsa,0,nullptr,rsaBlob.data(),&tooSmall)&&tooSmall==30);
 CHECK(!decode(0x10001,reinterpret_cast<const char*>(0x48),rsa,2,0,nullptr,nullptr,&decodedSize));
 std::array<unsigned char,64> invalidVersion{};void* invalidValue=nullptr;uint32_t invalidSize=0;
 CHECK(!value(invalidVersion.data(),u"\\",&invalidValue,&invalidSize));
 if(argc==1) return std::puts("RSA decoding and invalid-input checks passed"),0;
 std::vector<unsigned char> key;
 for(const char* name:{"amd_fidelityfx_loader_vk.dll","amd_fidelityfx_upscaler_vk.dll","amdvkffx64.dll"}) {
  auto path=std::filesystem::path(argv[1])/name;
  auto wide=path.u16string(); FileInfo f;f.path=wide.c_str();TrustData trust;trust.file=&f;
  int32_t result=verify(nullptr,action,&trust);
  std::printf("%s integrity: %08x\n",name,uint32_t(result));CHECK(result==0);
  uint32_t h=123,n=size(wide.c_str(),&h);CHECK(n>=52 && h==0);
  std::vector<unsigned char> metadata(n);CHECK(version(wide.c_str(),0,n,metadata.data()));
  void* fixed=nullptr;uint32_t fixedSize=0;CHECK(value(metadata.data(),u"\\",&fixed,&fixedSize)&&fixedSize==52);
  auto* versionWords=static_cast<uint32_t*>(fixed);CHECK(versionWords[0]==0xfeef04bd);
  std::printf("  file version: %u.%u.%u.%u\n",versionWords[2]>>16,versionWords[2]&65535,versionWords[3]>>16,versionWords[3]&65535);
  void* store=nullptr;void* message=nullptr;uint32_t encoding=0,content=0,format=0;
  CHECK(query(1,wide.c_str(),0x400,2,0,&encoding,&content,&format,&store,&message,nullptr));
  CHECK(encoding==0x10001&&content==10&&format==1&&store&&message);
  uint32_t paramSize=0;CHECK(param(message,6,0,nullptr,&paramSize));
  std::vector<unsigned char> signer(paramSize);CHECK(param(message,6,0,signer.data(),&paramSize));
  auto* si=reinterpret_cast<SignerInfo*>(signer.data());CertInfo wanted{};wanted.issuer=si->issuer;wanted.serial=si->serial;
  auto* cert=find(store,encoding,0,0xb0000,&wanted,nullptr);CHECK(cert&&cert->info&&cert->size);
  uint32_t keySize=0;void* decoded=nullptr;
  CHECK(decode(encoding,reinterpret_cast<const char*>(0x48),cert->info->publicKey.data,cert->info->publicKey.size,0x8000,nullptr,&decoded,&keySize));
  CHECK(keySize==0x21b);CHECK(*static_cast<uint32_t*>(decoded)==0x31415352);
  if(key.empty())key.assign(static_cast<unsigned char*>(decoded),static_cast<unsigned char*>(decoded)+keySize);
  else CHECK(std::memcmp(key.data(),decoded,keySize)==0);
  std::free(decoded);
  auto invalidWanted=wanted;invalidWanted.issuer.size++;CHECK(!find(store,encoding,0,0xb0000,&invalidWanted,nullptr));
 }
 // The cryptographic key returned above is byte-for-byte the driver's pinned AMD key.
 auto driver=std::filesystem::path(argv[1])/"amdvkffx64.dll";
 std::ifstream input(driver,std::ios::binary);std::vector<unsigned char> driverBytes((std::istreambuf_iterator<char>(input)),{});
 CHECK(driverBytes.size()>0x220be0+key.size());CHECK(!std::memcmp(key.data(),driverBytes.data()+0x220be0,key.size()));
 std::string temporaryPattern=(std::filesystem::temp_directory_path()/"ffx-security-XXXXXX").string();
 char* temporary=mkdtemp(temporaryPattern.data());CHECK(temporary);
 auto damaged=std::filesystem::path(temporary)/"modified.dll";
 driverBytes[0x1000]^=1;
 {std::ofstream output(damaged,std::ios::binary);output.write(reinterpret_cast<const char*>(driverBytes.data()),driverBytes.size());}
 auto damagedWide=damaged.u16string();FileInfo damagedInfo;damagedInfo.path=damagedWide.c_str();TrustData damagedTrust;damagedTrust.file=&damagedInfo;
 CHECK(verify(nullptr,action,&damagedTrust)!=0);
 std::filesystem::remove(damaged);
 std::filesystem::remove(temporary);
 CHECK(size(u"/does/not/exist.dll",nullptr)==0);
 CHECK(!query(1,u"/does/not/exist.dll",0x400,2,0,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr));
 std::puts("Version, Authenticode, signer key, and tamper checks passed");
}
