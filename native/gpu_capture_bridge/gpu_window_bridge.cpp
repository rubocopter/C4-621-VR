#include "gpu_window_bridge.hpp"
#include "gpu_image_transport.hpp"
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <filesystem>
#include <stdexcept>

namespace capture = winrt::Windows::Graphics::Capture;
namespace directx = winrt::Windows::Graphics::DirectX;
namespace projected = winrt::Windows::Graphics::DirectX::Direct3D11;
struct GpuWindowBridge::Impl {
    DWORD pid=0;
    HWND window=nullptr;
    UINT width=0,height=0;
    std::uint64_t counter=0;
    std::int64_t tick=0;
    std::unique_ptr<gpu_capture::GpuImageTransport> transport;
    capture::GraphicsCaptureItem item{nullptr};
    capture::Direct3D11CaptureFramePool pool{nullptr};
    capture::GraphicsCaptureSession session{nullptr};
    capture::Direct3D11CaptureFrame pending{nullptr};
    Impl(DWORD selected,ID3D12Device* device,ID3D12CommandQueue* queue):pid(selected) {
        if(!pid || !device || !queue) throw std::invalid_argument("GPU capture requires PID, device and queue");
        if(!capture::GraphicsCaptureSession::IsSupported()) throw std::runtime_error("Windows Graphics Capture unavailable");
        HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
        if(!process) throw std::runtime_error("Cannot query selected AC6 process");
        wchar_t path[32768]; DWORD length=32768;
        const bool named=QueryFullProcessImageNameW(process,0,path,&length)!=FALSE; CloseHandle(process);
        if(!named || _wcsicmp(std::filesystem::path(path).filename().c_str(),L"armoredcore6.exe")!=0)
            throw std::runtime_error("GPU capture requires armoredcore6.exe");
        struct Search { DWORD pid; HWND window=nullptr; } search{pid};
        EnumWindows([](HWND hwnd,LPARAM value)->BOOL {
            auto& s=*reinterpret_cast<Search*>(value); DWORD owner=0; GetWindowThreadProcessId(hwnd,&owner);
            if(owner==s.pid && IsWindowVisible(hwnd) && !GetWindow(hwnd,GW_OWNER)) { s.window=hwnd; return FALSE; }
            return TRUE;
        },reinterpret_cast<LPARAM>(&search));
        window=search.window; if(!window) throw std::runtime_error("No visible window for selected AC6 PID");
        auto factory=winrt::get_activation_factory<capture::GraphicsCaptureItem,IGraphicsCaptureItemInterop>();
        winrt::check_hresult(factory->CreateForWindow(window,winrt::guid_of<capture::GraphicsCaptureItem>(),winrt::put_abi(item)));
        const auto size=item.Size();
        if(size.Width<=0 || size.Height<=0) throw std::runtime_error("Invalid capture size");
        width=static_cast<UINT>(size.Width); height=static_cast<UINT>(size.Height);
        transport=std::make_unique<gpu_capture::GpuImageTransport>(device,queue,width,height);
        winrt::com_ptr<IDXGIDevice> dxgi;
        winrt::check_hresult(transport->Device11()->QueryInterface(__uuidof(IDXGIDevice),dxgi.put_void()));
        winrt::com_ptr<IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(),inspectable.put()));
        auto winrt_device=inspectable.as<projected::IDirect3DDevice>();
        pool=capture::Direct3D11CaptureFramePool::CreateFreeThreaded(winrt_device,directx::DirectXPixelFormat::B8G8R8A8UIntNormalized,2,size);
        session=pool.CreateCaptureSession(item); session.IsCursorCaptureEnabled(false); session.StartCapture();
    }
    ~Impl() {
        // Retain the WGC source until the GPU producer has finished reading it.
        if(transport) transport->Drain();
        if(pending) pending.Close();
        try { if(session) session.Close(); } catch(...) {}
        try { if(pool) pool.Close(); } catch(...) {}
    }
    bool Update() {
        DWORD owner=0;
        if(!IsWindow(window) || !GetWindowThreadProcessId(window,&owner) || owner!=pid)
            throw std::runtime_error("Selected AC6 window was closed");
        if(pending) {
            if(!transport->ProducerComplete()) return false;
            pending.Close(); pending=nullptr;
        }
        auto frame=pool.TryGetNextFrame(); if(!frame) return false;
        // Drop queued old frames without ever reading their pixels on CPU.
        for(unsigned n=0;n<2;++n) {
            auto newer=pool.TryGetNextFrame(); if(!newer) break;
            frame.Close(); frame=newer;
        }
        const auto size=frame.ContentSize();
        if(size.Width!=static_cast<int>(width) || size.Height!=static_cast<int>(height))
            throw std::runtime_error("AC6 window resized; restart GPU capture with the new dimensions");
        auto access=frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> source;
        winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D),source.put_void()));
        // Keep a reference before submitting: exceptions must not release an
        // in-flight WGC frame before the transport's bounded drain completes.
        pending=frame;
        transport->Submit(source.get());
        tick=frame.SystemRelativeTime().count(); ++counter;
        return true;
    }
};
GpuWindowBridge::GpuWindowBridge(DWORD pid,ID3D12Device* device,ID3D12CommandQueue* queue):impl_(std::make_unique<Impl>(pid,device,queue)) {}
GpuWindowBridge::~GpuWindowBridge()=default;
bool GpuWindowBridge::Update() { return impl_->Update(); }
void GpuWindowBridge::Blit(ID3D12Resource* target,DXGI_FORMAT format) { impl_->transport->Blit(target,format); }
UINT GpuWindowBridge::CaptureWidth() const noexcept { return impl_->width; }
UINT GpuWindowBridge::CaptureHeight() const noexcept { return impl_->height; }
std::uint64_t GpuWindowBridge::CaptureCounter() const noexcept { return impl_->counter; }
std::int64_t GpuWindowBridge::FreshnessTick() const noexcept { return impl_->tick; }
