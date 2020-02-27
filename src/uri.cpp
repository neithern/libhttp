#include "uri.h"
#include "utils.h"

namespace http
{

bool uri::parse(const std::string& url)
{
    size_t offset = 0;
    if (url.compare(0, 8, "https://") == 0)
    {
        port = "443";
        secure = true;
        offset = 8;
    }
    else if (url.compare(0, 7, "http://") == 0)
    {
        port = "80";
        secure = false;
        offset = 7;
    }
    else
        return false;

    size_t size = url.size();
    if (offset == size)
        return false;

    if (url[offset] == '[')
    {
        if (++offset == size)
            return false;

        size_t endBracket = url.find(']', offset);
        if (endBracket == std::string::npos)
            return false;

        host = url.substr(offset, endBracket - offset);
        offset = endBracket + 1;
    }
    else
    {
        host = url.substr(offset, url.find_first_of(":/", offset) - offset);
        offset += host.size();
    }

    if (offset == size)
    {
        path.clear();
        return true;
    }

    if (url[offset] == ':')
    {
        offset++;
        port = url.substr(offset, url.find('/', offset) - offset);
        offset += port.size();
    }

    path = offset == size ? "/" : url.substr(offset);
    return true;
}

std::string decode(const std::string& s)
{
    const size_t size = s.size();
    std::string result;
    for (size_t i = 0; i < size; i++)
    {
        if (s[i] == '%' && i + 1 < size)
        {
            if (s[i + 1] == 'u')
            {
                int val = 0;
                if (from_hex_to_i(s, i + 2, 4, val))
                {
                    // 4 digits Unicode codes
                    char buff[4];
                    size_t len = to_utf8(val, buff);
                    if (len > 0)
                    {
                        result.append(buff, len);
                    }
                    i += 5; // 'u0000'
                }
                else
                {
                    result += s[i];
                }
            }
            else
            {
                int val = 0;
                if (from_hex_to_i(s, i + 1, 2, val))
                {
                    // 2 digits hex codes
                    result += static_cast<char>(val);
                    i += 2; // '00'
                }
                else
                {
                    result += s[i];
                }
            }
        }
        else if (s[i] == '+')
        {
            result += ' ';
        }
        else
        {
            result += s[i];
        }
    }
    return result;
}

std::string uri::encode(const std::string& s)
{
    std::string result;
    for (auto c : s)
    {
        auto b = static_cast<uint8_t>(c);
        bool format = b >= 0x80;
        if (b < 0x80)
        {
            switch (c)
            {
            case ' ':
            case '+':
            case '\r':
            case '\n':
            case '\'':
            case ',':
            case ';':
            // case ':':
                format = true;
                break;
            }
        }
        if (format)
        {
            char hex[8] = {};
            size_t len = ::snprintf(hex, sizeof(hex), "%%%02X", b);
            result.append(hex, len);
        }
        else
        {
            result += c;
        }
    }
    return result;
}

} // namespace http