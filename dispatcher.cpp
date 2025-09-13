#include "resp_types.h"
#include <chrono>
#include <ctime>

// ===================== Dispatcher =====================
// Very small in-memory KV store for SET/GET/DEL
static unordered_map<string, Value> g_store;

static string toUpperASCII(string s) {
    for (char &c : s) c = (char)toupper((unsigned char)c);
    return s;
}

static RespValue makeCommandError(const string &msg) {
    return makeError("ERR " + msg);
}

// Get current time in seconds since epoch
static int getCurrentTime() {
    return static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

// Check if a value has expired
static bool isExpired(const Value& val) {
    if (val.ttl == -1) return false; // No TTL set
    return getCurrentTime() >= val.ttl;
}

// Clean up expired keys from store
static void cleanupExpiredKeys() {
    auto it = g_store.begin();
    while (it != g_store.end()) {
        if (isExpired(it->second)) {
            it = g_store.erase(it);
        } else {
            ++it;
        }
    }
}

// Expect command as RESP Array of Bulk Strings
RespValue dispatchCommand(const RespValue &cmd) {
    // Clean up expired keys periodically
    cleanupExpiredKeys();
    
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
        int ttl = -1; // Default: no expiration
        
        // Parse TTL options (EX, PX)
        for (size_t i = 3; i < args.size(); i += 2) {
            if (i + 1 >= args.size()) {
                return makeCommandError("syntax error");
            }
            
            string option = toUpperASCII(args[i]);
            string ttlStr = args[i + 1];
            
            try {
                int ttlValue = stoi(ttlStr);
                if (ttlValue <= 0) {
                    return makeCommandError("invalid expire time");
                }
                
                if (option == "EX") {
                    // EX: expire time in seconds
                    ttl = getCurrentTime() + ttlValue;
                } else if (option == "PX") {
                    // PX: expire time in milliseconds
                    ttl = getCurrentTime() + (ttlValue / 1000);
                } else {
                    return makeCommandError("unknown option for SET");
                }
            } catch (const std::exception&) {
                return makeCommandError("value is not an integer or out of range");
            }
        }
        
        g_store[key] = Value(value, ttl);
        return makeSimpleString("OK");
    }

    if (op == "GET") {
        if (args.size() != 2) return makeCommandError("wrong number of arguments for 'GET'");
        auto it = g_store.find(args[1]);
        if (it == g_store.end()) return makeNullBulkString();
        
        // Check if the value has expired
        if (isExpired(it->second)) {
            g_store.erase(it);
            return makeNullBulkString();
        }
        
        return makeBulkString(it->second.val);
    }

    if (op == "DEL") {
        if (args.size() < 2) return makeCommandError("wrong number of arguments for 'DEL'");
        long long removed = 0;
        for (size_t i = 1; i < args.size(); ++i) {
            auto it = g_store.find(args[i]);
            if (it != g_store.end()) {
                // Check if expired before counting as removed
                if (isExpired(it->second)) {
                    g_store.erase(it);
                } else {
                    g_store.erase(it);
                    removed++;
                }
            }
        }
        return makeInteger(removed);
    }

    return makeError("ERR unknown command '" + args[0] + "'");
}

