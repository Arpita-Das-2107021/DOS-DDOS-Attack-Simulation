// Web control panel for the local DoS/DDoS lab.
//
// This is itself a small local HTTP server. Open http://127.0.0.1:8081
// in a browser to get a dark-themed dashboard instead of a terminal menu.
// The dashboard calls a few JSON endpoints below to start a phase and
// poll its live progress:
//   1. Baseline - one client sending requests at a normal, spaced-out pace.
//   2. DoS      - one source opening many simultaneous connections.
//   3. DDoS     - many simulated bot sources, all still on loopback.
// Results (success/rejected/failed counts + latency) build up in a table
// on the page and can be downloaded as results.csv.

#include "common.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <winsock2.h>
#include <ws2tcpip.h>

using namespace std;
using namespace chrono;

const int MIN_DURATION_SEC = 5;
const int MAX_DURATION_SEC = 30; // keeps every run short and bounded

const int BASELINE_WORKERS = 1;
const int BASELINE_DELAY_MS = 300; // normal, human-like pace

const int DOS_WORKERS = 20;   // one logical source, many connections
const int DDOS_WORKERS = 60;  // many simulated bot sources
const int NO_DELAY = 0;       // attack phases send as fast as possible

struct PhaseResult {
    string name;
    int workers = 0;
    int durationSec = 0;
    long long total = 0, success = 0, rejected = 0, failed = 0;
    double avgLatencyMs = 0, minLatencyMs = 0, maxLatencyMs = 0;
    double throughputRps = 0;
};

// --- shared state between the HTTP handler threads and the phase runner ---

atomic<bool> gRunning{false};

mutex gMutex; // protects everything below
string gPhaseLabel = "-";
int gDurationSec = 0;
steady_clock::time_point gStartTime;
vector<PhaseResult> gResults;

// Counters for whichever phase is currently running.
atomic<long long> gSuccess{0}, gRejected{0}, gFailed{0};
mutex gLatencyMutex;
double gLatencySum = 0, gLatencyMin = 0, gLatencyMax = 0;
long long gLatencyCount = 0;

void resetCounters() {
    gSuccess = 0;
    gRejected = 0;
    gFailed = 0;
    gLatencySum = 0;
    gLatencyMin = 0;
    gLatencyMax = 0;
    gLatencyCount = 0;
}

void recordLatency(double ms) {
    lock_guard<mutex> lock(gLatencyMutex);
    if (gLatencyCount == 0 || ms < gLatencyMin) gLatencyMin = ms;
    if (ms > gLatencyMax) gLatencyMax = ms;
    gLatencySum += ms;
    gLatencyCount++;
}

// --- traffic generation against the dummy target server ---

// Sends one GET request to the target server and records the outcome.
void sendOneRequest() {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        gFailed++;
        return;
    }

    DWORD timeout = 2000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, HOST, &addr.sin_addr);

    auto start = steady_clock::now();

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(sock);
        gFailed++;
        return;
    }

    string request = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    if (send(sock, request.c_str(), (int)request.size(), 0) <= 0) {
        closesocket(sock);
        gFailed++;
        return;
    }

    char buffer[512];
    int bytes = recv(sock, buffer, sizeof(buffer) - 1, 0);
    closesocket(sock);

    if (bytes <= 0) {
        gFailed++;
        return;
    }

    double latencyMs = duration<double, milli>(steady_clock::now() - start).count();
    recordLatency(latencyMs);

    string response(buffer, bytes);
    if (response.find("200") != string::npos) gSuccess++;
    else if (response.find("503") != string::npos) gRejected++;
    else gFailed++;
}

// One worker thread: fires requests until the deadline, pausing `delayMs`
// between each one. delayMs = 0 means "as fast as possible".
void worker(steady_clock::time_point deadline, int delayMs) {
    while (steady_clock::now() < deadline) {
        sendOneRequest();
        if (delayMs > 0) this_thread::sleep_for(milliseconds(delayMs));
    }
}

// Runs one phase to completion (blocking) and returns its result.
// Called from a background thread so the web server stays responsive.
PhaseResult runOnePhase(const string& name, int workers, int delayMs, int durationSec) {
    resetCounters();
    auto start = steady_clock::now();
    {
        lock_guard<mutex> lock(gMutex);
        gPhaseLabel = name;
        gDurationSec = durationSec;
        gStartTime = start;
    }

    auto deadline = start + seconds(durationSec);
    vector<thread> pool;
    for (int i = 0; i < workers; i++) pool.emplace_back(worker, deadline, delayMs);
    for (auto& t : pool) t.join();

    PhaseResult r;
    r.name = name;
    r.workers = workers;
    r.durationSec = durationSec;
    r.success = gSuccess.load();
    r.rejected = gRejected.load();
    r.failed = gFailed.load();
    r.total = r.success + r.rejected + r.failed;
    r.avgLatencyMs = gLatencyCount ? gLatencySum / gLatencyCount : 0;
    r.minLatencyMs = gLatencyMin;
    r.maxLatencyMs = gLatencyMax;
    r.throughputRps = r.total / (double)durationSec;
    return r;
}

