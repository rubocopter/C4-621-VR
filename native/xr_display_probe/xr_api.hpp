#pragma once
#include <windows.h>
#include <d3d12.h>
#include <openxr.h>
#include <openxr_platform.h>
#include <stdexcept>
#include <string>

inline void XCheck(XrResult result,const char* call) {
    if (XR_FAILED(result)) throw std::runtime_error(std::string(call)+" result="+std::to_string(result));
}
inline void HCheck(HRESULT result,const char* call) {
    if (FAILED(result)) throw std::runtime_error(std::string(call)+" HRESULT="+std::to_string(static_cast<unsigned long>(result)));
}
#define XR_FUNCTIONS(F) \
 F(DestroyInstance) F(GetInstanceProperties) F(GetSystem) F(GetSystemProperties) \
 F(GetD3D12GraphicsRequirementsKHR) F(CreateSession) F(DestroySession) \
 F(CreateReferenceSpace) F(DestroySpace) F(LocateSpace) F(EnumerateSwapchainFormats) \
 F(CreateSwapchain) F(DestroySwapchain) F(EnumerateSwapchainImages) \
 F(AcquireSwapchainImage) F(WaitSwapchainImage) F(ReleaseSwapchainImage) \
 F(PollEvent) F(BeginSession) F(RequestExitSession) F(EndSession) \
 F(WaitFrame) F(BeginFrame) F(EndFrame)
struct XrApi {
    HMODULE loader=nullptr;
    PFN_xrGetInstanceProcAddr GetInstanceProcAddr=nullptr;
    PFN_xrCreateInstance CreateInstance=nullptr;
    XrInstance instance=XR_NULL_HANDLE;
#define DECLARE(name) PFN_xr##name name=nullptr;
    XR_FUNCTIONS(DECLARE)
#undef DECLARE
    XrApi() {
        loader=LoadLibraryExW(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\bin\\win64\\openxr_loader.dll",nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if(!loader) throw std::runtime_error("SteamVR OpenXR loader unavailable");
        GetInstanceProcAddr=reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(loader,"xrGetInstanceProcAddr"));
        if(!GetInstanceProcAddr) throw std::runtime_error("No xrGetInstanceProcAddr export");
        Load(XR_NULL_HANDLE,"xrCreateInstance",CreateInstance);
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(info.applicationInfo.applicationName,"AC6 XR display probe");
        info.applicationInfo.applicationVersion=1;
        info.applicationInfo.apiVersion=XR_MAKE_VERSION(1,0,0);
        const char* extensions[]={XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
        info.enabledExtensionCount=1; info.enabledExtensionNames=extensions;
        XCheck(CreateInstance(&info,&instance),"xrCreateInstance");
#define LOAD(name) Load(instance,"xr" #name,name);
        XR_FUNCTIONS(LOAD)
#undef LOAD
    }
    template<class T> void Load(XrInstance owner,const char* name,T& function) {
        XCheck(GetInstanceProcAddr(owner,name,reinterpret_cast<PFN_xrVoidFunction*>(&function)),name);
    }
    ~XrApi() {
        if(instance && DestroyInstance) DestroyInstance(instance);
        if(loader) FreeLibrary(loader);
    }
};
