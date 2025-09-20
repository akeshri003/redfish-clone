#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
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

// Cleanup expired keys from the cache
void cleanupExpiredKeys();

// Memory management functions
size_t estimateMemoryUsage();
void triggerEvictionIfNeeded();
void evictLFUKeys(size_t target_memory);

// AOF functions
void initAOF();
void appendToAOF(const string& command);
void replayAOF();
void fsyncAOF();

// Value: data type for KV store
struct Value {
    string val;
    int64_t ttl_ms;  // TTL in milliseconds since epoch, -1 means no expiration
    uint32_t access_count;  // LFU frequency counter
    int64_t last_access_time;  // Last access time for LFU decay

    Value() : val(""), ttl_ms(-1), access_count(0), last_access_time(0) {};  // Default constructor
    Value(string v, int64_t exp_ms) : val(v), ttl_ms(exp_ms), access_count(0), last_access_time(0) {};
    Value(string v) : val(v), ttl_ms(-1), access_count(0), last_access_time(0) {};
};

// Memory management and AOF structures
struct MemoryStats {
    size_t estimated_memory = 0;
    size_t memory_limit = 100 * 1024 * 1024;  // 100MB default limit
    uint64_t evictions_total = 0;
};

struct AOFConfig {
    bool enabled = false;
    string filename = "redis.aof";
    bool appendfsync_everysec = true;
    int64_t last_fsync_time = 0;
};

