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
    uint8_t totalLaps = 0;
    uint8_t sessionType = 0;
    uint16_t sessionTimeLeft = 0;
    uint8_t pitSpeedLimit = 0;
    uint8_t safetyCarStatus = 0;
    uint8_t pitStopWindowIdealLap = 0;
    uint8_t pitStopWindowLatestLap = 0;
    uint8_t pitStopRejoinPosition = 0;
    int8_t vehicleFiaFlags = -1;

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
    uint8_t drsActive = 0;
    uint8_t drsAllowed = 0;
    uint16_t drsActivationDistance = 0;
    uint8_t drsFault = 0;
    uint8_t ersFault = 0;

    std::array<uint16_t, 4> brakeTemps{0, 0, 0, 0};
    std::array<uint8_t, 4> tyreSurfaceTemps{0, 0, 0, 0};
    std::array<uint8_t, 4> tyreInnerTemps{0, 0, 0, 0};
    std::array<float, 4> tyreWear{0, 0, 0, 0};
    std::array<uint8_t, 4> tyreBlisters{0, 0, 0, 0};

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
HWND g_info = nullptr;
HINSTANCE g_instance = nullptr;

enum class RegulationMode {
    Reg2025,
    Reg2026
};

RegulationMode g_regulationMode = RegulationMode::Reg2026;

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

