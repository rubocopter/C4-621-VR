#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stddef.h>
size_t PairPresentSlot(void) { return offsetof(IDXGISwapChainVtbl,Present)/sizeof(void*); }
size_t PairExecuteSlot(void) { return offsetof(ID3D12CommandQueueVtbl,ExecuteCommandLists)/sizeof(void*); }
size_t PairBarrierSlot(void) { return offsetof(ID3D12GraphicsCommandListVtbl,ResourceBarrier)/sizeof(void*); }
size_t PairResetSlot(void) { return offsetof(ID3D12GraphicsCommandListVtbl,Reset)/sizeof(void*); }
size_t PairCloseSlot(void) { return offsetof(ID3D12GraphicsCommandListVtbl,Close)/sizeof(void*); }
size_t PairResizeSlot(void) { return offsetof(IDXGISwapChainVtbl,ResizeBuffers)/sizeof(void*); }
size_t PairResize1Slot(void) { return offsetof(IDXGISwapChain3Vtbl,ResizeBuffers1)/sizeof(void*); }