// Runs the requested phase (or all three) in the background, then clears
// gRunning so the dashboard knows it can start another one.
void runJob(string phase, int durationSec) {
    vector<PhaseResult> newResults;

    if (phase == "baseline") {
        newResults.push_back(runOnePhase("Baseline", BASELINE_WORKERS, BASELINE_DELAY_MS, durationSec));
    } else if (phase == "dos") {
        newResults.push_back(runOnePhase("DoS", DOS_WORKERS, NO_DELAY, durationSec));
    } else if (phase == "ddos") {
        newResults.push_back(runOnePhase("DDoS", DDOS_WORKERS, NO_DELAY, durationSec));
    } else if (phase == "all") {
        newResults.push_back(runOnePhase("Baseline", BASELINE_WORKERS, BASELINE_DELAY_MS, durationSec));
        newResults.push_back(runOnePhase("DoS", DOS_WORKERS, NO_DELAY, durationSec));
        newResults.push_back(runOnePhase("DDoS", DDOS_WORKERS, NO_DELAY, durationSec));
    }

    {
        lock_guard<mutex> lock(gMutex);
        for (auto& r : newResults) gResults.push_back(r);
    }

    gRunning = false;
}

// --- tiny JSON builders (field values are all server-controlled, so no escaping needed) ---

string statusJson() {
    bool running = gRunning.load();
    string phase;
    int durationSec;
    double elapsedSec;
    {
        lock_guard<mutex> lock(gMutex);
        phase = gPhaseLabel;
        durationSec = gDurationSec;
        elapsedSec = running ? duration<double>(steady_clock::now() - gStartTime).count() : durationSec;
    }
    if (elapsedSec > durationSec) elapsedSec = durationSec;

    ostringstream out;
    out << fixed << setprecision(1);
    out << "{"
        << "\"running\":" << (running ? "true" : "false") << ","
        << "\"phase\":\"" << phase << "\","
        << "\"elapsedSec\":" << elapsedSec << ","
        << "\"durationSec\":" << durationSec << ","
        << "\"total\":" << (gSuccess.load() + gRejected.load() + gFailed.load()) << ","
        << "\"success\":" << gSuccess.load() << ","
        << "\"rejected\":" << gRejected.load() << ","
        << "\"failed\":" << gFailed.load()
        << "}";
    return out.str();
}

string resultJson(const PhaseResult& r) {
    ostringstream out;
    out << fixed << setprecision(1);
    out << "{"
        << "\"name\":\"" << r.name << "\","
        << "\"workers\":" << r.workers << ","
        << "\"durationSec\":" << r.durationSec << ","
        << "\"total\":" << r.total << ","
        << "\"success\":" << r.success << ","
        << "\"rejected\":" << r.rejected << ","
        << "\"failed\":" << r.failed << ","
        << "\"avgLatencyMs\":" << r.avgLatencyMs << ","
        << "\"minLatencyMs\":" << r.minLatencyMs << ","
        << "\"maxLatencyMs\":" << r.maxLatencyMs << ","
        << "\"throughputRps\":" << r.throughputRps
        << "}";
    return out.str();
}

string resultsJson() {
    lock_guard<mutex> lock(gMutex);
    ostringstream out;
    out << "[";
    for (size_t i = 0; i < gResults.size(); i++) {
        if (i > 0) out << ",";
        out << resultJson(gResults[i]);
    }
    out << "]";
    return out.str();
}

string resultsCsv() {
    lock_guard<mutex> lock(gMutex);
    ostringstream out;
    out << "phase,workers,duration_s,total,success,rejected,failed,avg_latency_ms,min_latency_ms,max_latency_ms,throughput_rps\n";
    out << fixed << setprecision(1);
    for (const auto& r : gResults) {
        out << r.name << "," << r.workers << "," << r.durationSec << "," << r.total << ","
            << r.success << "," << r.rejected << "," << r.failed << ","
            << r.avgLatencyMs << "," << r.minLatencyMs << "," << r.maxLatencyMs << ","
            << r.throughputRps << "\n";
    }
    return out.str();
}

// --- minimal HTTP plumbing ---

