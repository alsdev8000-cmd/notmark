// NotMark -> HTML compiler (first pass)
// Usage: notmark input.nm > output.html
//        notmark < input.nm > output.html

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cctype>

// ---------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------

static std::string htmlEscape(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

// trim leading/trailing whitespace
static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// split string on a delimiter character, trimming each piece
static std::vector<std::string> splitTrim(const std::string &s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == delim) {
            out.push_back(trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(trim(cur));
    return out;
}

/* Extract a YouTube video ID from a raw ID or various URL formats:
   dQw4w9WgXcQ
   https://www.youtube.com/watch?v=dQw4w9WgXcQ
   https://youtu.be/dQw4w9WgXcQ
   https://www.youtube.com/embed/dQw4w9WgXcQ */
static std::string extractYouTubeId(const std::string &raw) {
    std::string s = trim(raw);

    auto isIdChar = [](char c) {
        return isalnum((unsigned char)c) || c == '_' || c == '-';
    };

    size_t pos = std::string::npos;
    if ((pos = s.find("v=")) != std::string::npos) {
        size_t start = pos + 2;
        size_t end = start;
        while (end < s.size() && isIdChar(s[end])) end++;
        return s.substr(start, end - start);
    }
    if ((pos = s.find("youtu.be/")) != std::string::npos) {
        size_t start = pos + 9;
        size_t end = start;
        while (end < s.size() && isIdChar(s[end])) end++;
        return s.substr(start, end - start);
    }
    if ((pos = s.find("embed/")) != std::string::npos) {
        size_t start = pos + 6;
        size_t end = start;
        while (end < s.size() && isIdChar(s[end])) end++;
        return s.substr(start, end - start);
    }
    // assume raw ID already
    return s;
}

// Handles: \escape, *bold*, _italic_, `code`, @directive[...]{...}
// ---------------------------------------------------------------------

class InlineParser {
public:
    explicit InlineParser(const std::string &src) : s(src), pos(0), n(src.size()) {}

    // Parse inline content until end-of-string OR until we hit a
    // top-level '}' when insideBraces is true (used for directive content).
    std::string parse(bool insideBraces = false) {
        std::string out;
        while (pos < n) {
            char c = s[pos];

            if (insideBraces && c == '}') {
                // stop, let caller consume the '}'
                break;
            }

            if (c == '\\' && pos + 1 < n) {
                // escape: output next char literally (html-escaped)
                out += htmlEscape(std::string(1, s[pos + 1]));
                pos += 2;
                continue;
            }

            if (c == '*') {
                std::string inner = readDelimited('*');
                out += "<strong>" + parseSub(inner) + "</strong>";
                continue;
            }

            if (c == '_') {
                std::string inner = readDelimited('_');
                out += "<em>" + parseSub(inner) + "</em>";
                continue;
            }

            if (c == '`') {
                std::string inner = readDelimitedRaw('`');
                out += "<code>" + htmlEscape(inner) + "</code>";
                continue;
            }

            if (c == '@') {
                std::string directiveHtml = parseDirective();
                out += directiveHtml;
                continue;
            }

            if (c == '[') {
                std::string linkHtml = tryParseInlineLink();
                if (!linkHtml.empty()) {
                    out += linkHtml;
                    continue;
                }
                // not a valid [text](url) -> literal '['
                out += "[";
                pos++;
                continue;
            }

            // plain character
            out += htmlEscape(std::string(1, c));
            pos++;
        }
        return out;
    }

private:
    const std::string &s;
    size_t pos;
    size_t n;

    // Recursively parse a sub-string (used for inner content of */_/directives)
    std::string parseSub(const std::string &text) {
        InlineParser p(text);
        return p.parse(false);
    }

    /* Read content between a pair of delimiter chars (e.g. * ... *)
     Returns the raw inner text (not yet parsed). Advances pos past closing delim.
     If no closing delim found, treats opening delim as literal text. */
    std::string readDelimited(char delim) {
        size_t start = pos; // position of opening delim
        size_t i = pos + 1;
        std::string inner;
        while (i < n) {
            if (s[i] == '\\' && i + 1 < n) {
                inner += s[i];
                inner += s[i + 1];
                i += 2;
                continue;
            }
            if (s[i] == delim) {
                // found closing delimiter
                pos = i + 1;
                return inner;
            }
            inner += s[i];
            i++;
        }
        // no closing delimiter found: treat delim as literal char
        pos = start + 1;
        return std::string(); // caller appended nothing useful; handle below
    }

    // Variant for `code` spans: do not interpret nested markup, but still
    // respect backslash-escapes for the closing backtick.
    std::string readDelimitedRaw(char delim) {
        size_t start = pos;
        size_t i = pos + 1;
        std::string inner;
        while (i < n) {
            if (s[i] == '\\' && i + 1 < n && s[i + 1] == delim) {
                inner += delim;
                i += 2;
                continue;
            }
            if (s[i] == delim) {
                pos = i + 1;
                return inner;
            }
            inner += s[i];
            i++;
        }
        pos = start + 1;
        return std::string();
    }

    /* Try to parse [text](url) starting at '['. If successful, advances pos
     past the closing ')' and returns the rendered <a> tag. If it's not a
     valid inline link (no matching ']' or no following '(...)'), returns
     empty string and leaves pos unchanged. */
    std::string tryParseInlineLink() {
        size_t start = pos;
        size_t i = pos + 1;
        std::string text;
        while (i < n) {
            if (s[i] == '\\' && i + 1 < n) { text += s[i]; text += s[i+1]; i += 2; continue; }
            if (s[i] == ']') break;
            if (s[i] == '\n') return ""; // no links across lines
            text += s[i];
            i++;
        }
        if (i >= n || s[i] != ']') return ""; // no closing ']'
        i++; // consume ']'

        if (i >= n || s[i] != '(') return ""; // must be immediately followed by '('
        i++; // consume '('

        std::string url;
        while (i < n) {
            if (s[i] == '\\' && i + 1 < n) { url += s[i+1]; i += 2; continue; }
            if (s[i] == ')') break;
            if (s[i] == '\n') return "";
            url += s[i];
            i++;
        }
        if (i >= n || s[i] != ')') return ""; // no closing ')'
        i++; // consume ')'

        pos = i;
        std::string linkText = parseSub(text);
        return "<a href=\"" + htmlEscape(trim(url)) + "\">" + linkText + "</a>";
        (void)start;
    }

    std::string parseDirective() {
        size_t start = pos;
        size_t i = pos + 1;
        std::string name;
        while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == '_')) {
            name += s[i];
            i++;
        }
        if (name.empty()) {
            // not a directive, literal '@'
            pos++;
            return "@";
        }

        std::string options;
        bool hasOptions = false;
        if (i < n && s[i] == '[') {
            hasOptions = true;
            i++;
            size_t depth = 1;
            while (i < n) {
                if (s[i] == '\\' && i + 1 < n) { options += s[i]; options += s[i+1]; i += 2; continue; }
                if (s[i] == '[') depth++;
                if (s[i] == ']') { depth--; if (depth == 0) { i++; break; } }
                options += s[i];
                i++;
            }
        }

        std::string content;
        bool hasContent = false;
        if (i < n && s[i] == '{') {
            hasContent = true;
            i++;
            size_t depth = 1;
            size_t contentStart = i;
            while (i < n) {
                if (s[i] == '\\' && i + 1 < n) { i += 2; continue; }
                if (s[i] == '{') depth++;
                if (s[i] == '}') { depth--; if (depth == 0) break; }
                i++;
            }
            content = s.substr(contentStart, i - contentStart);
            if (i < n) i++; // consume closing '}'
        }

        pos = i;
        (void)start;
        return renderDirective(name, options, hasOptions, content, hasContent);
    }

    // Render a directive to HTML based on its name.
    std::string renderDirective(const std::string &name, const std::string &options,
                                  bool hasOptions, const std::string &content, bool hasContent) {
        std::string body = hasContent ? parseSub(content) : std::string();

        if (name == "quote") {
            std::string cite = hasOptions ? htmlEscape(options) : "";
            std::string out = "<blockquote>" + body;
            if (!cite.empty()) out += "<cite> - " + cite + "</cite>";
            out += "</blockquote>";
            return out;
        }

        if (name == "code") {
            std::string lang = hasOptions ? trim(options) : "";
            std::string raw = hasContent ? content : "";
            // raw content of code blocks should not be inline-parsed,
            // just html-escaped, preserving backslashes literally.
            std::string escaped = htmlEscape(raw);
            std::string cls = lang.empty() ? "" : (" class=\"language-" + htmlEscape(lang) + "\"");
            return "<pre><code" + cls + ">" + escaped + "</code></pre>";
        }

        if (name == "table") {
            std::vector<std::string> headers = hasOptions ? splitTrim(options, ',') : std::vector<std::string>();
            std::vector<std::string> cells = hasContent ? splitTrim(content, ',') : std::vector<std::string>();
            std::string out = "<table>\n<thead><tr>";
            for (auto &h : headers) out += "<th>" + parseSub(h) + "</th>";
            out += "</tr></thead>\n<tbody>\n";
            size_t cols = headers.empty() ? cells.size() : headers.size();
            if (cols == 0) cols = 1;
            for (size_t i = 0; i < cells.size(); i += cols) {
                out += "<tr>";
                for (size_t j = 0; j < cols; j++) {
                    std::string val = (i + j < cells.size()) ? cells[i + j] : "";
                    out += "<td>" + parseSub(val) + "</td>";
                }
                out += "</tr>\n";
            }
            out += "</tbody>\n</table>";
            return out;
        }

        if (name == "image") {
            // @image[alt]{url}
            std::string alt = hasOptions ? trim(options) : "";
            std::string url = hasContent ? trim(content) : "";
            return "<img src=\"" + htmlEscape(url) + "\" alt=\"" + htmlEscape(alt) + "\">";
        }

        if (name == "video") {
            // @video[alt]{url}
            std::string alt = hasOptions ? trim(options) : "";
            std::string url = hasContent ? trim(content) : "";
            return "<video controls src=\"" + htmlEscape(url) + "\">" + htmlEscape(alt) + "</video>";
        }

        if (name == "yt") {
            // @yt[alt]{videoIdOrUrl}
            std::string alt = hasOptions ? trim(options) : "";
            std::string raw = hasContent ? trim(content) : "";
            std::string id = extractYouTubeId(raw);
            std::string embedSrc = "https://www.youtube.com/embed/" + id;
            std::string out = "<iframe width=\"560\" height=\"315\" src=\"" + htmlEscape(embedSrc) + "\"";
            out += " title=\"" + htmlEscape(alt) + "\"";
            out += " frameborder=\"0\" allow=\"accelerometer; autoplay; clipboard-write; encrypted-media; gyroscope; picture-in-picture; web-share\" referrerpolicy=\"strict-origin-when-cross-origin\" allowfullscreen></iframe>";
            return out;
        }

        if (name == "link") {
            // @link[alt]{url}
            std::string alt = hasOptions ? trim(options) : "";
            std::string url = hasContent ? trim(content) : "";
            std::string text = alt.empty() ? htmlEscape(url) : parseSub(alt);
            return "<a href=\"" + htmlEscape(url) + "\">" + text + "</a>";
        }

        if (name == "header") {
            // @header[level]{text}
            int level = 1;
            if (hasOptions) {
                try { level = std::stoi(trim(options)); } catch (...) { level = 1; }
            }
            if (level < 1) level = 1;
            if (level > 6) level = 6;
            std::string lvl = std::to_string(level);
            return "<h" + lvl + ">" + body + "</h" + lvl + ">";
        }

        if (name == "sameline") {
            // @sameline[a]{b} -> "a" then "b" with no break between
            std::string a = hasOptions ? parseSub(options) : "";
            std::string b = body;
            return a + b;
        }

        if (name == "newline") {
            // @newline[]{}
            return "<br>";
        }

        if (name == "color") {
            // @color[colorValue]{text}
            std::string col = hasOptions ? trim(options) : "inherit";
            return "<span style=\"color: " + htmlEscape(col) + ";\">" + body + "</span>";
        }

        if (name == "list") {
            // @list[ordered]{a, b, c}  or  @list[unordered]{a, b, c}
            std::string kind = hasOptions ? trim(options) : "unordered";
            bool ordered = (kind == "ordered");
            std::vector<std::string> items = hasContent ? splitTrim(content, ',') : std::vector<std::string>();
            std::string tag = ordered ? "ol" : "ul";
            std::string out = "<" + tag + ">\n";
            for (auto &item : items) {
                out += "<li>" + parseSub(item) + "</li>\n";
            }
            out += "</" + tag + ">";
            return out;
        }

        if (name == "tip") {
            // @tip[title]{content}
            std::string title = hasOptions ? trim(options) : "Tip";
            std::string out = "<div class=\"admonition admonition-tip\">";
            out += "<p class=\"admonition-title\">" + htmlEscape(title) + "</p>";
            out += "<p>" + body + "</p>";
            out += "</div>";
            return out;
        }

        if (name == "warning") {
            // @warning[title]{content}
            std::string title = hasOptions ? trim(options) : "Warning";
            std::string out = "<div class=\"admonition admonition-warning\">";
            out += "<p class=\"admonition-title\">" + htmlEscape(title) + "</p>";
            out += "<p>" + body + "</p>";
            out += "</div>";
            return out;
        }

        if (name == "rule") {
            // @rule[]{}
            return "<hr>";
        }

        if (name == "footer") {
            // @footer[]{text}
            return "<footer>" + body + "</footer>";
        }

        if (name == "mermaid") {
            // @mermaid[alt]{diagram text} -> <pre class="mermaid">...</pre>
            std::string alt = hasOptions ? trim(options) : "";
            std::string raw = hasContent ? content : "";
            std::string out = "<pre class=\"mermaid\"";
            if (!alt.empty()) out += " aria-label=\"" + htmlEscape(alt) + "\"";
            out += ">" + htmlEscape(raw) + "</pre>";
            return out;
        }

        if (name == "docstart" || name == "docend") {
            // @docstart[name]{} / @docend[name]{} -- markers used by @include,
            // emit nothing in normal output.
            return "";
        }

        if (name == "include") {
            // @include[name]{path} -- handled at the block/compile level via
            // preprocessing; if it reaches here unresolved, render nothing.
            return "";
        }

        // unknown directive: render as a <span> with data-directive attr
        std::string out = "<span data-directive=\"" + htmlEscape(name) + "\"";
        if (hasOptions) out += " data-options=\"" + htmlEscape(options) + "\"";
        out += ">" + body + "</span>";
        return out;
    }
};

