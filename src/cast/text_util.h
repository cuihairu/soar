// Case-insensitive text plumbing shared by the three cast parsers (SSDP
// reply headers, UPnP device-description tags, SOAP envelopes). All three
// wire formats speak "name: value" or "<tag>value</tag>" with no fixed
// casing, so the matching rules live here once and the parsers stay about
// their own formats. Plain string work — the socket side is renderer.cpp.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace soar::cast {

// Lower-cased copy (ASCII range only: every comparison the parsers make
// is over ASCII identifiers, and device-side text is not lower-cased).
std::string lower(std::string_view text);

// Case-insensitive test of two identifiers.
bool ciEqual(std::string_view a, std::string_view b);

// Value of the first `name:` header in a raw HTTP head, trimmed of the
// whitespace the wire form allows on both sides. "" when absent. The
// status line carries a colon too; it never matches a header name.
std::string fieldValue(std::string_view raw, const char* name);

// Text of the first <tag>...</tag>, entities decoded and the surrounding
// whitespace trimmed. "" for a self-closing tag, a malformed document or
// an absent tag. Tag names match case-insensitively and tolerate
// attributes ("<serviceType foo='1'>" still matches "serviceType").
std::string tagText(std::string_view xml, const char* tag);

// Every value of a repeating tag, in document order, each decoded.
std::vector<std::string> tagTexts(std::string_view xml, const char* tag);

// The five predefined XML entities plus numeric (&#72;, &#x48;) ones. An
// undefined escape stays on the wire untouched.
std::string xmlDecode(std::string_view text);

// Inverse of xmlDecode for the five named entities: safe element content
// and attribute values for SOAP argument text.
std::string xmlEscape(const std::string& text);

// Status code of a raw HTTP reply ("HTTP/1.1 200 OK\r\n..." → 200), or 0
// when the first line is not an HTTP status line.
int httpStatus(std::string_view response);

}  // namespace soar::cast
