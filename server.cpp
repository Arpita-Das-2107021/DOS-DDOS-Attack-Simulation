// Dummy web server for the local DoS/DDoS lab.
// It accepts a limited number of simultaneous requests ("capacity").
// Anything beyond that capacity gets an immediate 503 response,
// which is what lets us measure the effect of overload traffic.

#include "common.h"
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <winsock2.h>
#include <ws2tcpip.h>

using namespace std;
using namespace chrono;

const int CAPACITY = 8;     // max requests the server will serve at once
const int WORK_MS = 250;    // simulated time to handle one request

atomic<int> active{0};
atomic<long long> totalRequests{0};
atomic<long long> completed{0};
atomic<long long> rejected{0};

void sendResponse(SOCKET client, int code, const string& text, const string& body) {
    ostringstream out;
    out << "HTTP/1.1 " << code << " " << text << "\r\n"
        << "Content-Type: text/plain\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n\r\n"
        << body;

    string response = out.str();
    send(client, response.c_str(), (int)response.size(), 0);
}

void handleClient(SOCKET client) {
    char buffer[1024];
    recv(client, buffer, sizeof(buffer), 0); // read and discard the request line

    this_thread::sleep_for(milliseconds(WORK_MS)); // simulated work

    sendResponse(client, 200, "OK", "Hello from the lab server\n");

    closesocket(client);
    completed++;
    active--;
}

void rejectClient(SOCKET client) {
    // Drain whatever the client already sent before closing. On Windows,
    // closing a socket with unread input can hide the 503 reply.
    DWORD readTimeout = 250;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&readTimeout, sizeof(readTimeout));
    char discard[512];
    recv(client, discard, sizeof(discard), 0);

    sendResponse(client, 503, "Service Unavailable", "Server overloaded, try again later\n");
    closesocket(client);

    rejected++;
}

void printStats() {
    long long lastTotal = 0;
    while (true) {
        this_thread::sleep_for(seconds(1));
        long long total = totalRequests.load();
        cout << "[server] rate=" << (total - lastTotal) << "/s"
             << "  active=" << active.load() << "/" << CAPACITY
             << "  completed=" << completed.load()
             << "  rejected=" << rejected.load() << endl;
        lastTotal = total;
    }
}

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, HOST, &addr.sin_addr);

    bind(listener, (sockaddr*)&addr, sizeof(addr));
    listen(listener, SOMAXCONN);

    cout << "Dummy lab server running on http://" << HOST << ":" << PORT << "\n";
    cout << "Capacity: " << CAPACITY << " concurrent requests, " << WORK_MS << " ms simulated work per request\n\n";

    thread(printStats).detach();

    while (true) {
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;

        totalRequests++;

        // active.fetch_add(1) returns the count BEFORE this request was added.
        // If that was already at capacity, we are over budget: reject.
        if (active.fetch_add(1) >= CAPACITY) {
            active--;
            thread(rejectClient, client).detach();
            continue;
        }

        thread(handleClient, client).detach();
    }
}