// Check if `trimmed` is *entirely* a single @name[...]{...} directive call
// (nothing before or after it). Returns the directive name if so, "" otherwise.
static std::string soleDirectiveName(const std::string &trimmed) {
    if (trimmed.empty() || trimmed[0] != '@') return "";
    size_t i = 1;
    std::string name;
    while (i < trimmed.size() && (isalnum((unsigned char)trimmed[i]) || trimmed[i] == '-' || trimmed[i] == '_')) {
        name += trimmed[i];
        i++;
    }
    if (name.empty()) return "";

    // optional [options]
    if (i < trimmed.size() && trimmed[i] == '[') {
        size_t depth = 1;
        i++;
        while (i < trimmed.size() && depth > 0) {
            if (trimmed[i] == '\\' && i + 1 < trimmed.size()) { i += 2; continue; }
            if (trimmed[i] == '[') depth++;
            else if (trimmed[i] == ']') depth--;
            i++;
        }
        if (depth != 0) return ""; // unbalanced
    }

    // optional {content}
    if (i < trimmed.size() && trimmed[i] == '{') {
        size_t depth = 1;
        i++;
        while (i < trimmed.size() && depth > 0) {
            if (trimmed[i] == '\\' && i + 1 < trimmed.size()) { i += 2; continue; }
            if (trimmed[i] == '{') depth++;
            else if (trimmed[i] == '}') depth--;
            i++;
        }
        if (depth != 0) return ""; // unbalanced
    }

    // must be exactly at end of string for this to be a "sole" directive
    if (i != trimmed.size()) return "";

    return name;
}

