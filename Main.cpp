#define _WIN32_WINNT 0x0A00
#define _SILENCE_CXX20_CODECVT_FACETS_DEPRECATION_WARNING

#pragma warning(push, 0)
#pragma warning(disable : 4244 4267)
#include "crow_all.h"
#include "sqlite_modern_cpp.h"
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <iomanip>
#pragma warning(pop)

#include <iostream>
#include <sstream>
#include <fstream>
#include <string>
#include <vector>
#include <stdexcept>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <ctime>
#include <windows.h>
#include <shellapi.h>

// ----------------- Helpers (hash / hex / time / tokens) -----------------

static std::string to_hex(const unsigned char* data, size_t len) {
    std::ostringstream oss;
    for (size_t i = 0; i < len; ++i)
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)data[i];
    return oss.str();
}

static std::string sha256_hex(const std::string& input) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), hash);
    return to_hex(hash, SHA256_DIGEST_LENGTH);
}

static std::string make_salt(size_t bytes = 16) {
    std::vector<unsigned char> buf(bytes);
    if (RAND_bytes(buf.data(), static_cast<int>(buf.size())) != 1)
        throw std::runtime_error("RAND_bytes failed");
    return to_hex(buf.data(), buf.size());
}

static std::string salted_hash(const std::string& salt_hex, const std::string& password) {
    return sha256_hex(salt_hex + ":" + password);
}

// "YYYY-MM-DD HH:MM:SS" in UTC (lexicographically comparable)
static std::string now_utc_str() {
    using namespace std::chrono;
    auto now = system_clock::now();
    std::time_t tt = system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_s(&tm, &tt);
    char buf[20];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return std::string(buf);
}

static std::string random_token_hex(size_t bytes = 32) {
    std::vector<unsigned char> buf(bytes);
    if (RAND_bytes(buf.data(), static_cast<int>(buf.size())) != 1)
        throw std::runtime_error("RAND_bytes failed");
    return to_hex(buf.data(), buf.size());
}

// ----------------- Very simple token store (in-memory) ------------------

struct Session {
    std::string username;
    std::string role;
    std::string issued_at;
};

static std::unordered_map<std::string, Session> g_sessions;
static std::mutex g_sessions_mx;

static std::string get_token_from(const crow::request& req) {
    // Prefer "Authorization: Bearer <token>"
    auto it = req.headers.find("Authorization");
    if (it != req.headers.end()) {
        std::string v = it->second;
        const std::string p = "Bearer ";
        if (v.rfind(p, 0) == 0) return v.substr(p.size());
    }
    // Fallback: ?token=...
    auto q = crow::query_string(req.url_params);
    if (auto t = req.url_params.get("token")) return std::string(t);
    return {};
}

static bool auth(const crow::request& req, Session& out) {
    std::lock_guard<std::mutex> lock(g_sessions_mx);
    auto token = get_token_from(req);
    if (token.empty()) return false;
    auto it = g_sessions.find(token);
    if (it == g_sessions.end()) return false;
    out = it->second;
    return true;
}

// ----------------- Access logic helpers -----------------

struct DbAccess {
    bool canSeeDb = false;          // can open Database tab at all
    bool customersOnly = false;     // if true, worker can only see Customers
};

static DbAccess compute_db_access(sqlite::database& db, const std::string& username, const std::string& role) {
    DbAccess res{};
    if (role == "Admin") { res.canSeeDb = true; res.customersOnly = false; return res; }

    if (role == "Worker") {
        std::string db_until, all_until;
        int found = 0;
        db << "SELECT COALESCE(can_access_database_until,''), COALESCE(can_access_everyone_until,'') "
            "FROM users WHERE username=?;"
            << username
            >> [&](std::string u1, std::string u2) { db_until = u1; all_until = u2; found = 1; };

        if (!found) return res;

        auto now = now_utc_str();
        if (!all_until.empty() && all_until >= now) { res.canSeeDb = true; res.customersOnly = false; return res; }
        if (!db_until.empty() && db_until >= now) { res.canSeeDb = true; res.customersOnly = true;  return res; }
    }
    return res;
}

// ----------------- Main -----------------

