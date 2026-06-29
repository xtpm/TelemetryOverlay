#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <atomic>
#include <array>
#include <vector>
#include <algorithm>
#include "resource.h"

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Msimg32.lib")

namespace {

constexpr int HEADER_SIZE = 29;
constexpr int ERS_MAX_JOULES = 4000000;
constexpr int TIMER_ID = 1;
constexpr int FRAME_MS = 16;
constexpr int DELTA_UNKNOWN = INT32_MIN;

struct TelemetryState {
    bool connected = false;
    uint16_t packetFormat = 0;
    uint32_t packetCount = 0;
    uint64_t lastSeenTick = 0;

    uint16_t speed = 0;
    float throttle = 0;
    float brake = 0;
    float steer = 0;
    int8_t gear = 0;
    uint16_t rpm = 0;
    uint16_t maxRpm = 12000;
    uint8_t revLights = 0;

    uint32_t lastLapMs = 0;
    uint32_t currentLapMs = 0;
    float lapDistance = 0;
    uint32_t sector1Ms = 0;
    uint32_t sector2Ms = 0;
    uint8_t sector = 0;
    uint8_t lap = 0;
    uint8_t position = 0;
    uint8_t invalidLap = 0;
    uint8_t penalties = 0;
    uint8_t warnings = 0;

    float fuelLaps = 0;
    float fuelInTank = 0;
    float fuelCapacity = 0;
    float ersEnergy = 0;
    uint8_t ersMode = 0;
    uint8_t tyreAge = 0;
    uint8_t tyreCompound = 0;

    uint8_t activeAeroMode = 0;
    uint8_t activeAeroAvailable = 0;
    uint8_t overtakeActive = 0;

    uint32_t personalBestLapMs = 0;
    std::array<uint32_t, 3> personalBestSectorsMs{0, 0, 0};
    uint32_t sessionBestLapMs = 0;
    std::array<uint32_t, 3> sessionBestSectorsMs{0, 0, 0};

    uint8_t traceLap = 0;
    bool traceInvalid = false;
    uint32_t liveReferenceLapMs = 0;
    std::vector<std::pair<float, uint32_t>> currentLapTrace;
    std::vector<std::pair<float, uint32_t>> liveReferenceTrace;
    int stableDeltaMs = DELTA_UNKNOWN;
    uint64_t stableDeltaTick = 0;
};

TelemetryState g_state;
std::mutex g_stateMutex;
std::atomic<bool> g_running{true};
HWND g_hud = nullptr;
HWND g_timing = nullptr;
HINSTANCE g_instance = nullptr;

template <typename T>
T readAt(const uint8_t* data, size_t size, size_t offset) {
    T value{};
    if (offset + sizeof(T) <= size) {
        memcpy(&value, data + offset, sizeof(T));
    }
    return value;
}

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

std::wstring formatLap(uint32_t ms) {
    if (!ms) return L"--:--.---";
    uint32_t minutes = ms / 60000;
    double seconds = (ms % 60000) / 1000.0;
    wchar_t buf[32];
    swprintf_s(buf, L"%u:%06.3f", minutes, seconds);
    return buf;
}

std::wstring formatShortMs(uint32_t ms) {
    if (!ms) return L"--.---";
    wchar_t buf[32];
    swprintf_s(buf, L"%.3f", ms / 1000.0);
    return buf;
}

std::wstring formatDelta(int deltaMs) {
    if (deltaMs == DELTA_UNKNOWN) return L"--.---";
    wchar_t buf[32];
    swprintf_s(buf, L"%c%.3f", deltaMs >= 0 ? L'+' : L'-', std::abs(deltaMs) / 1000.0);
    return buf;
}

std::wstring gearText(int gear) {
    if (gear == -1) return L"R";
    if (gear == 0) return L"N";
    return std::to_wstring(gear);
}

std::wstring tyreName(uint8_t compound) {
    switch (compound) {
        case 7: return L"inter";
        case 8: return L"wet";
        case 16: return L"c5";
        case 17: return L"c4";
        case 18: return L"c3";
        case 19: return L"c2";
        case 20: return L"c1";
        case 21: return L"c0";
        case 22: return L"c6";
        default: return L"--";
    }
}

std::wstring ersModeText(uint8_t mode) {
    switch (mode) {
        case 0: return L"harvest";
        case 1: return L"medium";
        case 2: return L"hotlap";
        case 3: return L"overtake";
        default: return L"--";
    }
}

std::wstring wingText(const TelemetryState& s) {
    if (s.activeAeroMode) return L"straight";
    if (s.activeAeroAvailable) return L"ready";
    if (s.packetFormat == 2026) return L"std";
    return L"--";
}

COLORREF rgb(int r, int g, int b) {
    return RGB(r, g, b);
}

void fillRect(HDC dc, int x, int y, int w, int h, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    RECT rc{x, y, x + w, y + h};
    FillRect(dc, &rc, brush);
    DeleteObject(brush);
}

void strokeRect(HDC dc, int x, int y, int w, int h, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old = SelectObject(dc, pen);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, x, y, x + w, y + h);
    SelectObject(dc, old);
    DeleteObject(pen);
}

