#include "ScreenBackend.h"

// Windows screen-capture backend: DXGI Desktop Duplication (monitor
// capture, Windows 8+). Delivers BGRA frames; the cross-platform sink
// crops + uploads. Window capture (WGC) is a follow-up — DXGI duplication
// is monitor-only. Compiled only when RETROCAPTURE_SCREEN_DXGI is set
// (Windows); the test-pattern stub stands in otherwise.
#ifdef RETROCAPTURE_SCREEN_DXGI

#include "../utils/Logger.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{
template <class T> void safeRelease(T *&p) { if (p) { p->Release(); p = nullptr; } }

class DxgiScreenBackend : public ScreenBackend
{
public:
    explicit DxgiScreenBackend(IScreenFrameSink &sink) : m_sink(sink) {}
    ~DxgiScreenBackend() override { stop(); }

    bool start(const std::string &target, bool captureCursor) override
    {
        if (m_running.exchange(true)) return true;
        m_target = target;
        m_cursor = captureCursor;
        m_thread = std::thread(&DxgiScreenBackend::run, this);
        return true;
    }

    void stop() override
    {
        if (!m_running.exchange(false)) return;
        if (m_thread.joinable()) m_thread.join();
    }

    std::vector<DeviceInfo> listTargets() override { return enumerateOutputs(); }

private:
    static std::vector<DeviceInfo> enumerateOutputs()
    {
        std::vector<DeviceInfo> out;
        IDXGIFactory1 *factory = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                      reinterpret_cast<void **>(&factory))) || !factory)
            return out;

        IDXGIAdapter1 *adapter = nullptr;
        for (UINT ai = 0; factory->EnumAdapters1(ai, &adapter) == S_OK; ++ai)
        {
            IDXGIOutput *o = nullptr;
            for (UINT oi = 0; adapter->EnumOutputs(oi, &o) == S_OK; ++oi)
            {
                DXGI_OUTPUT_DESC desc;
                if (SUCCEEDED(o->GetDesc(&desc)))
                {
                    char name[64] = {0};
                    ::wcstombs(name, desc.DeviceName, sizeof(name) - 1);
                    DeviceInfo di;
                    di.id        = "monitor:" + std::to_string(out.size());
                    di.name      = "Display " + std::to_string(out.size()) +
                                   " (" + name + ")";
                    di.driver    = "dxgi";
                    di.available  = desc.AttachedToDesktop != 0;
                    out.push_back(di);
                }
                safeRelease(o);
            }
            safeRelease(adapter);
        }
        safeRelease(factory);

        if (out.empty())
        {
            DeviceInfo di;
            di.id = "monitor:0"; di.name = "Primary display"; di.driver = "dxgi";
            out.push_back(di);
        }
        return out;
    }

    int targetIndex() const
    {
        const auto p = m_target.find(':');
        if (p != std::string::npos)
        {
            const std::string n = m_target.substr(p + 1);
            if (!n.empty())
            {
                char *end = nullptr;
                const long v = std::strtol(n.c_str(), &end, 10);
                if (end != n.c_str() && v >= 0) return static_cast<int>(v);
            }
        }
        return 0;
    }

    void run()
    {
        const int wantOut = targetIndex();

        IDXGIFactory1 *factory = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                      reinterpret_cast<void **>(&factory))) || !factory)
        {
            LOG_ERROR("VideoCaptureScreen(dxgi): CreateDXGIFactory1 failed");
            m_running.store(false);
            return;
        }

        // Find the adapter + output for the global output index.
        IDXGIAdapter1 *adapter = nullptr;
        IDXGIOutput   *output  = nullptr;
        int globalIdx = 0;
        for (UINT ai = 0; !output && factory->EnumAdapters1(ai, &adapter) == S_OK; ++ai)
        {
            IDXGIOutput *o = nullptr;
            for (UINT oi = 0; adapter->EnumOutputs(oi, &o) == S_OK; ++oi)
            {
                if (globalIdx == wantOut) { output = o; break; }
                ++globalIdx;
                safeRelease(o);
            }
            if (!output) safeRelease(adapter);
        }
        if (!output) // fall back to adapter 0 / output 0
        {
            safeRelease(adapter);
            if (factory->EnumAdapters1(0, &adapter) == S_OK)
                adapter->EnumOutputs(0, &output);
        }
        if (!adapter || !output)
        {
            LOG_ERROR("VideoCaptureScreen(dxgi): no usable output");
            safeRelease(output); safeRelease(adapter); safeRelease(factory);
            m_running.store(false);
            return;
        }

        ID3D11Device        *dev = nullptr;
        ID3D11DeviceContext *ctx = nullptr;
        D3D_FEATURE_LEVEL    fl;
        HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                       nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
        if (FAILED(hr) || !dev || !ctx)
        {
            LOG_ERROR("VideoCaptureScreen(dxgi): D3D11CreateDevice failed");
            safeRelease(output); safeRelease(adapter); safeRelease(factory);
            m_running.store(false);
            return;
        }

        IDXGIOutput1 *output1 = nullptr;
        output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void **>(&output1));
        IDXGIOutputDuplication *dup = nullptr;
        if (output1) output1->DuplicateOutput(dev, &dup);
        if (!dup)
        {
            LOG_ERROR("VideoCaptureScreen(dxgi): DuplicateOutput failed");
            safeRelease(output1); safeRelease(ctx); safeRelease(dev);
            safeRelease(output); safeRelease(adapter); safeRelease(factory);
            m_running.store(false);
            return;
        }

        LOG_INFO("VideoCaptureScreen(dxgi): duplicating output " + std::to_string(wantOut));

        ID3D11Texture2D *staging = nullptr;
        UINT stW = 0, stH = 0;

        while (m_running.load())
        {
            DXGI_OUTDUPL_FRAME_INFO info;
            IDXGIResource *res = nullptr;
            hr = dup->AcquireNextFrame(100, &info, &res); // 100 ms poll
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) { continue; } // no new frame
            if (FAILED(hr))
            {
                // Access lost (resolution change, mode switch, secure
                // desktop) — rebuild the duplication.
                safeRelease(res);
                safeRelease(dup);
                if (output1) output1->DuplicateOutput(dev, &dup);
                if (!dup) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); }
                continue;
            }

            ID3D11Texture2D *tex = nullptr;
            res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex));
            if (tex)
            {
                D3D11_TEXTURE2D_DESC td;
                tex->GetDesc(&td);
                if (!staging || stW != td.Width || stH != td.Height)
                {
                    safeRelease(staging);
                    D3D11_TEXTURE2D_DESC sd = {};
                    sd.Width = td.Width; sd.Height = td.Height;
                    sd.MipLevels = 1; sd.ArraySize = 1;
                    sd.Format = td.Format; // B8G8R8A8_UNORM from the desktop
                    sd.SampleDesc.Count = 1;
                    sd.Usage = D3D11_USAGE_STAGING;
                    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    if (SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &staging)))
                    {
                        stW = td.Width; stH = td.Height;
                    }
                }
                if (staging)
                {
                    ctx->CopyResource(staging, tex);
                    D3D11_MAPPED_SUBRESOURCE map;
                    if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map)))
                    {
                        m_sink.onScreenFrame(static_cast<const uint8_t *>(map.pData),
                                             td.Width, td.Height,
                                             static_cast<uint32_t>(map.RowPitch),
                                             ScreenPixelFormat::BGRA);
                        ctx->Unmap(staging, 0);
                    }
                }
            }
            safeRelease(tex);
            safeRelease(res);
            dup->ReleaseFrame();
        }

        safeRelease(staging);
        safeRelease(dup);
        safeRelease(output1);
        safeRelease(ctx);
        safeRelease(dev);
        safeRelease(output);
        safeRelease(adapter);
        safeRelease(factory);
    }

    IScreenFrameSink &m_sink;
    std::atomic<bool> m_running{false};
    std::thread       m_thread;
    std::string       m_target;
    bool              m_cursor = true;
};
} // namespace

std::unique_ptr<ScreenBackend> createScreenBackend(IScreenFrameSink &sink)
{
    LOG_INFO("ScreenBackend: DXGI Desktop Duplication (Windows)");
    return std::make_unique<DxgiScreenBackend>(sink);
}

#endif // RETROCAPTURE_SCREEN_DXGI
