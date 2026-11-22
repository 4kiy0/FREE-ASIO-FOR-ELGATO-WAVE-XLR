// Wave XLR MK2 ASIO — pilote ASIO en mode utilisateur pour Elgato Wave XLR MK2.
// Principe : WASAPI en mode EXCLUSIF piloté par événements (contourne le mixeur Windows),
// un seul thread temps réel MMCSS "Pro Audio", conversion float32 pour l'hôte ASIO.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <shlobj.h>
#include <shellapi.h>
#include <atomic>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include "asio_abi.h"

// ---------------------------------------------------------------------------
// Identifiants
// ---------------------------------------------------------------------------
static const CLSID CLSID_WaveXlrAsio =
    {0xf80a5c59, 0x659e, 0x4a5a, {0x89, 0xd5, 0x02, 0x1f, 0x0d, 0x23, 0x5e, 0x48}};
static const wchar_t* kClsidStr    = L"{F80A5C59-659E-4A5A-89D5-021F0D235E48}";
static const wchar_t* kDriverNameW = L"Wave XLR MK2 ASIO";
static const char*    kDriverName  = "Wave XLR MK2 ASIO";
static const long     kDriverVersion = 1;

static const GUID kCLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID  kIID_IMMDeviceEnumerator  = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const IID  kIID_IAudioClient         = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const IID  kIID_IAudioRenderClient   = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
static const IID  kIID_IAudioCaptureClient  = {0xC8ADBD64, 0xE71E, 0x48A0, {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};
static const GUID kSubtypePCM   = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const PROPERTYKEY kPKEY_FriendlyName =
    {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

static HINSTANCE g_hInst = nullptr;
static std::atomic<long> g_dllRefs{0};

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ---------------------------------------------------------------------------
// Fichiers : %APPDATA%\WaveXLR-ASIO\config.ini et log.txt
// ---------------------------------------------------------------------------
static std::wstring AppDir() {
    wchar_t p[MAX_PATH] = L"";
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, p);
    std::wstring d = std::wstring(p) + L"\\WaveXLR-ASIO";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

static void Log(const wchar_t* fmt, ...) {
    wchar_t msg[1024];
    va_list ap; va_start(ap, fmt);
    vswprintf(msg, 1024, fmt, ap);
    va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t line[1200];
    swprintf(line, 1200, L"%04u-%02u-%02u %02u:%02u:%02u.%03u  %ls\r\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
    char utf8[2400];
    int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
    if (n <= 1) return;
    std::wstring path = AppDir() + L"\\log.txt";
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w; WriteFile(f, utf8, (DWORD)(n - 1), &w, nullptr);
    CloseHandle(f);
}

struct Config {
    std::wstring inMatch = L"Wave XLR";
    std::wstring outMatch = L"Wave XLR";
    int bufferFrames = 0;    // 0 = minimum du périphérique
    int safetyPeriods = 1;   // marge d'entrée, en périodes
    int sampleRate = 48000;
};

static std::wstring ConfigPath() { return AppDir() + L"\\config.ini"; }

static Config LoadConfig() {
    std::wstring ini = ConfigPath();
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const char* def =
            "; Wave XLR MK2 ASIO - configuration\r\n"
            "; Apres modification : REAPER > Preferences > Audio > Device > re-appliquer (ou relancer REAPER).\r\n"
            "\r\n"
            "[device]\r\n"
            "; Sous-chaine du nom Windows du peripherique (la liste complete est dans log.txt)\r\n"
            "InputMatch=Wave XLR\r\n"
            "OutputMatch=Wave XLR\r\n"
            "\r\n"
            "[audio]\r\n"
            "; Buffer ASIO prefere en echantillons. 0 = minimum accepte par le pilote Elgato.\r\n"
            "BufferFrames=0\r\n"
            "; Marge cote entree, en periodes. 0 = latence minimale (risque de clics), 1 = stable.\r\n"
            "SafetyPeriods=1\r\n"
            "SampleRate=48000\r\n";
        HANDLE f = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(f, def, (DWORD)strlen(def), &w, nullptr); CloseHandle(f); }
    }
    Config c;
    wchar_t buf[256];
    GetPrivateProfileStringW(L"device", L"InputMatch", c.inMatch.c_str(), buf, 256, ini.c_str());  c.inMatch = buf;
    GetPrivateProfileStringW(L"device", L"OutputMatch", c.outMatch.c_str(), buf, 256, ini.c_str()); c.outMatch = buf;
    c.bufferFrames  = (int)GetPrivateProfileIntW(L"audio", L"BufferFrames", c.bufferFrames, ini.c_str());
    c.safetyPeriods = (int)GetPrivateProfileIntW(L"audio", L"SafetyPeriods", c.safetyPeriods, ini.c_str());
    c.sampleRate    = (int)GetPrivateProfileIntW(L"audio", L"SampleRate", c.sampleRate, ini.c_str());
    c.safetyPeriods = std::max(0, std::min(c.safetyPeriods, 8));
    c.bufferFrames  = std::max(0, std::min(c.bufferFrames, 4096));
    return c;
}

// ---------------------------------------------------------------------------
// Outils WASAPI
// ---------------------------------------------------------------------------
struct EndpointInfo { std::wstring id, name; };

static std::wstring Lower(std::wstring s) { for (auto& ch : s) ch = (wchar_t)towlower(ch); return s; }

static std::vector<EndpointInfo> ListEndpoints(IMMDeviceEnumerator* en, EDataFlow flow) {
    std::vector<EndpointInfo> out;
    IMMDeviceCollection* col = nullptr;
    if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) return out;
    UINT n = 0; col->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        IMMDevice* d = nullptr;
        if (FAILED(col->Item(i, &d))) continue;
        EndpointInfo e;
        LPWSTR id = nullptr;
        if (SUCCEEDED(d->GetId(&id))) { e.id = id; CoTaskMemFree(id); }
        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT v; PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(kPKEY_FriendlyName, &v)) && v.vt == VT_LPWSTR) e.name = v.pwszVal;
            PropVariantClear(&v);
            ps->Release();
        }
        d->Release();
        out.push_back(e);
    }
    col->Release();
    return out;
}

