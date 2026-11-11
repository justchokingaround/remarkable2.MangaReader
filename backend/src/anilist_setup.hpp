#pragma once

// One-shot local web page for connecting AniList from a phone.
//
// AniList tokens are ~1000 characters, which nobody wants to type on an e-ink keyboard, so
// while the "Connect AniList" screen is open the backend serves a tiny page on the LAN
// (http://<rm-ip>:<port>/<random code>). The device shows it as a QR code; the phone opens
// it, logs in on anilist.co (implicit grant + "pin" redirect, which displays the token),
// and pastes the token back into the page. The server stops on success, when the screen
// is closed, or after 15 minutes.

#include "anilist.hpp"
#include "third_party/qrcodegen.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <functional>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <random>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace anilist {

class SetupServer {
public:
    struct Callbacks {
        std::function<std::string()> clientId;                                      // "" if none
        std::function<void(const std::string&)> setClientId;
        std::function<std::string(const std::string& token, std::string* user)> submitToken;  // "" = ok
        std::function<void(const std::string& user)> onConnected;
    };

private:
    Callbacks cb_;
    std::atomic<bool> running_{false};
    std::thread th_;
    int listenFd_ = -1;
    int port_ = 0;
    std::string code_;
    std::mutex mu_;

    static std::string htmlEscape(const std::string& s) {
        std::string o;
        for (char c : s) {
            switch (c) {
                case '&': o += "&amp;"; break;
                case '<': o += "&lt;"; break;
                case '>': o += "&gt;"; break;
                case '"': o += "&quot;"; break;
                default: o.push_back(c);
            }
        }
        return o;
    }

