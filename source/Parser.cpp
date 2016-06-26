#include "Parser.hpp"
#include <cassert>
namespace http
{
    template<class IMPL, class Message>
    Parser<IMPL,Message>::Parser()
    {
        reset();
    }
    template<class IMPL, class Message>
    void Parser<IMPL, Message>::reset()
    {
        parser_status = NOT_STARTED;

        message.headers.clear();
        message.body.clear();

        expected_body_len = 0;
        buffer.clear();
    }

    template<class IMPL, class Message>
    size_t Parser<IMPL, Message>::read(const uint8_t * data, size_t len)
    {
        assert(len > 0);
        std::string line;
        auto p = data, end = data + len;

        while (p != end)
        {
            size_t len2 = 0;
            if (parser_status == NOT_STARTED && read_line(&line, &len2, p, end - p))
            {
                assert(buffer.empty());
                ((IMPL*)this)->read_first_line(line);
                parser_status = READING_HEADERS;
            }
            p += len2; len2 = 0;

            if (parser_status == READING_HEADERS && read_line(&line, &len2, p, end - p))
            {
                assert(buffer.empty());
                if (line.empty())
                {
                    //end of headers
                    if (message.headers.get("Transfer-Encoding") == "chunked")
                    {
                        expected_body_len = 0;
                        parser_status = READING_BODY_CHUNKED_LENGTH;
                    }
                    else
                    {
                        auto it = message.headers.find("Content-Length");
                        if (it != message.headers.end())
                        {
                            expected_body_len = std::stoul(it->second);
                            parser_status = READING_BODY;
                        }
                        else
                        {
                            parser_status = COMPLETED;
                        }
                    }
                }
                else add_header(line);
            }
            p += len2; len2 = 0;

            if (parser_status == READING_BODY || parser_status == READING_BODY_CHUNKED)
            {
                assert(!buffer.size());
                assert(expected_body_len > 0);
                auto remaining = expected_body_len - message.body.size();
                if ((size_t)(end - p) >= remaining)
                {
                    message.body.insert(message.body.end(), p, p + remaining);
                    p += remaining;
                    parser_status = parser_status == READING_BODY ? COMPLETED : READING_BODY_CHUNKED_TERMINATOR;
                }
                else
                {
                    message.body.insert(message.body.end(), p, end);
                    p = end;
                }
            }

            if (parser_status == READING_BODY_CHUNKED_TERMINATOR && read_line(&line, &len2, p, end - p))
            {
                if (line.empty()) parser_status = READING_BODY_CHUNKED_LENGTH;
                else throw std::runtime_error("Expected chunk \\r\\n terminator");
            }
            p += len2; len2 = 0;

            if (parser_status == READING_BODY_CHUNKED_LENGTH && read_line(&line, &len2, p, end - p))
            {
                auto chunk_len = std::stoul(line, nullptr, 16);
                if (chunk_len)
                {
                    parser_status = READING_BODY_CHUNKED;
                    expected_body_len += chunk_len;
                }
                else parser_status = READING_TRAILER_HEADERS;
            }
            p += len2; len2 = 0;

            if (parser_status == READING_TRAILER_HEADERS && read_line(&line, &len2, p, end - p))
            {
                assert(buffer.empty());
                if (line.empty()) parser_status = COMPLETED; //end of headers
                else add_header(line);
            }
            p += len2; len2 = 0;
        }

        assert(p > data && p <= end);
        assert(p == end || is_completed());
        return p - data;
    }

    template<class IMPL, class Message>
    bool Parser<IMPL, Message>::read_line(std::string * line, size_t *consumed_len, const uint8_t *data, size_t len)
    {
        if (!len) return false;

        //The last byte of buffer might be a \r, and the first byte of data might be a \n
        if (buffer.size() && buffer.back() == '\r' && data[0] == '\n')
        {
            line->assign(buffer.data(), buffer.data() + buffer.size() - 1); //assign, but exclude the \r
            buffer.clear();
            *consumed_len = 1; //1 for the \n
            return true;
        }

        auto p = data;
        auto end = data + len - 1;
        while (true)
        {
            //looking for \r\n, so search for \r first and ignore the last byte
            auto p2 = (const uint8_t*)memchr(p, '\r', end - p);
            if (p2)
            {
                if (p2[1] == '\n')
                {
                    //found end of string
                    line->assign(buffer.data(), buffer.data() + buffer.size());
                    line->insert(line->end(), data, p2);
                    *consumed_len = p2 - data + 2;
                    buffer.clear();
                    return true;
                }
                else
                {
                    //just a \r by itself, look for the next one
                    p = p2 + 1;
                }
            }
            else break; //reached end of buffer
        }
        //not found, so all of data must be part of the line
        buffer.insert(buffer.end(), data, data + len);
        *consumed_len = len;
        return false;
    }

    template<class IMPL, class Message>
    void Parser<IMPL, Message>::add_header(const std::string &line)
    {
        auto colon = line.find(':', 0);
        auto first_val = line.find_first_not_of(' ', colon + 1);
        std::string name = line.substr(0, colon);
        std::string value = line.substr(first_val);
        message.headers.add(name, value);
    }

    void RequestParser::read_first_line(const std::string &line)
    {
        //e.g. 'GET /index.html HTTP/1.1'
        auto method_end = line.find(' ');
        auto url_end = line.find_last_of(' ');

        auto method_str = line.substr(0, method_end);
        message.method = method_from_string(method_str);

        message.raw_url = line.substr(method_end + 1, url_end - method_end - 1);
        message.url = Url::parse_request(message.raw_url);

        //TODO: Version
    }

    void ResponseParser::read_first_line(const std::string &line)
    {
        //e.g. 'HTTP/1.1 200 OK'
        auto ver_end = line.find(' ', 0);
        auto code_end = line.find(' ', ver_end + 1);

        std::string code_str = line.substr(ver_end + 1, code_end - ver_end);
        message.status_code = (StatusCode)std::stoi(code_str);
        message.status_msg = line.substr(code_end + 1);

        //TODO: Version
    }

    template class Parser<RequestParser, Request>;
    template class Parser<ResponseParser, Response>;
}