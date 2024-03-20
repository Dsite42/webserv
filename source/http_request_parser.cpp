#include "http_request_parser.hpp"
#include "utility.hpp"
#include "debug_utility.hpp"

#include <cstring>

/* Constructs a HTTP request parser using the given rules */
HttpRequestParser::HttpRequestParser(const ServerConfig &config, uint32_t host, uint16_t port)
    : _config(config)
    , _host(host)
    , _port(port)
{
    reset();
}

/* Prepares the parser to consume the next request */
void HttpRequestParser::reset()
{
    _request            = HttpRequest();
    _request.clientHost = _host;
    _request.clientPort = _port;
    _phase              = HTTP_REQUEST_HEADER;
    _headerLength       = 0;
}

/* Commits data to the parser, returns whether the parser has transitioned into a final phase */
bool HttpRequestParser::commit(Slice &data)
{
    while (data.getLength() > 0)
    {
        switch (_phase)
        {
            case HTTP_REQUEST_HEADER:
                _phase = handleHeader(data);
                break;
            case HTTP_REQUEST_BODY_RAW:
                _phase = handleBodyRaw(data);
                break;
            case HTTP_REQUEST_BODY_CHUNKED_HEADER:
                _phase = handleBodyChunkedHeader(data);
                break;
            case HTTP_REQUEST_BODY_CHUNKED_BODY:
                _phase = handleBodyChunkedBody(data);
                break;
            case HTTP_REQUEST_BODY_CHUNKED_CRLF:
                _phase = handleBodyChunkedCRLF(data);
                break;

            // If in a final state already, immediately return false to prevent
            // requests from being handled multiple times
            case HTTP_REQUEST_HEADER_EXCEED:
            case HTTP_REQUEST_BODY_EXCEED:
            case HTTP_REQUEST_MALFORMED:
            case HTTP_REQUEST_COMPLETED:
                return false;
        }
        // If the state was transitioned into a final state, immediately return true
        if (_phase == HTTP_REQUEST_HEADER_EXCEED
         || _phase == HTTP_REQUEST_BODY_EXCEED
         || _phase == HTTP_REQUEST_MALFORMED
         || _phase == HTTP_REQUEST_COMPLETED)
            return true;
    }
    return false;
}

/* Handles a data commit in the `HTTP_REQUEST_HEADER` phase */
HttpRequestPhase HttpRequestParser::handleHeader(Slice &data)
{
    // Calculate the maximum amount of bytes that can be copied from the data slice
    size_t headerSpace = HTTP_REQUEST_HEADER_MAX_LENGTH - _headerLength;
    size_t copyLength = data.getLength();
    if (copyLength > headerSpace)
        copyLength = headerSpace;

    // Copy the data into the header buffer and save the fresh data's offset
    size_t freshOffset = _headerLength;
    memcpy(&_headerBuffer[freshOffset], &data[0], copyLength);
    _headerLength += copyLength;

    // Search for CRLF from up to 3 bytes below the fresh data
    size_t searchOffset = freshOffset;
    if (searchOffset < 3)
        searchOffset = 0;
    else
        searchOffset -= 3;
    const char *position = reinterpret_cast<const char *>(
        memmem(&_headerBuffer[searchOffset], _headerLength - searchOffset, "\r\n\r\n", 4)
    );

    // If the CRLF is not found, the header can't be completed yet
    if (position == NULL)
    {
        // Consume all copied bytes
        data.consumeStart(copyLength);

        // If the header length has been reached, it is impossible for a header to exist,
        // the request is malformed
        if (_headerLength == HTTP_REQUEST_HEADER_MAX_LENGTH)
            return HTTP_REQUEST_HEADER_EXCEED;

        // More data is required to complete the header
        return HTTP_REQUEST_HEADER;
    }
    size_t headerEndOffset = position - _headerBuffer;

    // The header was completed, parse it
    if (!parseHeader(Slice(_headerBuffer, headerEndOffset)))
        return HTTP_REQUEST_MALFORMED;

    // Only consume the header part (including the CRLF) of the data slice
    size_t remainder = _headerLength - (headerEndOffset + 4) + data.getLength() - copyLength;
    data.consumeStart(data.getLength() - remainder);

    // Search for the headers that decide the body phase and process them
    const HttpRequest::Header *contentLength = _request.findHeader(C_SLICE("Content-Length"));
    const HttpRequest::Header *transferEncoding = _request.findHeader(C_SLICE("Transfer-Encoding"));
    if (transferEncoding != NULL)
    {
        // Only chunked transfer encoding is supported
        if (transferEncoding->getValue() != "chunked")
            return HTTP_REQUEST_MALFORMED;

        // Requests with a chunked transfer encoding can not have a content length
        if (contentLength != NULL)
            return HTTP_REQUEST_MALFORMED;

        return HTTP_REQUEST_BODY_CHUNKED_HEADER;
    }
    else if (contentLength != NULL)
    {
        // Expect a valid unsigned integer as the content length
        if (!Utility::parseSize(contentLength->getValue(), _bodyRemainder))
            return HTTP_REQUEST_MALFORMED;

        // Expect the content length to be within the allowed range
        if (_bodyRemainder > _config.maxBodySize)
            return HTTP_REQUEST_BODY_EXCEED;

        // The request can be completed immediately if the content length is zero
        if (_bodyRemainder == 0)
            return HTTP_REQUEST_COMPLETED;

        return HTTP_REQUEST_BODY_RAW;
    }

    // Missing transfer encoding and content length is treated like a zero-length body
    return HTTP_REQUEST_COMPLETED;
}

