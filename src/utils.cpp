#include <stdio.h>
#include <stdlib.h>
#include <cctype>
#include <regex>
#include "common.h"
#include "utils.h"

namespace http
{

int decode_hex(int ch)
{
    if ('0' <= ch && ch <= '9')
    {
        return ch - '0';
    }
    else if ('A' <= ch && ch <= 'F')
    {
        return ch - 'A' + 0xa;
    }
    else if ('a' <= ch && ch <= 'f')
    {
        return ch - 'a' + 0xa;
    }
    else
    {
        return -1;
    }
}

std::string file_extension(const std::string& path)
{
    std::smatch m;
    static auto re = std::regex("\\.([a-zA-Z0-9]+)$");
    if (std::regex_search(path, m, re))
        return m[1];
    return std::string();
}

bool from_hex_to_i(const std::string& s, size_t i, size_t cnt, int& val)
{
    if (i >= s.size())
    {
        return false;
    }

    val = 0;
    for (; cnt; i++, cnt--)
    {
        if (!s[i])
        {
            return false;
        }
        int v = decode_hex(s[i]);
        if (v != -1)
        {
            val = val * 16 + v;
        }
        else
        {
            return false;
        }
    }
    return true;
}

bool parse_range(const std::string& s, std::optional<int64_t>& begin, std::optional<int64_t>& end)
{
    if (s.empty())
        return false;

    size_t pos = s.find_first_of("=-");
    if (pos == std::string::npos)
        return false;
    else if (s[pos] == '=')
        return parse_range(s.substr(pos + 1), begin, end);

    std::string s1 = s.substr(0, pos);
    std::string s2 = s.substr(pos + 1);
    begin = strtoll(s1.c_str(), nullptr, 10);
    if (!s2.empty())
        end = strtoll(s2.c_str(), nullptr, 10);
    return true;
}

size_t to_utf8(int code, char* buf)
{
    if (code < 0x0080)
    {
        buf[0] = (code & 0x7F);
        return 1;
    }
    else if (code < 0x0800)
    {
        buf[0] = (0xC0 | ((code >> 6) & 0x1F));
        buf[1] = (0x80 | (code & 0x3F));
        return 2;
    }
    else if (code < 0xD800)
    {
        buf[0] = (0xE0 | ((code >> 12) & 0xF));
        buf[1] = (0x80 | ((code >> 6) & 0x3F));
        buf[2] = (0x80 | (code & 0x3F));
        return 3;
    }
    else if (code < 0xE000)
    {
        // D800 - DFFF is invalid...
        return 0;
    }
    else if (code < 0x10000)
    {
        buf[0] = (0xE0 | ((code >> 12) & 0xF));
        buf[1] = (0x80 | ((code >> 6) & 0x3F));
        buf[2] = (0x80 | (code & 0x3F));
        return 3;
    }
    else if (code < 0x110000)
    {
        buf[0] = (0xF0 | ((code >> 18) & 0x7));
        buf[1] = (0x80 | ((code >> 12) & 0x3F));
        buf[2] = (0x80 | ((code >> 6) & 0x3F));
        buf[3] = (0x80 | (code & 0x3F));
        return 4;
    }

    // NOTREACHED
    return 0;
}

} // namespace http