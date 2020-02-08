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
    std::string result;
    for (size_t i = 0; i < s.size(); i++)
    {
        if (s[i] == '%' && i + 1 < s.size())
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
    for (auto i = 0; s[i]; i++)
    {
        switch (s[i])
        {
        case ' ':
            result += "%20";
            break;
        case '+':
            result += "%2B";
            break;
        case '\r':
            result += "%0D";
            break;
        case '\n':
            result += "%0A";
            break;
        case '\'':
            result += "%27";
            break;
        case ',':
            result += "%2C";
            break;
        // case ':': result += "%3A"; break; // ok? probably...
        case ';':
            result += "%3B";
            break;
        default:
            auto c = static_cast<uint8_t>(s[i]);
            if (c >= 0x80)
            {
                result += '%';
                char hex[4];
                size_t len = snprintf(hex, sizeof(hex) - 1, "%02X", c);
                assert(len == 2);
                result.append(hex, len);
            }
            else
            {
                result += s[i];
            }
            break;
        }
    }
    return result;
}

} // namespace http