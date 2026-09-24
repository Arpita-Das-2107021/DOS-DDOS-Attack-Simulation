#pragma once

// Safety boundary for the whole lab: server and client only ever
// talk to the loopback interface, never a real network address.
const char* const HOST = "127.0.0.1";
const int PORT = 8080;         // dummy target server being tested
const int CONTROL_PORT = 8081; // web control panel (client.exe)