std::wstring formatClock(uint16_t secondsLeft) {
    if (!secondsLeft) return L"--:--";
    wchar_t buf[16];
    swprintf_s(buf, L"%u:%02u", secondsLeft / 60, secondsLeft % 60);
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

std::wstring sessionTypeText(uint8_t type) {
    switch (type) {
        case 1: return L"P1";
        case 2: return L"P2";
        case 3: return L"P3";
        case 4: return L"SHORT P";
        case 5: return L"Q1";
        case 6: return L"Q2";
        case 7: return L"Q3";
        case 8: return L"SHORT Q";
        case 10: return L"RACE";
        case 11: return L"RACE 2";
        case 12: return L"RACE 3";
        case 13: return L"TT";
        default: return L"--";
    }
}

std::wstring safetyCarText(uint8_t status) {
    switch (status) {
        case 1: return L"SC";
        case 2: return L"VSC";
        case 3: return L"FORM";
        default: return L"CLEAR";
    }
}

std::wstring flagsText(int8_t flags) {
    switch (flags) {
        case -1: return L"INV";
        case 1: return L"GREEN";
        case 2: return L"BLUE";
        case 3: return L"YELLOW";
        case 4: return L"RED";
        default: return L"--";
    }
}

std::wstring warningText(const TelemetryState& s, float ersPct) {
    if (s.drsFault) return L"DRS FAULT";
    if (s.ersFault) return L"ERS FAULT";
    if (g_regulationMode == RegulationMode::Reg2025 && s.drsAllowed) return L"DRS READY";
    if (g_regulationMode == RegulationMode::Reg2026 && s.activeAeroAvailable) return L"AERO READY";
    if (s.safetyCarStatus) return safetyCarText(s.safetyCarStatus);
    if (s.vehicleFiaFlags == 3 || s.vehicleFiaFlags == 4) return flagsText(s.vehicleFiaFlags);
    return L"NOMINAL";
}

std::wstring ersStatusText(const TelemetryState& s, float ersPct) {
    if (s.ersFault) return L"fault";
    if (ersPct <= 10.0f) return L"low";
    return ersModeText(s.ersMode);
}

std::wstring wingText(const TelemetryState& s) {
    if (g_regulationMode == RegulationMode::Reg2025) {
        if (s.drsActive) return L"open";
        if (s.drsAllowed) return L"ready";
        if (s.drsActivationDistance) return std::to_wstring(s.drsActivationDistance) + L"m";
        return L"--";
    }
    if (s.activeAeroMode) return L"straight";
    if (s.activeAeroAvailable) return L"ready";
    if (s.packetFormat == 2026) return L"std";
    return L"--";
}

bool regulationSystemActive(const TelemetryState& s) {
    return g_regulationMode == RegulationMode::Reg2025 ? s.drsActive != 0 : s.activeAeroMode != 0;
}

COLORREF tempColor(uint16_t temp) {
    if (temp >= 115) return RGB(255, 74, 74);
    if (temp >= 105) return RGB(245, 213, 71);
    return RGB(35, 243, 106);
}

COLORREF brakeColor(uint16_t temp) {
    if (temp >= 950) return RGB(255, 74, 74);
    if (temp >= 800) return RGB(245, 213, 71);
    return RGB(35, 243, 106);
}

COLORREF wearColor(float wear) {
    if (wear >= 65.0f) return RGB(255, 74, 74);
    if (wear >= 40.0f) return RGB(245, 213, 71);
    return RGB(35, 243, 106);
}

const wchar_t* regulationTitle() {
    return g_regulationMode == RegulationMode::Reg2025 ? L"2025 regs" : L"2026 regs";
}

bool supportedPacketFormat(uint16_t format) {
    return format == 2026 || format == 2025 || format == 2024 || format == 2023;
}

bool looksLike2026Layout(uint8_t packetId, size_t size, uint16_t format) {
    if (format == 2026) return true;
    if (packetId == 6) return size > 1400;
    if (packetId == 7) return size > 1300;
    return false;
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
    strokeRect(dc, x, y, w, 34, rgb(64, 64, 64));
    drawText(dc, label, x + 3, y + 4, w - 6, 10, small, rgb(160, 160, 154), DT_CENTER);
    drawText(dc, value, x + 3, y + 17, w - 6, 15, valueFont, rgb(245, 245, 243), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void drawStatCell(HDC dc, const wchar_t* label, const std::wstring& value, int x, int y, int w, HFONT small, HFONT valueFont, bool divider) {
    if (divider) fillRect(dc, x, y + 7, 1, 26, rgb(58, 58, 56));
    drawText(dc, label, x + 8, y + 5, w - 16, 10, small, rgb(160, 160, 154), DT_CENTER);
    drawText(dc, value, x + 8, y + 18, w - 16, 15, valueFont, rgb(245, 245, 243), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void drawInfoCell(HDC dc, const wchar_t* label, const std::wstring& value, int x, int y, int w, HFONT small, HFONT valueFont, COLORREF color = RGB(245, 245, 243)) {
    drawText(dc, label, x, y, w, 10, small, rgb(160, 160, 154), DT_LEFT);
    drawText(dc, value, x, y + 12, w, 16, valueFont, color, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void drawTyreCorner(HDC dc, const wchar_t* label, uint8_t temp, float wear, uint8_t blister, int x, int y, int w, HFONT tiny, HFONT valueFont) {
    drawText(dc, label, x, y, 22, 11, tiny, rgb(160, 160, 154), DT_LEFT);
    drawText(dc, std::to_wstring(temp) + L"C", x + 24, y, 42, 12, valueFont, tempColor(temp), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    drawText(dc, std::to_wstring(static_cast<int>(std::round(wear))) + L"%", x + 70, y, 34, 12, valueFont, wearColor(wear), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (blister) {
        drawText(dc, L"B" + std::to_wstring(blister), x + w - 26, y, 26, 12, tiny, rgb(255, 74, 74), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
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

int fallbackDelta(const TelemetryState& s) {
    auto ref = s.personalBestLapMs ? s.personalBestSectorsMs : s.sessionBestSectorsMs;
    uint32_t refLap = s.personalBestLapMs ? s.personalBestLapMs : s.sessionBestLapMs;
    if (!refLap) return DELTA_UNKNOWN;

    if (!ref[0] && !ref[1] && !ref[2]) {
        return s.currentLapMs ? static_cast<int>(s.currentLapMs) - static_cast<int>(refLap) : DELTA_UNKNOWN;
    }

    if (!ref[2] && ref[0] && ref[1] && refLap > ref[0] + ref[1]) {
        ref[2] = refLap - ref[0] - ref[1];
    }

    if (s.sector <= 1 && ref[0] && s.currentLapMs) {
        return static_cast<int>(s.currentLapMs) - static_cast<int>(ref[0]);
    }

    if (s.sector == 2 && ref[0] && ref[1] && s.sector1Ms && s.currentLapMs >= s.sector1Ms) {
        uint32_t currentIntoSector = s.currentLapMs - s.sector1Ms;
        uint32_t refSectorProgress = currentIntoSector < ref[1] ? currentIntoSector : ref[1];
        uint32_t refAtPoint = ref[0] + refSectorProgress;
        return static_cast<int>(s.currentLapMs) - static_cast<int>(refAtPoint);
    }

    if (s.sector >= 3 && ref[0] && ref[1] && ref[2] && s.sector1Ms && s.sector2Ms) {
        uint32_t currentS3Start = s.sector1Ms + s.sector2Ms;
        if (s.currentLapMs >= currentS3Start) {
            uint32_t currentIntoSector = s.currentLapMs - currentS3Start;
            uint32_t refSectorProgress = currentIntoSector < ref[2] ? currentIntoSector : ref[2];
            uint32_t refAtPoint = ref[0] + ref[1] + refSectorProgress;
            return static_cast<int>(s.currentLapMs) - static_cast<int>(refAtPoint);
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
    if (delta == DELTA_UNKNOWN) delta = fallbackDelta(s);
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
    s.lapDistance = readAt<float>(data, size, base + 20);
    s.position = readAt<uint8_t>(data, size, base + 32);
    s.lap = readAt<uint8_t>(data, size, base + 33);
    s.sector = readAt<uint8_t>(data, size, base + 36) + 1;
    s.invalidLap = readAt<uint8_t>(data, size, base + 37);
    s.penalties = readAt<uint8_t>(data, size, base + 38);
    s.warnings = readAt<uint8_t>(data, size, base + 39);
    s.vehicleFiaFlags = readAt<int8_t>(data, size, base + 54);
    updateLapTrace(s);
}

void parseSession(const uint8_t* data, size_t size, TelemetryState& s) {
    if (HEADER_SIZE + 656 > size) return;
    size_t base = HEADER_SIZE;
    s.totalLaps = readAt<uint8_t>(data, size, base + 3);
    s.sessionType = readAt<uint8_t>(data, size, base + 6);
    s.sessionTimeLeft = readAt<uint16_t>(data, size, base + 9);
    s.pitSpeedLimit = readAt<uint8_t>(data, size, base + 13);
    s.safetyCarStatus = readAt<uint8_t>(data, size, base + 124);
    s.pitStopWindowIdealLap = readAt<uint8_t>(data, size, base + 653);
    s.pitStopWindowLatestLap = readAt<uint8_t>(data, size, base + 654);
    s.pitStopRejoinPosition = readAt<uint8_t>(data, size, base + 655);
}

void parseTelemetry(const uint8_t* data, size_t size, uint16_t format, uint8_t playerIndex, TelemetryState& s) {
    bool layout2026 = looksLike2026Layout(6, size, format);
    size_t stride = layout2026 ? 59 : 60;
    size_t base = HEADER_SIZE + static_cast<size_t>(playerIndex) * stride;
    if (base + stride > size) return;
    s.speed = readAt<uint16_t>(data, size, base + 0);
    s.throttle = readAt<float>(data, size, base + 2);
    s.steer = readAt<float>(data, size, base + 6);
    s.brake = readAt<float>(data, size, base + 10);
    s.gear = readAt<int8_t>(data, size, base + 15);
    s.rpm = readAt<uint16_t>(data, size, base + 16);
    s.drsActive = layout2026 ? 0 : readAt<uint8_t>(data, size, base + 18);
    s.revLights = readAt<uint8_t>(data, size, base + 19);
    size_t brakeBase = base + (layout2026 ? 21 : 22);
    size_t surfaceBase = base + (layout2026 ? 29 : 30);
    size_t innerBase = base + (layout2026 ? 33 : 34);
    for (int i = 0; i < 4; ++i) {
        s.brakeTemps[i] = readAt<uint16_t>(data, size, brakeBase + i * 2);
        s.tyreSurfaceTemps[i] = readAt<uint8_t>(data, size, surfaceBase + i);
        s.tyreInnerTemps[i] = readAt<uint8_t>(data, size, innerBase + i);
    }
}

void parseStatus(const uint8_t* data, size_t size, uint16_t format, uint8_t playerIndex, TelemetryState& s) {
    bool layout2026 = looksLike2026Layout(7, size, format);
    size_t stride = layout2026 ? 59 : 55;
    size_t base = HEADER_SIZE + static_cast<size_t>(playerIndex) * stride;
    if (base + stride > size) return;
    s.fuelInTank = readAt<float>(data, size, base + 5);
    s.fuelCapacity = readAt<float>(data, size, base + 9);
    s.fuelLaps = readAt<float>(data, size, base + 13);
    s.maxRpm = readAt<uint16_t>(data, size, base + 17);
    if (!s.maxRpm) s.maxRpm = 12000;
    s.drsAllowed = readAt<uint8_t>(data, size, base + 22);
    s.drsActivationDistance = readAt<uint16_t>(data, size, base + 23);
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

void parseDamage(const uint8_t* data, size_t size, uint8_t playerIndex, TelemetryState& s) {
    constexpr size_t stride = 46;
    size_t base = HEADER_SIZE + static_cast<size_t>(playerIndex) * stride;
    if (base + stride > size) return;
    for (int i = 0; i < 4; ++i) {
        s.tyreWear[i] = readAt<float>(data, size, base + i * 4);
        s.tyreBlisters[i] = readAt<uint8_t>(data, size, base + 24 + i);
    }
    s.drsFault = readAt<uint8_t>(data, size, base + 34);
    s.ersFault = readAt<uint8_t>(data, size, base + 35);
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
    if (!supportedPacketFormat(format) || playerIndex >= 24) return;

    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_state.packetFormat = format;
    g_state.connected = true;
    g_state.lastSeenTick = GetTickCount64();
    g_state.packetCount++;
    if (packetId == 1) parseSession(data, size, g_state);
    if (packetId == 2) parseLapData(data, size, playerIndex, g_state);
    if (packetId == 6) parseTelemetry(data, size, format, playerIndex, g_state);
    if (packetId == 7) parseStatus(data, size, format, playerIndex, g_state);
    if (packetId == 10) parseDamage(data, size, playerIndex, g_state);
    if (packetId == 14) parseTimeTrial(data, size, g_state);
    if (packetId == 16 && looksLike2026Layout(packetId, size, format)) parseTelemetry2(data, size, playerIndex, g_state);
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
        if (hwnd == g_info) g_info = nullptr;
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
    HFONT value = makeFont(12, FW_BOLD, L"Bahnschrift SemiCondensed");
    HFONT big = makeFont(46, FW_BOLD, L"Bahnschrift SemiCondensed");
    HFONT med = makeFont(25, FW_BOLD, L"Bahnschrift SemiCondensed");

    float ers = clampf(s.ersEnergy / static_cast<float>(ERS_MAX_JOULES), 0, 1);
    COLORREF accent = g_regulationMode == RegulationMode::Reg2025 ? rgb(245, 213, 71) : rgb(35, 243, 106);
    COLORREF panel = rgb(8, 9, 11);
    COLORREF line = rgb(58, 58, 56);
    COLORREF muted = rgb(150, 150, 144);
    COLORREF systemColor = regulationSystemActive(s) ? rgb(35, 243, 106) : rgb(255, 74, 74);
    float ersPct = ers * 100.0f;
    COLORREF ersColor = s.ersFault || ersPct <= 10.0f ? rgb(255, 74, 74) : accent;

    fillRect(memDc, 12, 12, 94, 92, panel);
    strokeRect(memDc, 12, 12, 94, 92, line);
    fillRect(memDc, 12, 12, 4, 92, accent);
    drawText(memDc, L"GEAR", 24, 21, 70, 11, tiny, muted, DT_CENTER);
    drawText(memDc, gearText(s.gear), 22, 32, 76, 58, big, rgb(245, 245, 243), DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    fillRect(memDc, 116, 12, 302, 92, panel);
    strokeRect(memDc, 116, 12, 302, 92, line);
    drawText(memDc, L"r_", 128, 20, 24, 16, small, rgb(245, 245, 243), DT_LEFT);
    drawText(memDc, s.connected ? L"telemetry live" : L"checking...", 154, 21, 112, 14, tiny, muted, DT_LEFT);
    if (s.packetFormat) {
        std::wstring udpFormat = L"udp " + std::to_wstring(s.packetFormat);
        drawText(memDc, udpFormat, 330, 21, 76, 14, tiny, muted, DT_RIGHT);
    }

    int revX = 128;
    int lit = static_cast<int>(std::round(clampf(s.revLights / 100.0f, 0, 1) * 20));
    for (int i = 0; i < 20; ++i) {
        COLORREF c = rgb(39, 39, 39);
        if (i < lit) c = i < 12 ? rgb(35, 243, 106) : i < 16 ? rgb(245, 213, 71) : rgb(255, 74, 74);
        fillRect(memDc, revX + i * 13, 42, 10, 8, c);
    }

    drawText(memDc, L"KM/H", 128, 58, 48, 12, small, muted, DT_LEFT);
    drawText(memDc, std::to_wstring(s.speed), 128, 70, 92, 30, med, rgb(245, 245, 243), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    drawText(memDc, L"RPM", 252, 58, 48, 12, small, muted, DT_LEFT);
    wchar_t rpmBuf[16];
    swprintf_s(rpmBuf, L"%05u", s.rpm);
    drawText(memDc, rpmBuf, 252, 70, 104, 30, med, rgb(245, 245, 243), DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    fillRect(memDc, 428, 12, 180, 100, panel);
    strokeRect(memDc, 428, 12, 180, 100, line);
    drawText(memDc, regulationTitle(), 442, 20, 72, 12, tiny, muted, DT_LEFT);
    drawText(memDc, g_regulationMode == RegulationMode::Reg2025 ? L"DRS" : L"AERO", 442, 34, 72, 26, med, systemColor, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    drawText(memDc, wingText(s), 522, 37, 72, 20, value, systemColor, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    fillRect(memDc, 442, 63, 148, 1, line);
    drawText(memDc, L"ERS", 442, 70, 34, 11, tiny, ersColor, DT_LEFT);
    drawText(memDc, ersStatusText(s, ersPct), 480, 70, 56, 11, tiny, ersColor, DT_LEFT);
    drawText(memDc, std::to_wstring(static_cast<int>(std::round(ersPct))) + L"%", 540, 70, 50, 11, tiny, ersColor, DT_RIGHT);
    fillRect(memDc, 442, 87, 148, 9, rgb(35, 35, 35));
    fillRect(memDc, 442, 87, static_cast<int>(148 * ers), 9, ersColor);

    fillRect(memDc, 12, 122, 596, 42, panel);
    strokeRect(memDc, 12, 122, 596, 42, line);
    wchar_t pen[16];
    swprintf_s(pen, L"%us / %uw", s.penalties, s.warnings);
    wchar_t fuel[16];
    swprintf_s(fuel, L"%.1fL", s.fuelInTank);
    wchar_t est[16];
    swprintf_s(est, L"%.1f", s.fuelLaps);
    drawStatCell(memDc, L"pos", std::to_wstring(s.position ? s.position : 0), 18, 124, 44, small, value, false);
    drawStatCell(memDc, L"lap", std::to_wstring(s.lap ? s.lap : 0), 62, 124, 44, small, value, true);
    drawStatCell(memDc, L"sec", std::to_wstring(s.sector ? s.sector : 0), 106, 124, 44, small, value, true);
    drawStatCell(memDc, L"pen", pen, 150, 124, 92, small, value, true);
    drawStatCell(memDc, L"fuel", fuel, 242, 124, 74, small, value, true);
    drawStatCell(memDc, L"est", est, 316, 124, 64, small, value, true);
    drawStatCell(memDc, L"ers", ersStatusText(s, ersPct), 380, 124, 94, small, value, true);
    drawStatCell(memDc, L"tyre", tyreName(s.tyreCompound) + L" " + std::to_wstring(s.tyreAge) + L"l", 474, 124, 128, small, value, true);

    fillRect(memDc, 12, 176, 596, 30, panel);
    strokeRect(memDc, 12, 176, 596, 30, line);
    drawBar(memDc, L"thr", s.throttle, 24, 188, 172, rgb(35, 243, 106), tiny);
    drawBar(memDc, L"brk", s.brake, 224, 188, 172, rgb(255, 74, 74), tiny);
    drawSteer(memDc, s.steer, 424, 188, 172, tiny);

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

void paintInfo(HWND hwnd) {
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
    }

    HFONT tiny = makeFont(8, FW_BOLD);
    HFONT value = makeFont(12, FW_BOLD, L"Bahnschrift SemiCondensed");
    COLORREF panel = rgb(8, 9, 11);
    COLORREF line = rgb(58, 58, 56);
    COLORREF muted = rgb(150, 150, 144);
    float ers = clampf(s.ersEnergy / static_cast<float>(ERS_MAX_JOULES), 0, 1);
    float ersPct = ers * 100.0f;

    fillRect(memDc, 12, 12, 188, 76, panel);
    strokeRect(memDc, 12, 12, 188, 76, line);
    drawText(memDc, L"RACE / PIT", 24, 22, 160, 12, tiny, muted, DT_LEFT);
    drawInfoCell(memDc, L"session", sessionTypeText(s.sessionType), 24, 40, 64, tiny, value);
    drawInfoCell(memDc, L"time", formatClock(s.sessionTimeLeft), 94, 40, 48, tiny, value);
    drawInfoCell(memDc, L"flags", safetyCarText(s.safetyCarStatus) != L"CLEAR" ? safetyCarText(s.safetyCarStatus) : flagsText(s.vehicleFiaFlags), 148, 40, 42, tiny, value,
        s.safetyCarStatus || s.vehicleFiaFlags == 3 || s.vehicleFiaFlags == 4 ? rgb(245, 213, 71) : rgb(245, 245, 243));
    std::wstring pitWindow = s.pitStopWindowIdealLap ? std::to_wstring(s.pitStopWindowIdealLap) + L"-" + std::to_wstring(s.pitStopWindowLatestLap) : L"--";
    drawInfoCell(memDc, L"pit win", pitWindow, 24, 66, 56, tiny, value);
    drawInfoCell(memDc, L"rejoin", s.pitStopRejoinPosition ? std::to_wstring(s.pitStopRejoinPosition) : L"--", 88, 66, 48, tiny, value);
    drawInfoCell(memDc, L"limit", s.pitSpeedLimit ? std::to_wstring(s.pitSpeedLimit) : L"--", 144, 66, 44, tiny, value);

    fillRect(memDc, 212, 12, 236, 76, panel);
    strokeRect(memDc, 212, 12, 236, 76, line);
    drawText(memDc, L"TYRE HEALTH", 224, 22, 112, 12, tiny, muted, DT_LEFT);
    uint16_t maxBrake = 0;
    for (uint16_t brakeTemp : s.brakeTemps) {
        if (brakeTemp > maxBrake) maxBrake = brakeTemp;
    }
    drawText(memDc, L"brk " + std::to_wstring(maxBrake) + L"C", 360, 22, 74, 12, tiny, brakeColor(maxBrake), DT_RIGHT);
    drawTyreCorner(memDc, L"FL", s.tyreSurfaceTemps[2], s.tyreWear[2], s.tyreBlisters[2], 224, 42, 104, tiny, value);
    drawTyreCorner(memDc, L"FR", s.tyreSurfaceTemps[3], s.tyreWear[3], s.tyreBlisters[3], 334, 42, 104, tiny, value);
    drawTyreCorner(memDc, L"RL", s.tyreSurfaceTemps[0], s.tyreWear[0], s.tyreBlisters[0], 224, 66, 104, tiny, value);
    drawTyreCorner(memDc, L"RR", s.tyreSurfaceTemps[1], s.tyreWear[1], s.tyreBlisters[1], 334, 66, 104, tiny, value);

    fillRect(memDc, 460, 12, 148, 76, panel);
    strokeRect(memDc, 460, 12, 148, 76, line);
    drawText(memDc, L"WARNINGS", 472, 22, 120, 12, tiny, muted, DT_LEFT);
    std::wstring alert = warningText(s, ersPct);
    COLORREF alertColor = alert == L"NOMINAL" ? rgb(35, 243, 106) : rgb(255, 74, 74);
    if (alert == L"DRS READY" || alert == L"AERO READY") alertColor = rgb(245, 213, 71);
    if (alert == L"SC" || alert == L"VSC" || alert == L"FORM") alertColor = rgb(245, 213, 71);
    drawText(memDc, alert, 472, 42, 124, 20, value, alertColor, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    drawInfoCell(memDc, L"faults", (s.drsFault || s.ersFault) ? L"check" : L"none", 472, 66, 58, tiny, value,
        (s.drsFault || s.ersFault) ? rgb(255, 74, 74) : rgb(245, 245, 243));
    drawInfoCell(memDc, L"ers", ersStatusText(s, ersPct), 536, 66, 58, tiny, value,
        (s.ersFault || ersPct <= 10.0f) ? rgb(255, 74, 74) : rgb(245, 245, 243));

    BitBlt(dc, 0, 0, rc.right, rc.bottom, memDc, 0, 0, SRCCOPY);

    DeleteObject(tiny);
    DeleteObject(value);
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

LRESULT CALLBACK infoProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        paintInfo(hwnd);
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
    ID_REG_2025 = 1001,
    ID_REG_2026 = 1002,
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
    {ID_REG_2025, {24, 126, 456, 188}, L"2025 Regulations", L"DRS era overlay for 2025 cars"},
    {ID_REG_2026, {24, 204, 456, 266}, L"2026 Regulations", L"active aero overlay for 2026 cars"},
    {ID_EXIT, {336, 316, 456, 352}, L"exit", L"close launcher"}
};

void launchRegulation(RegulationMode mode) {
    g_regulationMode = mode;
    if (!g_hud) {
        g_hud = createOverlayWindow(L"F125CppHud", L"HUD", 80, 80, 620, 220, hudProc);
    }
    if (!g_timing) {
        g_timing = createOverlayWindow(L"F125CppTiming", L"Timing", 82, 38, 430, 128, timingProc);
    }
    if (!g_info) {
        g_info = createOverlayWindow(L"F125CppInfo", L"Info", 82, 182, 620, 100, infoProc);
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
        drawText(dc, L"select", action.rect.right - 76, action.rect.top + 22, 54, 16, bodyFont, hover ? rgb(35, 243, 106) : rgb(167, 167, 162), DT_RIGHT);
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
    drawText(memDc, L"choose regulation format", 40, 82, 260, 16, label, rgb(167, 167, 162), DT_LEFT);

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

    drawText(memDc, L"tip: set the same UDP format in F1 25 telemetry settings.", 24, 326, 310, 16, body, rgb(110, 110, 104), DT_LEFT);

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
                } else if (action.id == ID_REG_2025) {
                    launchRegulation(RegulationMode::Reg2025);
                } else {
                    launchRegulation(RegulationMode::Reg2026);
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
