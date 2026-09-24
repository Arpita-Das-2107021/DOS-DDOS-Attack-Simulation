# Builds both programs with g++, without needing CMake/Ninja.
# Winsock functions (socket, connect, send, ...) live in ws2_32.dll,
# so -lws2_32 must always be passed when linking either file.

g++ -std=c++17 -Wall -Wextra -static -o server.exe server.cpp -lws2_32
if (-not $?) { exit 1 }

g++ -std=c++17 -Wall -Wextra -static -o client.exe client.cpp -lws2_32
if (-not $?) { exit 1 }

Write-Host "Build complete: server.exe and client.exe"
