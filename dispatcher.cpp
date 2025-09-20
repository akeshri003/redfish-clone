#include "resp_types.h"
#include <chrono>
#include <ctime>
#include <cctype>
#include <unordered_map>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <vector>

// ===================== Dispatcher =====================
// Very small in-memory KV store for SET/GET/DEL
static unordered_map<string, Value> kv_cache;
// Efficient expiration tracking: maps key -> expire time in milliseconds
static unordered_map<string, int64_t> expires;
// Memory management and AOF
static MemoryStats mem_stats;
static AOFConfig aof_config;
static ofstream aof_file;

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

// Estimate memory usage of the cache
size_t estimateMemoryUsage() {
    size_t total = 0;
    for (const auto& pair : kv_cache) {
        // Key size + value size + overhead
        total += pair.first.size() + pair.second.val.size() + sizeof(Value) + 32; // 32 bytes overhead
    }
    return total;
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

// LFU eviction: evict keys with lowest frequency
void evictLFUKeys(size_t target_memory) {
    // Create vector of key-value pairs for sorting
    vector<pair<string, Value*>> items;
    for (auto& pair : kv_cache) {
        items.push_back({pair.first, &pair.second});
    }
    
    // Sort by access count (LFU)
    sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
        return a.second->access_count < b.second->access_count;
    });
    
    // Evict keys until we reach target memory
    size_t current_memory = estimateMemoryUsage();
    for (const auto& item : items) {
        if (current_memory <= target_memory) break;
        
        const string& key = item.first;
        kv_cache.erase(key);
        expires.erase(key);
        mem_stats.evictions_total++;
        
        current_memory = estimateMemoryUsage();
    }
}

// Trigger eviction if memory usage exceeds limit
void triggerEvictionIfNeeded() {
    mem_stats.estimated_memory = estimateMemoryUsage();
    if (mem_stats.estimated_memory > mem_stats.memory_limit) {
        // Evict to 80% of limit
        size_t target = mem_stats.memory_limit * 0.8;
        evictLFUKeys(target);
        mem_stats.estimated_memory = estimateMemoryUsage();
    }
}

// AOF functions
void initAOF() {
    if (aof_config.enabled) {
        aof_file.open(aof_config.filename, ios::app);
        if (!aof_file.is_open()) {
            cerr << "Failed to open AOF file: " << aof_config.filename << endl;
            aof_config.enabled = false;
        }
    }
}

void appendToAOF(const string& command) {
    if (aof_config.enabled && aof_file.is_open()) {
        aof_file << command;
        aof_file.flush();
        
        // Check if we need to fsync
        if (aof_config.appendfsync_everysec) {
            int64_t now = getCurrentTimeMs();
            if (now - aof_config.last_fsync_time >= 1000) { // 1 second
                fsyncAOF();
                aof_config.last_fsync_time = now;
            }
        }
    }
}

void fsyncAOF() {
    if (aof_config.enabled && aof_file.is_open()) {
        aof_file.flush();
        // Note: In a real implementation, you'd call fsync() on the file descriptor
        // For simplicity, we just flush the stream
    }
}