// Directive names that should be emitted as block-level HTML (no <p> wrap)
// when they are the sole content of a line.
static bool isBlockDirective(const std::string &name) {
    return name == "table" || name == "list" || name == "tip" || name == "warning" ||
           name == "rule" || name == "header" || name == "code" || name == "quote" ||
           name == "image" || name == "video" || name == "yt" || name == "footer" ||
           name == "mermaid";
}

// Handles: headings (#, ##, ...), fenced code blocks (```), paragraphs
// ---------------------------------------------------------------------

// ---------------------------------------------------------------------
// @include preprocessing
// ---------------------------------------------------------------------

// Parse a line that is solely "@include[name]{path}" (or "@include[]{path}"
// for the whole file). Returns true and fills name/path if matched.
static bool parseIncludeLine(const std::string &trimmed, std::string &name, std::string &path) {
    if (trimmed.rfind("@include[", 0) != 0) return false;
    size_t close = trimmed.find(']', 9);
    if (close == std::string::npos) return false;
    name = trim(trimmed.substr(9, close - 9));
    size_t brace = close + 1;
    if (brace >= trimmed.size() || trimmed[brace] != '{') return false;
    size_t end = trimmed.rfind('}');
    if (end == std::string::npos || end <= brace) return false;
    path = trim(trimmed.substr(brace + 1, end - brace - 1));
    return true;
}

