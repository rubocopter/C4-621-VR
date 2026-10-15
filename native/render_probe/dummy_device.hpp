#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <stdexcept>
#include <string>
#include <cstdio>
using Microsoft::WRL::ComPtr;
inline void Check(HRESULT result,const char* name) {
    if(FAILED(result)) throw std::runtime_error(std::string(name)+": "+std::to_string(static_cast<unsigned long>(result)));
}
struct DummyDevice {
    HWND window=nullptr;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<IDXGISwapChain1> swapchain;
    DummyDevice() {
        window=CreateWindowExW(0,L"STATIC",L"AC6 temporary D3D12 observer",WS_POPUP,0,0,16,16,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!window) throw std::runtime_error("Create hidden diagnostic window failed");
        Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"CreateDXGIFactory1");
        Check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"D3D12CreateDevice");
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"CreateCommandQueue");
        Check(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)),"CreateCommandAllocator");
        Check(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"CreateCommandList");
        Check(list->Close(),"Close dummy command list");
        DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width=desc.Height=16; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count=1; desc.BufferCount=2; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        Check(factory->CreateSwapChainForHwnd(queue.Get(),window,&desc,nullptr,nullptr,&swapchain),"CreateSwapChainForHwnd");
    }
    ~DummyDevice() { swapchain.Reset(); if(window) DestroyWindow(window); }
    void ExerciseDepth(void(*prepare)(ID3D12Resource*)=nullptr) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width=2560; desc.Height=prepare?1444:1440; desc.DepthOrArraySize=desc.MipLevels=1;
        desc.Format=prepare?DXGI_FORMAT_R32G8X24_TYPELESS:DXGI_FORMAT_D32_FLOAT; desc.SampleDesc.Count=1; desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear{}; clear.Format=prepare?DXGI_FORMAT_D32_FLOAT_S8X24_UINT:desc.Format; clear.DepthStencil.Depth=1;
        ComPtr<ID3D12Resource> depth;
        Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_DEPTH_WRITE,&clear,IID_PPV_ARGS(&depth)),"Create synthetic depth");
        if(prepare) prepare(depth.Get());
        ComPtr<ID3D12Resource> unrelated;
        if(prepare) {
            D3D12_RESOURCE_DESC buffer{}; buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width=4096;
            buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1; buffer.SampleDesc.Count=1; buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&unrelated)),"Create unrelated batch resource");
        }
        Check(allocator->Reset(),"Reset dummy allocator"); Check(list->Reset(allocator.Get(),nullptr),"Reset dummy list");
        ComPtr<ID3D12DescriptorHeap> dsv;
        D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors=1;
        Check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&dsv)),"Create test DSV heap");
        D3D12_DEPTH_STENCIL_VIEW_DESC view{}; view.Format=clear.Format; view.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
        auto handle=dsv->GetCPUDescriptorHandleForHeapStart(); device->CreateDepthStencilView(depth.Get(),&view,handle);
        list->ClearDepthStencilView(handle,D3D12_CLEAR_FLAG_DEPTH,0.25f,0,0,nullptr);
        D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource=depth.Get();
        b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore=D3D12_RESOURCE_STATE_DEPTH_WRITE; b.Transition.StateAfter=D3D12_RESOURCE_STATE_DEPTH_READ;
        D3D12_RESOURCE_BARRIER batch[3]={b,b,{}};
        if(prepare) {
            // Depth and stencil are independent planes of the same resource.
            batch[0].Transition.Subresource=0;
            batch[1].Transition.Subresource=1;
            batch[2].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; batch[2].Transition.pResource=unrelated.Get();
            batch[2].Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            batch[2].Transition.StateBefore=D3D12_RESOURCE_STATE_COMMON; batch[2].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST;
        }
        list->ResourceBarrier(prepare?3:1,batch);
        std::swap(b.Transition.StateBefore,b.Transition.StateAfter); list->ResourceBarrier(1,&b);
        Check(list->Close(),"Close exercised list"); ID3D12CommandList* lists[]={list.Get()}; queue->ExecuteCommandLists(1,lists);
        ComPtr<ID3D12Fence> fence; Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Create test fence");
        Check(queue->Signal(fence.Get(),1),"Signal test fence");
        const auto deadline=GetTickCount64()+5000;
        while(fence->GetCompletedValue()<1 && GetTickCount64()<deadline) Sleep(1);
        if(fence->GetCompletedValue()<1) {
            // Standalone test only; no in-flight resource destruction on timeout.
            std::fprintf(stderr,"Synthetic GPU test timed out\n"); std::fflush(nullptr); TerminateProcess(GetCurrentProcess(),3);
        }
        swapchain->Present(0,0);
    }
};
