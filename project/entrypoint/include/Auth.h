#ifndef AUTH_H
#define AUTH_H

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <algorithm>
#include <random>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <mutex>
#include <fstream>
#include <cstring>

// ==================== SHA-256 ====================
class SHA256 {
public:
    static std::string hash(const std::string& data) {
        SHA256 ctx;
        ctx.update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        uint8_t digest[32];
        ctx.final(digest);
        std::stringstream ss;
        for (int i = 0; i < 32; ++i)
            ss << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
        return ss.str();
    }

private:
    static constexpr size_t BLOCK_SIZE = 64;
    uint32_t state[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    uint8_t buffer[BLOCK_SIZE] = {};
    size_t buffer_used = 0;
    uint64_t total_len = 0;

    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void processBlock(const uint8_t* block) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (block[i * 4] << 24) | (block[i * 4 + 1] << 16) |
                   (block[i * 4 + 2] << 8) | block[i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
            uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t temp1 = h + S1 + ch + k[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }

    void update(const uint8_t* data, size_t len) {
        total_len += len;
        while (len > 0) {
            size_t space = BLOCK_SIZE - buffer_used;
            size_t take = std::min(space, len);
            memcpy(buffer + buffer_used, data, take);
            buffer_used += take;
            data += take;
            len -= take;
            if (buffer_used == BLOCK_SIZE) {
                processBlock(buffer);
                buffer_used = 0;
            }
        }
    }

    void final(uint8_t* digest) {
        uint64_t bit_len = total_len * 8;
        buffer[buffer_used++] = 0x80;
        if (buffer_used > 56) {
            while (buffer_used < BLOCK_SIZE) buffer[buffer_used++] = 0;
            processBlock(buffer);
            buffer_used = 0;
        }
        while (buffer_used < 56) buffer[buffer_used++] = 0;
        for (int i = 7; i >= 0; --i)
            buffer[56 + i] = (bit_len >> (56 - i * 8)) & 0xFF;
        processBlock(buffer);
        for (int i = 0; i < 8; ++i) {
            digest[i * 4] = (state[i] >> 24) & 0xFF;
            digest[i * 4 + 1] = (state[i] >> 16) & 0xFF;
            digest[i * 4 + 2] = (state[i] >> 8) & 0xFF;
            digest[i * 4 + 3] = state[i] & 0xFF;
        }
    }

    static constexpr uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
};

// ==================== Base64 ====================
class Base64 {
public:
    static std::string encode(const std::string& input) {
        static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        int val = 0, valb = -6;
        for (unsigned char c : input) {
            val = (val << 8) + c;
            valb += 8;
            while (valb >= 0) {
                out.push_back(chars[(val >> valb) & 0x3F]);
                valb -= 6;
            }
        }
        if (valb > -6) out.push_back(chars[((val << 8) >> (valb + 8)) & 0x3F]);
        while (out.size() % 4) out.push_back('=');
        return out;
    }

    static std::string decode(const std::string& input) {
        static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        std::vector<int> T(256, -1);
        for (int i = 0; i < 64; i++) T[chars[i]] = i;

        int val = 0, valb = -8;
        for (unsigned char c : input) {
            if (T[c] == -1) break;
            val = (val << 6) + T[c];
            valb += 6;
            if (valb >= 0) {
                out.push_back(char((val >> valb) & 0xFF));
                valb -= 8;
            }
        }
        return out;
    }
};

// ==================== HMAC-SHA256 ====================
class HMAC {
public:
    static std::string sign(const std::string& key, const std::string& data) {
        const size_t block_size = 64;
        std::string key_block = key;
        if (key_block.size() > block_size)
            key_block = SHA256::hash(key_block);
        key_block.resize(block_size, '\0');

        std::string o_key_pad(block_size, '\0'), i_key_pad(block_size, '\0');
        for (size_t i = 0; i < block_size; ++i) {
            o_key_pad[i] = key_block[i] ^ 0x5c;
            i_key_pad[i] = key_block[i] ^ 0x36;
        }
        return SHA256::hash(o_key_pad + SHA256::hash(i_key_pad + data));
    }
};

// ==================== JWT (упрощённый) ====================
class JWT {
public:
    static std::string generate(const std::string& username, const std::string& secret,
                                int64_t expire_seconds = 3600) {
        auto now = std::chrono::system_clock::now();
        auto exp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count() + expire_seconds;

        std::stringstream header_ss, payload_ss;
        header_ss << "alg=HS256,typ=JWT";
        payload_ss << "user=" << username << ",exp=" << exp;

        std::string header = Base64::encode(header_ss.str());
        std::string payload = Base64::encode(payload_ss.str());
        std::string signature = Base64::encode(HMAC::sign(secret, header + "." + payload));
        return header + "." + payload + "." + signature;
    }

    static std::string validate(const std::string& token, const std::string& secret) {
        auto parts = split(token, '.');
        if (parts.size() != 3) return "";

        std::string header = parts[0], payload = parts[1], signature = parts[2];
        std::string expected_sig = Base64::encode(HMAC::sign(secret, header + "." + payload));
        if (expected_sig != signature) return "";

        std::string decoded_payload = Base64::decode(payload);
        auto kv = parseKeyValue(decoded_payload);
        int64_t exp = 0;
        std::string user;
        for (const auto& p : kv) {
            if (p.first == "exp") exp = std::stoll(p.second);
            else if (p.first == "user") user = p.second;
        }
        auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch()).count();
        if (now > exp) return "";
        return user;
    }

private:
    static std::vector<std::string> split(const std::string& s, char delim) {
        std::vector<std::string> res;
        std::stringstream ss(s);
        std::string item;
        while (std::getline(ss, item, delim)) res.push_back(item);
        return res;
    }

    static std::vector<std::pair<std::string, std::string>> parseKeyValue(const std::string& str) {
        std::vector<std::pair<std::string, std::string>> result;
        std::stringstream ss(str);
        std::string item;
        while (std::getline(ss, item, ',')) {
            auto eq = item.find('=');
            if (eq != std::string::npos) {
                result.emplace_back(item.substr(0, eq), item.substr(eq + 1));
            }
        }
        return result;
    }
};

// ==================== Права и операции ====================
enum class Operation : uint8_t {
    READ             = 1 << 0,
    WRITE            = 1 << 1,
    CREATE_TABLE     = 1 << 2,
    DROP_TABLE       = 1 << 3,
    DELETE_DATABASE  = 1 << 4,
    CREATE_DATABASE  = 1 << 5,
    ADMIN            = 1 << 6
};

// ==================== Учётные записи ====================
struct User {
    std::string username;
    std::string password_hash;
    std::string salt;
    std::vector<std::string> groups;
    std::unordered_map<std::string, uint8_t> permissions;
};

struct Group {
    std::string name;
    std::unordered_map<std::string, uint8_t> permissions;
};

// ==================== AuthManager с персистентностью ====================
class AuthManager {
public:
    AuthManager(const std::string& storageFile = "auth.data") 
        : storageFile_(storageFile)
    {
        if (!load()) {
            // Первый запуск: генерируем секрет и создаём администратора по умолчанию
            std::lock_guard<std::mutex> lock(mutex_);
            generateSecret();
            addGroupInternal("admin");
            createUserInternal("admin", "admin", true);
            save();
        }
    }