    static std::string urlDecode(const std::string& s) {
        std::string o;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '+') o.push_back(' ');
            else if (s[i] == '%' && i + 2 < s.size()) {
                o.push_back(static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16)));
                i += 2;
            } else o.push_back(s[i]);
        }
        return o;
    }

    static std::string formField(const std::string& body, const std::string& key) {
        size_t pos = 0;
        while (pos <= body.size()) {
            size_t amp = body.find('&', pos);
            std::string kv = body.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
            size_t eq = kv.find('=');
            if (eq != std::string::npos && kv.substr(0, eq) == key) return urlDecode(kv.substr(eq + 1));
            if (amp == std::string::npos) break;
            pos = amp + 1;
        }
        return "";
    }

    std::string page(const std::string& msg, bool isError, bool done) {
        std::string cid = cb_.clientId ? cb_.clientId() : "";
        std::string base = "/" + code_;
        std::string h =
            "<!doctype html><html><head><meta charset=utf-8>"
            "<meta name=viewport content='width=device-width,initial-scale=1'>"
            "<title>rm2manga: connect AniList</title><style>"
            "body{font-family:-apple-system,system-ui,sans-serif;max-width:560px;margin:0 auto;padding:20px;"
            "font-size:17px;line-height:1.45;color:#111}h1{font-size:24px}"
            ".step{border:2px solid #111;border-radius:10px;padding:14px;margin:14px 0}"
            ".n{display:inline-block;background:#111;color:#fff;border-radius:50%;width:26px;height:26px;"
            "text-align:center;margin-right:8px}a.btn,button{display:block;width:100%;box-sizing:border-box;"
            "background:#02a9ff;color:#fff;border:0;border-radius:8px;padding:14px;font-size:18px;"
            "text-align:center;text-decoration:none;margin-top:10px;font-weight:600}"
            "textarea,input{width:100%;box-sizing:border-box;font-size:15px;padding:10px;border:1px solid #888;"
            "border-radius:6px}textarea{height:120px}code{background:#eee;padding:2px 5px;border-radius:4px;"
            "word-break:break-all}.msg{padding:12px;border-radius:8px;margin:12px 0}"
            ".err{background:#fde2e2}.ok{background:#e0f6e0}small{color:#555}"
            "</style></head><body><h1>Connect AniList to your reMarkable</h1>";
        if (!msg.empty()) {
            h += "<div class='msg " + std::string(isError ? "err" : "ok") + "'>" + htmlEscape(msg) + "</div>";
        }
        if (done) {
            h += "<p>All set. You can close this page and put your phone away.</p></body></html>";
            return h;
        }
        if (cid.empty()) {
            h += "<div class=step><span class=n>1</span><b>Create an AniList app (one time, 1 minute)</b>"
                 "<p>Open <a href='https://anilist.co/settings/developer' target=_blank>anilist.co/settings/developer</a>"
                 ", tap <i>Create New Client</i>. Name it anything (e.g. <i>rm2manga</i>) and use this "
                 "<b>Redirect URL</b>:</p><p><code>" + std::string(kPinRedirect) + "</code></p>"
                 "<p>Save, then copy the <b>Client ID</b> (a number) into the box below.</p>"
                 "<form method=post action='" + base + "/client'><input name=client_id inputmode=numeric "
                 "placeholder='Client ID, e.g. 12345' autocomplete=off>"
                 "<button type=submit>Continue</button></form></div>";
        } else {
            h += "<div class=step><span class=n>1</span><b>Log in on AniList</b>"
                 "<a class=btn href='" + htmlEscape(authorizeUrl(cid)) + "' target=_blank>Log in with AniList</a>"
                 "<p><small>Tap <i>Authorize</i>. AniList then shows a long token.</small></p></div>"
                 "<div class=step><span class=n>2</span><b>Copy that token and paste it here</b>"
                 "<form method=post action='" + base + "/token'><textarea name=token "
                 "placeholder='Paste the token here' autocomplete=off></textarea>"
                 "<button type=submit>Connect</button></form></div>"
                 "<form method=post action='" + base + "/client'><input type=hidden name=client_id value=''>"
                 "<p><small>Using AniList client id " + htmlEscape(cid) + ". "
                 "<button type=submit style='display:inline;width:auto;background:none;color:#02a9ff;"
                 "padding:0;font-size:14px;font-weight:400'>Use a different one</button></small></p></form>";
        }
        h += "</body></html>";
        return h;
    }

    static void sendAll(int fd, const std::string& data) {
        size_t off = 0;
        while (off < data.size()) {
            ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
            if (n <= 0) return;
            off += static_cast<size_t>(n);
        }
    }

    static void respond(int fd, int status, const std::string& body, const std::string& extraHeaders = "") {
        std::string st = status == 200 ? "200 OK" : status == 303 ? "303 See Other" : "404 Not Found";
        std::string r = "HTTP/1.1 " + st + "\r\nContent-Type: text/html; charset=utf-8\r\n"
                        "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: " +
                        std::to_string(body.size()) + "\r\n" + extraHeaders + "\r\n" + body;
        sendAll(fd, r);
    }

    void handle(int fd) {
        std::string req;
        char buf[4096];
        size_t headerEnd = std::string::npos;
        size_t contentLen = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline && req.size() < 65536) {
            struct pollfd p = {fd, POLLIN, 0};
            if (::poll(&p, 1, 500) <= 0) continue;
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            req.append(buf, static_cast<size_t>(n));
            if (headerEnd == std::string::npos) {
                headerEnd = req.find("\r\n\r\n");
                if (headerEnd != std::string::npos) {
                    std::string lower = util::toLower(req.substr(0, headerEnd));
                    size_t cl = lower.find("content-length:");
                    if (cl != std::string::npos) contentLen = std::strtoul(lower.c_str() + cl + 15, nullptr, 10);
                }
            }
            if (headerEnd != std::string::npos && req.size() >= headerEnd + 4 + contentLen) break;
        }
        if (headerEnd == std::string::npos) return;
        size_t sp1 = req.find(' ');
        size_t sp2 = req.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) return;
        std::string method = req.substr(0, sp1);
        std::string path = req.substr(sp1 + 1, sp2 - sp1 - 1);
        std::string body = req.substr(headerEnd + 4);
        std::string base = "/" + code_;

        if (path.rfind(base, 0) != 0) {
            respond(fd, 404, "<h1>Not found</h1><p>Scan the QR code on your reMarkable again.</p>");
            return;
        }
        std::string sub = path.substr(base.size());
        if (method == "POST" && sub == "/client") {
            std::string id = util::trim(formField(body, "client_id"));
            bool valid = std::all_of(id.begin(), id.end(), ::isdigit);
            if (cb_.setClientId && valid) cb_.setClientId(id);
            respond(fd, 303, "", "Location: " + base + "\r\n");
            return;
        }
        if (method == "POST" && sub == "/token") {
            std::string user;
            std::string err = cb_.submitToken ? cb_.submitToken(formField(body, "token"), &user) : "unavailable";
            if (err.empty()) {
                respond(fd, 200, page("Connected as " + user + ".", false, true));
                if (cb_.onConnected) cb_.onConnected(user);
                running_ = false;  // done
            } else {
                respond(fd, 200, page(err, true, false));
            }
            return;
        }
        respond(fd, 200, page("", false, false));
    }

    void loop() {
        auto started = std::chrono::steady_clock::now();
        while (running_.load()) {
            if (std::chrono::steady_clock::now() - started > std::chrono::minutes(15)) break;
            struct pollfd p = {listenFd_, POLLIN, 0};
            if (::poll(&p, 1, 400) <= 0) continue;
            int c = ::accept(listenFd_, nullptr, nullptr);
            if (c < 0) continue;
            handle(c);
            ::close(c);
        }
        running_ = false;
        std::lock_guard<std::mutex> lk(mu_);
        if (listenFd_ >= 0) { ::close(listenFd_); listenFd_ = -1; }
    }