string loadDashboardHtml() {
    ifstream file("dashboard.html");
    if (!file.is_open()) return "<h1>dashboard.html not found next to client.exe</h1>";
    return string((istreambuf_iterator<char>(file)), istreambuf_iterator<char>());
}

bool readRequest(SOCKET client, string& method, string& path, string& query) {
    char buffer[2048] = {};
    int bytes = recv(client, buffer, sizeof(buffer) - 1, 0);
    if (bytes <= 0) return false;

    string request(buffer, bytes);
    size_t methodEnd = request.find(' ');
    size_t pathEnd = request.find(' ', methodEnd + 1);
    if (methodEnd == string::npos || pathEnd == string::npos) return false;

    method = request.substr(0, methodEnd);
    string target = request.substr(methodEnd + 1, pathEnd - methodEnd - 1);

    size_t qpos = target.find('?');
    if (qpos == string::npos) {
        path = target;
        query = "";
    } else {
        path = target.substr(0, qpos);
        query = target.substr(qpos + 1);
    }
    return true;
}

string getQueryParam(const string& query, const string& key) {
    string search = key + "=";
    size_t pos = query.find(search);
    if (pos == string::npos) return "";
    pos += search.size();
    size_t end = query.find('&', pos);
    return query.substr(pos, end == string::npos ? string::npos : end - pos);
}

void sendHttpResponse(SOCKET client, int code, const string& statusText, const string& contentType,
                       const string& body, const string& extraHeaders = "") {
    ostringstream out;
    out << "HTTP/1.1 " << code << " " << statusText << "\r\n"
        << "Content-Type: " << contentType << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << "Cache-Control: no-store\r\n"
        << extraHeaders
        << "\r\n"
        << body;

    string response = out.str();
    send(client, response.c_str(), (int)response.size(), 0);
}

void handleConnection(SOCKET client) {
    string method, path, query;
    if (!readRequest(client, method, path, query)) {
        closesocket(client);
        return;
    }

    if (path == "/") {
        sendHttpResponse(client, 200, "OK", "text/html", loadDashboardHtml());

    } else if (path == "/api/status") {
        sendHttpResponse(client, 200, "OK", "application/json", statusJson());

    } else if (path == "/api/results") {
        sendHttpResponse(client, 200, "OK", "application/json", resultsJson());

    } else if (path == "/api/csv") {
        sendHttpResponse(client, 200, "OK", "text/csv", resultsCsv(),
                          "Content-Disposition: attachment; filename=results.csv\r\n");

    } else if (path == "/api/run") {
        string phase = getQueryParam(query, "phase");
        bool validPhase = (phase == "baseline" || phase == "dos" || phase == "ddos" || phase == "all");

        if (!validPhase) {
            sendHttpResponse(client, 400, "Bad Request", "application/json", "{\"ok\":false,\"error\":\"unknown phase\"}");
        } else {
            int duration = 10;
            string durationStr = getQueryParam(query, "duration");
            if (!durationStr.empty()) {
                try { duration = stoi(durationStr); } catch (...) { duration = 10; }
            }
            duration = max(MIN_DURATION_SEC, min(MAX_DURATION_SEC, duration));

            bool expected = false;
            if (!gRunning.compare_exchange_strong(expected, true)) {
                sendHttpResponse(client, 409, "Conflict", "application/json",
                                  "{\"ok\":false,\"error\":\"a phase is already running\"}");
            } else {
                thread(runJob, phase, duration).detach();
                sendHttpResponse(client, 200, "OK", "application/json", "{\"ok\":true}");
            }
        }

    } else if (path == "/api/reset") {
        if (gRunning.load()) {
            sendHttpResponse(client, 409, "Conflict", "application/json", "{\"ok\":false,\"error\":\"a phase is running\"}");
        } else {
            lock_guard<mutex> lock(gMutex);
            gResults.clear();
            sendHttpResponse(client, 200, "OK", "application/json", "{\"ok\":true}");
        }

    } else {
        sendHttpResponse(client, 404, "Not Found", "text/plain", "Not found\n");
    }

    closesocket(client);
}

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CONTROL_PORT);
    inet_pton(AF_INET, HOST, &addr.sin_addr);

    bind(listener, (sockaddr*)&addr, sizeof(addr));
    listen(listener, SOMAXCONN);

    cout << "==================================================\n";
    cout << " DoS / DDoS Local Lab - Control Panel\n";
    cout << "==================================================\n";
    cout << "Open http://" << HOST << ":" << CONTROL_PORT << " in your browser.\n";
    cout << "Target server under test: http://" << HOST << ":" << PORT << "\n\n";

    while (true) {
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        thread(handleConnection, client).detach();
    }
}