// Parse a line that is solely "@docstart[name]{}" or "@docend[name]{}".
// Returns "start"/"end" and fills name, or "" if no match.
static std::string parseDocMarker(const std::string &trimmed, std::string &name) {
    std::string kind;
    size_t prefixLen = 0;
    if (trimmed.rfind("@docstart[", 0) == 0) { kind = "start"; prefixLen = 10; }
    else if (trimmed.rfind("@docend[", 0) == 0) { kind = "end"; prefixLen = 8; }
    else return "";

    size_t close = trimmed.find(']', prefixLen);
    if (close == std::string::npos) return "";
    name = trim(trimmed.substr(prefixLen, close - prefixLen));
    return kind;
}

// Extract a named region from `content`, delimited by lines that are solely
// @docstart[name]{} and @docend[name]{}. If name is empty, or no matching
// markers are found, returns the whole content unchanged.
static std::string extractRegion(const std::string &content, const std::string &name) {
    if (name.empty()) return content;

    std::istringstream in(content);
    std::string line;
    std::ostringstream out;
    bool inRegion = false;
    bool found = false;

    while (std::getline(in, line)) {
        std::string trimmed = trim(line);
        std::string markerName;
        std::string kind = parseDocMarker(trimmed, markerName);
        if (kind == "start" && markerName == name) {
            inRegion = true;
            found = true;
            continue;
        }
        if (kind == "end" && markerName == name) {
            inRegion = false;
            continue;
        }
        if (inRegion) out << line << "\n";
    }

    if (!found) return content; // no matching region: fall back to whole file
    return out.str();
}