static HRESULT ActivateClient(const std::wstring& id, IAudioClient** cl) {
    *cl = nullptr;
    IMMDeviceEnumerator* en = nullptr;
    HRESULT hr = CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator, (void**)&en);
    if (FAILED(hr)) return hr;
    IMMDevice* dev = nullptr;
    hr = en->GetDevice(id.c_str(), &dev);
    if (SUCCEEDED(hr)) hr = dev->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, (void**)cl);
    SafeRelease(dev);
    SafeRelease(en);
    return hr;
}

static WAVEFORMATEXTENSIBLE MakeFormat(int rate, int ch, int container, int valid, bool isFloat) {
    WAVEFORMATEXTENSIBLE w;
    memset(&w, 0, sizeof(w));
    w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w.Format.nChannels = (WORD)ch;
    w.Format.nSamplesPerSec = (DWORD)rate;
    w.Format.wBitsPerSample = (WORD)container;
    w.Format.nBlockAlign = (WORD)(ch * container / 8);
    w.Format.nAvgBytesPerSec = w.Format.nBlockAlign * (DWORD)rate;
    w.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    w.Samples.wValidBitsPerSample = (WORD)valid;
    w.dwChannelMask = ch == 1 ? SPEAKER_FRONT_CENTER : ch == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
    w.SubFormat = isFloat ? kSubtypeFloat : kSubtypePCM;
    return w;
}

// Cherche un format exclusif accepté. channels<=0 : essaie le nombre du mix format, puis 2, puis 1.
static bool FindExclusiveFormat(IAudioClient* c, int rate, int channels, WAVEFORMATEXTENSIBLE& out) {
    std::vector<int> chs;
    if (channels > 0) chs.push_back(channels);
    else {
        WAVEFORMATEX* mix = nullptr;
        if (SUCCEEDED(c->GetMixFormat(&mix)) && mix) { chs.push_back(mix->nChannels); CoTaskMemFree(mix); }
        chs.push_back(2); chs.push_back(1);
    }
    struct F { int cont, valid; bool fl; };
    const F fmts[] = {{32, 24, false}, {24, 24, false}, {32, 32, false}, {32, 32, true}, {16, 16, false}};
    for (int ch : chs)
        for (const F& f : fmts) {
            WAVEFORMATEXTENSIBLE w = MakeFormat(rate, ch, f.cont, f.valid, f.fl);
            if (c->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, (WAVEFORMATEX*)&w, nullptr) == S_OK) { out = w; return true; }
        }
    return false;
}

static REFERENCE_TIME FramesToRT(long frames, double rate) {
    return (REFERENCE_TIME)std::ceil((double)frames * 10000000.0 / rate);
}
static long RTToFrames(REFERENCE_TIME t, double rate) {
    return (long)std::ceil((double)t * rate / 10000000.0 - 1e-6);
}

static unsigned long long NowNs() {
    static LARGE_INTEGER freq = {};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    unsigned long long q = (unsigned long long)c.QuadPart, f = (unsigned long long)freq.QuadPart;
    return (q / f) * 1000000000ull + (q % f) * 1000000000ull / f;
}

// File d'attente entrelacée (float), mémoire réservée à l'avance pour le thread temps réel.
struct Fifo {
    int ch = 1;
    std::vector<float> buf;
    size_t rd = 0; // en échantillons
    void init(int channels, size_t reserveFrames) { ch = std::max(1, channels); buf.clear(); buf.reserve(reserveFrames * ch); rd = 0; }
    size_t frames() const { return (buf.size() - rd) / ch; }
    float* append(size_t n) { size_t old = buf.size(); buf.resize(old + n * ch); return buf.data() + old; }
    void pushZero(size_t n) { float* p = append(n); std::fill(p, p + n * ch, 0.f); }
    const float* front() const { return buf.data() + rd; }
    void consume(size_t n) {
        rd += std::min(n, frames()) * ch;
        if (rd == buf.size()) { buf.clear(); rd = 0; }
        else if (rd > 4096 && rd * 2 > buf.size()) { buf.erase(buf.begin(), buf.begin() + rd); rd = 0; }
    }
    void clear() { buf.clear(); rd = 0; }
};