    bool createUser(const std::string& username, const std::string& password, bool isAdmin = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (users_.count(username)) return false;
        createUserInternal(username, password, isAdmin);
        save();
        return true;
    }

    bool addUserToGroup(const std::string& username, const std::string& groupname) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!users_.count(username) || !groups_.count(groupname)) return false;
        auto& grps = users_[username].groups;
        if (std::find(grps.begin(), grps.end(), groupname) == grps.end()) {
            grps.push_back(groupname);
            save();
            return true;
        }
        return false;
    }

    bool addGroup(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (groups_.count(name)) return false;
        addGroupInternal(name);
        save();
        return true;
    }

    void setDefaultDBPermissions(const std::string& db, uint8_t flags) {
        std::lock_guard<std::mutex> lock(mutex_);
        default_db_permissions_[db] = flags;
        save();
    }

    void setGroupDBPermissions(const std::string& groupname, const std::string& db, uint8_t flags) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!groups_.count(groupname)) return;
        groups_[groupname].permissions[db] = flags;
        save();
    }

    void setUserDBPermissions(const std::string& username, const std::string& db, uint8_t flags) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!users_.count(username)) return;
        users_[username].permissions[db] = flags;
        save();
    }

    std::string login(const std::string& username, const std::string& password) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = users_.find(username);
        if (it == users_.end()) return "";
        const User& u = it->second;
        if (hashPassword(password, u.salt) != u.password_hash) return "";
        return JWT::generate(username, secret_);
    }

    std::string validateToken(const std::string& token) {
        return JWT::validate(token, secret_);
    }

    bool isAdmin(const std::string& username) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = users_.find(username);
        if (it == users_.end()) return false;
        return std::find(it->second.groups.begin(), it->second.groups.end(), "admin") != it->second.groups.end();
    }

    bool checkPermission(const std::string& username, const std::string& db, Operation op) {
        if (op == Operation::ADMIN) return isAdmin(username);
        if (op == Operation::CREATE_DATABASE) {
            // Только администраторы могут создавать БД (можно расширить)
            return isAdmin(username);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        auto user_it = users_.find(username);
        if (user_it == users_.end()) return false;
        const User& user = user_it->second;
        uint8_t required = static_cast<uint8_t>(op);

        // Личные права пользователя
        auto perm_it = user.permissions.find(db);
        if (perm_it != user.permissions.end() && (perm_it->second & required)) return true;

        // Права групп
        for (const std::string& grp : user.groups) {
            auto grp_it = groups_.find(grp);
            if (grp_it != groups_.end()) {
                auto p = grp_it->second.permissions.find(db);
                if (p != grp_it->second.permissions.end() && (p->second & required)) return true;
            }
        }

        // Права по умолчанию
        auto def = default_db_permissions_.find(db);
        if (def != default_db_permissions_.end() && (def->second & required)) return true;

        return false;
    }

    // ---------- Новые методы для команд управления ----------
    std::string listUsers() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::stringstream ss;
        for (const auto& [name, user] : users_) {
            ss << name;
            if (std::find(user.groups.begin(), user.groups.end(), "admin") != user.groups.end())
                ss << " (admin)";
            ss << "\n";
        }
        return ss.str();
    }

    std::string listGroups() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::stringstream ss;
        for (const auto& [name, grp] : groups_) ss << name << "\n";
        return ss.str();
    }

    std::string getUserPermissions(const std::string& username) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = users_.find(username);
        if (it == users_.end()) return "User not found\n";
        const User& u = it->second;
        std::stringstream ss;
        ss << "User: " << username << "\nGroups: ";
        for (size_t i = 0; i < u.groups.size(); ++i) {
            if (i) ss << ", ";
            ss << u.groups[i];
        }
        ss << "\nPersonal permissions:\n";
        for (const auto& [db, flags] : u.permissions) {
            ss << "  " << db << ": " << (int)flags << "\n";
        }
        for (const auto& grp_name : u.groups) {
            auto git = groups_.find(grp_name);
            if (git != groups_.end()) {
                ss << "Group " << grp_name << " permissions:\n";
                for (const auto& [db, flags] : git->second.permissions) {
                    ss << "  " << db << ": " << (int)flags << "\n";
                }
            }
        }
        return ss.str();
    }

    std::string getGroupPermissions(const std::string& groupname) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = groups_.find(groupname);
        if (it == groups_.end()) return "Group not found\n";
        const Group& g = it->second;
        std::stringstream ss;
        ss << "Group: " << groupname << "\nPermissions:\n";
        for (const auto& [db, flags] : g.permissions) {
            ss << "  " << db << ": " << (int)flags << "\n";
        }
        return ss.str();
    }