// Recursively resolve @include[name]{path} lines in `source`. `baseDir` is
// the directory of the file `source` came from, used to resolve relative
// include paths. `depth` guards against include cycles.
static std::string resolveIncludes(const std::string &source, const std::string &baseDir, int depth = 0) {
    if (depth > 16) {
        return source; // give up on deeply nested/cyclic includes
    }

    std::istringstream in(source);
    std::string line;
    std::ostringstream out;

    while (std::getline(in, line)) {
        std::string trimmed = trim(line);
        std::string name, path;
        if (parseIncludeLine(trimmed, name, path)) {
            std::string fullPath = path;
            if (!path.empty() && path[0] != '/' && !baseDir.empty()) {
                fullPath = baseDir + "/" + path;
            }
            std::ifstream f(fullPath);
            if (!f) {
                out << "<!-- include not found: " << htmlEscape(fullPath) << " -->\n";
                continue;
            }
            std::ostringstream ss;
            ss << f.rdbuf();
            std::string included = ss.str();

            included = extractRegion(included, name);

            // resolve nested includes relative to the included file's directory
            std::string includedDir = baseDir;
            size_t slash = fullPath.find_last_of('/');
            if (slash != std::string::npos) includedDir = fullPath.substr(0, slash);
            included = resolveIncludes(included, includedDir, depth + 1);

            out << included;
            if (!included.empty() && included.back() != '\n') out << "\n";
            continue;
        }

        out << line << "\n";
    }

    return out.str();
}