HFONT makeFont(int size, int weight = FW_BOLD, const wchar_t* face = L"Bahnschrift") {
    return CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

void drawText(HDC dc, const std::wstring& text, int x, int y, int w, int h, HFONT font, COLORREF color, UINT format) {
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    RECT rc{x, y, x + w, y + h};
    DrawTextW(dc, text.c_str(), -1, &rc, format | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);
}

void drawChip(HDC dc, const wchar_t* label, const std::wstring& value, int x, int y, int w, HFONT small, HFONT valueFont) {
    strokeRect(dc, x, y, w, 28, rgb(64, 64, 64));
    drawText(dc, label, x + 2, y + 3, w - 4, 9, small, rgb(160, 160, 154), DT_CENTER);
    drawText(dc, value, x + 2, y + 15, w - 4, 12, valueFont, rgb(245, 245, 243), DT_CENTER);
}

void drawBar(HDC dc, const wchar_t* label, float value, int x, int y, int w, COLORREF color, HFONT font) {
    drawText(dc, label, x, y - 1, 28, 12, font, rgb(167, 167, 162), DT_LEFT);
    fillRect(dc, x + 34, y + 3, w - 68, 8, rgb(39, 39, 39));
    fillRect(dc, x + 34, y + 3, static_cast<int>((w - 68) * clampf(value, 0, 1)), 8, color);
    wchar_t buf[8];
    swprintf_s(buf, L"%03d", static_cast<int>(std::round(value * 100)));
    drawText(dc, buf, x + w - 28, y - 1, 28, 14, font, rgb(245, 245, 243), DT_RIGHT);
}

void drawSteer(HDC dc, float steer, int x, int y, int w, HFONT font) {
    drawText(dc, L"str", x, y - 1, 28, 12, font, rgb(167, 167, 162), DT_LEFT);
    int bx = x + 34;
    int bw = w - 68;
    fillRect(dc, bx, y + 3, bw, 8, rgb(39, 39, 39));
    fillRect(dc, bx + bw / 2, y, 1, 14, rgb(105, 105, 100));
    int amount = static_cast<int>((bw / 2) * std::abs(clampf(steer, -1, 1)));
    if (steer < 0) {
        fillRect(dc, bx + bw / 2 - amount, y + 3, amount, 8, rgb(245, 213, 71));
    } else {
        fillRect(dc, bx + bw / 2, y + 3, amount, 8, rgb(245, 213, 71));
    }
    wchar_t buf[8];
    swprintf_s(buf, L"%c%02d", steer < -0.01f ? L'L' : steer > 0.01f ? L'R' : L'C', static_cast<int>(std::round(std::abs(steer) * 100)));
    drawText(dc, buf, x + w - 32, y - 1, 32, 14, font, rgb(245, 245, 243), DT_RIGHT);
}

int referenceDelta(const TelemetryState& s) {
    if (s.liveReferenceTrace.size() >= 2 && s.currentLapMs > 0 && s.lapDistance >= 0) {
        const auto& trace = s.liveReferenceTrace;
        auto it = std::lower_bound(
            trace.begin(),
            trace.end(),
            s.lapDistance,
            [](const std::pair<float, uint32_t>& sample, float distance) {
                return sample.first < distance;
            });

        if (it != trace.begin() && it != trace.end()) {
            auto prev = it - 1;
            float span = it->first - prev->first;
            float t = span > 0.01f ? (s.lapDistance - prev->first) / span : 0.0f;
            uint32_t refMs = static_cast<uint32_t>(prev->second + (it->second - prev->second) * clampf(t, 0.0f, 1.0f));
            int delta = static_cast<int>(s.currentLapMs) - static_cast<int>(refMs);
            if (std::abs(delta) < 60000) return delta;
        }
    }

    return DELTA_UNKNOWN;
}

void updateLapTrace(TelemetryState& s) {
    if (s.traceLap != 0 && s.lap != s.traceLap) {
        if (!s.traceInvalid && s.lastLapMs > 0 && s.currentLapTrace.size() > 20 &&
            (s.liveReferenceLapMs == 0 || s.lastLapMs <= s.liveReferenceLapMs)) {
            s.liveReferenceLapMs = s.lastLapMs;
            s.liveReferenceTrace = s.currentLapTrace;
        }

        s.currentLapTrace.clear();
        s.traceInvalid = false;
        s.traceLap = s.lap;
    }

    if (s.traceLap == 0) s.traceLap = s.lap;
    if (s.invalidLap) s.traceInvalid = true;

    if (s.currentLapMs == 0 || s.lapDistance < 0 || s.traceInvalid) return;

    if (!s.currentLapTrace.empty()) {
        const auto& last = s.currentLapTrace.back();
        if (s.lapDistance + 20.0f < last.first || s.currentLapMs + 250 < last.second) {
            s.currentLapTrace.clear();
            s.traceInvalid = true;
            return;
        }
    }

    if (s.currentLapTrace.empty() ||
        s.lapDistance - s.currentLapTrace.back().first >= 2.0f ||
        s.currentLapMs - s.currentLapTrace.back().second >= 100) {
        s.currentLapTrace.push_back({s.lapDistance, s.currentLapMs});
    }

    int delta = referenceDelta(s);
    uint64_t now = GetTickCount64();
    if (delta != DELTA_UNKNOWN &&
        (s.stableDeltaMs == DELTA_UNKNOWN || std::abs(delta - s.stableDeltaMs) < 3000)) {
        s.stableDeltaMs = delta;
        s.stableDeltaTick = now;
    } else if (s.stableDeltaTick && now - s.stableDeltaTick > 1000) {
        s.stableDeltaMs = DELTA_UNKNOWN;
    }
}

void parseLapData(const uint8_t* data, size_t size, uint8_t playerIndex, TelemetryState& s) {
    size_t base = HEADER_SIZE + static_cast<size_t>(playerIndex) * 57;
    if (base + 57 > size) return;
    s.lastLapMs = readAt<uint32_t>(data, size, base + 0);
    s.currentLapMs = readAt<uint32_t>(data, size, base + 4);
    s.sector1Ms = readAt<uint16_t>(data, size, base + 8) + readAt<uint8_t>(data, size, base + 10) * 60000u;
    s.sector2Ms = readAt<uint16_t>(data, size, base + 11) + readAt<uint8_t>(data, size, base + 13) * 60000u;
    s.lapDistance = readAt<float>(data, size, base + 18);
    s.position = readAt<uint8_t>(data, size, base + 32);
    s.lap = readAt<uint8_t>(data, size, base + 33);
    s.sector = readAt<uint8_t>(data, size, base + 36) + 1;
    s.invalidLap = readAt<uint8_t>(data, size, base + 37);
    s.penalties = readAt<uint8_t>(data, size, base + 38);
    s.warnings = readAt<uint8_t>(data, size, base + 39);
    updateLapTrace(s);
}

void parseTelemetry(const uint8_t* data, size_t size, uint16_t format, uint8_t playerIndex, TelemetryState& s) {
    size_t stride = format == 2026 ? 59 : 60;
    size_t base = HEADER_SIZE + static_cast<size_t>(playerIndex) * stride;
    if (base + stride > size) return;
    s.speed = readAt<uint16_t>(data, size, base + 0);
    s.throttle = readAt<float>(data, size, base + 2);
    s.steer = readAt<float>(data, size, base + 6);
    s.brake = readAt<float>(data, size, base + 10);
    s.gear = readAt<int8_t>(data, size, base + 15);
    s.rpm = readAt<uint16_t>(data, size, base + 16);
    s.revLights = readAt<uint8_t>(data, size, base + 19);
}

void parseStatus(const uint8_t* data, size_t size, uint16_t format, uint8_t playerIndex, TelemetryState& s) {
    size_t stride = format == 2026 ? 59 : 55;
    size_t base = HEADER_SIZE + static_cast<size_t>(playerIndex) * stride;
    if (base + stride > size) return;
    s.fuelInTank = readAt<float>(data, size, base + 5);
    s.fuelCapacity = readAt<float>(data, size, base + 9);
    s.fuelLaps = readAt<float>(data, size, base + 13);
    s.maxRpm = readAt<uint16_t>(data, size, base + 17);
    if (!s.maxRpm) s.maxRpm = 12000;
    s.tyreCompound = readAt<uint8_t>(data, size, base + 25);
    s.tyreAge = readAt<uint8_t>(data, size, base + 27);
    s.ersEnergy = readAt<float>(data, size, base + 37);
    s.ersMode = readAt<uint8_t>(data, size, base + 41);
}

void parseTelemetry2(const uint8_t* data, size_t size, uint8_t playerIndex, TelemetryState& s) {
    size_t base = HEADER_SIZE + static_cast<size_t>(playerIndex) * 10;
    if (base + 10 > size) return;
    s.activeAeroMode = readAt<uint8_t>(data, size, base + 0);
    s.activeAeroAvailable = readAt<uint8_t>(data, size, base + 1);
    s.overtakeActive = readAt<uint8_t>(data, size, base + 5);
}

void parseTimeTrial(const uint8_t* data, size_t size, TelemetryState& s) {
    if (HEADER_SIZE + 75 > size) return;
    auto readSet = [&](size_t base, uint32_t& lap, std::array<uint32_t, 3>& sectors) {
        if (!readAt<uint8_t>(data, size, base + 24)) return;
        lap = readAt<uint32_t>(data, size, base + 2);
        sectors[0] = readAt<uint32_t>(data, size, base + 6);
        sectors[1] = readAt<uint32_t>(data, size, base + 10);
        sectors[2] = readAt<uint32_t>(data, size, base + 14);
    };
    readSet(HEADER_SIZE, s.sessionBestLapMs, s.sessionBestSectorsMs);
    readSet(HEADER_SIZE + 25, s.personalBestLapMs, s.personalBestSectorsMs);
}

void parsePacket(const uint8_t* data, size_t size) {
    if (size < HEADER_SIZE) return;
    uint16_t format = readAt<uint16_t>(data, size, 0);
    uint8_t packetId = readAt<uint8_t>(data, size, 6);
    uint8_t playerIndex = readAt<uint8_t>(data, size, 27);
    if ((format != 2025 && format != 2026) || playerIndex >= 24) return;

    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_state.packetFormat = format;
    g_state.connected = true;
    g_state.lastSeenTick = GetTickCount64();
    g_state.packetCount++;
    if (packetId == 2) parseLapData(data, size, playerIndex, g_state);
    if (packetId == 6) parseTelemetry(data, size, format, playerIndex, g_state);
    if (packetId == 7) parseStatus(data, size, format, playerIndex, g_state);
    if (packetId == 14) parseTimeTrial(data, size, g_state);
    if (packetId == 16 && format == 2026) parseTelemetry2(data, size, playerIndex, g_state);
}

void udpThread() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(20777);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    DWORD timeout = 250;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    uint8_t buffer[2048];
    while (g_running) {
        int received = recv(sock, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
        if (received > 0) parsePacket(buffer, static_cast<size_t>(received));
    }

    closesocket(sock);
    WSACleanup();
}

LRESULT CALLBACK overlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        return 1;
    }
    if (msg == WM_LBUTTONDOWN) {
        ReleaseCapture();
        SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return 0;
    }
    if (msg == WM_TIMER) {
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        DestroyWindow(hwnd);
        return 0;
    }
    if (msg == WM_DESTROY) {
        if (hwnd == g_hud) g_hud = nullptr;
        if (hwnd == g_timing) g_timing = nullptr;
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

void paintHud(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    HDC memDc = CreateCompatibleDC(dc);
    HBITMAP memBitmap = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ oldBitmap = SelectObject(memDc, memBitmap);

    fillRect(memDc, 0, 0, rc.right, rc.bottom, rgb(3, 4, 5));
    strokeRect(memDc, 0, 0, rc.right, rc.bottom, rgb(116, 116, 110));
    strokeRect(memDc, 6, 6, rc.right - 12, rc.bottom - 12, rgb(34, 34, 34));

    TelemetryState s;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        s = g_state;
        if (GetTickCount64() - s.lastSeenTick > 2000) s.connected = false;
    }

    HFONT tiny = makeFont(8, FW_BOLD);
    HFONT small = makeFont(9, FW_BOLD);
    HFONT value = makeFont(13, FW_BOLD, L"Bahnschrift SemiCondensed");
    HFONT big = makeFont(38, FW_BOLD, L"Bahnschrift SemiCondensed");
    HFONT med = makeFont(27, FW_BOLD, L"Bahnschrift SemiCondensed");

    float ers = clampf(s.ersEnergy / static_cast<float>(ERS_MAX_JOULES), 0, 1);
    COLORREF gearFill = ers >= 0.99f ? rgb(35, 243, 106) : rgb(20, 82, 43);
    fillRect(memDc, 12, 14, static_cast<int>(68 * ers), 58, gearFill);
    strokeRect(memDc, 12, 14, 68, 58, rgb(64, 64, 64));
    drawText(memDc, gearText(s.gear), 12, 20, 68, 44, big, rgb(245, 245, 243), DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    drawText(memDc, L"r_", 92, 10, 22, 16, small, rgb(245, 245, 243), DT_LEFT);
    drawText(memDc, s.connected ? L"telemetry live" : L"checking...", 120, 11, 112, 14, tiny, rgb(110, 110, 104), DT_LEFT);

    int revX = 92;
    int lit = static_cast<int>(std::round(clampf(s.revLights / 100.0f, 0, 1) * 18));
    for (int i = 0; i < 18; ++i) {
        COLORREF c = rgb(44, 44, 44);
        if (i < lit) c = i < 12 ? rgb(35, 243, 106) : i < 15 ? rgb(245, 213, 71) : rgb(255, 74, 74);
        fillRect(memDc, revX + i * 20, 32, 17, 7, c);
    }

    drawText(memDc, L"KM/H", 92, 48, 60, 10, small, rgb(167, 167, 162), DT_LEFT);
    drawText(memDc, std::to_wstring(s.speed), 92, 58, 74, 28, med, rgb(245, 245, 243), DT_LEFT);
    drawText(memDc, L"RPM", 190, 48, 60, 10, small, rgb(167, 167, 162), DT_LEFT);
    wchar_t rpmBuf[16];
    swprintf_s(rpmBuf, L"%05u", s.rpm);
    drawText(memDc, rpmBuf, 190, 58, 94, 28, med, rgb(245, 245, 243), DT_LEFT);

    fillRect(memDc, 0, 86, rc.right, 1, rgb(28, 28, 28));
    int y = 94;
    drawChip(memDc, L"pos", std::to_wstring(s.position ? s.position : 0), 12, y, 34, small, value);
    drawChip(memDc, L"lap", std::to_wstring(s.lap ? s.lap : 0), 50, y, 34, small, value);
    drawChip(memDc, L"sec", std::to_wstring(s.sector ? s.sector : 0), 88, y, 34, small, value);
    wchar_t pen[16];
    swprintf_s(pen, L"%us / %uw", s.penalties, s.warnings);
    drawChip(memDc, L"pen", pen, 128, y, 70, small, value);
    drawChip(memDc, L"wing", wingText(s), 204, y, 64, small, value);
    wchar_t fuel[16];
    swprintf_s(fuel, L"%.1fL", s.fuelInTank);
    drawChip(memDc, L"fuel", fuel, 274, y, 58, small, value);
    wchar_t est[16];
    swprintf_s(est, L"%.1f", s.fuelLaps);
    drawChip(memDc, L"est", est, 338, y, 50, small, value);
    drawChip(memDc, L"ers", ersModeText(s.ersMode), 394, y, 72, small, value);
    drawChip(memDc, L"tyre", tyreName(s.tyreCompound) + L" " + std::to_wstring(s.tyreAge) + L"l", 472, y, 60, small, value);

    drawBar(memDc, L"thr", s.throttle, 12, 150, 160, rgb(35, 243, 106), tiny);
    drawBar(memDc, L"brk", s.brake, 190, 150, 160, rgb(255, 74, 74), tiny);
    drawSteer(memDc, s.steer, 368, 150, 160, tiny);

    BitBlt(dc, 0, 0, rc.right, rc.bottom, memDc, 0, 0, SRCCOPY);

    DeleteObject(tiny);
    DeleteObject(small);
    DeleteObject(value);
    DeleteObject(big);
    DeleteObject(med);
    SelectObject(memDc, oldBitmap);
    DeleteObject(memBitmap);
    DeleteDC(memDc);
    EndPaint(hwnd, &ps);
}

void paintTiming(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    HDC memDc = CreateCompatibleDC(dc);
    HBITMAP memBitmap = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ oldBitmap = SelectObject(memDc, memBitmap);

    fillRect(memDc, 0, 0, rc.right, rc.bottom, rgb(9, 10, 12));
    strokeRect(memDc, 0, 0, rc.right, rc.bottom, rgb(116, 116, 110));

    TelemetryState s;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        s = g_state;
    }

    HFONT small = makeFont(12, FW_BOLD);
    HFONT value = makeFont(16, FW_BOLD, L"Bahnschrift SemiCondensed");
    HFONT sectorFont = makeFont(14, FW_BOLD, L"Bahnschrift SemiCondensed");

    uint32_t best = s.personalBestLapMs ? s.personalBestLapMs : s.sessionBestLapMs;
    int delta = s.stableDeltaMs;

    drawText(memDc, L"current:", 16, 15, 80, 16, small, rgb(167, 167, 162), DT_LEFT);
    drawText(memDc, formatLap(s.currentLapMs), 150, 15, 90, 16, value, rgb(245, 245, 243), DT_RIGHT);
    drawText(memDc, L"best:", 250, 15, 70, 16, small, rgb(167, 167, 162), DT_LEFT);
    drawText(memDc, formatLap(best), 330, 15, 84, 16, value, rgb(245, 245, 243), DT_RIGHT);

    drawText(memDc, L"last:", 16, 40, 80, 16, small, rgb(167, 167, 162), DT_LEFT);
    drawText(memDc, formatLap(s.lastLapMs), 150, 40, 90, 16, value, rgb(245, 245, 243), DT_RIGHT);
    drawText(memDc, L"delta:", 250, 40, 70, 16, small, rgb(167, 167, 162), DT_LEFT);
    COLORREF deltaColor = delta == DELTA_UNKNOWN ? rgb(245, 245, 243) : (delta <= 0 ? rgb(35, 243, 106) : rgb(255, 74, 74));
    drawText(memDc, formatDelta(delta), 330, 40, 84, 16, value, deltaColor, DT_RIGHT);

    strokeRect(memDc, 16, 72, rc.right - 32, 36, rgb(64, 64, 64));
    int col = (rc.right - 32) / 3;
    fillRect(memDc, 16 + col, 72, 1, 36, rgb(64, 64, 64));
    fillRect(memDc, 16 + col * 2, 72, 1, 36, rgb(64, 64, 64));
    uint32_t s3 = s.lastLapMs && s.sector1Ms && s.sector2Ms ? s.lastLapMs - s.sector1Ms - s.sector2Ms : 0;
    drawText(memDc, L"s1", 16, 76, col, 12, small, rgb(167, 167, 162), DT_CENTER);
    drawText(memDc, formatShortMs(s.sector1Ms), 16, 91, col, 14, sectorFont, rgb(245, 245, 243), DT_CENTER);
    drawText(memDc, L"s2", 16 + col, 76, col, 12, small, rgb(167, 167, 162), DT_CENTER);
    drawText(memDc, formatShortMs(s.sector2Ms), 16 + col, 91, col, 14, sectorFont, rgb(245, 245, 243), DT_CENTER);
    drawText(memDc, L"s3", 16 + col * 2, 76, col, 12, small, rgb(167, 167, 162), DT_CENTER);
    drawText(memDc, formatShortMs(s3), 16 + col * 2, 91, col, 14, sectorFont, rgb(245, 245, 243), DT_CENTER);

    BitBlt(dc, 0, 0, rc.right, rc.bottom, memDc, 0, 0, SRCCOPY);

    DeleteObject(small);
    DeleteObject(value);
    DeleteObject(sectorFont);
    SelectObject(memDc, oldBitmap);
    DeleteObject(memBitmap);
    DeleteDC(memDc);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK hudProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        paintHud(hwnd);
        return 0;
    }
    return overlayProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK timingProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        paintTiming(hwnd);
        return 0;
    }
    return overlayProc(hwnd, msg, wp, lp);
}