int main() {
    try {
        sqlite::database db("users.db");
        
        // Create tables
        db <<
            "CREATE TABLE IF NOT EXISTS users ("
            " id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " username TEXT NOT NULL UNIQUE,"
            " password_hash TEXT NOT NULL,"
            " password_salt TEXT NOT NULL,"
            " email TEXT NOT NULL UNIQUE,"
            " first_name TEXT,"
            " last_name TEXT,"
            " birth_year INTEGER,"
            " gender TEXT,"
            " phone_number TEXT,"
            " address TEXT,"
            " role TEXT NOT NULL DEFAULT 'Customer',"
            " can_access_database_until DATETIME,"
            " can_access_everyone_until DATETIME,"
            " created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
            ");";

        db <<
            "CREATE TABLE IF NOT EXISTS announcements ("
            " id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " text TEXT NOT NULL,"
            " author_username TEXT,"
            " created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
            ");";

        db <<
            "CREATE TABLE IF NOT EXISTS tasks ("
            " id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " title TEXT NOT NULL,"
            " body TEXT,"
            " created_by TEXT,"
            " created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
            ");";

        // Bootstrap admin if missing
        int admin_count = 0;
        db << "SELECT COUNT(*) FROM users WHERE username='Xoid';" >> admin_count;

        if (admin_count == 0) {
            std::cout << "No Admin found, creating default Admin account...\n";

            std::string username = "Xoid";
            std::string password = "Batu1234.";
            std::string email = "batutekoz@gmail.com";
            std::string first = "Batu";
            std::string last = "Tekoz";
            int         byear = 2007;
            std::string gender = "Male";
            std::string phone = "+31622266035";
            std::string address = "Reeuwijk, Netherlands";

            std::string salt = make_salt();
            std::string hash = salted_hash(salt, password);

            db << "INSERT INTO users "
                "(username, password_hash, password_salt, email, first_name, last_name, birth_year, gender, phone_number, address, role) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'Admin');"
                << username << hash << salt << email
                << first << last << byear
                << gender << phone << address;

            std::cout << "Default Admin account created!\n";
        }

        crow::SimpleApp app;

        // Serve the single-page frontend
        CROW_ROUTE(app, "/")([] {
            std::ifstream file("../../index.html");
            if (!file.is_open()) {
                return crow::response(404, "index.html not found");
            }
            std::ostringstream buf;
            buf << file.rdbuf();
            return crow::response{ buf.str() };
            });

        // -------- Auth endpoints --------

        CROW_ROUTE(app, "/register").methods("POST"_method)([&db](const crow::request& req) {
            auto body = crow::json::load(req.body);
            if (!body) return crow::response(400, "Invalid JSON");

            try {
                std::string username = body["username"].s();
                std::string password = body["password"].s();
                std::string email = body["email"].s();
                std::string first_name = body["first_name"].s();
                std::string last_name = body["last_name"].s();
                int         birth_year = body["birth_year"].i();
                std::string gender = body["gender"].s();
                std::string phone = body["phone"].s();
                std::string address = body["address"].s();

                std::string salt = make_salt();
                std::string hash = salted_hash(salt, password);

                db << "INSERT INTO users "
                    "(username, password_hash, password_salt, email, first_name, last_name, birth_year, gender, phone_number, address, role) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'Customer');"
                    << username << hash << salt << email
                    << first_name << last_name << birth_year
                    << gender << phone << address;
            }
            catch (std::exception& e) {
                return crow::response(400, std::string("Error: ") + e.what());
            }
            return crow::response(200, "Registration successful!");
            });

        CROW_ROUTE(app, "/login").methods("POST"_method)([&db](const crow::request& req) {
            auto body = crow::json::load(req.body);
            if (!body) return crow::response(400, "Invalid JSON");

            std::string username = body["username"].s();
            std::string password = body["password"].s();

            std::string stored_hash, stored_salt, role;
            int found = 0;

            db << "SELECT password_hash, password_salt, role FROM users WHERE username=?;"
                << username
                >> [&](std::string hash, std::string salt, std::string r) {
                stored_hash = hash; stored_salt = salt; role = r; found = 1;
                };

            if (!found) return crow::response(401, "Invalid username or password");
            if (salted_hash(stored_salt, password) != stored_hash)
                return crow::response(401, "Invalid username or password");

            // make token
            std::string token = random_token_hex(32);
            {
                std::lock_guard<std::mutex> lock(g_sessions_mx);
                g_sessions[token] = { username, role, now_utc_str() };
            }

            crow::json::wvalue res;
            res["message"] = "Login successful!";
            res["role"] = role;
            res["username"] = username;
            res["token"] = token;
            return crow::response(200, res);
            });

        // Current user + computed permissions
        CROW_ROUTE(app, "/me").methods("GET"_method)([&db](const crow::request& req) {
            Session s;
            if (!auth(req, s)) return crow::response(401, "Unauthorized");

            DbAccess da = compute_db_access(db, s.username, s.role);

            crow::json::wvalue res;
            res["username"] = s.username;
            res["role"] = s.role;
            res["canSeeDb"] = da.canSeeDb;
            res["customersOnly"] = da.customersOnly;
            return crow::response(200, res);
            });

        // -------- Announcements --------

        CROW_ROUTE(app, "/announcements").methods("GET"_method)([&db](const crow::request& req) {
            // Everyone can read
            crow::json::wvalue::list arr;
            db << "SELECT id, text, COALESCE(author_username,''), COALESCE(created_at,'') "
                "FROM announcements ORDER BY id DESC;"
                >> [&](int id, std::string text, std::string author, std::string created_at) {
                crow::json::wvalue item;
                item["id"] = id;
                item["text"] = text;
                item["author"] = author;
                item["created_at"] = created_at;
                arr.push_back(std::move(item));
                };
            crow::json::wvalue res;
            res["items"] = std::move(arr);
            return crow::response(200, res);
            });

        CROW_ROUTE(app, "/announcements").methods("POST"_method)([&db](const crow::request& req) {
            Session s;
            if (!auth(req, s)) return crow::response(401, "Unauthorized");
            if (s.role != "Admin") return crow::response(403, "Only Admin can post announcements");

            auto body = crow::json::load(req.body);
            if (!body) return crow::response(400, "Invalid JSON");

            std::string text = body["text"].s();
            if (text.empty()) return crow::response(400, "Text required");

            db << "INSERT INTO announcements (text, author_username) VALUES (?, ?);"
                << text << s.username;

            return crow::response(200, "Announcement posted");
            });

        // -------- Tasks (visible for Admin & Worker) --------

        CROW_ROUTE(app, "/tasks").methods("GET"_method)([&db](const crow::request& req) {
            Session s;
            if (!auth(req, s)) return crow::response(401, "Unauthorized");
            if (!(s.role == "Admin" || s.role == "Worker"))
                return crow::response(403, "Not allowed");

            crow::json::wvalue::list arr;
            db << "SELECT id, title, COALESCE(body,''), COALESCE(created_by,''), COALESCE(created_at,'') "
                "FROM tasks ORDER BY id DESC;"
                >> [&](int id, std::string title, std::string body, std::string created_by, std::string created_at) {
                crow::json::wvalue item;
                item["id"] = id;
                item["title"] = title;
                item["body"] = body;
                item["created_by"] = created_by;
                item["created_at"] = created_at;
                arr.push_back(std::move(item));
                };
            crow::json::wvalue res;
            res["items"] = std::move(arr);
            return crow::response(200, res);
            });

        // Optional: Admin can create tasks
        CROW_ROUTE(app, "/tasks").methods("POST"_method)([&db](const crow::request& req) {
            Session s;
            if (!auth(req, s)) return crow::response(401, "Unauthorized");
            if (s.role != "Admin") return crow::response(403, "Only Admin can create tasks");

            auto body = crow::json::load(req.body);
            if (!body) return crow::response(400, "Invalid JSON");

            // helpers
            auto getOrEmpty = [&](const char* k) -> std::string {
                if (body.has(std::string(k)) && body[std::string(k)].t() == crow::json::type::String) {
                    return body[std::string(k)].s();
                }
                return "";
                };

            std::string title = getOrEmpty("title");
            std::string tbody = getOrEmpty("body");

            if (title.empty()) return crow::response(400, "Title required");

            try {
                db << "INSERT INTO tasks (title, body, created_by) VALUES (?, ?, ?);"
                    << title << tbody << s.username;
            }
            catch (std::exception& e) {
                return crow::response(500, std::string("DB insert failed: ") + e.what());
            }

            return crow::response(200, "Task created");
        });

        // -------- Members / Database --------
        // GET /members : list users (Admin: all; Worker: allowed if access_* valid; Customer: never)
        CROW_ROUTE(app, "/members").methods("GET"_method)([&db](const crow::request& req) {
            Session s;
            if (!auth(req, s)) return crow::response(401, "Unauthorized");

            DbAccess da = compute_db_access(db, s.username, s.role);
            if (!(s.role == "Admin" || (s.role == "Worker" && da.canSeeDb))) {
                return crow::response(403, "Not allowed to read database");
            }

            crow::json::wvalue::list arr;
            if (s.role == "Admin" || (s.role == "Worker" && !da.customersOnly)) {
                // All users
                db << "SELECT id, username, email, COALESCE(first_name,''), COALESCE(last_name,''), "
                    "COALESCE(birth_year,0), COALESCE(gender,''), COALESCE(phone_number,''), COALESCE(address,''), role, "
                    "COALESCE(can_access_database_until,''), COALESCE(can_access_everyone_until,''), COALESCE(created_at,'') "
                    "FROM users ORDER BY id ASC;"
                    >> [&](int id, std::string u, std::string e, std::string fn, std::string ln,
                        int by, std::string g, std::string ph, std::string ad, std::string role,
                        std::string dbu, std::string eau, std::string created_at) {
                            crow::json::wvalue item;
                            item["id"] = id; item["username"] = u; item["email"] = e;
                            item["first_name"] = fn; item["last_name"] = ln; item["birth_year"] = by;
                            item["gender"] = g; item["phone_number"] = ph; item["address"] = ad;
                            item["role"] = role;
                            item["can_access_database_until"] = dbu;
                            item["can_access_everyone_until"] = eau;
                            item["created_at"] = created_at;
                            arr.push_back(std::move(item));
                    };
            }
            else {
                // Worker with customersOnly: only Customers
                db << "SELECT id, username, email, COALESCE(first_name,''), COALESCE(last_name,''), "
                    "COALESCE(birth_year,0), COALESCE(gender,''), COALESCE(phone_number,''), COALESCE(address,''), role, "
                    "COALESCE(can_access_database_until,''), COALESCE(can_access_everyone_until,''), COALESCE(created_at,'') "
                    "FROM users WHERE role='Customer' ORDER BY id ASC;"
                    >> [&](int id, std::string u, std::string e, std::string fn, std::string ln,
                        int by, std::string g, std::string ph, std::string ad, std::string role,
                        std::string dbu, std::string eau, std::string created_at) {
                            crow::json::wvalue item;
                            item["id"] = id; item["username"] = u; item["email"] = e;
                            item["first_name"] = fn; item["last_name"] = ln; item["birth_year"] = by;
                            item["gender"] = g; item["phone_number"] = ph; item["address"] = ad;
                            item["role"] = role;
                            item["can_access_database_until"] = dbu;
                            item["can_access_everyone_until"] = eau;
                            item["created_at"] = created_at;
                            arr.push_back(std::move(item));
                    };
            }

            crow::json::wvalue res;
            res["items"] = std::move(arr);
            return crow::response(200, res);
            });

        // Admin-only: update a user's fields
        CROW_ROUTE(app, "/members/update").methods("POST"_method)([&db](const crow::request& req) {
            Session s;
            if (!auth(req, s)) return crow::response(401, "Unauthorized");
            if (s.role != "Admin") return crow::response(403, "Only Admin can update users");

            auto body = crow::json::load(req.body);
            if (!body) return crow::response(400, "Invalid JSON");

            int id = body["id"].i();
            // Accept these fields (if present)
            auto getOrEmpty = [&](const char* k) -> std::string {
                if (body.has(std::string(k)) && body[std::string(k)].t() == crow::json::type::String) {
                    return body[std::string(k)].s();
                }
                return "";
            };
            auto getOrInt = [&](const char* k) -> int {
                if (body.has(std::string(k)) && body[std::string(k)].t() == crow::json::type::Number) {
                    return body[std::string(k)].i();
                }
                return 0;
            };

            std::string first_name = getOrEmpty("first_name");
            std::string last_name = getOrEmpty("last_name");
            int         birth_year = getOrInt("birth_year");
            std::string gender = getOrEmpty("gender");
            std::string phone = getOrEmpty("phone_number");
            std::string address = getOrEmpty("address");
            std::string email = getOrEmpty("email");
            std::string role = getOrEmpty("role");
            std::string db_until = getOrEmpty("can_access_database_until");
            std::string all_until = getOrEmpty("can_access_everyone_until");

            try {
                db << "UPDATE users SET "
                    "first_name=?, last_name=?, birth_year=?, gender=?, phone_number=?, address=?, email=?, role=?, "
                    "can_access_database_until=?, can_access_everyone_until=? WHERE id=?;"
                    << first_name << last_name << birth_year << gender << phone << address << email << role
                    << db_until << all_until << id;
            }
            catch (std::exception& e) {
                return crow::response(400, std::string("Update failed: ") + e.what());
            }
            return crow::response(200, "User updated");
            });

        // -------------- Start server --------------
        std::thread server_thread([&app]() {
            app.port(18080).multithreaded().run();
            });

        std::this_thread::sleep_for(std::chrono::seconds(1)); // give server a moment

        ShellExecuteA(NULL, "open", "http://localhost:18080", NULL, NULL, SW_SHOWNORMAL);

        server_thread.join();
    }
    catch (std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
    return 0;
}
