// Replacement for the legacy JSON decoder's unimplemented Unicode escape case.
// Included into the generated JSON translation unit, never into CUDAynamics.
std::string json::parsing::decode_string(const char* input) {
    const std::string s(input);std::string result;
    if(s.size()<2 || s.front()!='"' || s.back()!='"')throw json::parsing_error("Invalid JSON string");
    auto hex=[&](size_t& i)->unsigned {
        unsigned code=0;
        for(int n=0;n<4;++n) {
            if(++i>=s.size()-1)throw json::parsing_error("Truncated Unicode escape");
            char c=s[i];unsigned v;
            if(c>='0'&&c<='9')v=c-'0';else if(c>='a'&&c<='f')v=c-'a'+10;
            else if(c>='A'&&c<='F')v=c-'A'+10;else throw json::parsing_error("Invalid Unicode escape");
            code=(code<<4)|v;
        }return code;
    };
    auto utf8=[&](unsigned cp) {
        if(cp<=0x7f)result+=char(cp);
        else if(cp<=0x7ff){result+=char(0xc0|(cp>>6));result+=char(0x80|(cp&63));}
        else if(cp<=0xffff){result+=char(0xe0|(cp>>12));result+=char(0x80|((cp>>6)&63));result+=char(0x80|(cp&63));}
        else {result+=char(0xf0|(cp>>18));result+=char(0x80|((cp>>12)&63));result+=char(0x80|((cp>>6)&63));result+=char(0x80|(cp&63));}
    };
    for(size_t i=1;i<s.size()-1;++i) {
        char c=s[i];
        if(c!='\\'){if(static_cast<unsigned char>(c)<32)throw json::parsing_error("Control character in string");result+=c;continue;}
        if(++i>=s.size()-1)throw json::parsing_error("Truncated JSON escape");
        switch(s[i]) {
            case '"':case '\\':case '/':result+=s[i];break;
            case 'b':result+='\b';break;case 'f':result+='\f';break;case 'n':result+='\n';break;
            case 'r':result+='\r';break;case 't':result+='\t';break;
            case 'u': {
                unsigned cp=hex(i);
                if(cp>=0xd800&&cp<=0xdbff) {
                    if(i+2>=s.size()-1 || s[i+1]!='\\' || s[i+2]!='u')throw json::parsing_error("Missing low surrogate");
                    i+=2;unsigned low=hex(i);if(low<0xdc00||low>0xdfff)throw json::parsing_error("Invalid low surrogate");
                    cp=0x10000+((cp-0xd800)<<10)+(low-0xdc00);
                }else if(cp>=0xdc00&&cp<=0xdfff)throw json::parsing_error("Unpaired low surrogate");
                utf8(cp);break;
            }
            default:throw json::parsing_error("Invalid JSON escape");
        }
    }return result;
}
