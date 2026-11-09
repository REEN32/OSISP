#ifdef _WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "Ws2_32.lib")
using sock_t = SOCKET;
#define CLOSE closesocket
#else
#include <arpa/inet.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define CLOSE close
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <deque>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
using namespace std;
static mutex state_mutex, log_mutex;

struct Message
{
    uint64_t id;
    string from, text;
};

static map<string, deque<Message>> message_queues;
static set<string> online_users;
static uint64_t next_message_id = 1;
static atomic<bool> running{true};
static mutex clients_mutex;
static set<sock_t> clients;

// Idle clients may stay connected indefinitely. The socket receive timeout applies
// only after the first byte of a new frame is ready, or while waiting for an ACK.
static bool wait_for_request(sock_t s)
{
    while (running)
    {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(s, &read_set);
        timeval interval{1, 0};
        int result = select(static_cast<int>(s + 1), &read_set, nullptr, nullptr, &interval);
        if (result > 0)
        {
            return true;
        }
        if (result == SOCKET_ERROR)
        {
            return false;
        }
    }
    return false;
}

static void log_event(const string &role, const string &id, const string &peer, const string &op,
                      const string &result)
{
    lock_guard<mutex> l(log_mutex);
    auto now = chrono::system_clock::now();
    auto t = chrono::system_clock::to_time_t(now);
    tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    cout << '[' << put_time(&tmv, "%H:%M:%S") << '.' << setw(3) << setfill('0')
         << chrono::duration_cast<chrono::milliseconds>(now.time_since_epoch()).count() % 1000
         << setfill(' ') << "] " << role << " id=" << id << " peer=" << peer << ' ' << op << ' '
         << result << endl;
}

static void log_socket_error(const char *op)
{
#ifdef _WIN32
    int e = WSAGetLastError();
    char *msg = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, e, 0, (LPSTR)&msg, 0, nullptr);
    cerr << op << " WSA error=" << e << ' ' << (msg ? msg : "unknown") << endl;
    if (msg)
    {
        LocalFree(msg);
    }
#else
    perror(op);
#endif
}

static bool transfer_all(sock_t s, char *p, size_t n, bool sending)
{
    while (n)
    {
        int k = sending ? send(s, p, (int)n, 0) : recv(s, p, (int)n, 0);
        if (k == SOCKET_ERROR)
        {
            log_socket_error(sending ? "send" : "recv");
            return false;
        }
        if (k == 0)
        {
            return false;
        }
        p += k;
        n -= k;
    }
    return true;
}

static bool send_frame(sock_t s, const string &x)
{
    if (x.empty() || x.size() > 65536)
    {
        return false;
    }
    uint32_t n = htonl((uint32_t)x.size());
    return transfer_all(s, (char *)&n, 4, true) &&
           transfer_all(s, (char *)x.data(), x.size(), true);
}

static bool receive_frame(sock_t s, string &x)
{
    uint32_t n;
    if (!transfer_all(s, (char *)&n, 4, false))
    {
        return false;
    }
    n = ntohl(n);
    if (n == 0 || n > 65536)
    {
        return false;
    }
    x.resize(n);
    return transfer_all(s, &x[0], n, false);
}

static vector<string> split_fields(const string &s)
{
    vector<string> v;
    istringstream in(s);
    string a;
    while (getline(in, a, '\t'))
    {
        v.push_back(a);
    }
    return v;
}

static bool valid_identifier(const string &x)
{
    return !x.empty() && x.size() <= 32 &&
           all_of(x.begin(), x.end(),
                  [](unsigned char c) { return isalnum(c) || c == '_' || c == '-'; });
}

