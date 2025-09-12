#include "resp_types.h"

// Helpers
static bool parseCRLF(const string &s, size_t pos) {
    return pos + 1 < s.size() && s[pos] == '\r' && s[pos + 1] == '\n';
}

static bool readLine(const string &s, size_t start, size_t &next, string &line) {
    // Find CRLF starting from start; return substring without CRLF
    size_t cr = s.find("\r\n", start);
    if (cr == string::npos) return false; // need more data
    line.assign(s.data() + start, cr - start);
    next = cr + 2; // skip CRLF
    return true;
}

static bool parseInteger(const string &s, size_t start, size_t &next, long long &val) {
    string line;
    if (!readLine(s, start, next, line)) return false;
    if (line.empty()) return false;
    // Allow optional leading +/-
    size_t idx = 0;
    bool neg = false;
    if (line[idx] == '+' || line[idx] == '-') { neg = (line[idx] == '-'); idx++; }
    if (idx >= line.size()) return false;
    long long num = 0;
    for (; idx < line.size(); ++idx) {
        char c = line[idx];
        if (c < '0' || c > '9') return false;
        num = num * 10 + (c - '0');
    }
    val = neg ? -num : num;
    return true;
}

static bool parseBulkString(const string &s, size_t start, size_t &next, RespValue &out, string &err) {
    long long len = 0;
    size_t after_len = 0;
    if (!parseInteger(s, start, after_len, len)) {
        return false; // incomplete or invalid length; we treat invalid later if we can detect
    }
    if (len < -1) { err = "Invalid bulk string length"; return false; }
    if (len == -1) {
        out = RespValue{Type::BulkString};
        out.is_null = true;
        out.buf.clear();
        next = after_len;
        return true;
    }
    size_t needed = static_cast<size_t>(len);
    if (after_len + needed + 2 > s.size()) return false; // need more
    out = RespValue{Type::BulkString};
    out.is_null = false;
    out.buf.assign(s.data() + after_len, needed);
    if (!parseCRLF(s, after_len + needed)) { err = "Bulk string missing CRLF"; return false; }
    next = after_len + needed + 2;
    return true;
}

static bool parseArray(const string &s, size_t start, size_t &next, RespValue &out, string &err) {
    long long count = 0;
    size_t after_count = 0;
    if (!parseInteger(s, start, after_count, count)) return false;
    if (count < -1) { err = "Invalid array length"; return false; }
    if (count == -1) {
        out = RespValue{Type::Array};
        out.is_null = true;
        next = after_count;
        return true;
    }
    out = RespValue{Type::Array};
    out.is_null = false;
    out.array.clear();
    out.array.reserve(static_cast<size_t>(count));

    size_t cursor = after_count;
    for (long long i = 0; i < count; ++i) {
        if (cursor >= s.size()) return false; // need more data
        RespValue elem;
        size_t consumed_elem = 0;
        string parse_err;
        size_t tmpConsumed = 0;
        // Recurse using public API to leverage type dispatch
        if (!tryParseRespMessage(string(s.data() + cursor, s.size() - cursor), tmpConsumed, elem, parse_err)) {
            if (!parse_err.empty()) { err = parse_err; }
            return false;
        }
        consumed_elem = tmpConsumed;
        out.array.push_back(std::move(elem));
        cursor += consumed_elem;
    }
    next = cursor;
    return true;
}

bool tryParseRespMessage(const string &data, size_t &consumed, RespValue &out, string &error_message) {
    error_message.clear();
    consumed = 0;
    if (data.empty()) return false;

    char prefix = data[0];
    size_t next = 1; // position after prefix for line-based types
    switch (prefix) {
        case '+': {
            string line;
            if (!readLine(data, next, next, line)) return false;
            out = RespValue{Type::SimpleString};
            out.buf = line;
            consumed = next;
            return true;
        }
        case '-': {
            string line;
            if (!readLine(data, next, next, line)) return false;
            out = RespValue{Type::Error};
            out.buf = line;
            consumed = next;
            return true;
        }
        case ':': {
            long long v = 0;
            if (!parseInteger(data, next, next, v)) return false;
            out = RespValue{Type::Integer};
            out.integer = v;
            consumed = next;
            return true;
        }
        case '$': {
            size_t after = 0;
            string err;
            if (!parseBulkString(data, next, after, out, err)) {
                if (!err.empty()) { error_message = err; }
                return false;
            }
            consumed = after;
            return true;
        }
        case '*': {
            size_t after = 0;
            string err;
            if (!parseArray(data, next, after, out, err)) {
                if (!err.empty()) { error_message = err; }
                return false;
            }
            consumed = after;
            return true;
        }
        default:
            error_message = "Unknown RESP type prefix";
            return false;
    }
}

bool parseResp(const string &data, RespValue &out, size_t &consumed, string &error_message) {
    return tryParseRespMessage(data, consumed, out, error_message);
}

