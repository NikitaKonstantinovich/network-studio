#pragma once
#include <string>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <stdexcept>
namespace studio::archive_codec {
inline void check(bool ok,const char* message){if(!ok)throw std::runtime_error(std::string("History archive: ")+message);}
inline uint64_t hash(const std::string& s){uint64_t h=14695981039346656037ull;for(unsigned char c:s){h^=c;h*=1099511628211ull;}return h;}
struct Writer {
    std::string bytes;
    void u(uint64_t v){for(int i=0;i<8;++i)bytes.push_back(char(v>>(i*8)));}
    void d(double v){uint64_t b;std::memcpy(&b,&v,8);u(b);}
    void text(const std::string& s){u(s.size());bytes+=s;}
    std::string sealed(){u(hash(bytes));return std::move(bytes);}
};
struct Reader {
    const std::string& bytes;size_t pos=0;
    uint64_t u(){check(bytes.size()-pos>=8,"truncated record");uint64_t n=0;for(int i=0;i<8;++i)n|=uint64_t((unsigned char)bytes[pos++])<<(i*8);return n;}
    double d(){auto b=u();double v;std::memcpy(&v,&b,8);check(std::isfinite(v),"non-finite value");return v;}
    std::string text(size_t max=64*1024*1024){auto n=u();check(n<=max&&n<=bytes.size()-pos,"invalid record length");auto s=bytes.substr(pos,size_t(n));pos+=size_t(n);return s;}
    void end(){check(pos==bytes.size(),"unexpected trailing bytes");}
};
inline std::string read(const std::filesystem::path& p,size_t max=64*1024*1024+1024){std::ifstream f(p,std::ios::binary|std::ios::ate);check(bool(f),"cannot open file");auto n=f.tellg();check(n>=0&&uint64_t(n)<=max,"file size exceeds limit");std::string b(size_t(n),'\0');f.seekg(0);f.read(b.data(),n);check(bool(f),"read failed");return b;}
inline std::string verified(const std::filesystem::path& p,size_t max=64*1024*1024+1024){auto b=read(p,max);check(b.size()>=8,"truncated block");Reader r{b,b.size()-8};auto expected=r.u();b.resize(b.size()-8);check(hash(b)==expected,"checksum mismatch");return b;}
}