/* Handles a data commit in the `HTTP_REQUEST_BODY_RAW` phase */
HttpRequestPhase HttpRequestParser::handleBodyRaw(Slice &data)
{
    (void)data;
    throw std::runtime_error("Not implemented");
}

/* Handles a data commit in the `HTTP_REQUEST_BODY_CHUNKED_HEADER` phase */
HttpRequestPhase HttpRequestParser::handleBodyChunkedHeader(Slice &data)
{
    (void)data;
    throw std::runtime_error("Not implemented");
}

/* Handles a data commit in the `HTTP_REQUEST_BODY_CHUNKED_BODY` phase */
HttpRequestPhase HttpRequestParser::handleBodyChunkedBody(Slice &data)
{
    (void)data;
    throw std::runtime_error("Not implemented");
}

/* Handles a data commit in the `HTTP_REQUEST_BODY_CHUNKED_CRLF` phase */
HttpRequestPhase HttpRequestParser::handleBodyChunkedCRLF(Slice &data)
{
    (void)data;
    throw std::runtime_error("Not implemented");
}

/* Parses the given HTTP header */
bool HttpRequestParser::parseHeader(Slice data)
{
    Slice methodSlice;
    Slice querySlice;
    Slice versionSlice;

    // Expect a valid HTTP method in the first field of the request line
    if (!data.splitStart(' ', methodSlice))
        return false;
    if ((_request.method = parseHttpMethod(methodSlice)) == HTTP_METHOD_NONE)
        return false;

    // Expect a query in the second field of the request line
    if (!data.splitStart(' ', querySlice))
        return false;
    _request.query = querySlice.toString();

    // Expect HTTP 1.0 or 1.1 in the third field of the request line
    if (data.consumeStart(C_SLICE("HTTP/1.1")))
        _request.isLegacy = false;
    else if (data.consumeStart(C_SLICE("HTTP/1.0")))
        _request.isLegacy = true;
    else
        return false;

    // Catch garbage after a valid request line
    if (data.getLength() > 0 && !data.consumeStart(C_SLICE("\r\n")))
        return false;

    // Parse and collect header fields
    while (data.getLength() > 0)
    {
        Slice headerName;
        Slice headerValue;

        if (!data.splitStart(C_SLICE(": "), headerName))
            return false;
        if (!data.splitStart(C_SLICE("\r\n"), headerValue))
        {
            headerValue = data;
            data = Slice();
        }

        _request.headers.push_back(
            HttpRequest::Header(headerName.toString(), headerValue.toString())
        );
    }

    return true;
}
