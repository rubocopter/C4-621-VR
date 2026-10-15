// Explicit live-window diagnostic. CPU readback is used only once to save this
// test's final preview, never by GpuWindowBridge's production image path.
#include "gpu_window_bridge.hpp"
#include <winrt/base.h>
#include <wrl/client.h>
#include <dxgi1_6.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
using Microsoft::WRL::ComPtr;
static void Check(HRESULT hr) { if(FAILED(hr)) throw std::runtime_error("live GPU test HRESULT "+std::to_string(static_cast<unsigned long>(hr))); }
int wmain(int argc,wchar_t** argv) try {
    if(argc!=3) throw std::runtime_error("Usage: gpu_bridge_live_test GAME_PID NEW_OUTPUT_PREFIX");
    const DWORD pid=std::stoul(argv[1]); const std::filesystem::path prefix=argv[2];
    auto bitmap=prefix; bitmap+=L".bmp"; auto report=prefix; report+=L".json";
    if(std::filesystem::exists(bitmap)||std::filesystem::exists(report)) throw std::runtime_error("Existing evidence");
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    ComPtr<ID3D12Device> device; Check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
    ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC q{}; q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    Check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
    D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=1280; d.Height=720;
    d.DepthOrArraySize=d.MipLevels=1; d.SampleDesc.Count=1; d.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> target; Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&target)));
    GpuWindowBridge bridge(pid,device.Get(),queue.Get());
    const auto began=std::chrono::steady_clock::now(); unsigned frames=0;
    while(std::chrono::steady_clock::now()-began<std::chrono::seconds(5)) {
        if(bridge.Update()) { bridge.Blit(target.Get(),d.Format); ++frames; }
        Sleep(1);
    }
    if(!frames) throw std::runtime_error("No WGC frames received");
    const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-began).count();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 total=0; device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&total);
    D3D12_RESOURCE_DESC b{}; b.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; b.Width=total;
    b.Height=b.DepthOrArraySize=b.MipLevels=1; b.SampleDesc.Count=1; b.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    heap.Type=D3D12_HEAP_TYPE_READBACK; ComPtr<ID3D12Resource> read;
    Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&b,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&read)));
    ComPtr<ID3D12CommandAllocator> allocator; Check(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list; Check(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; barrier.Transition={target.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1,&barrier);
    D3D12_TEXTURE_COPY_LOCATION from{}; from.pResource=target.Get(); from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION to{}; to.pResource=read.Get(); to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint=fp;
    list->CopyTextureRegion(&to,0,0,0,&from,nullptr); std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter); list->ResourceBarrier(1,&barrier);
    Check(list->Close()); ID3D12CommandList* lists[]={list.Get()}; queue->ExecuteCommandLists(1,lists);
    ComPtr<ID3D12Fence> fence; Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))); Check(queue->Signal(fence.Get(),1));
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr); if(!event) throw std::runtime_error("Event creation failed");
    Check(fence->SetEventOnCompletion(1,event));
    if(WaitForSingleObject(event,5000)!=WAIT_OBJECT_0) { std::cerr<<"live preview fence timeout\n"; TerminateProcess(GetCurrentProcess(),3); }
    CloseHandle(event);
    void* data=nullptr; D3D12_RANGE range{0,static_cast<SIZE_T>(total)}; Check(read->Map(0,&range,&data));
    std::vector<unsigned char> pixels(1280*720*4);
    for(unsigned y=0;y<720;++y) for(unsigned x=0;x<1280;++x) {
        const auto* s=static_cast<const unsigned char*>(data)+fp.Offset+y*fp.Footprint.RowPitch+x*4;
        auto* p=pixels.data()+(y*1280+x)*4; p[0]=s[2]; p[1]=s[1]; p[2]=s[0]; p[3]=255;
    }
    D3D12_RANGE no_write{0,0}; read->Unmap(0,&no_write);
    BITMAPFILEHEADER file{}; file.bfType=0x4d42; file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER); file.bfSize=file.bfOffBits+static_cast<DWORD>(pixels.size());
    BITMAPINFOHEADER info{}; info.biSize=sizeof(info); info.biWidth=1280; info.biHeight=-720; info.biPlanes=1; info.biBitCount=32;
    std::ofstream picture(bitmap,std::ios::binary); picture.write(reinterpret_cast<const char*>(&file),sizeof(file)); picture.write(reinterpret_cast<const char*>(&info),sizeof(info)); picture.write(reinterpret_cast<const char*>(pixels.data()),pixels.size());
    if(!picture) throw std::runtime_error("Cannot save live test preview");
    std::ofstream output(report); output<<"{\"pid\":"<<pid<<",\"captured_frames\":"<<frames<<",\"elapsed_seconds\":"<<elapsed<<",\"capture_rate_hz\":"<<frames/elapsed<<",\"source_width\":"<<bridge.CaptureWidth()<<",\"source_height\":"<<bridge.CaptureHeight()<<",\"source_time_100ns\":"<<bridge.FreshnessTick()<<",\"production_cpu_pixel_readback\":false,\"test_only_final_readback\":true}\n";
    std::cout<<"GPU window capture: "<<frames<<" frames in "<<elapsed<<" seconds\n"; return 0;
} catch(const winrt::hresult_error& e) { std::cerr<<winrt::to_string(e.message())<<'\n'; return 1; }
catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
