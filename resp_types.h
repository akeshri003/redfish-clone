#include <stdlib.h>
#include <bits/stdc++.h>
#include <cstring>
#include <optional>

using namespace std;


enum class Type {
    SimpleString, Error, Integer, BulkString, Array, Null
};


struct RespValue {
    Type type;
    string buf;
    long long integer = 0;
    vector<RespValue> array;
    bool is_null = false;
};

// Streaming-friendly RESP parsing API
// Attempts to parse a single RESP value from data starting at offset 0.
// On success, returns true, writes the parsed value to out, and sets consumed to
// the number of bytes consumed. On incomplete input, returns false with empty
// error_message and consumed left unmodified. On protocol error, returns false
// and sets error_message.
bool tryParseRespMessage(const string &data, size_t &consumed, RespValue &out, string &error_message);

// Convenience: parse a single RESP value from an entire buffer.
// Returns true only if a full value is parsed. consumed will indicate bytes
// consumed (may be less than data.size() if trailing bytes exist).
bool parseResp(const string &data, RespValue &out, size_t &consumed, string &error_message);

// RESP value constructors
RespValue makeSimpleString(const string &s);
RespValue makeError(const string &s);
RespValue makeInteger(long long v);
RespValue makeBulkString(const string &s);
RespValue makeNullBulkString();
RespValue makeArray(const vector<RespValue> &elems);
RespValue makeNullArray();

// Serializer: encodes a RespValue into a string
string serializeResp(const RespValue &v);

// Dispatcher: execute a RESP Array command and return a RESP response
RespValue dispatchCommand(const RespValue &cmd);

// Value: data type for KV store
struct Value {
    string val;
    int ttl;

    Value(string v, int exp) : val(v), ttl(exp) {};
    Value(string v) : val(v), ttl(-1) {};
};