HWND createOverlayWindow(const wchar_t* cls, const wchar_t* title, int x, int y, int w, int h, WNDPROC proc) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = g_instance;
    wc.lpszClassName = cls;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(g_instance, MAKEINTRESOURCE(IDI_APP_ICON));
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        cls, title, WS_POPUP,
        x, y, w, h, nullptr, nullptr, g_instance, nullptr);
    SetTimer(hwnd, TIMER_ID, FRAME_MS, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    return hwnd;
}

enum MenuCommand {
    ID_HUD = 1001,
    ID_TIMING = 1002,
    ID_BOTH = 1003,
    ID_EXIT = 1004
};

struct LauncherAction {
    int id;
    RECT rect;
    const wchar_t* title;
    const wchar_t* subtitle;
};

int g_hoverAction = 0;

LauncherAction g_actions[] = {
    {ID_BOTH, {24, 112, 456, 162}, L"launch everything", L"opens retrial HUD and timing strip"},
    {ID_HUD, {24, 174, 456, 224}, L"retrial HUD only", L"speed, gear, inputs, systems"},
    {ID_TIMING, {24, 236, 456, 286}, L"timing strip only", L"current, best, delta, sectors"},
    {ID_EXIT, {336, 316, 456, 352}, L"exit", L"close launcher"}
};