struct Stream {
    std::wstring id;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    IAudioCaptureClient* capture = nullptr;
    HANDLE event = nullptr;
    WAVEFORMATEXTENSIBLE fmt{};
    UINT32 frames = 0;      // période WASAPI (= taille du buffer en mode exclusif événementiel)
    long latency = 0;       // latence de flux en échantillons
    int ch = 0, container = 0;
    bool isFloat = false;

    void release() {
        if (client) client->Stop();
        SafeRelease(render); SafeRelease(capture); SafeRelease(client);
        if (event) { CloseHandle(event); event = nullptr; }
    }
};

static void DevToFloat(const BYTE* src, float* dst, size_t n, const Stream& s) {
    if (s.isFloat) { memcpy(dst, src, n * sizeof(float)); return; }
    switch (s.container) {
    case 32: { const int32_t* p = (const int32_t*)src; for (size_t i = 0; i < n; ++i) dst[i] = (float)(p[i] * (1.0 / 2147483648.0)); break; }
    case 24: for (size_t i = 0; i < n; ++i, src += 3) {
                 int32_t v = (int32_t)(((uint32_t)src[0] << 8) | ((uint32_t)src[1] << 16) | ((uint32_t)src[2] << 24));
                 dst[i] = (float)(v * (1.0 / 2147483648.0));
             } break;
    case 16: { const int16_t* p = (const int16_t*)src; for (size_t i = 0; i < n; ++i) dst[i] = p[i] * (1.f / 32768.f); break; }
    }
}

static void FloatToDev(const float* src, BYTE* dst, size_t n, const Stream& s) {
    if (s.isFloat) { memcpy(dst, src, n * sizeof(float)); return; }
    for (size_t i = 0; i < n; ++i) {
        double x = src[i];
        if (!(x > -1.0)) x = -1.0; else if (x > 1.0) x = 1.0;
        switch (s.container) {
        case 32: ((int32_t*)dst)[i] = (int32_t)std::lrint(x * 2147483647.0); break;
        case 24: { int32_t v = (int32_t)std::lrint(x * 8388607.0); BYTE* p = dst + i * 3; p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); p[2] = (BYTE)(v >> 16); break; }
        case 16: ((int16_t*)dst)[i] = (int16_t)std::lrint(x * 32767.0); break;
        }
    }
}

// Initialise un flux exclusif événementiel, en gérant l'erreur d'alignement.
static HRESULT InitExclusive(Stream& s, REFERENCE_TIME period, bool isRender) {
    HRESULT hr = ActivateClient(s.id, &s.client);
    if (FAILED(hr)) return hr;
    hr = s.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                              period, period, (WAVEFORMATEX*)&s.fmt, nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 f = 0; s.client->GetBufferSize(&f);
        SafeRelease(s.client);
        Log(L"  periode non alignee, nouvel essai avec %u echantillons", f);
        period = FramesToRT((long)f, s.fmt.Format.nSamplesPerSec);
        hr = ActivateClient(s.id, &s.client);
        if (FAILED(hr)) return hr;
        hr = s.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                  period, period, (WAVEFORMATEX*)&s.fmt, nullptr);
    }
    if (FAILED(hr)) return hr;
    s.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if ((hr = s.client->SetEventHandle(s.event)) != S_OK) return hr;
    if (FAILED(hr = s.client->GetBufferSize(&s.frames))) return hr;
    REFERENCE_TIME lat = 0;
    s.client->GetStreamLatency(&lat);
    s.latency = RTToFrames(lat, s.fmt.Format.nSamplesPerSec);
    s.ch = s.fmt.Format.nChannels;
    s.container = s.fmt.Format.wBitsPerSample;
    s.isFloat = s.fmt.SubFormat == kSubtypeFloat;
    if (isRender) hr = s.client->GetService(kIID_IAudioRenderClient, (void**)&s.render);
    else          hr = s.client->GetService(kIID_IAudioCaptureClient, (void**)&s.capture);
    return hr;
}

// ---------------------------------------------------------------------------
// Le pilote
// ---------------------------------------------------------------------------
class WaveXlrAsio : public IASIO {
public:
    WaveXlrAsio() { ++g_dllRefs; InitializeSRWLock(&posLock_); }
    virtual ~WaveXlrAsio() { disposeBuffers(); --g_dllRefs; }

