#include "window_capture.hpp"
#include "frame.hpp"
#include <d3d11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <filesystem>
#include <stdexcept>

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

struct WindowCapture::Impl {
    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11DeviceContext> context;
    com_ptr<ID3D11Texture2D> staging;
    GraphicsCaptureItem item{nullptr};
    Direct3D11CaptureFramePool pool{nullptr};
    GraphicsCaptureSession session{nullptr};
    winrt::Windows::Graphics::SizeInt32 size{};
};

WindowCapture::WindowCapture(DWORD pid, IDXGIAdapter1* adapter):impl(std::make_unique<Impl>()) {
    init_apartment(apartment_type::multi_threaded);
    if (!GraphicsCaptureSession::IsSupported()) throw std::runtime_error("Windows Graphics Capture unavailable");
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if (!process) throw std::runtime_error("Cannot query selected game process");
    wchar_t path[32768]; DWORD length=32768;
    const bool named=QueryFullProcessImageNameW(process,0,path,&length)!=FALSE;
    CloseHandle(process);
    if (!named || _wcsicmp(std::filesystem::path(path).filename().c_str(),L"armoredcore6.exe")!=0)
        throw std::runtime_error("Window capture requires armoredcore6.exe");
    struct Search { DWORD pid; HWND window=nullptr; } search{pid};
    EnumWindows([](HWND window, LPARAM value)->BOOL {
        auto& s=*reinterpret_cast<Search*>(value); DWORD owner=0;
        GetWindowThreadProcessId(window,&owner);
        if(owner==s.pid && IsWindowVisible(window) && !GetWindow(window,GW_OWNER)) {
            s.window=window; return FALSE;
        }
        return TRUE;
    },reinterpret_cast<LPARAM>(&search));
    if (!search.window) throw std::runtime_error("No visible AC6 window for selected PID");
    check_hresult(D3D11CreateDevice(adapter,D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,0,D3D11_SDK_VERSION,impl->device.put(),nullptr,impl->context.put()));
    auto dxgi=impl->device.as<IDXGIDevice>();
    com_ptr<IInspectable> inspectable;
    check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(),inspectable.put()));
    auto projected=inspectable.as<IDirect3DDevice>();
    auto factory=get_activation_factory<GraphicsCaptureItem,IGraphicsCaptureItemInterop>();
    check_hresult(factory->CreateForWindow(search.window,guid_of<GraphicsCaptureItem>(),put_abi(impl->item)));
    impl->size=impl->item.Size();
    impl->pool=Direct3D11CaptureFramePool::CreateFreeThreaded(projected,DirectXPixelFormat::B8G8R8A8UIntNormalized,2,impl->size);
    impl->session=impl->pool.CreateCaptureSession(impl->item);
    impl->session.IsCursorCaptureEnabled(false);
    impl->session.StartCapture();
}

WindowCapture::~WindowCapture() {
    if (impl->session) impl->session.Close();
    if (impl->pool) impl->pool.Close();
}

bool WindowCapture::Next(std::vector<std::uint8_t>& rgba, unsigned width, unsigned height) {
    auto frame=impl->pool.TryGetNextFrame();
    if (!frame) return false;
    const auto content=frame.ContentSize();
    if (content.Width!=impl->size.Width || content.Height!=impl->size.Height)
        throw std::runtime_error("AC6 window resized; stop and restart capture with the new size");
    auto access=frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    com_ptr<ID3D11Texture2D> texture;
    check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D),texture.put_void()));
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    if (!impl->staging) {
        desc.BindFlags=0; desc.MiscFlags=0; desc.Usage=D3D11_USAGE_STAGING; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        check_hresult(impl->device->CreateTexture2D(&desc,nullptr,impl->staging.put()));
    }
    impl->context->CopyResource(impl->staging.get(),texture.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check_hresult(impl->context->Map(impl->staging.get(),0,D3D11_MAP_READ,0,&mapped));
    try {
        rgba=FitWindowFrame(static_cast<const std::uint8_t*>(mapped.pData),content.Width,content.Height,mapped.RowPitch,width,height);
    } catch (...) { impl->context->Unmap(impl->staging.get(),0); throw; }
    impl->context->Unmap(impl->staging.get(),0);
    frame.Close();
    return true;
}
