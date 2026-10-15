#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stddef.h>
size_t Ac6PresentSlot(void) { return offsetof(IDXGISwapChainVtbl,Present)/sizeof(void*); }
size_t Ac6ExecuteSlot(void) { return offsetof(ID3D12CommandQueueVtbl,ExecuteCommandLists)/sizeof(void*); }
size_t Ac6BarrierSlot(void) { return offsetof(ID3D12GraphicsCommandListVtbl,ResourceBarrier)/sizeof(void*); }