    // IUnknown — l'hôte ASIO demande le CLSID comme IID.
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == CLSID_WaveXlrAsio) { *ppv = static_cast<IASIO*>(this); AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)++refs_; }
    STDMETHODIMP_(ULONG) Release() override { long r = --refs_; if (r == 0) delete this; return (ULONG)r; }

    ASIOBool init(void*) override {
        cfg_ = LoadConfig();
        rate_ = cfg_.sampleRate > 0 ? cfg_.sampleRate : 48000;
        Log(L"=== init %ls v%ld ===", kDriverNameW, kDriverVersion);
        if (!probeDevices()) return ASIOFalse;
        return ASIOTrue;
    }
    void getDriverName(char* name) override { strcpy(name, kDriverName); }
    long getDriverVersion() override { return kDriverVersion; }
    void getErrorMessage(char* s) override { strncpy(s, err_, 123); s[123] = 0; }

    ASIOError getChannels(long* in, long* out) override {
        if (!inCh_ || !outCh_) return ASE_NotPresent;
        if (in) *in = inCh_;
        if (out) *out = outCh_;
        return ASE_OK;
    }
    ASIOError getLatencies(long* in, long* out) override {
        if (!inCh_) return ASE_NotPresent;
        long b = created_ ? B_ : preferredFrames();
        long p = created_ ? (long)ren_.frames : std::max(b, devMinFrames_);
        long safety = created_ ? safetyFrames_ : cfg_.safetyPeriods * p;
        if (in)  *in  = b + safety + (created_ ? cap_.latency : p);
        if (out) *out = std::max(b, p) + (created_ ? ren_.latency : p);
        return ASE_OK;
    }
    ASIOError getBufferSize(long* mn, long* mx, long* pref, long* gran) override {
        if (!inCh_) return ASE_NotPresent;
        long m = std::max(16L, devMinFrames_);
        if (mn) *mn = m;
        if (mx) *mx = std::max(m, 4096L);
        if (pref) *pref = preferredFrames();
        if (gran) *gran = 1;
        return ASE_OK;
    }
    ASIOError canSampleRate(ASIOSampleRate r) override { return rateSupported(r) ? ASE_OK : ASE_NoClock; }
    ASIOError getSampleRate(ASIOSampleRate* r) override { if (!r) return ASE_InvalidParameter; *r = rate_; return ASE_OK; }
    ASIOError setSampleRate(ASIOSampleRate r) override {
        if (r == rate_) return ASE_OK;
        if (r <= 0 || !rateSupported(r)) return ASE_NoClock;
        rate_ = r;
        updateMinPeriod();
        Log(L"frequence -> %.0f Hz", r);
        if (created_) requestReset();
        else if (cb_ && cb_->sampleRateDidChange) cb_->sampleRateDidChange(r);
        return ASE_OK;
    }
    ASIOError getClockSources(ASIOClockSource* c, long* num) override {
        if (!c || !num || *num < 1) return ASE_InvalidParameter;
        memset(c, 0, sizeof(*c));
        c->index = 0; c->associatedChannel = -1; c->associatedGroup = -1; c->isCurrentSource = ASIOTrue;
        strcpy(c->name, "Interne (USB)");
        *num = 1;
        return ASE_OK;
    }
    ASIOError setClockSource(long ref) override { return ref == 0 ? ASE_OK : ASE_InvalidParameter; }
    ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* t) override {
        if (!running_) return ASE_SPNotAdvancing;
        AcquireSRWLockShared(&posLock_);
        unsigned long long p = lastPos_, ns = lastNs_;
        ReleaseSRWLockShared(&posLock_);
        if (sPos) { sPos->hi = (unsigned long)(p >> 32); sPos->lo = (unsigned long)(p & 0xffffffffu); }
        if (t)    { t->hi = (unsigned long)(ns >> 32);  t->lo = (unsigned long)(ns & 0xffffffffu); }
        return ASE_OK;
    }
    ASIOError getChannelInfo(ASIOChannelInfo* info) override {
        if (!info) return ASE_InvalidParameter;
        long n = info->isInput ? inCh_ : outCh_;
        if (info->channel < 0 || info->channel >= n) return ASE_InvalidParameter;
        info->isActive = ASIOFalse;
        for (auto& c : chans_) if (c.isInput == (info->isInput != 0) && c.index == info->channel) info->isActive = ASIOTrue;
        info->channelGroup = 0;
        info->type = ASIOSTFloat32LSB;
        if (info->isInput) snprintf(info->name, 32, n == 1 ? "Wave XLR Mic" : "Wave XLR In %ld", info->channel + 1);
        else if (n == 2)   snprintf(info->name, 32, info->channel == 0 ? "Wave XLR Out L" : "Wave XLR Out R");
        else               snprintf(info->name, 32, "Wave XLR Out %ld", info->channel + 1);
        return ASE_OK;
    }

    ASIOError createBuffers(ASIOBufferInfo* infos, long num, long size, ASIOCallbacks* cbs) override {
        if (created_) disposeBuffers();
        if (!infos || num <= 0 || size <= 0 || !cbs || !inCh_) return ASE_InvalidParameter;
        for (long i = 0; i < num; ++i) {
            long n = infos[i].isInput ? inCh_ : outCh_;
            if (infos[i].channelNum < 0 || infos[i].channelNum >= n) return ASE_InvalidParameter;
        }
        B_ = size; cb_ = cbs;
        storage_.assign((size_t)num * 2 * size, 0.f);
        chans_.clear();
        for (long i = 0; i < num; ++i) {
            Chan c; c.isInput = infos[i].isInput != 0; c.index = infos[i].channelNum;
            c.buf[0] = storage_.data() + (size_t)i * 2 * size;
            c.buf[1] = c.buf[0] + size;
            infos[i].buffers[0] = c.buf[0];
            infos[i].buffers[1] = c.buf[1];
            chans_.push_back(c);
        }
        timeInfo_ = cb_->asioMessage && cb_->asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr) == 1;

        evSetup_   = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        evStart_   = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        evStarted_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        evStop_    = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        evStopped_ = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        evQuit_    = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        setupHr_ = E_FAIL;
        thread_ = CreateThread(nullptr, 0, &WaveXlrAsio::ThreadProc, this, 0, nullptr);
        if (!thread_ || WaitForSingleObject(evSetup_, 5000) != WAIT_OBJECT_0 || FAILED(setupHr_)) {
            char m[128];
            snprintf(m, sizeof(m), "Ouverture exclusive impossible (0x%08lX). Fermez Wave Link / autorisez le mode exclusif.", (unsigned long)setupHr_);
            setError(m);
            created_ = true; // pour que disposeBuffers nettoie
            disposeBuffers();
            return ASE_HWMalfunction;
        }
        created_ = true;
        Log(L"buffers: ASIO=%ld, WASAPI in=%u out=%u, marge=%ld, timeInfo=%d", B_, cap_.frames, ren_.frames, safetyFrames_, (int)timeInfo_);
        return ASE_OK;
    }

    ASIOError disposeBuffers() override {
        if (!created_) return ASE_OK;
        stop();
        if (thread_) {
            SetEvent(evQuit_);
            WaitForSingleObject(thread_, 5000);
            CloseHandle(thread_); thread_ = nullptr;
        }
        for (HANDLE* h : {&evSetup_, &evStart_, &evStarted_, &evStop_, &evStopped_, &evQuit_})
            if (*h) { CloseHandle(*h); *h = nullptr; }
        chans_.clear(); storage_.clear();
        cb_ = nullptr; created_ = false;
        return ASE_OK;
    }

    ASIOError start() override {
        if (!created_ || !thread_) return ASE_InvalidMode;
        if (running_) return ASE_OK;
        startHr_ = E_FAIL;
        ResetEvent(evStarted_);
        SetEvent(evStart_);
        if (WaitForSingleObject(evStarted_, 3000) != WAIT_OBJECT_0 || FAILED(startHr_)) {
            setError("Demarrage du flux audio impossible.");
            return ASE_HWMalfunction;
        }
        return ASE_OK;
    }
    ASIOError stop() override {
        if (!created_ || !thread_) return ASE_OK;
        SetEvent(evStop_);
        WaitForSingleObject(evStopped_, 3000);
        return ASE_OK;
    }

    ASIOError controlPanel() override {
        wchar_t msg[1024];
        swprintf(msg, 1024,
                 L"%ls\n\nEntree : %ls\nSortie : %ls\n\nFrequence : %.0f Hz\nBuffer minimum du pilote Elgato : %ld echantillons (%.2f ms)\n"
                 L"Marge entree : %d periode(s)\n\nOuvrir le fichier de configuration ?",
                 kDriverNameW, inName_.c_str(), outName_.c_str(), rate_, devMinFrames_,
                 devMinFrames_ * 1000.0 / rate_, cfg_.safetyPeriods);
        if (MessageBoxW(nullptr, msg, kDriverNameW, MB_YESNO | MB_ICONINFORMATION) == IDYES)
            ShellExecuteW(nullptr, L"open", L"notepad.exe", ConfigPath().c_str(), nullptr, SW_SHOWNORMAL);
        return ASE_OK;
    }
    ASIOError future(long selector, void*) override {
        return selector == kAsioCanTimeInfo ? ASE_SUCCESS : ASE_InvalidParameter;
    }
    ASIOError outputReady() override { return ASE_NotPresent; }

