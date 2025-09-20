#include "resp_types.h"
#include <chrono>
#include <ctime>

// ===================== Dispatcher =====================
// Very small in-memory KV store for SET/GET/DEL
static unordered_map<string, Value> kv_cache;
// Efficient expiration tracking: maps key -> expire time in milliseconds
static unordered_map<string, int64_t> expires;

static string toUpperASCII(string s) {
    for (char &c : s) c = (char)toupper((unsigned char)c);
    return s;
}

static RespValue makeCommandError(const string &msg) {
    return makeError("ERR " + msg);
}

// Get current time in milliseconds since epoch using monotonic clock
static int64_t getCurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Check if a value has expired
static bool isExpired(const Value& val) {
    if (val.ttl_ms == -1) return false; // No TTL set
    return getCurrentTimeMs() >= val.ttl_ms;
}

// Clean up expired keys from store using efficient expiration tracking
void cleanupExpiredKeys() {
    int64_t now = getCurrentTimeMs();
    auto it = expires.begin();
    while (it != expires.end()) {
        if (now >= it->second) {
            // Key has expired, remove from both maps
            kv_cache.erase(it->first);
            it = expires.erase(it);
        } else {
            ++it;
        }
    }
}

// Expect command as RESP Array of Bulk Strings
RespValue dispatchCommand(const RespValue &cmd) {
    if (cmd.type != Type::Array || cmd.is_null) return makeCommandError("protocol error: expected array");
    if (cmd.array.empty()) return makeCommandError("missing command");
    // Ensure all elements are bulk strings
    vector<string> args;
    args.reserve(cmd.array.size());
    for (const auto &e : cmd.array) {
        if (e.type != Type::BulkString || e.is_null) return makeCommandError("arguments must be bulk strings");
        args.push_back(e.buf);
    }
    string op = toUpperASCII(args[0]);

    if (op == "PING") {
        if (args.size() == 1) return makeSimpleString("PONG");
        if (args.size() == 2) return makeBulkString(args[1]);
        return makeCommandError("wrong number of arguments for 'PING'");
    }

    if (op == "ECHO") {
        if (args.size() != 2) return makeCommandError("wrong number of arguments for 'ECHO'");
        return makeBulkString(args[1]);
    }

    if (op == "SET") {
        if (args.size() < 3) return makeCommandError("wrong number of arguments for 'SET'");
        
        string key = args[1];
        string value = args[2];
        int64_t ttl_ms = -1; // Default: no expiration
        
        // Parse TTL options (EX, PX)
        for (size_t i = 3; i < args.size(); i += 2) {
            if (i + 1 >= args.size()) {
                return makeCommandError("syntax error");
            }
            
            string option = toUpperASCII(args[i]);
            string ttlStr = args[i + 1];
            
            try {
                int64_t ttlValue = stoll(ttlStr);
                if (ttlValue <= 0) {
                    return makeCommandError("invalid expire time");
                }
                
                int64_t now_ms = getCurrentTimeMs();
                if (option == "EX") {
                    // EX: expire time in seconds
                    ttl_ms = now_ms + (ttlValue * 1000);
                } else if (option == "PX") {
                    // PX: expire time in milliseconds
                    ttl_ms = now_ms + ttlValue;
                } else {
                    return makeCommandError("unknown option for SET");
                }
            } catch (const std::exception&) {
                return makeCommandError("value is not an integer or out of range");
            }
        }
        
        // Store the value
        kv_cache[key] = Value(value, ttl_ms);
        
        // Update expiration tracking
        if (ttl_ms != -1) {
            expires[key] = ttl_ms;
        } else {
            expires.erase(key); // Remove from expiration tracking if no TTL
        }
        
        return makeSimpleString("OK");
    }

    if (op == "GET") {
        if (args.size() != 2) return makeCommandError("wrong number of arguments for 'GET'");
        auto it = kv_cache.find(args[1]);
        if (it == kv_cache.end()) return makeNullBulkString();
        
        // Check if the value has expired using efficient expiration tracking
        auto exp_it = expires.find(args[1]);
        if (exp_it != expires.end() && getCurrentTimeMs() >= exp_it->second) {
            // Key has expired, remove from both maps
            kv_cache.erase(it);
            expires.erase(exp_it);
            return makeNullBulkString();
        }
        
        return makeBulkString(it->second.val);
    }

    if (op == "DEL") {
        if (args.size() < 2) return makeCommandError("wrong number of arguments for 'DEL'");
        long long removed = 0;
        for (size_t i = 1; i < args.size(); ++i) {
            auto it = kv_cache.find(args[i]);
            if (it != kv_cache.end()) {
                // Check if expired before counting as removed using efficient expiration tracking
                auto exp_it = expires.find(args[i]);
                bool expired = (exp_it != expires.end() && getCurrentTimeMs() >= exp_it->second);
                
                // Remove from both maps
                kv_cache.erase(it);
                if (exp_it != expires.end()) {
                    expires.erase(exp_it);
                }
                
                // Only count as removed if not expired
                if (!expired) {
                    removed++;
                }
            }
        }
        return makeInteger(removed);
    }

    return makeError("ERR unknown command '" + args[0] + "'");
}

