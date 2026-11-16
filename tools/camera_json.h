// Minimal strict JSON reader for recorder metadata and delivery status.
#ifndef CAMERA_JSON_H
#define CAMERA_JSON_H
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace camera {
struct Json {
    enum Type { Null, Bool, Number, String, Object, Array } type = Null;
    bool boolean = false;
    std::string text;
    std::map<std::string, Json> object;
    std::vector<Json> array;
    const Json& at(const std::string& k) const {
        if (type != Object || !object.count(k)) throw std::runtime_error("missing JSON field: " + k);
        return object.at(k);
    }
    double number() const {
        if (type != Number) throw std::runtime_error("expected JSON number");
        char *end = nullptr; double n = std::strtod(text.c_str(), &end);
        if (!end || *end || !std::isfinite(n)) throw std::runtime_error("invalid JSON number");
        return n;
    }
};
inline std::string json_quote(const std::string& s) {
    static const char *hex = "0123456789abcdef";
    std::string r = "\"";
    for (size_t i=0; i<s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '"' || c == '\\') { r += '\\'; r += char(c); }
        else if (c < 0x20) { r += "\\u00"; r += hex[c >> 4]; r += hex[c & 15]; }
        else if (c < 0x80) r += char(c);
        else {
            size_t len = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 :
                         c >= 0xf0 && c <= 0xf4 ? 4 : 0;
            bool valid = len && i + len <= s.size();
            for (size_t k=1; valid && k<len; ++k)
                valid = (static_cast<unsigned char>(s[i+k]) & 0xc0) == 0x80;
            if (valid) {
                auto next = static_cast<unsigned char>(s[i+1]);
                if ((c==0xe0 && next<0xa0) || (c==0xed && next>=0xa0) ||
                    (c==0xf0 && next<0x90) || (c==0xf4 && next>=0x90)) valid=false;
            }
            if (valid) { r.append(s,i,len); i += len-1; }
            else r += "\\ufffd";
        }
    }
    return r + "\"";
}
class JsonParser {
    const std::string &s; size_t p = 0;
    void space() { while (p < s.size() && (s[p]==' ' || s[p]=='\n' || s[p]=='\r' || s[p]=='\t')) ++p; }
    bool take(char c) { space(); if (p < s.size() && s[p] == c) { ++p; return true; } return false; }
    [[noreturn]] void bad() const { throw std::runtime_error("invalid JSON at byte " + std::to_string(p)); }
    unsigned hex4() {
        unsigned n = 0;
        for (int i=0; i<4; ++i) {
            if (p == s.size()) bad();
            char c = s[p++]; n *= 16;
            if (c >= '0' && c <= '9') n += c-'0';
            else if (c >= 'a' && c <= 'f') n += c-'a'+10;
            else if (c >= 'A' && c <= 'F') n += c-'A'+10;
            else bad();
        }
        return n;
    }
    static void utf8(std::string& out, unsigned c) {
        if (c < 0x80) out += char(c);
        else if (c < 0x800) { out += char(0xc0 | (c>>6)); out += char(0x80 | (c&63)); }
        else if (c < 0x10000) { out += char(0xe0 | (c>>12)); out += char(0x80 | ((c>>6)&63)); out += char(0x80 | (c&63)); }
        else { out += char(0xf0 | (c>>18)); out += char(0x80 | ((c>>12)&63)); out += char(0x80 | ((c>>6)&63)); out += char(0x80 | (c&63)); }
    }
    std::string str() {
        if (!take('"')) bad();
        std::string out;
        while (p < s.size()) {
            unsigned char c = s[p++];
            if (c == '"') return out;
            if (c < 0x20) bad();
            if (c != '\\') { out += char(c); continue; }
            if (p == s.size()) bad();
            c = s[p++];
            if (c=='"' || c=='\\' || c=='/') out += char(c);
            else if (c=='b') out += '\b'; else if (c=='f') out += '\f';
            else if (c=='n') out += '\n'; else if (c=='r') out += '\r'; else if (c=='t') out += '\t';
            else if (c=='u') {
                unsigned u = hex4();
                if (u >= 0xd800 && u <= 0xdbff) {
                    if (p+2 > s.size() || s[p++] != '\\' || s[p++] != 'u') bad();
                    unsigned v = hex4(); if (v < 0xdc00 || v > 0xdfff) bad();
                    u = 0x10000 + ((u-0xd800)<<10) + (v-0xdc00);
                } else if (u >= 0xdc00 && u <= 0xdfff) bad();
                utf8(out, u);
            } else bad();
        }
        bad();
    }
    Json value(unsigned depth) {
        if (depth > 64) bad();
        space(); if (p == s.size()) bad();
        Json j;
        if (s[p] == '"') { j.type=Json::String; j.text=str(); return j; }
        if (take('{')) {
            j.type=Json::Object;
            if (take('}')) return j;
            do { auto k=str(); if (!take(':')) bad(); if (!j.object.emplace(k,value(depth+1)).second) bad(); }
            while (take(','));
            if (!take('}')) bad();
            return j;
        }
        if (take('[')) {
            j.type=Json::Array;
            if (take(']')) return j;
            do { j.array.push_back(value(depth+1)); } while (take(','));
            if (!take(']')) bad();
            return j;
        }
        if (s.compare(p,4,"null")==0) { p+=4; return j; }
        if (s.compare(p,4,"true")==0) { p+=4; j.type=Json::Bool; j.boolean=true; return j; }
        if (s.compare(p,5,"false")==0) { p+=5; j.type=Json::Bool; return j; }
        size_t start=p;
        if (s[p]=='-') ++p;
        if (p==s.size()) bad();
        if (s[p]=='0') ++p;
        else { if (s[p]<'1'||s[p]>'9') bad(); while (p<s.size() && std::isdigit((unsigned char)s[p])) ++p; }
        if (p<s.size() && s[p]=='.') { ++p; size_t q=p; while(p<s.size()&&std::isdigit((unsigned char)s[p]))++p; if(p==q)bad(); }
        if (p<s.size() && (s[p]=='e'||s[p]=='E')) { ++p; if(p<s.size()&&(s[p]=='+'||s[p]=='-'))++p; size_t q=p; while(p<s.size()&&std::isdigit((unsigned char)s[p]))++p; if(p==q)bad(); }
        j.type=Json::Number; j.text=s.substr(start,p-start); (void)j.number(); return j;
    }
public:
    explicit JsonParser(const std::string& input) : s(input) {}
    Json parse() { Json j=value(0); space(); if(p != s.size()) bad(); return j; }
};
inline Json parse_json(const std::string& s) { return JsonParser(s).parse(); }
}
#endif