static bool valid_number(const string &x)
{
    return !x.empty() && x.size() <= 18 &&
           all_of(x.begin(), x.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
}

static bool deliver_pending(sock_t s, const string &id, const string &request_id)
{
    for (;;)
    {
        Message m;
        bool empty;
        {
            lock_guard<mutex> l(state_mutex);
            empty = message_queues[id].empty();
            if (!empty)
            {
                m = message_queues[id].front();
            }
        }
        if (empty)
        {
            return send_frame(s, "1\tEND\t" + request_id);
        }
        if (!send_frame(s, "1\tMESSAGE\t" + request_id + "\t" + to_string(m.id) + "\t" + m.from +
                               "\t" + m.text))
        {
            return false;
        }
        string a;
        if (!receive_frame(s, a))
        {
            return false;
        }
        auto f = split_fields(a);
        if (f.size() != 4 || f[0] != "1" || f[1] != "ACK" || f[2] != request_id ||
            f[3] != to_string(m.id))
        {
            send_frame(s, "1\tERROR\t" + request_id + "\tBAD_ACK");
            return false;
        }
        {
            lock_guard<mutex> l(state_mutex);
            if (!message_queues[id].empty() && message_queues[id].front().id == m.id)
            {
                message_queues[id].pop_front();
            }
        }
        log_event("SERVER", id, "-", "ACK messageId=" + to_string(m.id), "OK");
    }
}

static void handle_session(sock_t s, string peer)
{
    string id, x;
    while (wait_for_request(s) && receive_frame(s, x))
    {
        auto f = split_fields(x);
        string request_id = f.size() > 2 ? f[2] : "0";
        if (f.size() < 3 || f[0] != "1" || !valid_number(request_id))
        {
            if (!send_frame(s, "1\tERROR\t0\tBAD_FRAME"))
            {
                break;
            }
            continue;
        }
        string cmd = f[1];
        log_event("SERVER", id.empty() ? "-" : id, peer, cmd + " requestId=" + request_id, "RECV");
        if (cmd == "LOGIN" && f.size() == 4 && id.empty() && valid_identifier(f[3]))
        {
            bool accepted;
            {
                lock_guard<mutex> l(state_mutex);
                accepted = online_users.insert(f[3]).second;
                if (accepted)
                {
                    id = f[3];
                }
            }
            if (!accepted)
            {
                if (!send_frame(s, "1\tERROR\t" + request_id + "\tALREADY_ONLINE"))
                {
                    break;
                }
            }
            else if (!send_frame(s, "1\tOK\t" + request_id + "\tLOGIN") ||
                     !deliver_pending(s, id, request_id))
            {
                break;
            }
        }
        else if (id.empty())
        {
            if (!send_frame(s, "1\tERROR\t" + request_id + "\tLOGIN_REQUIRED"))
            {
                break;
            }
        }
        else if (cmd == "SEND" && f.size() == 5 && valid_identifier(f[3]) && !f[4].empty() &&
                 f[4].size() <= 60000)
        {
            string ans;
            {
                lock_guard<mutex> l(state_mutex);
                auto &q = message_queues[f[3]];
                if (q.size() >= 20)
                {
                    ans = "1\tERROR\t" + request_id + "\tQUEUE_FULL";
                }
                else
                {
                    auto mid = next_message_id++;
                    q.push_back({mid, id, f[4]});
                    ans = "1\tOK\t" + request_id + "\tQUEUED\t" + to_string(mid);
                }
            }
            log_event("SERVER", id, peer, "SEND requestId=" + request_id, ans);
            if (!send_frame(s, ans))
            {
                break;
            }
        }
        else if (cmd == "GET_PENDING" && f.size() == 3)
        {
            if (!deliver_pending(s, id, request_id))
            {
                break;
            }
        }
        else if (cmd == "LOGOUT" && f.size() == 3)
        {
            {
                lock_guard<mutex> lock(state_mutex);
                online_users.erase(id);
                log_event("SERVER", id, peer, "LOGOUT requestId=" + request_id, "OK");
                id.clear();
            }
            send_frame(s, "1\tOK\t" + request_id + "\tLOGOUT");
            break;
        }
        else if (!send_frame(s, "1\tERROR\t" + request_id + "\tBAD_COMMAND_OR_FIELDS"))
        {
            break;
        }
    }
    {
        lock_guard<mutex> l(state_mutex);
        if (!id.empty())
        {
            online_users.erase(id);
        }
    }
    log_event("SERVER", id.empty() ? "-" : id, peer, "DISCONNECT", "CLOSED");
    {
        lock_guard<mutex> lock(clients_mutex);
        clients.erase(s);
    }
    CLOSE(s);
}

static string option(int argc, char **argv, const string &key, const string &def)
{
    for (int i = 2; i + 1 < argc; i++)
    {
        if (argv[i] == key)
        {
            return argv[i + 1];
        }
    }
    return def;
}

static void configure_timeout(sock_t s, int ms)
{
#ifdef _WIN32
    DWORD t = ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&t, sizeof t);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char *)&t, sizeof t);
#else
    timeval t{ms / 1000, (ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof t);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof t);