private:
    struct Chan { bool isInput; long index; float* buf[2]; };

    void setError(const char* m) {
        strncpy(err_, m, sizeof(err_) - 1); err_[sizeof(err_) - 1] = 0;
        wchar_t w[256]; MultiByteToWideChar(CP_UTF8, 0, err_, -1, w, 256);
        Log(L"ERREUR: %ls", w);
    }

    long preferredFrames() const {
        long m = std::max(16L, devMinFrames_);
        return cfg_.bufferFrames > 0 ? std::max(m, (long)cfg_.bufferFrames) : m;
    }

    bool probeDevices() {
        IMMDeviceEnumerator* en = nullptr;
        if (FAILED(CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator, (void**)&en))) {
            setError("MMDeviceEnumerator indisponible."); return false;
        }
        auto caps = ListEndpoints(en, eCapture);
        auto rens = ListEndpoints(en, eRender);
        en->Release();
        std::wstring im = Lower(cfg_.inMatch), om = Lower(cfg_.outMatch);
        inId_.clear(); outId_.clear();
        for (auto& e : caps) { Log(L"  entree dispo : %ls", e.name.c_str()); if (inId_.empty() && Lower(e.name).find(im) != std::wstring::npos) { inId_ = e.id; inName_ = e.name; } }
        for (auto& e : rens) { Log(L"  sortie dispo : %ls", e.name.c_str()); if (outId_.empty() && Lower(e.name).find(om) != std::wstring::npos) { outId_ = e.id; outName_ = e.name; } }
        if (inId_.empty() || outId_.empty()) { setError("Wave XLR introuvable (verifiez InputMatch/OutputMatch dans config.ini)."); return false; }
        Log(L"entree choisie : %ls", inName_.c_str());
        Log(L"sortie choisie : %ls", outName_.c_str());

        IAudioClient* ci = nullptr; IAudioClient* co = nullptr;
        bool ok = SUCCEEDED(ActivateClient(inId_, &ci)) && SUCCEEDED(ActivateClient(outId_, &co));
        WAVEFORMATEXTENSIBLE fi{}, fo{};
        if (ok && !(FindExclusiveFormat(ci, (int)rate_, 0, fi) && FindExclusiveFormat(co, (int)rate_, 0, fo))) {
            Log(L"%.0f Hz refuse en exclusif, essai 48000 Hz", rate_);
            rate_ = 48000;
            ok = FindExclusiveFormat(ci, 48000, 0, fi) && FindExclusiveFormat(co, 48000, 0, fo);
        }
        SafeRelease(ci); SafeRelease(co);
        if (!ok) { setError("Aucun format exclusif accepte. Activez 'Autoriser les applications a prendre le controle exclusif'."); return false; }
        inCh_ = fi.Format.nChannels; outCh_ = fo.Format.nChannels;
        Log(L"format exclusif : entree %dch %d/%d bits, sortie %dch %d/%d bits, %.0f Hz",
            inCh_, fi.Format.wBitsPerSample, fi.Samples.wValidBitsPerSample,
            outCh_, fo.Format.wBitsPerSample, fo.Samples.wValidBitsPerSample, rate_);
        updateMinPeriod();
        return true;
    }

    void updateMinPeriod() {
        REFERENCE_TIME mn = 0, def = 0, worst = 0;
        for (const std::wstring* id : {&inId_, &outId_}) {
            IAudioClient* c = nullptr;
            if (SUCCEEDED(ActivateClient(*id, &c)) && SUCCEEDED(c->GetDevicePeriod(&def, &mn))) worst = std::max(worst, mn);
            SafeRelease(c);
        }
        devMinFrames_ = worst > 0 ? RTToFrames(worst, rate_) : 128;
        Log(L"periode minimale exclusive : %ld echantillons (%.2f ms)", devMinFrames_, devMinFrames_ * 1000.0 / rate_);
    }

    bool rateSupported(double r) {
        if (inId_.empty()) return false;
        IAudioClient* ci = nullptr; IAudioClient* co = nullptr;
        WAVEFORMATEXTENSIBLE f;
        bool ok = SUCCEEDED(ActivateClient(inId_, &ci)) && SUCCEEDED(ActivateClient(outId_, &co)) &&
                  FindExclusiveFormat(ci, (int)r, inCh_, f) && FindExclusiveFormat(co, (int)r, outCh_, f);
        SafeRelease(ci); SafeRelease(co);
        return ok;
    }

    void requestReset() {
        if (cb_ && cb_->asioMessage && cb_->asioMessage(kAsioSelectorSupported, kAsioResetRequest, nullptr, nullptr))
            cb_->asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
    }

    // ---- thread audio ----
    static DWORD WINAPI ThreadProc(LPVOID p) { return static_cast<WaveXlrAsio*>(p)->workerMain(); }

    HRESULT setupStreams() {
        cap_ = Stream(); ren_ = Stream();
        cap_.id = inId_; ren_.id = outId_;
        HRESULT hr;
        IAudioClient* c = nullptr;
        if (FAILED(hr = ActivateClient(inId_, &c))) return hr;
        bool ok = FindExclusiveFormat(c, (int)rate_, inCh_, cap_.fmt); SafeRelease(c);
        if (FAILED(hr = ActivateClient(outId_, &c))) return hr;
        ok = ok && FindExclusiveFormat(c, (int)rate_, outCh_, ren_.fmt); SafeRelease(c);
        if (!ok) return AUDCLNT_E_UNSUPPORTED_FORMAT;

        long want = std::max(B_, devMinFrames_);
        REFERENCE_TIME per = FramesToRT(want, rate_);
        if (FAILED(hr = InitExclusive(ren_, per, true)))  { Log(L"init sortie exclusive: 0x%08lX", (unsigned long)hr); return hr; }
        if (FAILED(hr = InitExclusive(cap_, per, false))) { Log(L"init entree exclusive: 0x%08lX", (unsigned long)hr); return hr; }

        safetyFrames_ = cfg_.safetyPeriods * (long)std::max(ren_.frames, cap_.frames);
        size_t big = (size_t)std::max<long>(B_, (long)std::max(ren_.frames, cap_.frames)) * 16 + 8192;
        inF_.init(inCh_, big);
        outF_.init(outCh_, big);
        return S_OK;
    }

    DWORD workerMain() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        setupHr_ = setupStreams();
        SetEvent(evSetup_);
        if (SUCCEEDED(setupHr_)) {
            HANDLE h[2] = {evStart_, evQuit_};
            while (WaitForMultipleObjects(2, h, FALSE, INFINITE) == WAIT_OBJECT_0) streamLoop();
        }
        cap_.release(); ren_.release();
        CoUninitialize();
        return 0;
    }

    void streamLoop() {
        ResetEvent(evStopped_);
        ResetEvent(evStop_);
        DWORD task = 0;
        HANDLE av = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
        if (av) AvSetMmThreadPriority(av, AVRT_PRIORITY_CRITICAL);
        else SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

        inF_.clear(); outF_.clear();
        inF_.pushZero((size_t)safetyFrames_);
        bufIdx_ = 0; pos_ = 0;
        ren_.client->Reset(); cap_.client->Reset();
        BYTE* d = nullptr;
        HRESULT hr = ren_.render->GetBuffer(ren_.frames, &d);
        if (SUCCEEDED(hr)) hr = ren_.render->ReleaseBuffer(ren_.frames, AUDCLNT_BUFFERFLAGS_SILENT);
        if (SUCCEEDED(hr)) hr = cap_.client->Start();
        if (SUCCEEDED(hr)) hr = ren_.client->Start();
        startHr_ = hr;
        running_ = SUCCEEDED(hr);
        SetEvent(evStarted_);
        if (!running_) Log(L"echec demarrage: 0x%08lX", (unsigned long)hr);

        HANDLE w[2] = {ren_.event, evStop_};
        int timeouts = 0;
        while (running_) {
            DWORD r = WaitForMultipleObjects(2, w, FALSE, 200);
            if (r == WAIT_OBJECT_0 + 1) break;
            if (r == WAIT_TIMEOUT) {
                if (++timeouts >= 5) { Log(L"le peripherique ne repond plus"); requestReset(); break; }
                continue;
            }
            timeouts = 0;
            if (!processPeriod()) { Log(L"peripherique perdu/invalide, demande de reset"); requestReset(); break; }
        }
        running_ = false;
        ren_.client->Stop(); cap_.client->Stop();
        if (av) AvRevertMmThreadCharacteristics(av);
        SetEvent(evStopped_);
    }

    bool readCapture() {
        UINT32 pk = 0;
        HRESULT hr;
        while (SUCCEEDED(hr = cap_.capture->GetNextPacketSize(&pk)) && pk > 0) {
            BYTE* d = nullptr; UINT32 n = 0; DWORD fl = 0;
            hr = cap_.capture->GetBuffer(&d, &n, &fl, nullptr, nullptr);
            if (FAILED(hr)) return false;
            if (hr == AUDCLNT_S_BUFFER_EMPTY || n == 0) break;
            if (fl & AUDCLNT_BUFFERFLAGS_SILENT) inF_.pushZero(n);
            else DevToFloat(d, inF_.append(n), (size_t)n * inCh_, cap_);
            cap_.capture->ReleaseBuffer(n);
        }
        return SUCCEEDED(hr);
    }

    void runCycle() {
        const int idx = bufIdx_;
        const size_t B = (size_t)B_;
        // entrées
        size_t take = std::min(inF_.frames(), B);
        const float* src = inF_.front();
        for (auto& c : chans_) {
            if (!c.isInput) continue;
            float* dst = c.buf[idx];
            for (size_t f = 0; f < take; ++f) dst[f] = src[f * inCh_ + c.index];
            std::fill(dst + take, dst + B, 0.f);
        }
        inF_.consume(take);

        unsigned long long ns = NowNs();
        AcquireSRWLockExclusive(&posLock_);
        lastPos_ = pos_; lastNs_ = ns;
        ReleaseSRWLockExclusive(&posLock_);

        if (timeInfo_ && cb_->bufferSwitchTimeInfo) {
            ASIOTime t; memset(&t, 0, sizeof(t));
            t.timeInfo.speed = 1.0;
            t.timeInfo.systemTime.hi = (unsigned long)(ns >> 32);  t.timeInfo.systemTime.lo = (unsigned long)(ns & 0xffffffffu);
            t.timeInfo.samplePosition.hi = (unsigned long)(pos_ >> 32); t.timeInfo.samplePosition.lo = (unsigned long)(pos_ & 0xffffffffu);
            t.timeInfo.sampleRate = rate_;
            t.timeInfo.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid;
            cb_->bufferSwitchTimeInfo(&t, idx, ASIOTrue);
        } else {
            cb_->bufferSwitch(idx, ASIOTrue);
        }

        // sorties
        float* out = outF_.append(B);
        std::fill(out, out + B * outCh_, 0.f);
        for (auto& c : chans_) {
            if (c.isInput) continue;
            const float* s = c.buf[idx];
            for (size_t f = 0; f < B; ++f) out[f * outCh_ + c.index] = s[f];
        }
        pos_ += B;
        bufIdx_ ^= 1;
    }

    bool processPeriod() {
        if (!readCapture()) return false;
        const UINT32 P = ren_.frames;
        while (outF_.frames() < P) runCycle();
        BYTE* d = nullptr;
        if (FAILED(ren_.render->GetBuffer(P, &d))) return false;
        FloatToDev(outF_.front(), d, (size_t)P * outCh_, ren_);
        outF_.consume(P);
        if (FAILED(ren_.render->ReleaseBuffer(P, 0))) return false;
        // borne la latence d'entrée si la capture prend de l'avance
        size_t maxIn = (size_t)safetyFrames_ + (size_t)std::max<long>(B_, (long)cap_.frames) * 2;
        if (inF_.frames() > maxIn) inF_.consume(inF_.frames() - (size_t)safetyFrames_);
        return true;
    }

    std::atomic<long> refs_{1};
    Config cfg_;
    std::wstring inId_, outId_, inName_, outName_;
    double rate_ = 48000;
    long devMinFrames_ = 0, inCh_ = 0, outCh_ = 0;
    char err_[128] = "";

    bool created_ = false, timeInfo_ = false;
    long B_ = 0;
    ASIOCallbacks* cb_ = nullptr;
    std::vector<Chan> chans_;
    std::vector<float> storage_;

    HANDLE thread_ = nullptr, evSetup_ = nullptr, evStart_ = nullptr, evStarted_ = nullptr,
           evStop_ = nullptr, evStopped_ = nullptr, evQuit_ = nullptr;
    HRESULT setupHr_ = E_FAIL, startHr_ = E_FAIL;
    std::atomic<bool> running_{false};
    Stream cap_, ren_;
    Fifo inF_, outF_;
    long safetyFrames_ = 0;
    int bufIdx_ = 0;
    unsigned long long pos_ = 0, lastPos_ = 0, lastNs_ = 0;
    SRWLOCK posLock_;
};

