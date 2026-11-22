#include "PrivateServerCert.hpp"
#include "Misc.hpp"
#include "Log.h"
#include <jni.h>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>

namespace HoshimiLocal::PrivateServer {
namespace {
bool IsWhitespace(char c) { return c==' '||c=='\t'||c=='\r'||c=='\n'; }
int Base64Value(char c) {
    if (c>='A'&&c<='Z') return c-'A';
    if (c>='a'&&c<='z') return c-'a'+26;
    if (c>='0'&&c<='9') return c-'0'+52;
    if (c=='+') return 62;
    if (c=='/') return 63;
    return -1;
}
std::vector<uint8_t> DecodeBase64(const std::string& input, bool& ok) {
    ok=true; std::string s;
    for (char c:input) if (!IsWhitespace(c)) s.push_back(c);
    if (s.empty()||s.size()%4!=0) { ok=false; return {}; }
    std::vector<uint8_t> out; out.reserve(s.size()/4*3);
    for (size_t i=0;i<s.size();i+=4) {
        int v[4]={0,0,0,0};
        for (int j=0;j<4;++j) {
            char c=s[i+j];
            if (c=='=') v[j]=0;
            else { int x=Base64Value(c); if (x<0){ok=false;return{};} v[j]=x; }
        }
        uint32_t t=(v[0]<<18)|(v[1]<<12)|(v[2]<<6)|v[3];
        out.push_back((t>>16)&0xFF);
        if (s[i+2]!='=') out.push_back((t>>8)&0xFF);
        if (s[i+3]!='=') out.push_back(t&0xFF);
    }
    if (out.empty()) { ok=false; return {}; }
    return out;
}
std::vector<std::vector<uint8_t>> ParsePemCertificates(const std::string& pem) {
    std::vector<std::vector<uint8_t>> result;
    const std::string b="-----BEGIN CERTIFICATE-----", e="-----END CERTIFICATE-----";
    size_t p=0;
    while (true) {
        size_t i=pem.find(b,p); if (i==std::string::npos) break;
        size_t j=pem.find(e,i); if (j==std::string::npos) break;
        std::string body=pem.substr(i+b.size(), j-(i+b.size()));
        bool ok=false; auto der=DecodeBase64(body,ok);
        if (ok&&!der.empty()) result.push_back(std::move(der));
        p=j+e.size();
    }
    return result;
}
bool CheckException(JNIEnv* env,bool describe) {
    if (env->ExceptionCheck()) { if (describe) env->ExceptionDescribe(); env->ExceptionClear(); return true; }
    return false;
}
jobject ParseCertificate(JNIEnv* env, const std::vector<uint8_t>& der, jobject factory,
                        jmethodID genCert, jclass baisClass, jmethodID baisCtor,
                        const char* failure) {
    jbyteArray arr=env->NewByteArray((jsize)der.size());
    if (arr==nullptr || CheckException(env,true)) { HoshimiLocal::Log::ErrorFmt(failure); return nullptr; }
    env->SetByteArrayRegion(arr,0,(jsize)der.size(),reinterpret_cast<const jbyte*>(der.data()));
    if (CheckException(env,true)) { env->DeleteLocalRef(arr); HoshimiLocal::Log::ErrorFmt(failure); return nullptr; }
    jobject stream=env->NewObject(baisClass,baisCtor,arr);
    if (stream==nullptr || CheckException(env,true)) { env->DeleteLocalRef(arr); HoshimiLocal::Log::ErrorFmt(failure); return nullptr; }
    jobject cert=env->CallObjectMethod(factory,genCert,stream);
    bool exc=CheckException(env,true);
    env->DeleteLocalRef(stream); env->DeleteLocalRef(arr);
    if (exc || cert==nullptr) { if (cert) env->DeleteLocalRef(cert); HoshimiLocal::Log::ErrorFmt(failure); return nullptr; }
    return cert;
}
} // namespace
static std::mutex g_mutex;
static std::string g_cachedPem;
static std::vector<std::vector<uint8_t>> g_cachedCaDer;
bool VerifyChainAgainstPem(const unsigned char* leafDer, std::size_t leafDerLen, const std::string& caPem) {
    if (leafDer==nullptr || leafDerLen==0) { HoshimiLocal::Log::ErrorFmt("PrivateServerCert: null or empty leaf certificate"); return false; }
    std::vector<std::vector<uint8_t>> caDers;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_cachedPem!=caPem) { g_cachedPem=caPem; g_cachedCaDer=ParsePemCertificates(caPem); HoshimiLocal::Log::InfoFmt("PrivateServerCert: parsed %zu CA cert(s)",g_cachedCaDer.size()); }
        caDers=g_cachedCaDer;
    }
    if (caDers.empty()) return false;
    JNIEnv* env=HoshimiLocal::Misc::GetJNIEnv();
    if (env==nullptr) { HoshimiLocal::Log::ErrorFmt("PrivateServerCert: GetJNIEnv returned null"); return false; }
    jclass cfClass=nullptr,baisClass=nullptr,x509Class=nullptr,x500Class=nullptr;
    jstring x509Str=nullptr; jobject factory=nullptr,leafCert=nullptr,leafIssuer=nullptr;
    jmethodID getInstance=nullptr,genCert=nullptr,baisCtor=nullptr,checkValidity=nullptr,verify=nullptr,getPublicKey=nullptr,getIssuer=nullptr,getSubject=nullptr,equals=nullptr;
    bool result=false;
    cfClass=env->FindClass("java/security/cert/CertificateFactory");
    if (cfClass==nullptr || CheckException(env,true)) { HoshimiLocal::Log::ErrorFmt("PrivateServerCert: CertificateFactory class not found"); goto cleanup; }
    getInstance=env->GetStaticMethodID(cfClass,"getInstance","(Ljava/lang/String;)Ljava/security/cert/CertificateFactory;");
    if (getInstance==nullptr || CheckException(env,true)) { HoshimiLocal::Log::ErrorFmt("PrivateServerCert: getInstance not found"); goto cleanup; }
    genCert=env->GetMethodID(cfClass,"generateCertificate","(Ljava/io/InputStream;)Ljava/security/cert/Certificate;");
    if (genCert==nullptr || CheckException(env,true)) { HoshimiLocal::Log::ErrorFmt("PrivateServerCert: generateCertificate not found"); goto cleanup; }
    baisClass=env->FindClass("java/io/ByteArrayInputStream");
    if (baisClass==nullptr || CheckException(env,true)) { HoshimiLocal::Log::ErrorFmt("PrivateServerCert: ByteArrayInputStream not found"); goto cleanup; }
    baisCtor=env->GetMethodID(baisClass,"<init>","([B)V");
    if (baisCtor==nullptr || CheckException(env,true)) { HoshimiLocal::Log::ErrorFmt("PrivateServerCert: ByteArrayInputStream ctor not found"); goto cleanup; }
    x509Class=env->FindClass("java/security/cert/X509Certificate");
    if (x509Class==nullptr || CheckException(env,true)) { HoshimiLocal::Log::ErrorFmt("PrivateServerCert: X509Certificate not found"); goto cleanup; }
    checkValidity=env->GetMethodID(x509Class,"checkValidity","()V");
    if (checkValidity==nullptr || CheckException(env,true)) { HoshimiLocal::Log::ErrorFmt("PrivateServerCert: checkValidity not found"); goto cleanup; }
    verify=env->GetMethodID(x509Class,"verify","(Ljava/security/PublicKey;)V");
    if (verify==nullptr || CheckException(env,true)) { HoshimiLocal::Log::ErrorFmt("PrivateServerCert: verify not found"); goto cleanup; }
    getPublicKey=env->GetMethodID(x509Class,"getPublicKey","()Ljava/security/PublicKey;");
    if (getPublicKey==nullptr || CheckException(env,true)) { HoshimiLocal::Log::ErrorFmt("PrivateServerCert: getPublicKey not found"); goto cleanup; }
    getIssuer=env->GetMethodID(x509Class,"getIssuerX500Principal","()Ljavax/security/auth/x500/X500Principal;");
    if (getIssuer==nullptr || CheckException(env,true)) { HoshimiLocal::Log::ErrorFmt("PrivateServerCert: getIssuer not found"); goto cleanup; }
    getSubject=env->GetMethodID(x509Class,"getSubjectX500Principal","()Ljavax/security/auth/x500/X500Principal;");
    if (getSubject==nullptr || CheckException(env,true)) { HoshimiLocal::Log::ErrorFmt("PrivateServerCert: getSubject not found"); goto cleanup; }
    x500Class=env->FindClass("javax/security/auth/x500/X500Principal");
    if (x500Class==nullptr || CheckException(env,true)) { HoshimiLocal::Log::ErrorFmt("PrivateServerCert: X500Principal not found"); goto cleanup; }
    equals=env->GetMethodID(x500Class,"equals","(Ljava/lang/Object;)Z");
    if (equals==nullptr || CheckException(env,true)) { HoshimiLocal::Log::ErrorFmt("PrivateServerCert: equals not found"); goto cleanup; }
    x509Str=env->NewStringUTF("X.509");
    if (x509Str==nullptr || CheckException(env,true)) { HoshimiLocal::Log::ErrorFmt("PrivateServerCert: failed to create X.509 string"); goto cleanup; }
    factory=env->CallStaticObjectMethod(cfClass,getInstance,x509Str);
    if (factory==nullptr || CheckException(env,true)) { HoshimiLocal::Log::ErrorFmt("PrivateServerCert: CertificateFactory.getInstance failed"); goto cleanup; }
    {
        std::vector<uint8_t> der(leafDer, leafDer+leafDerLen);
        leafCert=ParseCertificate(env,der,factory,genCert,baisClass,baisCtor,"PrivateServerCert: failed to parse leaf certificate");
    }
    if (leafCert==nullptr) goto cleanup;
    if (!env->IsInstanceOf(leafCert,x509Class)) { HoshimiLocal::Log::ErrorFmt("PrivateServerCert: leaf is not an X509Certificate"); goto cleanup; }
    env->CallVoidMethod(leafCert,checkValidity);
    if (CheckException(env,true)) { HoshimiLocal::Log::ErrorFmt("PrivateServerCert: leaf certificate is not currently valid"); goto cleanup; }
    leafIssuer=env->CallObjectMethod(leafCert,getIssuer);
    if (leafIssuer==nullptr || CheckException(env,true)) { HoshimiLocal::Log::ErrorFmt("PrivateServerCert: failed to get leaf issuer"); goto cleanup; }
    for (const auto& caDer : caDers) {
        jobject caCert=ParseCertificate(env,caDer,factory,genCert,baisClass,baisCtor,"PrivateServerCert: failed to parse CA certificate");
        if (caCert==nullptr) goto cleanup;
        if (!env->IsInstanceOf(caCert,x509Class)) { env->DeleteLocalRef(caCert); HoshimiLocal::Log::ErrorFmt("PrivateServerCert: CA is not an X509Certificate"); goto cleanup; }
        jobject caSubject=env->CallObjectMethod(caCert,getSubject);
        if (caSubject==nullptr || CheckException(env,true)) { if (caSubject) env->DeleteLocalRef(caSubject); env->DeleteLocalRef(caCert); HoshimiLocal::Log::ErrorFmt("PrivateServerCert: failed to get CA subject"); goto cleanup; }
        const jboolean issuerMatches=env->CallBooleanMethod(leafIssuer,equals,caSubject);
        if (CheckException(env,true)) { env->DeleteLocalRef(caSubject); env->DeleteLocalRef(caCert); HoshimiLocal::Log::ErrorFmt("PrivateServerCert: issuer comparison failed"); goto cleanup; }
        if (issuerMatches!=JNI_TRUE) { env->DeleteLocalRef(caSubject); env->DeleteLocalRef(caCert); continue; }
        jobject caPublicKey=env->CallObjectMethod(caCert,getPublicKey);
        if (caPublicKey==nullptr || CheckException(env,true)) { env->DeleteLocalRef(caSubject); env->DeleteLocalRef(caCert); HoshimiLocal::Log::ErrorFmt("PrivateServerCert: failed to get CA public key"); goto cleanup; }
        env->CallVoidMethod(leafCert,verify,caPublicKey);
        const bool verifyThrew=CheckException(env,false);
        env->DeleteLocalRef(caPublicKey); env->DeleteLocalRef(caSubject); env->DeleteLocalRef(caCert);
        if (!verifyThrew) { result=true; goto cleanup; }
    }
    if (!result) HoshimiLocal::Log::ErrorFmt("PrivateServerCert: certificate verification failed");
cleanup:
    if (leafIssuer!=nullptr) env->DeleteLocalRef(leafIssuer);
    if (leafCert!=nullptr) env->DeleteLocalRef(leafCert);
    if (factory!=nullptr) env->DeleteLocalRef(factory);
    if (x509Str!=nullptr) env->DeleteLocalRef(x509Str);
    if (x500Class!=nullptr) env->DeleteLocalRef(x500Class);
    if (x509Class!=nullptr) env->DeleteLocalRef(x509Class);
    if (baisClass!=nullptr) env->DeleteLocalRef(baisClass);
    if (cfClass!=nullptr) env->DeleteLocalRef(cfClass);
    return result;
}
} // namespace HoshimiLocal::PrivateServer