#endif
}

static bool receive_pending(sock_t s, const string &request_id)
{
    string x;
    while (receive_frame(s, x))
    {
        auto f = split_fields(x);
        if (f.size() < 3 || f[0] != "1" || f[2] != request_id)
        {
            return false;
        }
        if (f[1] == "END")
        {
            return true;
        }
        if (f[1] != "MESSAGE" || f.size() != 6)
        {
            return false;
        }
        cout << "MESSAGE id=" << f[3] << " from=" << f[4] << " text=" << f[5] << endl;
        if (!send_frame(s, "1\tACK\t" + request_id + "\t" + f[3]))
        {
            return false;
        }
    }
    return false;
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w))
    {
        log_socket_error("WSAStartup");
        return 1;
    }
    struct WinsockCleanup
    {
        ~WinsockCleanup()
        {
            WSACleanup();
        }
    } winsock_cleanup;
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    if (argc < 2)
    {
        cerr << "Usage: LAB5 server|client [--host 127.0.0.1] [--port 20027] [--timeout 30000] "
                "[--id name]\n";
        return 1;
    }
    string role = argv[1], host = option(argc, argv, "--host", "127.0.0.1"),
           ports = option(argc, argv, "--port", "20027"),
           ts = option(argc, argv, "--timeout", "30000"), id = option(argc, argv, "--id", "");
    if (!valid_number(ports) || !valid_number(ts) || stoll(ports) < 20000 || stoll(ports) > 29999 ||
        stoll(ts) < 100 || stoll(ts) > 3600000)
    {
        cerr << "Invalid port or timeout\n";
        return 1;
    }
    int port = stoi(ports), ms = stoi(ts);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
    {
        cerr << "Invalid IPv4 address\n";
        return 1;
    }
    sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
    {
        log_socket_error("socket");
        return 1;
    }
    if (role == "server")
    {
        int yes = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *)&yes, sizeof yes);
        if (::bind(s, (sockaddr *)&addr, sizeof addr) == SOCKET_ERROR ||
            listen(s, 16) == SOCKET_ERROR)
        {
            log_socket_error("bind/listen");
            CLOSE(s);
            return 1;
        }
        log_event("SERVER", "-", host + ":" + ports, "START", "OK");
        thread admin(
            [addr]
            {
                string line;
                while (getline(cin, line))
                {
                    if (line == "stop")
                    {
                        running = false;
                        {
                            lock_guard<mutex> lock(clients_mutex);
                            for (sock_t client : clients)
                            {
#ifdef _WIN32
                                shutdown(client, SD_BOTH);
#else
                                shutdown(client, SHUT_RDWR);
#endif
                            }
                        }
                        sock_t wake = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                        if (wake != INVALID_SOCKET)
                        {
                            ::connect(wake, (const sockaddr *)&addr, sizeof addr);
                            CLOSE(wake);
                        }
                        break;
                    }
                }
            });
        admin.detach();
        vector<thread> threads;
        while (running)
        {
            sockaddr_in remote{};
#ifdef _WIN32
            int len = sizeof remote;
#else
            socklen_t len = sizeof remote;
#endif
            sock_t c = accept(s, (sockaddr *)&remote, &len);
            if (!running)
            {
                if (c != INVALID_SOCKET)
                {
                    CLOSE(c);
                }
                break;
            }
            if (c == INVALID_SOCKET)
            {
                if (running)
                {
                    log_socket_error("accept");
                }
                break;
            }
            configure_timeout(c, ms);
            {
                lock_guard<mutex> lock(clients_mutex);
                if (!running)
                {
                    CLOSE(c);
                    break;
                }
                clients.insert(c);
            }
            char ip[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &remote.sin_addr, ip, sizeof ip);
            threads.emplace_back(handle_session, c,
                                 string(ip) + ":" + to_string(ntohs(remote.sin_port)));
        }
        CLOSE(s);
        for (auto &t : threads)
        {
            t.join();
        }
    }
    else if (role == "client" && valid_identifier(id))
    {
        configure_timeout(s, ms);
        if (connect(s, (sockaddr *)&addr, sizeof addr) == SOCKET_ERROR)
        {
            log_socket_error("connect");
            CLOSE(s);
            return 1;
        }
        string peer = host + ":" + ports;
        unsigned long request_id = 1;
        string response;
        auto send_request = [&](const string &x)
        {
            log_event("CLIENT", id, peer, "REQUEST requestId=" + to_string(request_id), "SENT");
            return send_frame(s, x) && receive_frame(s, response);
        };
        string r = to_string(request_id);
        if (!send_request("1\tLOGIN\t" + r + "\t" + id))
        {
            cerr << "LOGIN failed\n";
            CLOSE(s);
            return 1;
        }
        auto login_fields = split_fields(response);
        if (login_fields.size() != 4 || login_fields[0] != "1" || login_fields[1] != "OK" ||
            login_fields[2] != r || login_fields[3] != "LOGIN")
        {
            cerr << "Login rejected: " << response << '\n';
            CLOSE(s);
            return 1;
        }
        cout << "Connected as " << id << ".\n";
        if (!receive_pending(s, r))
        {
            cerr << "Could not receive pending messages.\n";
            CLOSE(s);
            return 1;
        }
        // Both the input loop and the polling thread use one socket. Keep each
        // complete request/response exchange under the same mutex.
        mutex exchange_mutex;
        mutex stop_mutex;
        condition_variable stop_condition;
        bool stop_polling = false;
        atomic<bool> connected{true};
        auto fetch_pending = [&]()
        {
            string current_request = to_string(++request_id);
            return send_frame(s, "1\tGET_PENDING\t" + current_request) &&
                   receive_pending(s, current_request);
        };

        thread poller(
            [&]()
            {
                unique_lock<mutex> stop_lock(stop_mutex);
                while (!stop_condition.wait_for(stop_lock, chrono::seconds(1),
                                                [&] { return stop_polling; }))
                {
                    stop_lock.unlock();
                    {
                        lock_guard<mutex> exchange_lock(exchange_mutex);
                        if (connected && !fetch_pending())
                        {
                            connected = false;
                            cerr << "Connection lost or request timed out. Press Enter to exit.\n";
                        }
                    }
                    stop_lock.lock();
                    if (!connected)
                    {
                        break;
                    }
                }
            });

        cout << "Commands: SEND recipient message | GET_PENDING | LOGOUT\n";
        string line;
        while (getline(cin, line))
        {
            lock_guard<mutex> exchange_lock(exchange_mutex);
            if (!connected)
            {
                break;
            }
            istringstream input(line);
            string command;
            input >> command;
            transform(command.begin(), command.end(), command.begin(),
                      [](unsigned char c) { return static_cast<char>(toupper(c)); });
            if (command == "LOGOUT")
            {
                r = to_string(++request_id);
                if (send_request("1\tLOGOUT\t" + r))
                {
                    cout << "Disconnected.\n";
                }
                connected = false;
                break;
            }
            else if (command == "GET_PENDING")
            {
                if (!fetch_pending())
                {
                    cerr << "Receive error or timeout\n";
                    connected = false;
                    break;
                }
            }
            else if (command == "SEND")
            {
                string dest, body;
                input >> dest;
                getline(input >> ws, body);
                if (!valid_identifier(dest) || body.empty() || body.size() > 60000 ||
                    body.find('\t') != string::npos)
                {
                    cout << "Usage: SEND recipient message (non-empty, no tabs, max 60000 bytes)\n";
                    continue;
                }
                r = to_string(++request_id);
                if (!send_request("1\tSEND\t" + r + "\t" + dest + "\t" + body))
                {
                    cerr << "Send/receive error or timeout\n";
                    connected = false;
                    break;
                }
                auto fields = split_fields(response);
                if (fields.size() >= 4 && fields[1] == "ERROR" && fields[3] == "QUEUE_FULL")
                {
                    cout << "Message rejected: recipient's queue is full (20 messages).\n";
                }
                else
                {
                    if (fields.size() >= 5 && fields[1] == "OK" && fields[3] == "QUEUED")
                    {
                        cout << "Message accepted (id=" << fields[4] << ").\n";
                    }
                    else
                    {
                        cout << "Server response: " << response << endl;
                    }
                }
            }
            else if (!command.empty())
            {
                cout << "Commands: SEND recipient message | GET_PENDING | LOGOUT\n";
            }
        }
        {
            lock_guard<mutex> lock(stop_mutex);
            stop_polling = true;
        }
        stop_condition.notify_one();
        poller.join();
        CLOSE(s);
    }
    else
    {
        cerr << "Invalid role or client id\n";
        CLOSE(s);
        return 1;
    }
    return 0;
}
