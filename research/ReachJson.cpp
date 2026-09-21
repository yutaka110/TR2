#include "ReachJson.h"
#include <charconv>
#include <cmath>
#include <stdexcept>
#include <string_view>

namespace reach {
namespace {
class Reader {
    const std::string& text;
    size_t pos = 0;
    [[noreturn]] void Fail(const char* reason) const {
        throw std::runtime_error(std::string("JSON byte ") + std::to_string(pos) + ": " + reason);
    }
    void Space() { while (pos < text.size() && (text[pos]==' ' || text[pos]=='\t' || text[pos]=='\r' || text[pos]=='\n')) ++pos; }
    bool Eat(char ch) { Space(); if (pos < text.size() && text[pos] == ch) { ++pos; return true; } return false; }
    void Need(char ch) { if (!Eat(ch)) Fail("unexpected character"); }
    unsigned Hex() {
        unsigned value = 0;
        for (int i=0;i<4;++i) {
            if (pos == text.size()) Fail("truncated unicode escape");
            const char ch=text[pos++];
            const int n=ch>='0'&&ch<='9'?ch-'0':ch>='a'&&ch<='f'?ch-'a'+10:ch>='A'&&ch<='F'?ch-'A'+10:-1;
            if (n<0) Fail("invalid unicode escape");
            value=value*16+n;
        }
        return value;
    }
    void Utf8(std::string& result, unsigned n) {
        if (n<0x80) result+=static_cast<char>(n);
        else if(n<0x800) { result+=static_cast<char>(0xc0|(n>>6)); result+=static_cast<char>(0x80|(n&63)); }
        else if(n<0x10000) { result+=static_cast<char>(0xe0|(n>>12)); result+=static_cast<char>(0x80|((n>>6)&63)); result+=static_cast<char>(0x80|(n&63)); }
        else { result+=static_cast<char>(0xf0|(n>>18)); result+=static_cast<char>(0x80|((n>>12)&63)); result+=static_cast<char>(0x80|((n>>6)&63)); result+=static_cast<char>(0x80|(n&63)); }
    }
    std::string String() {
        Need('"'); std::string result;
        while(pos < text.size()) {
            unsigned char ch=static_cast<unsigned char>(text[pos++]);
            if(ch=='"') return result;
            if(ch<32) Fail("unescaped control character");
            if(ch=='\\') {
                if(pos==text.size()) Fail("truncated escape");
                switch(text[pos++]) {
                case '"': result+='"'; break; case '\\': result+='\\'; break; case '/': result+='/'; break;
                case 'b': result+='\b'; break; case 'f': result+='\f'; break; case 'n': result+='\n'; break;
                case 'r': result+='\r'; break; case 't': result+='\t'; break;
                case 'u': {
                    unsigned n=Hex();
                    if(n>=0xd800 && n<=0xdbff) {
                        if(text.compare(pos,2,"\\u")!=0) Fail("missing low surrogate");
                        pos+=2; const unsigned low=Hex();
                        if(low<0xdc00||low>0xdfff) Fail("invalid low surrogate");
                        n=0x10000+(n-0xd800)*1024+low-0xdc00;
                    } else if(n>=0xdc00&&n<=0xdfff) Fail("unpaired low surrogate");
                    Utf8(result,n); break;
                }
                default: Fail("invalid escape");
                }
            } else if (ch<128) result+=static_cast<char>(ch);
            else {
                // Validate raw UTF-8 too; malformed paths must not be silently rewritten.
                int count=ch>=0xc2&&ch<=0xdf?1:ch>=0xe0&&ch<=0xef?2:ch>=0xf0&&ch<=0xf4?3:-1;
                if(count<0) Fail("invalid UTF-8");
                unsigned n=ch&((1u<<(6-count))-1);
                for(int i=0;i<count;++i) {
                    if(pos==text.size()) Fail("truncated UTF-8");
                    unsigned char tail=static_cast<unsigned char>(text[pos++]);
                    if((tail&0xc0)!=0x80) Fail("invalid UTF-8 continuation");
                    n=(n<<6)|(tail&63);
                }
                if(n<(count==1?0x80u:count==2?0x800u:0x10000u)||n>0x10ffff||(n>=0xd800&&n<=0xdfff)) Fail("invalid UTF-8 code point");
                Utf8(result,n);
            }
        }
        Fail("unterminated string");
    }
    Json Value(unsigned depth) {
        if(depth>16) Fail("nesting limit exceeded");
        Space(); if(pos==text.size()) Fail("missing value");
        if(text[pos]=='"') return Json{String()};
        if(Eat('{')) {
            Json::Object obj;
            if(Eat('}')) return Json{obj};
            do {
                const auto key=String(); Need(':'); auto value=Value(depth+1);
                if(!obj.emplace(key,std::move(value)).second) Fail("duplicate member");
                if(Eat('}')) return Json{obj};
                Need(',');
            } while(true);
        }
        if(Eat('[')) {
            Json::Array array;
            if(Eat(']')) return Json{array};
            do { array.push_back(Value(depth+1)); if(Eat(']')) return Json{array}; Need(','); } while(true);
        }
        for(const auto literal : {"true","false","null"}) {
            const std::string_view token(literal);
            if(text.compare(pos,token.size(),token)==0) {
                pos+=token.size();
                if(token=="null") return Json{};
                return Json{token=="true"};
            }
        }
        const size_t begin=pos;
        if(text[pos]=='-') ++pos;
        auto digit=[&](){return pos<text.size()&&text[pos]>='0'&&text[pos]<='9';};
        if(!digit()) Fail("expected number");
        if(text[pos]=='0') ++pos; else while(digit()) ++pos;
        if(pos<text.size()&&text[pos]=='.') { ++pos; if(!digit()) Fail("missing fraction"); while(digit()) ++pos; }
        if(pos<text.size()&&(text[pos]=='e'||text[pos]=='E')) {
            ++pos; if(pos<text.size()&&(text[pos]=='+'||text[pos]=='-')) ++pos;
            if(!digit()) Fail("missing exponent"); while(digit()) ++pos;
        }
        double number=0;
        const auto parsed=std::from_chars(text.data()+begin,text.data()+pos,number);
        if(parsed.ec!=std::errc{}||parsed.ptr!=text.data()+pos||!std::isfinite(number)) Fail("invalid finite number");
        return Json{number};
    }
public:
    explicit Reader(const std::string& s):text(s) {}
    Json Read() {
        if(text.size()>65536) Fail("configuration exceeds 64 KiB");
        if(text.compare(0,3,"\xef\xbb\xbf")==0) pos=3;
        auto value=Value(0); Space(); if(pos!=text.size()) Fail("trailing data"); return value;
    }
};
}
const Json::Object& Json::ObjectValue() const {
    if(const auto p=std::get_if<Object>(&value)) return *p;
    throw std::runtime_error("expected JSON object");
}
const Json& Json::At(const std::string& key) const {
    const auto& object=ObjectValue(); const auto found=object.find(key);
    if(found==object.end()) throw std::runtime_error("missing config key: "+key);
    return found->second;
}
std::string Json::StringValue() const {
    if(const auto p=std::get_if<std::string>(&value)) return *p;
    throw std::runtime_error("expected JSON string");
}
double Json::NumberValue() const {
    if(const auto p=std::get_if<double>(&value)) return *p;
    throw std::runtime_error("expected JSON number");
}
Json ParseJson(const std::string& text) { return Reader(text).Read(); }
std::string JsonString(const std::string& text) {
    constexpr char hex[]="0123456789abcdef";
    std::string result="\"";
    for(unsigned char ch:text) {
        if(ch=='"'||ch=='\\') { result+='\\'; result+=static_cast<char>(ch); }
        else if(ch<32) { result+="\\u00"; result+=hex[ch>>4]; result+=hex[ch&15]; }
        else result+=static_cast<char>(ch);
    }
    return result+'"';
}
}