void replayAOF() {
    if (!aof_config.enabled) return;
    
    ifstream aof_read(aof_config.filename);
    if (!aof_read.is_open()) {
        cout << "No AOF file found, starting fresh" << endl;
        return;
    }
    
    cout << "Replaying AOF file..." << endl;
    string line;
    string buffer;
    
    while (getline(aof_read, line)) {
        buffer += line + "\n";
        
        // Try to parse complete RESP messages
        size_t consumed = 0;
        while (consumed < buffer.size()) {
            RespValue cmd;
            string error;
            size_t msg_consumed = 0;
            
            if (tryParseRespMessage(buffer.substr(consumed), msg_consumed, cmd, error)) {
                // Execute the command silently (don't append to AOF during replay)
                bool old_aof_enabled = aof_config.enabled;
                aof_config.enabled = false;
                dispatchCommand(cmd);
                aof_config.enabled = old_aof_enabled;
                
                consumed += msg_consumed;
            } else {
                break; // Need more data
            }
        }
        
        if (consumed > 0) {
            buffer.erase(0, consumed);
        }
    }
    
    cout << "AOF replay completed" << endl;
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
        
        // Check memory limit and trigger eviction if needed
        triggerEvictionIfNeeded();
        
        // Store the value with LFU tracking
        int64_t now_ms = getCurrentTimeMs();
        Value new_value(value, ttl_ms);
        new_value.last_access_time = now_ms;
        new_value.access_count = 1; // Initial access count
        
        kv_cache[key] = new_value;
        
        // Update expiration tracking
        if (ttl_ms != -1) {
            expires[key] = ttl_ms;
        } else {
            expires.erase(key); // Remove from expiration tracking if no TTL
        }
        
        // Append to AOF
        string aof_cmd = serializeResp(cmd);
        appendToAOF(aof_cmd);
        
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
        
        // Update LFU access tracking
        int64_t now_ms = getCurrentTimeMs();
        it->second.access_count++;
        it->second.last_access_time = now_ms;
        
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
        
        // Append to AOF
        string aof_cmd = serializeResp(cmd);
        appendToAOF(aof_cmd);
        
        return makeInteger(removed);
    }

    // Configuration commands
    if (op == "CONFIG") {
        if (args.size() < 2) return makeCommandError("wrong number of arguments for 'CONFIG'");
        
        string subcmd = toUpperASCII(args[1]);
        if (subcmd == "SET") {
            if (args.size() != 4) return makeCommandError("wrong number of arguments for 'CONFIG SET'");
            
            string param = toUpperASCII(args[2]);
            string value = args[3];
            
            if (param == "MAXMEMORY") {
                try {
                    size_t limit = stoull(value);
                    mem_stats.memory_limit = limit;
                    return makeSimpleString("OK");
                } catch (const std::exception&) {
                    return makeCommandError("invalid memory limit value");
                }
            } else if (param == "APPENDFSYNC") {
                if (value == "everysec") {
                    aof_config.appendfsync_everysec = true;
                    return makeSimpleString("OK");
                } else if (value == "no") {
                    aof_config.appendfsync_everysec = false;
                    return makeSimpleString("OK");
                } else {
                    return makeCommandError("invalid appendfsync value");
                }
            } else {
                return makeCommandError("unknown configuration parameter");
            }
        } else if (subcmd == "GET") {
            if (args.size() != 3) return makeCommandError("wrong number of arguments for 'CONFIG GET'");
            
            string param = toUpperASCII(args[2]);
            if (param == "MAXMEMORY") {
                return makeArray({makeBulkString("maxmemory"), makeBulkString(to_string(mem_stats.memory_limit))});
            } else if (param == "APPENDFSYNC") {
                string sync_mode = aof_config.appendfsync_everysec ? "everysec" : "no";
                return makeArray({makeBulkString("appendfsync"), makeBulkString(sync_mode)});
            } else {
                return makeCommandError("unknown configuration parameter");
            }
        } else {
            return makeCommandError("unknown CONFIG subcommand");
        }
    }
    
    if (op == "AOF") {
        if (args.size() != 2) return makeCommandError("wrong number of arguments for 'AOF'");
        
        string subcmd = toUpperASCII(args[1]);
        if (subcmd == "ENABLE") {
            aof_config.enabled = true;
            initAOF();
            return makeSimpleString("OK");
        } else if (subcmd == "DISABLE") {
            aof_config.enabled = false;
            if (aof_file.is_open()) {
                aof_file.close();
            }
            return makeSimpleString("OK");
        } else {
            return makeCommandError("unknown AOF subcommand");
        }
    }
    
    if (op == "INFO") {
        if (args.size() == 1) {
            // Return basic info
            string info = "used_memory:" + to_string(mem_stats.estimated_memory) + "\r\n";
            info += "maxmemory:" + to_string(mem_stats.memory_limit) + "\r\n";
            info += "evicted_keys:" + to_string(mem_stats.evictions_total) + "\r\n";
            info += "aof_enabled:" + string(aof_config.enabled ? "1" : "0") + "\r\n";
            return makeBulkString(info);
        }
    }

    return makeError("ERR unknown command '" + args[0] + "'");
}