// Strip any remaining top-level @docstart[...]{}/ @docend[...]{} marker
// lines (used when a file defines regions but is compiled standalone).
static std::string stripDocMarkers(const std::string &source) {
    std::istringstream in(source);
    std::string line;
    std::ostringstream out;
    while (std::getline(in, line)) {
        std::string trimmed = trim(line);
        std::string name;
        if (!parseDocMarker(trimmed, name).empty()) continue;
        out << line << "\n";
    }
    return out.str();
}

static std::string parseInline(const std::string &text) {
    InlineParser p(text);
    return p.parse(false);
}

static std::string compile(const std::string &source) {
    std::istringstream in(source);
    std::string line;
    std::ostringstream out;

    std::vector<std::string> paragraphBuf;

    auto flushParagraph = [&]() {
        if (paragraphBuf.empty()) return;
        std::string joined;
        for (size_t i = 0; i < paragraphBuf.size(); i++) {
            if (i) joined += "\n";
            joined += paragraphBuf[i];
        }
        out << "<p>" << parseInline(joined) << "</p>\n";
        paragraphBuf.clear();
    };

    while (std::getline(in, line)) {
        std::string trimmed = trim(line);

        // blank line -> paragraph break
        if (trimmed.empty()) {
            flushParagraph();
            continue;
        }

        // heading: one or more '#' followed by space
        if (trimmed[0] == '#') {
            size_t level = 0;
            while (level < trimmed.size() && trimmed[level] == '#') level++;
            if (level <= 6 && level < trimmed.size() && trimmed[level] == ' ') {
                flushParagraph();
                std::string text = trim(trimmed.substr(level + 1));
                out << "<h" << level << ">" << parseInline(text) << "</h" << level << ">\n";
                continue;
            }
        }

        // fenced code block: ```lang ... ```
        if (trimmed.rfind("```", 0) == 0) {
            flushParagraph();
            std::string lang = trim(trimmed.substr(3));
            std::ostringstream code;
            std::string codeLine;
            bool closed = false;
            while (std::getline(in, codeLine)) {
                if (trim(codeLine).rfind("```", 0) == 0) { closed = true; break; }
                code << codeLine << "\n";
            }
            std::string cls = lang.empty() ? "" : (" class=\"language-" + htmlEscape(lang) + "\"");
            out << "<pre><code" << cls << ">" << htmlEscape(code.str()) << "</code></pre>\n";
            (void)closed;
            continue;
        }

        // sole block-level directive on its own line: emit without <p> wrap
        {
            std::string dname = soleDirectiveName(trimmed);
            if (!dname.empty() && isBlockDirective(dname)) {
                flushParagraph();
                out << parseInline(trimmed) << "\n";
                continue;
            }
        }

        // otherwise: part of a paragraph
        paragraphBuf.push_back(line);
    }
    flushParagraph();

    return out.str();
}