void launchAction(int id) {
    if ((id == ID_HUD || id == ID_BOTH) && !g_hud) {
            g_hud = createOverlayWindow(L"F125CppHud", L"HUD", 80, 80, 545, 168, hudProc);
    }
    if ((id == ID_TIMING || id == ID_BOTH) && !g_timing) {
        g_timing = createOverlayWindow(L"F125CppTiming", L"Timing", 82, 38, 430, 128, timingProc);
    }
}

void drawLauncherCard(HDC dc, const LauncherAction& action, bool hover, HFONT titleFont, HFONT bodyFont) {
    COLORREF fill = hover ? rgb(22, 24, 27) : rgb(11, 12, 14);
    COLORREF border = hover ? rgb(245, 245, 243) : rgb(64, 64, 64);
    fillRect(dc, action.rect.left, action.rect.top, action.rect.right - action.rect.left, action.rect.bottom - action.rect.top, fill);
    strokeRect(dc, action.rect.left, action.rect.top, action.rect.right - action.rect.left, action.rect.bottom - action.rect.top, border);
    if (hover) {
        fillRect(dc, action.rect.left, action.rect.top, 4, action.rect.bottom - action.rect.top, rgb(35, 243, 106));
    }
    drawText(dc, action.title, action.rect.left + 16, action.rect.top + 9, 260, 16, titleFont, rgb(245, 245, 243), DT_LEFT);
    drawText(dc, action.subtitle, action.rect.left + 16, action.rect.top + 29, 290, 14, bodyFont, rgb(167, 167, 162), DT_LEFT);
    if (action.id != ID_EXIT) {
        drawText(dc, L"open", action.rect.right - 66, action.rect.top + 16, 44, 16, bodyFont, hover ? rgb(35, 243, 106) : rgb(167, 167, 162), DT_RIGHT);
    }
}

