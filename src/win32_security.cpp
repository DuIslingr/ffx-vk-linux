#include "win32_security.h"
#include "teb.h"
#include "win32.h"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/pkcs7.h>
#include <openssl/x509.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define MS __attribute__((ms_abi))
namespace ffxvk {
namespace {
using Bytes = std::vector<unsigned char>;
template<class T> T read(const Bytes& b, size_t o) {
    T v{};
    if (o <= b.size() && sizeof v <= b.size() - o) std::memcpy(&v, b.data() + o, sizeof v);
    return v;
}
std::string pathOf(const char16_t* w) {
    std::string p = fromUtf16(w);
    std::replace(p.begin(), p.end(), '\\', '/');
    return p;
}
struct PeFile {
    Bytes bytes;
    size_t opt = 0, section = 0;
    uint32_t certificate = 0, certificateSize = 0, headers = 0;
    uint16_t sectionCount = 0;
    bool load(const char16_t* path) {
        std::ifstream in(pathOf(path), std::ios::binary | std::ios::ate);
        if (!in) return false;
        const auto length = in.tellg();
        if (length < 0 || length > 256 * 1024 * 1024) return false;
        bytes.resize(size_t(length));
        in.seekg(0);
        if (!in.read(reinterpret_cast<char*>(bytes.data()),length)) return false;
        const size_t pe = read<uint32_t>(bytes, 60);
        if (read<uint16_t>(bytes, 0) != 0x5a4d || pe > bytes.size() || bytes.size() - pe < 24 || read<uint32_t>(bytes, pe) != 0x4550) return false;
        opt = pe + 24;
        const auto optionalSize = read<uint16_t>(bytes, pe + 20);
        if (optionalSize < 152 || opt > bytes.size() || optionalSize > bytes.size() - opt || read<uint16_t>(bytes, opt) != 0x20b) return false;
        section = opt + optionalSize;
        sectionCount = read<uint16_t>(bytes, pe + 6);
        if (sectionCount > (bytes.size() - section) / 40) return false;
        headers = read<uint32_t>(bytes, opt + 60);
        certificate = read<uint32_t>(bytes, opt + 144);
        certificateSize = read<uint32_t>(bytes, opt + 148);
        return headers >= section + sectionCount * 40 && headers <= bytes.size();
    }
    size_t fileOffset(uint32_t rva, size_t length) const {
        for (unsigned i = 0; i < sectionCount; ++i) {
            const size_t s = section + i * 40;
            const uint32_t va = read<uint32_t>(bytes, s + 12), rawSize = read<uint32_t>(bytes, s + 16), raw = read<uint32_t>(bytes, s + 20);
            if (rva < va || uint64_t(rva) + length > uint64_t(va) + rawSize) continue;
            const size_t o = size_t(raw) + (rva - va);
            if (o <= bytes.size() && length <= bytes.size() - o) return o;
        }
        return 0;
    }
    Bytes version() const {
        const uint32_t rva = read<uint32_t>(bytes, opt + 128), size = read<uint32_t>(bytes, opt + 132);
        const size_t root = fileOffset(rva, size);
        if (!root || size < 16) return {};
        uint32_t directory = 0;
        for (int depth = 0; depth < 3; ++depth) {
            if (directory > size || size - directory < 16) return {};
            const size_t d = root + directory;
            const unsigned n = unsigned(read<uint16_t>(bytes, d + 12)) + read<uint16_t>(bytes, d + 14);
            if (n > (size - directory - 16) / 8) return {};
            bool found = false;
            for (unsigned i = 0; i < n; ++i) {
                const size_t e = d + 16 + i * 8;
                if (depth == 0 && read<uint32_t>(bytes, e) != 16) continue; // RT_VERSION
                const uint32_t target = read<uint32_t>(bytes, e + 4);
                if (depth < 2) {
                    if (!(target & 0x80000000)) continue;
                    directory = target & 0x7fffffff;
                    found = true;
                    break;
                }
                if ((target & 0x80000000) || target > size || size - target < 16) continue;
                const uint32_t dataRva = read<uint32_t>(bytes, root + target), dataSize = read<uint32_t>(bytes, root + target + 4);
                const size_t off = fileOffset(dataRva, dataSize);
                if (!off || dataSize < 6) return {};
                const uint16_t length = read<uint16_t>(bytes, off);
                if (length > dataSize || length < 6) return {};
                return Bytes(bytes.begin() + off, bytes.begin() + off + length);
            }
            if (!found) return {};
        }
        return {};
    }
};
// Read a single definite-length ASN.1 TLV. Authenticode signs the contents of
// SpcIndirectDataContent's outer SEQUENCE, rather than its tag/length bytes.
bool payload(const unsigned char* p, size_t n, const unsigned char*& data, size_t& size) {
    if (n < 2) return false;
    size_t h = 2, length = p[1];
    if (length & 128) {
        const size_t count = length & 127;
        if (!count || count > sizeof(size_t) || count > n - h) return false;
        length = 0;
        for (size_t i = 0; i < count; ++i) length = (length << 8) | p[h++];
    }
    if (length > n - h) return false;
    data = p + h;
    size = length;
    return true;
}
struct Blob { uint32_t size; unsigned char* data; };
struct Algorithm { const char* oid; Blob parameters; };
struct BitBlob { uint32_t size; unsigned char* data; uint32_t unused; };
struct CertInfo {
    uint32_t version;
    Blob serial;
    Algorithm signature;
    Blob issuer;
    uint64_t notBefore, notAfter;
    Blob subject;
    Algorithm publicAlgorithm;
    BitBlob publicKey;
    BitBlob issuerUnique, subjectUnique;
    uint32_t extensionCount;
    void* extensions;
};
static_assert(offsetof(CertInfo, issuer) == 48);
static_assert(offsetof(CertInfo, publicKey) == 120);
struct CertContext { uint32_t encoding; unsigned char* encoded; uint32_t size; CertInfo* info; void* store; };
static_assert(offsetof(CertContext, info) == 24);
struct SignedFile {
    std::unique_ptr<PKCS7, decltype(&PKCS7_free)> p7{nullptr, PKCS7_free};
    X509* signer = nullptr; // borrowed from p7
    Bytes issuer, serial, publicKey, cert;
    CertInfo info{};
    CertContext context{};
};
std::mutex storeMutex;
// Construct after OpenSSL registers its cleanup so certificates are freed first.
std::vector<std::unique_ptr<SignedFile>>& stores() {
    OPENSSL_init_crypto(0, nullptr);
    static std::vector<std::unique_ptr<SignedFile>> objects;
    return objects;
}

bool verifyPeDigest(const PeFile& pe, PKCS7* p7) {
    auto* indirect = p7->d.sign->contents;
    if (!indirect || !indirect->d.other || indirect->d.other->type != V_ASN1_SEQUENCE) return false;
    char oid[80]{};
    OBJ_obj2txt(oid, sizeof oid, indirect->type, 1);
    if (std::strcmp(oid, "1.3.6.1.4.1.311.2.1.4") != 0) return false;
    const auto* sequence = indirect->d.other->value.sequence;
    const unsigned char *body = nullptr, *first = nullptr;
    size_t bodySize = 0, firstSize = 0;
    if (!payload(sequence->data, sequence->length, body, bodySize) || !payload(body, bodySize, first, firstSize)) return false;
    const size_t firstTotal = size_t(first - body) + firstSize;
    if (firstTotal >= bodySize) return false;
    const unsigned char* cursor = body + firstTotal;
    std::unique_ptr<X509_SIG, decltype(&X509_SIG_free)> digestInfo(d2i_X509_SIG(nullptr, &cursor, bodySize - firstTotal), X509_SIG_free);
    if (!digestInfo) return false;
    const X509_ALGOR* algorithm = nullptr;
    const ASN1_OCTET_STRING* expected = nullptr;
    X509_SIG_get0(digestInfo.get(), &algorithm, &expected);
    const EVP_MD* md = EVP_get_digestbyobj(algorithm->algorithm);
    if (!md) return false;
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> hash(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!hash || EVP_DigestInit_ex(hash.get(), md, nullptr) != 1) return false;
    auto add = [&](size_t start, size_t end) {
        return start <= end && end <= pe.bytes.size() && EVP_DigestUpdate(hash.get(), pe.bytes.data() + start, end - start) == 1;
    };
    if (!add(0, pe.opt + 64) || !add(pe.opt + 68, pe.opt + 144) || !add(pe.opt + 152, pe.headers)) return false;
    std::vector<std::pair<size_t,size_t>> sections;
    for (unsigned i = 0; i < pe.sectionCount; ++i) {
        const size_t s = pe.section + i * 40, len = read<uint32_t>(pe.bytes, s + 16), off = read<uint32_t>(pe.bytes, s + 20);
        if (len) sections.emplace_back(off, len);
    }
    std::sort(sections.begin(), sections.end());
    size_t hashed = pe.headers;
    for (const auto& [offset, len] : sections) {
        // Reject overlapping/gapped section layouts rather than guessing which
        // bytes a nonstandard image's Windows verifier would authenticate.
        if (offset != hashed || len > pe.bytes.size() - std::min(offset,pe.bytes.size()) || !add(offset, offset + len)) return false;
        hashed += len;
    }
    if (pe.certificate < hashed || pe.certificate > pe.bytes.size() || pe.certificateSize > pe.bytes.size() - pe.certificate) return false;
    if (!add(hashed, pe.certificate) || !add(size_t(pe.certificate) + pe.certificateSize, pe.bytes.size())) return false;
    std::array<unsigned char, EVP_MAX_MD_SIZE> actual{};
    unsigned actualSize = 0;
    return EVP_DigestFinal_ex(hash.get(), actual.data(), &actualSize) == 1 && actualSize == unsigned(expected->length) &&
           CRYPTO_memcmp(actual.data(), expected->data, actualSize) == 0;
}
std::unique_ptr<SignedFile> signedFile(const char16_t* path) {
    PeFile pe;
    if (!path || !pe.load(path) || pe.certificateSize < 8 || pe.certificate > pe.bytes.size() || pe.certificateSize > pe.bytes.size() - pe.certificate) return {};
    const size_t certLength = read<uint32_t>(pe.bytes, pe.certificate);
    if (certLength < 8 || certLength > pe.certificateSize || read<uint16_t>(pe.bytes, pe.certificate + 6) != 2) return {};
    const unsigned char* cursor = pe.bytes.data() + pe.certificate + 8;
    auto result = std::make_unique<SignedFile>();
    result->p7.reset(d2i_PKCS7(nullptr, &cursor, certLength - 8));
    auto* p7 = result->p7.get();
    if (!p7 || !PKCS7_type_is_signed(p7) || !p7->d.sign || sk_PKCS7_SIGNER_INFO_num(p7->d.sign->signer_info) != 1 || !verifyPeDigest(pe,p7)) return {};
    auto* si = sk_PKCS7_SIGNER_INFO_value(p7->d.sign->signer_info, 0);
    result->signer = PKCS7_cert_from_signer_info(p7, si);
    const auto* sequence = p7->d.sign->contents->d.other->value.sequence;
    const unsigned char* body = nullptr;
    size_t bodySize = 0;
    if (!result->signer || !payload(sequence->data, sequence->length, body, bodySize)) return {};
    const EVP_MD* md = EVP_get_digestbyobj(si->digest_alg->algorithm);
    if (!md) return {};
    BIO* hash = BIO_new(BIO_f_md());
    BIO* memory = BIO_new_mem_buf(body, int(bodySize));
    if (!hash || !memory) { BIO_free(hash); BIO_free(memory); return {}; }
    BIO_set_md(hash, md);
    BIO_push(hash, memory);
    std::array<char,4096> discard;
    while (BIO_read(hash,discard.data(),discard.size()) > 0) {}
    const int verified = PKCS7_signatureVerify(hash,p7,si,result->signer);
    BIO_free_all(hash);
    if (verified != 1) return {};
    unsigned char* encoded = nullptr;
    int n = i2d_X509_NAME(X509_get_issuer_name(result->signer), &encoded);
    if (n <= 0) return {};
    result->issuer.assign(encoded, encoded + n); OPENSSL_free(encoded);
    const ASN1_INTEGER* serial = X509_get0_serialNumber(result->signer);
    result->serial.assign(serial->data, serial->data + serial->length);
    std::reverse(result->serial.begin(),result->serial.end()); // CRYPT_INTEGER_BLOB is little endian.
    X509_PUBKEY* spki = X509_get_X509_PUBKEY(result->signer);
    const unsigned char* key = nullptr;
    int keySize = 0;
    if (!X509_PUBKEY_get0_param(nullptr,&key,&keySize,nullptr,spki) || keySize <= 0) return {};
    result->publicKey.assign(key,key + keySize);
    encoded = nullptr;
    n = i2d_X509(result->signer,&encoded);
    if (n <= 0) return {};
    result->cert.assign(encoded, encoded + n); OPENSSL_free(encoded);
    result->info.version = uint32_t(X509_get_version(result->signer));
    result->info.serial = {uint32_t(result->serial.size()),result->serial.data()};
    result->info.issuer = {uint32_t(result->issuer.size()),result->issuer.data()};
    result->info.publicKey = {uint32_t(result->publicKey.size()),result->publicKey.data(),0};
    result->context = {0x10001,result->cert.data(),uint32_t(result->cert.size()),&result->info,result.get()};
    return result;
}

MS int32_t winVerifyTrust(void*, const void* action, void* data) {
    static constexpr unsigned char genericVerifyV2[] = {0x6b,0xc5,0xaa,0x00,0x44,0xcd,0xd0,0x11,0x8c,0xc2,0x00,0xc0,0x4f,0xc2,0x95,0xee};
    if (!action || std::memcmp(action,genericVerifyV2,sizeof genericVerifyV2)) return int32_t(0x800b0002u);
    if (!data) return int32_t(0x80070057u);
    const auto* b = static_cast<const unsigned char*>(data);
    auto get32 = [&](size_t o) { uint32_t v; std::memcpy(&v,b+o,4); return v; };
    if (get32(0) < 80) return int32_t(0x80070057u);
    if (get32(48) == 2) return 0; // No retained trust state to close.
    if (get32(32) != 1) return int32_t(0x800b0001u);
    const unsigned char* file = nullptr;
    std::memcpy(&file,b+40,sizeof file);
    const char16_t* path = nullptr;
    if (file) std::memcpy(&path,file+8,sizeof path);
    // This verifies the signed PE bytes and the signing key. Windows root-store,
    // revocation and timestamp policies are deliberately not claimed here. The
    // caller additionally pins AMD's complete RSA public key via Crypt32.
    const int32_t result = signedFile(path) ? 0 : int32_t(0x80096010u);
    if (std::getenv("FFXVK_DEBUG")) std::fprintf(stderr,"ffx-vk-linux: WinVerifyTrust(%s): %08x (PE digest and signer signature)\n",pathOf(path).c_str(),uint32_t(result));
    return result;
}
MS int cryptQueryObject(uint32_t type, const void* path, uint32_t contentFlags, uint32_t formatFlags, uint32_t,
                       uint32_t* encoding, uint32_t* content, uint32_t* format, void** store, void** message, const void**) {
    if (type != 1 || !(contentFlags & 0x400) || !(formatFlags & 2)) return 0;
    auto object = signedFile(static_cast<const char16_t*>(path));
    if (!object) return 0;
    auto* ptr = object.get();
    { std::lock_guard lock(storeMutex); stores().push_back(std::move(object)); }
    if (encoding) *encoding = 0x10001;
    if (content) *content = 10;
    if (format) *format = 1;
    if (store) *store = ptr;
    if (message) *message = ptr;
    return 1;
}
SignedFile* storeOf(void* h) {
    for (auto& s : stores()) if (s.get() == h) return s.get();
    return nullptr;
}
struct SignerInfo { uint32_t version; Blob issuer, serial; Algorithm digest, encryption; Blob encryptedHash; uint32_t authCount; void* auth; uint32_t unauthCount; void* unauth; };
MS int cryptMsgGetParam(void* message, uint32_t parameter, uint32_t index, void* out, uint32_t* size) {
    if (parameter != 6 || index || !size) return 0;
    std::lock_guard lock(storeMutex);
    auto* s = storeOf(message);
    if (!s) return 0;
    const uint32_t need = sizeof(SignerInfo);
    const uint32_t capacity = *size;
    *size = need;
    if (!out) return 1;
    if (capacity < need) { lastError() = 234; return 0; }
    SignerInfo info{};
    info.version = 1;
    info.issuer = s->info.issuer;
    info.serial = s->info.serial;
    std::memcpy(out,&info,sizeof info);
    return 1;
}
MS const CertContext* certFindCertificateInStore(void* store, uint32_t, uint32_t, uint32_t findType, const CertInfo* wanted, const void*) {
    if (findType != 0xb0000 || !wanted) return nullptr;
    std::lock_guard lock(storeMutex);
    auto* s = storeOf(store);
    if (!s || wanted->issuer.size != s->issuer.size() || wanted->serial.size != s->serial.size() ||
        !wanted->issuer.data || !wanted->serial.data || std::memcmp(wanted->issuer.data,s->issuer.data(),s->issuer.size()) ||
        std::memcmp(wanted->serial.data,s->serial.data(),s->serial.size())) return nullptr;
    return &s->context;
}
MS int cryptDecodeObjectEx(uint32_t, const char* type, const unsigned char* encoded, uint32_t encodedSize,
                          uint32_t flags, const void* allocation, void* out, uint32_t* size) {
    if (uintptr_t(type) != 0x48 || !encoded || !encodedSize || !size || allocation) return 0;
    const unsigned char* p = encoded;
    // RSAPublicKey is the DER SEQUENCE inside SubjectPublicKeyInfo's BIT STRING.
    const unsigned char* sequence = nullptr;
    size_t sequenceSize = 0;
    if (encoded[0] != 0x30 || !payload(encoded,encodedSize,sequence,sequenceSize)) return 0;
    p = sequence;
    std::unique_ptr<ASN1_INTEGER,decltype(&ASN1_INTEGER_free)> modulus(d2i_ASN1_INTEGER(nullptr,&p,sequenceSize),ASN1_INTEGER_free);
    if (!modulus || p > sequence + sequenceSize) return 0;
    std::unique_ptr<ASN1_INTEGER,decltype(&ASN1_INTEGER_free)> exponent(d2i_ASN1_INTEGER(nullptr,&p,sequence + sequenceSize - p),ASN1_INTEGER_free);
    if (!exponent || p != sequence + sequenceSize) return 0;
    std::unique_ptr<BIGNUM,decltype(&BN_free)> n(ASN1_INTEGER_to_BN(modulus.get(),nullptr),BN_free), e(ASN1_INTEGER_to_BN(exponent.get(),nullptr),BN_free);
    if (!n || !e || BN_is_negative(n.get()) || BN_is_negative(e.get()) || BN_is_zero(n.get()) || BN_is_zero(e.get())) return 0;
    const uint32_t modulusSize = BN_num_bytes(n.get()), exponentSize = BN_num_bytes(e.get()), need = 24 + modulusSize + exponentSize;
    const uint32_t capacity = *size;
    *size = need;
    if (!out) return 1;
    unsigned char* dest;
    if (flags & 0x8000) {
        dest = static_cast<unsigned char*>(std::malloc(need));
        if (!dest) return 0;
        *static_cast<void**>(out) = dest;
    } else {
        if (capacity < need) { lastError() = 234; return 0; }
        dest = static_cast<unsigned char*>(out);
    }
    const uint32_t header[] = {0x31415352,uint32_t(BN_num_bits(n.get())),exponentSize,modulusSize,0,0};
    std::memcpy(dest,header,sizeof header);
    BN_bn2bin(e.get(),dest+24);
    BN_bn2bin(n.get(),dest+24+exponentSize);
    return 1;
}
MS uint32_t versionSize(const char16_t* path, uint32_t* handle) {
    if (handle) *handle = 0;
    PeFile file;
    return file.load(path) ? uint32_t(file.version().size()) : 0;
}
MS int versionInfo(const char16_t* path, uint32_t, uint32_t size, void* out) {
    PeFile file;
    if (!out || !file.load(path)) return 0;
    const auto version = file.version();
    if (version.empty() || size < version.size()) return 0;
    std::memcpy(out,version.data(),version.size());
    return 1;
}
MS int versionValue(const void* block, const char16_t* query, void** out, uint32_t* size) {
    if (!block || !query || query[0] != u'\\' || query[1] != 0 || !out || !size) return 0;
    const auto* b = static_cast<const unsigned char*>(block);
    uint16_t length, valueLength;
    std::memcpy(&length,b,2); std::memcpy(&valueLength,b+2,2);
    constexpr char16_t key[] = u"VS_VERSION_INFO";
    constexpr size_t valueOffset = (6 + sizeof key + 3) & ~size_t(3);
    if (length < valueOffset + 52 || valueLength != 52 || std::memcmp(b+6,key,sizeof key)) return 0;
    uint32_t signature;
    std::memcpy(&signature,b+valueOffset,4);
    if (signature != 0xfeef04bd) return 0;
    *out = const_cast<unsigned char*>(b+valueOffset);
    *size = valueLength;
    return 1;
}
} // namespace
void* securityStandIn(const std::string& dll, const std::string& name) {
    if (dll == "wintrust.dll" && name == "WinVerifyTrust") return reinterpret_cast<void*>(&winVerifyTrust);
    if (dll == "crypt32.dll") {
        if (name == "CryptQueryObject") return reinterpret_cast<void*>(&cryptQueryObject);
        if (name == "CryptMsgGetParam") return reinterpret_cast<void*>(&cryptMsgGetParam);
        if (name == "CertFindCertificateInStore") return reinterpret_cast<void*>(&certFindCertificateInStore);
        if (name == "CryptDecodeObjectEx") return reinterpret_cast<void*>(&cryptDecodeObjectEx);
    }
    if (dll == "version.dll") {
        if (name == "GetFileVersionInfoSizeW") return reinterpret_cast<void*>(&versionSize);
        if (name == "GetFileVersionInfoW") return reinterpret_cast<void*>(&versionInfo);
        if (name == "VerQueryValueW") return reinterpret_cast<void*>(&versionValue);
    }
    return nullptr;
}
} // namespace ffxvk