private:
    std::string storageFile_;
    std::unordered_map<std::string, User> users_;
    std::unordered_map<std::string, Group> groups_;
    std::unordered_map<std::string, uint8_t> default_db_permissions_;
    std::string secret_;
    mutable std::mutex mutex_;

    // ---------- вспомогательные методы (без блокировки) ----------
    void generateSecret() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 255);
        std::stringstream ss;
        for (int i = 0; i < 32; ++i) 
            ss << std::hex << std::setw(2) << std::setfill('0') << dis(gen);
        secret_ = ss.str();
    }

    std::string generateSalt(size_t length = 16) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(33, 126);
        std::string salt;
        for (size_t i = 0; i < length; ++i) salt += static_cast<char>(dis(gen));
        return salt;
    }

    std::string hashPassword(const std::string& password, const std::string& salt) {
        return SHA256::hash(salt + password);
    }

    // Внутренние функции (вызываются при уже захваченном mutex)
    void addGroupInternal(const std::string& name) {
        groups_[name] = Group{name, {}};
    }

    void createUserInternal(const std::string& username, const std::string& password, bool isAdmin) {
        User u;
        u.username = username;
        u.salt = generateSalt();
        u.password_hash = hashPassword(password, u.salt);
        if (isAdmin) u.groups.push_back("admin");
        users_[username] = u;
    }

    // ---------- персистентность ----------
    bool load() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ifstream file(storageFile_);
        if (!file.is_open()) return false;

        users_.clear();
        groups_.clear();
        default_db_permissions_.clear();

        std::string line;
        while (std::getline(file, line)) {
            if (line.empty()) continue;
            std::istringstream iss(line);
            std::string type;
            std::getline(iss, type, ':');

            if (type == "SECRET") {
                std::getline(iss, secret_);
            }
            else if (type == "USER") {
                User u;
                std::string field;
                std::getline(iss, u.username, ':');
                std::getline(iss, u.password_hash, ':');
                std::getline(iss, u.salt, ':');
                std::getline(iss, field, ':'); // groups (comma-separated)
                std::istringstream gs(field);
                std::string g;
                while (std::getline(gs, g, ',')) {
                    if (!g.empty()) u.groups.push_back(g);
                }
                std::getline(iss, field); // permissions
                if (!field.empty()) {
                    std::istringstream ps(field);
                    std::string perm;
                    while (std::getline(ps, perm, ',')) {
                        auto eq = perm.find('=');
                        if (eq != std::string::npos) {
                            std::string db = perm.substr(0, eq);
                            uint8_t flags = std::stoi(perm.substr(eq+1));
                            u.permissions[db] = flags;
                        }
                    }
                }
                users_[u.username] = u;
            }
            else if (type == "GROUP") {
                Group g;
                std::string field;
                std::getline(iss, g.name, ':');
                std::getline(iss, field);
                if (!field.empty()) {
                    std::istringstream ps(field);
                    std::string perm;
                    while (std::getline(ps, perm, ',')) {
                        auto eq = perm.find('=');
                        if (eq != std::string::npos) {
                            std::string db = perm.substr(0, eq);
                            uint8_t flags = std::stoi(perm.substr(eq+1));
                            g.permissions[db] = flags;
                        }
                    }
                }
                groups_[g.name] = g;
            }
            else if (type == "DEFAULT") {
                std::string field;
                std::getline(iss, field);
                auto eq = field.find('=');
                if (eq != std::string::npos) {
                    std::string db = field.substr(0, eq);
                    uint8_t flags = std::stoi(field.substr(eq+1));
                    default_db_permissions_[db] = flags;
                }
            }
        }
        return !secret_.empty();
    }

    void save() {
        // Вызывается только под захваченным mutex_
        std::ofstream file(storageFile_);
        if (!file.is_open()) return;

        file << "SECRET:" << secret_ << "\n";

        for (const auto& [name, user] : users_) {
            file << "USER:" << name << ":" << user.password_hash << ":" << user.salt << ":";
            for (size_t i = 0; i < user.groups.size(); ++i) {
                if (i > 0) file << ",";
                file << user.groups[i];
            }
            file << ":";
            bool first = true;
            for (const auto& [db, flags] : user.permissions) {
                if (!first) file << ",";
                file << db << "=" << (int)flags;
                first = false;
            }
            file << "\n";
        }

        for (const auto& [name, group] : groups_) {
            file << "GROUP:" << name << ":";
            bool first = true;
            for (const auto& [db, flags] : group.permissions) {
                if (!first) file << ",";
                file << db << "=" << (int)flags;
                first = false;
            }
            file << "\n";
        }

        for (const auto& [db, flags] : default_db_permissions_) {
            file << "DEFAULT:" << db << "=" << (int)flags << "\n";
        }
    }
};

#endif // AUTH_H