// ---------------------------------------------------------------------
// HTML page wrapper with CodeMirror 6 (CDN, autoloaded highlighting)
// ---------------------------------------------------------------------

static std::string wrapFullPage(const std::string &body, const std::string &title) {
    std::ostringstream out;
    out << "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n";
    out << "<meta charset=\"utf-8\">\n";
    out << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
    out << "<title>" << htmlEscape(title) << "</title>\n";
    // highlight.js: zero-config, auto-detects language from class="language-x"
    out << "<link rel=\"stylesheet\" href=\"https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/styles/github-dark.min.css\">\n";
    out << "<script src=\"https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/highlight.min.js\"></script>\n";
    out << "<script type=\"module\">\n"
           "  import mermaid from 'https://cdn.jsdelivr.net/npm/mermaid@11/dist/mermaid.esm.min.mjs';\n"
           "  mermaid.initialize({ startOnLoad: true });\n"
           "</script>\n";
    out << "<style>\n"
           "body{font-family:system-ui,sans-serif;max-width:800px;margin:2rem auto;padding:0 1rem;line-height:1.6;}\n"
           "pre{padding:1em;border-radius:6px;overflow:auto;}\n"
           "table{border-collapse:collapse;}\n"
           "th,td{border:1px solid #ccc;padding:0.4em 0.8em;}\n"
           "blockquote{border-left:4px solid #ccc;margin:1em 0;padding:0.2em 1em;color:#555;}\n"
           ".admonition{border-left:4px solid #888;margin:1em 0;padding:0.5em 1em;border-radius:4px;background:#f5f5f5;}\n"
           ".admonition-title{font-weight:bold;margin:0 0 0.3em 0;}\n"
           ".admonition-tip{border-color:#2e8b57;background:#eaf7ef;}\n"
           ".admonition-warning{border-color:#cc8400;background:#fff6e5;}\n"
           "hr{border:none;border-top:1px solid #ccc;margin:2em 0;}\n"
           "</style>\n";
    out << "</head>\n<body>\n";
    out << body;
    out << "\n<script>hljs.highlightAll();</script>\n";
    out << "</body>\n</html>\n";
    return out.str();
}

// ---------------------------------------------------------------------
// main
// ---------------------------------------------------------------------

static void printHelp() {
    std::cout <<
        "NotMark - a markdown-like compiler for web pages\n\n"
        "Usage:\n"
        "  notmark input.nm > output.html\n"
        "  notmark input.nm --full > output.html\n"
        "  notmark < input.nm > output.html\n\n"
        "Options:\n"
        "  -f, --full       Wrap output in a complete HTML page (with styling and syntax highlighting)\n"
        "  -h, --help       Show this help message and exit\n"
        "  -v, --version    Show version information and exit\n\n"
        "If no input file is given, NotMark reads from standard input.\n";
}

static void printVersion() {
    std::cout << "notmark version 1.0.0\n";
}

int main(int argc, char **argv) {
    std::string inputFile;
    bool full = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--full" || arg == "-f") {
            full = true;
        } else if (arg == "--help" || arg == "-h") {
            printHelp();
            return 0;
        } else if (arg == "--version" || arg == "-v") {
            printVersion();
            return 0;
        } else {
            inputFile = arg;
        }
    }

    std::string source;
    std::string baseDir = ".";

    if (!inputFile.empty()) {
        std::ifstream f(inputFile);
        if (!f) {
            std::cerr << "notmark: cannot open file: " << inputFile << "\n";
            return 1;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        source = ss.str();
        size_t slash = inputFile.find_last_of('/');
        if (slash != std::string::npos) baseDir = inputFile.substr(0, slash);
    } else {
        std::ostringstream ss;
        ss << std::cin.rdbuf();
        source = ss.str();
    }

    source = resolveIncludes(source, baseDir);
    source = stripDocMarkers(source);

    std::string body = compile(source);

    if (full) {
        std::cout << wrapFullPage(body, "NotMark Document");
    } else {
        std::cout << body;
    }
    return 0;
}