public:
    explicit SetupServer(Callbacks cb) : cb_(std::move(cb)) {}
    ~SetupServer() { stop(); }

    bool running() const { return running_.load(); }

    // Returns false if no port could be bound.
    bool start() {
        if (running_.load()) return true;
        if (th_.joinable()) th_.join();
        std::random_device rd;
        static const char* hex = "abcdefghjkmnpqrstuvwxyz23456789";
        code_.clear();
        for (int i = 0; i < 6; ++i) code_.push_back(hex[rd() % 31]);

        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        bool bound = false;
        for (int p = 8765; p < 8780 && !bound; ++p) {
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_ANY);
            a.sin_port = htons(static_cast<uint16_t>(p));
            if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
                bound = true;
                port_ = p;
            }
        }
        if (!bound || ::listen(fd, 4) != 0) {
            ::close(fd);
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            listenFd_ = fd;
        }
        running_ = true;
        th_ = std::thread([this]() { loop(); });
        return true;
    }

    void stop() {
        running_ = false;
        if (th_.joinable() && th_.get_id() != std::this_thread::get_id()) th_.join();
    }

    // URLs for every non-loopback IPv4 interface, Wi-Fi first.
    std::vector<std::string> urls() const {
        std::vector<std::pair<int, std::string>> found;
        struct ifaddrs* ifs = nullptr;
        if (::getifaddrs(&ifs) == 0) {
            for (auto* i = ifs; i; i = i->ifa_next) {
                if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
                if (i->ifa_flags & IFF_LOOPBACK) continue;
                char ip[INET_ADDRSTRLEN];
                auto* sin = reinterpret_cast<sockaddr_in*>(i->ifa_addr);
                ::inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
                std::string name = i->ifa_name ? i->ifa_name : "";
                int prio = name.rfind("wlan", 0) == 0 ? 0 : (name.rfind("usb", 0) == 0 ? 2 : 1);
                found.emplace_back(prio, "http://" + std::string(ip) + ":" + std::to_string(port_) + "/" + code_);
            }
            ::freeifaddrs(ifs);
        }
        std::sort(found.begin(), found.end());
        std::vector<std::string> out;
        for (auto& f : found) out.push_back(f.second);
        return out;
    }

    // QR code rows as "0101..." strings
    static Json::Value qrRows(const std::string& text) {
        Json::Value rows = Json::Value::array();
        try {
            auto qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
            for (int y = 0; y < qr.getSize(); ++y) {
                std::string row;
                for (int x = 0; x < qr.getSize(); ++x) row.push_back(qr.getModule(x, y) ? '1' : '0');
                rows.push(row);
            }
        } catch (...) {
        }
        return rows;
    }
};

}  // namespace anilist