// ---------------------------------------------------------------------------
// COM : fabrique, enregistrement
// ---------------------------------------------------------------------------
class Factory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)++g_dllRefs; }
    STDMETHODIMP_(ULONG) Release() override { return (ULONG)--g_dllRefs; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        WaveXlrAsio* o = new (std::nothrow) WaveXlrAsio();
        if (!o) return E_OUTOFMEMORY;
        HRESULT hr = o->QueryInterface(riid, ppv);
        o->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override { if (lock) ++g_dllRefs; else --g_dllRefs; return S_OK; }
};
static Factory g_factory;

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (rclsid != CLSID_WaveXlrAsio) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}
STDAPI DllCanUnloadNow() { return g_dllRefs == 0 ? S_OK : S_FALSE; }

static bool RegSetStr(const std::wstring& key, const wchar_t* name, const std::wstring& val) {
    HKEY h;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr) != ERROR_SUCCESS) return false;
    LONG r = RegSetValueExW(h, name, 0, REG_SZ, (const BYTE*)val.c_str(), (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return r == ERROR_SUCCESS;
}

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_hInst, path, MAX_PATH);
    std::wstring clsKey = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsidStr;
    std::wstring asioKey = std::wstring(L"SOFTWARE\\ASIO\\") + kDriverNameW;
    bool ok = RegSetStr(clsKey, nullptr, kDriverNameW) &&
              RegSetStr(clsKey + L"\\InprocServer32", nullptr, path) &&
              RegSetStr(clsKey + L"\\InprocServer32", L"ThreadingModel", L"Apartment") &&
              RegSetStr(asioKey, L"CLSID", kClsidStr) &&
              RegSetStr(asioKey, L"Description", kDriverNameW);
    return ok ? S_OK : E_ACCESSDENIED;
}

STDAPI DllUnregisterServer() {
    std::wstring clsKey = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsidStr;
    std::wstring asioKey = std::wstring(L"SOFTWARE\\ASIO\\") + kDriverNameW;
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, clsKey.c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, asioKey.c_str());
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_hInst = h; DisableThreadLibraryCalls(h); }
    return TRUE;
}
