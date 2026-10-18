// Bounded monitor-only integration diagnostic; readback only for two previews.
#include "live_stereo.hpp"
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <iostream>
#include <chrono>
#include <stdexcept>
#include <filesystem>
#include <cmath>
#include <string>
#include <limits>
using Microsoft::WRL::ComPtr;
static void Check(HRESULT hr) { if(FAILED(hr)) throw std::runtime_error("D3D12 diagnostic HRESULT "+std::to_string(static_cast<unsigned long>(hr))); }
static UINT ParseDimension(const wchar_t* text) {
    if(!*text) throw std::runtime_error("Output dimensions must be unsigned integers");
    UINT value=0;
    for(;*text;++text) {
        if(*text<L'0'||*text>L'9') throw std::runtime_error("Output dimensions must be unsigned integers");
        const UINT digit=static_cast<UINT>(*text-L'0');
        if(value>((std::numeric_limits<UINT>::max)()-digit)/10) throw std::runtime_error("Output dimension overflow");
        value=value*10+digit;
    }
    return value;
}
int wmain(int argc,wchar_t** argv) try {
    if(argc!=4&&argc!=6) throw std::runtime_error("Usage: mapping-name seconds NEW-preview-prefix [width height]");
    const double seconds=std::stod(argv[2]);if(!std::isfinite(seconds)||seconds<1||seconds>10) throw std::runtime_error("Diagnostic duration 1..10 seconds");
    UINT eyeWidth=1280,eyeHeight=720;
    if(argc==6) { eyeWidth=ParseDimension(argv[4]);eyeHeight=ParseDimension(argv[5]); }
    if(!((eyeWidth==1280&&eyeHeight==720)||(eyeWidth==1920&&eyeHeight==1080)||(eyeWidth==2560&&eyeHeight==1440))) throw std::runtime_error("Output dimensions must be 1280x720, 1920x1080 or 2560x1440");
    ComPtr<IDXGIFactory6> factory;Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<ID3D12Device> device;
    for(UINT i=0;;++i) {
        ComPtr<IDXGIAdapter1> adapter;auto hr=factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter));if(hr==DXGI_ERROR_NOT_FOUND) break;Check(hr);
        DXGI_ADAPTER_DESC1 desc{};Check(adapter->GetDesc1(&desc));if(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)))) break;
    }
    if(!device) throw std::runtime_error("Hardware adapter unavailable");
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;Check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
    LiveStereo stream(device.Get(),queue.Get(),argv[1],0.5f,eyeWidth,eyeHeight);
    const auto started=GetTickCount64();UINT64 count=0,first=0;
    while(GetTickCount64()-started<seconds*1000 && !(GetAsyncKeyState(VK_F12)&0x8000)) {
        if(stream.Update()) {
            ++count;if(!first) { first=stream.FrameId();stream.SavePreview(std::wstring(argv[3])+L"-first"); }
        } else Sleep(1);
    }
    if(count) stream.SavePreview(std::wstring(argv[3])+L"-last");
    std::cout<<"{\"frames\":"<<count<<",\"first_id\":"<<first<<",\"last_id\":"<<stream.FrameId()<<",\"width\":"<<stream.Width()<<",\"height\":"<<stream.Height()<<",\"production_cpu_readback\":false,\"eye_width\":"<<stream.EyeWidth()<<",\"eye_height\":"<<stream.EyeHeight()<<",\"projection\":"<<stream.Calibration().Json()<<"}\n";
    return count>2?0:2;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