void paintLauncher(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc{};
    GetClientRect(hwnd, &rc);

    HDC memDc = CreateCompatibleDC(dc);
    HBITMAP memBitmap = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ oldBitmap = SelectObject(memDc, memBitmap);

    fillRect(memDc, 0, 0, rc.right, rc.bottom, rgb(4, 5, 6));
    strokeRect(memDc, 0, 0, rc.right, rc.bottom, rgb(116, 116, 110));
    strokeRect(memDc, 8, 8, rc.right - 16, rc.bottom - 16, rgb(34, 34, 34));
    fillRect(memDc, 24, 24, 4, 64, rgb(35, 243, 106));

    HFONT title = makeFont(28, FW_BOLD, L"Bahnschrift SemiCondensed");
    HFONT logo = makeFont(18, FW_BOLD);
    HFONT label = makeFont(11, FW_BOLD);
    HFONT body = makeFont(10, FW_BOLD);

    drawText(memDc, L"r_", 40, 24, 34, 20, logo, rgb(245, 245, 243), DT_LEFT);
    drawText(memDc, L"f1 telemetry", 40, 48, 220, 34, title, rgb(245, 245, 243), DT_LEFT);
    drawText(memDc, L"native c++ overlay launcher", 40, 82, 260, 16, label, rgb(167, 167, 162), DT_LEFT);

    TelemetryState s;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        s = g_state;
    }
    drawText(memDc, s.connected ? L"udp live" : L"waiting for udp", 330, 34, 126, 16, label, s.connected ? rgb(35, 243, 106) : rgb(245, 213, 71), DT_RIGHT);
    drawText(memDc, L"127.0.0.1:20777", 330, 55, 126, 16, body, rgb(167, 167, 162), DT_RIGHT);

    for (const auto& action : g_actions) {
        drawLauncherCard(memDc, action, g_hoverAction == action.id, label, body);
    }

    drawText(memDc, L"tip: drag overlay windows to place them. esc closes each overlay.", 24, 326, 310, 16, body, rgb(110, 110, 104), DT_LEFT);

    BitBlt(dc, 0, 0, rc.right, rc.bottom, memDc, 0, 0, SRCCOPY);

    DeleteObject(title);
    DeleteObject(logo);
    DeleteObject(label);
    DeleteObject(body);
    SelectObject(memDc, oldBitmap);
    DeleteObject(memBitmap);
    DeleteDC(memDc);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK menuProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        return 1;
    }
    if (msg == WM_CREATE) {
        SetTimer(hwnd, TIMER_ID, 250, nullptr);
        return 0;
    }
    if (msg == WM_PAINT) {
        paintLauncher(hwnd);
        return 0;
    }
    if (msg == WM_TIMER) {
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if (msg == WM_MOUSEMOVE) {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int hover = 0;
        for (const auto& action : g_actions) {
            if (PtInRect(&action.rect, pt)) {
                hover = action.id;
                break;
            }
        }
        if (hover != g_hoverAction) {
            g_hoverAction = hover;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        return 0;
    }
    if (msg == WM_MOUSELEAVE) {
        g_hoverAction = 0;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if (msg == WM_LBUTTONDOWN) {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        for (const auto& action : g_actions) {
            if (PtInRect(&action.rect, pt)) {
                if (action.id == ID_EXIT) {
                    DestroyWindow(hwnd);
                } else {
                    launchAction(action.id);
                }
                return 0;
            }
        }
        return 0;
    }
    if (msg == WM_DESTROY) {
        g_running = false;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    g_instance = hInstance;
    std::thread network(udpThread);

    WNDCLASSW wc{};
    wc.lpfnWndProc = menuProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"F125CppMenu";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_ICON));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    HWND menu = CreateWindowW(L"F125CppMenu", L"F1 25 C++ Overlay Launcher",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        160, 160, 500, 410, nullptr, nullptr, hInstance, nullptr);
    ShowWindow(menu, nCmdShow);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_running = false;
    if (network.joinable()) network.join();
    return 0;
}
