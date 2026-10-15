#pragma once
#include "../render_probe/dummy_device.hpp"
// Changing 10-bit color and patterned depth expose an off-by-one frame pair.
struct PairFixture : DummyDevice {
    ComPtr<ID3D12Resource> depth;
    ComPtr<ID3D12DescriptorHeap> rtv,dsv;
    PairFixture() {
        Check(swapchain->ResizeBuffers(2,320,180,DXGI_FORMAT_R10G10B10A2_UNORM,0),"Resize fixture");
        D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=320; d.Height=180;
        d.DepthOrArraySize=d.MipLevels=1; d.Format=DXGI_FORMAT_R32G8X24_TYPELESS; d.SampleDesc.Count=1; d.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_HEAP_PROPERTIES h{}; h.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_CLEAR_VALUE clear{}; clear.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT; clear.DepthStencil.Depth=1;
        Check(device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_DEPTH_WRITE,&clear,IID_PPV_ARGS(&depth)),"Fixture depth");
        D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.NumDescriptors=1; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        Check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&dsv)),"Fixture DSV heap"); hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        Check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv)),"Fixture RTV heap");
        D3D12_DEPTH_STENCIL_VIEW_DESC view{}; view.Format=clear.Format; view.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(depth.Get(),&view,dsv->GetCPUDescriptorHandleForHeapStart());
    }
    void Frame(unsigned number) {
        ComPtr<IDXGISwapChain3> s; Check(swapchain.As(&s),"Fixture swap3");
        ComPtr<ID3D12Resource> back; Check(s->GetBuffer(s->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&back)),"Fixture backbuffer");
        Check(allocator->Reset(),"Fixture reset allocator"); Check(list->Reset(allocator.Get(),nullptr),"Fixture reset list");
        D3D12_RESOURCE_BARRIER b[2]{}; for(auto& t:b) t.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[0].Transition={back.Get(),0,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET};
        b[1].Transition={depth.Get(),0,number?D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_DEPTH_WRITE};
        if(number) list->ResourceBarrier(2,b); else list->ResourceBarrier(1,b);
        auto ch=rtv->GetCPUDescriptorHandleForHeapStart(),dh=dsv->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(back.Get(),nullptr,ch);
        const float first[4]={0.1f,0.9f,0.2f,1},second[4]={0.75f,0.25f,0.5f,1},third[4]={0.2f,0.5f,0.9f,1};
        list->ClearRenderTargetView(ch,number==0?first:number==1?second:third,0,nullptr);
        list->ClearDepthStencilView(dh,D3D12_CLEAR_FLAG_DEPTH,number==0?0.8f:number==1?0.4f:0.9f,0,0,nullptr);
        D3D12_RECT left{0,0,80,180}; list->ClearDepthStencilView(dh,D3D12_CLEAR_FLAG_DEPTH,number==0?0.6f:number==1?0.2f:0.7f,0,1,&left);
        b[0].Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET; b[0].Transition.StateAfter=D3D12_RESOURCE_STATE_PRESENT;
        b[1].Transition.StateBefore=D3D12_RESOURCE_STATE_DEPTH_WRITE; b[1].Transition.StateAfter=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        // Depth and final color deliberately finish in separate submissions,
        // matching the live renderer's observed organization.
        list->ResourceBarrier(1,b+1); Check(list->Close(),"Fixture close depth");
        ComPtr<ID3D12CommandAllocator> final_allocator;
        ComPtr<ID3D12GraphicsCommandList> final_list;
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&final_allocator)),"Fixture final allocator");
        Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,final_allocator.Get(),nullptr,IID_PPV_ARGS(&final_list)),"Fixture final list");
        Check(final_list->Close(),"Fixture initial final close"); Check(final_list->Reset(final_allocator.Get(),nullptr),"Fixture final reset");
        final_list->ResourceBarrier(1,b); Check(final_list->Close(),"Fixture close color");
        ID3D12CommandList* world[]={list.Get()},*last[]={final_list.Get()};
        queue->ExecuteCommandLists(1,world); queue->ExecuteCommandLists(1,last);
        if(number==1) { queue->ExecuteCommandLists(1,world); queue->ExecuteCommandLists(1,last); }
        swapchain->Present(0,0);
        ComPtr<ID3D12Fence> fence; Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Fixture fence"); Check(queue->Signal(fence.Get(),1),"Fixture signal");
        const auto until=GetTickCount64()+5000;
        while(fence->GetCompletedValue()<1 && GetTickCount64()<until) Sleep(1);
        if(fence->GetCompletedValue()<1 || fence->GetCompletedValue()==UINT64_MAX) TerminateProcess(GetCurrentProcess(),3);
    }
};
