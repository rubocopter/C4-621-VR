// Temporary D3D12/OpenXR transport probe. No writes to game memory or files.
// Quad layers validate display output; they do not create stereo game geometry.
#include "gpu_upload.hpp"
#include "window_capture.hpp"
#include "pose_publisher.hpp"
#include "capture_health.hpp"
#include "gpu_window_bridge.hpp"
#include "static_stereo.hpp"
#include "../live_stereo/live_stereo.hpp"
#include "stereo_frame.hpp"
#include "presentation_options.hpp"
#include "frame_metrics.hpp"
#include <winrt/base.h>
#include <DirectXMath.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <algorithm>

using Clock=std::chrono::steady_clock;
constexpr unsigned WIDTH=1280,HEIGHT=720;

static std::string Quote(const std::string& value) {
    std::ostringstream out; out << '"';
    for(unsigned char c:value) {
        if(c=='"' || c=='\\') out << '\\' << c;
        else if(c<32) out << ' ';
        else out << c;
    }
    out << '"'; return out.str();
}
static std::vector<std::uint8_t> Pattern(unsigned eye) {
    std::vector<std::uint8_t> pixels(WIDTH*HEIGHT*4);
    for(unsigned y=0;y<HEIGHT;++y) for(unsigned x=0;x<WIDTH;++x) {
        auto* p=pixels.data()+(y*WIDTH+x)*4;
        const bool grid=x%80<2 || y%80<2;
        p[0]=grid?100:14; p[1]=grid?100:20; p[2]=grid?100:28; p[3]=255;
        if(x<12 || y<12 || x>=WIDTH-12 || y>=HEIGHT-12) {
            p[0]=eye?255:40; p[1]=eye?130:210; p[2]=eye?30:255;
        }
        // Large L or R. Only the border and letter differ between the eyes.
        const unsigned gx=(x-520)/45,gy=(y-200)/45;
        const unsigned left[7]={16,16,16,16,16,16,31};
        const unsigned right[7]={30,17,17,30,20,18,17};
        if(x>=520 && x<745 && y>=200 && y<515 && ((eye?right[gy]:left[gy])&(16>>gx)))
            p[0]=p[1]=p[2]=230;
    }
    return pixels;
}
static void SavePreview(const std::filesystem::path& path,const std::vector<std::uint8_t>& rgba) {
    if(std::filesystem::exists(path)) throw std::runtime_error("Preview path already exists");
    BITMAPFILEHEADER file{}; file.bfType=0x4d42; file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER);
    file.bfSize=file.bfOffBits+WIDTH*HEIGHT*4;
    BITMAPINFOHEADER info{}; info.biSize=sizeof(info); info.biWidth=WIDTH; info.biHeight=-int(HEIGHT);
    info.biPlanes=1; info.biBitCount=32; info.biCompression=BI_RGB;
    auto bgra=rgba;
    for(std::size_t i=0;i<bgra.size();i+=4) std::swap(bgra[i],bgra[i+2]);
    std::ofstream output(path,std::ios::binary);
    output.write(reinterpret_cast<const char*>(&file),sizeof(file));
    output.write(reinterpret_cast<const char*>(&info),sizeof(info));
    output.write(reinterpret_cast<const char*>(bgra.data()),bgra.size());
    if(!output) throw std::runtime_error("Cannot save image preview");
}
struct Probe {
    XrApi xr;
    std::unique_ptr<GpuUpload> gpu;
    std::unique_ptr<WindowCapture> capture;
    std::unique_ptr<GpuWindowBridge> gpu_capture;
    std::unique_ptr<LiveStereo> pair_stream;
    std::unique_ptr<PosePublisher> publisher;
    XrSession session=XR_NULL_HANDLE;
    XrSpace local=XR_NULL_HANDLE,view=XR_NULL_HANDLE;
    std::array<XrSwapchain,2> chains{};
    std::array<std::vector<XrSwapchainImageD3D12KHR>,2> images;
    XrSessionState state=XR_SESSION_STATE_UNKNOWN;
    bool running=false,stop=false,tracking_ready=false;
    std::ofstream log;
    std::uint64_t frames=0,submitted=0,visible_frames=0,focused=0,captured=0,changed=0,valid_poses=0,capture_losses=0,discarded_source_frames=0;
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    std::filesystem::path output;
    bool static_stereo=false;
    std::array<std::vector<std::uint8_t>,2> static_pixels;
    PresentationOptions options;
    FrameMetric wait_timing,update_timing,stereo_timing,end_timing,source_age_timing,frame_interval_timing;
    std::array<FrameMetric,2> blit_timings;
    static double Milliseconds(Clock::time_point start) { return std::chrono::duration<double,std::milli>(Clock::now()-start).count(); }
    void Timing(const char* name,const FrameMetric& metric) {
        const auto value=metric.Summary();
        Event("{\"kind\":\"cpu_stage_timing_ms\",\"stage\":"+Quote(name)+",\"count\":"+std::to_string(value.count)+",\"sampled\":"+std::to_string(value.sampled)+
            ",\"mean\":"+std::to_string(value.mean)+",\"p50\":"+std::to_string(value.p50)+",\"p95\":"+std::to_string(value.p95)+",\"max\":"+std::to_string(value.maximum)+"}");
    }
    void Event(const std::string& row) { log << row << '\n'; log.flush(); std::cout << row << std::endl; }
    explicit Probe(const std::filesystem::path& path,DWORD pid,bool tracking,bool gpu_mode,
        const std::array<std::filesystem::path,2>& stereo_paths,const std::wstring& stream_name,PresentationOptions settings={}):output(path),options(settings) {
        options.Validate(!stream_name.empty(),tracking);
        if(std::filesystem::exists(path)) throw std::runtime_error("Evidence file already exists");
        if(path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
        log.open(path); if(!log) throw std::runtime_error("Cannot create evidence file");
        static_stereo=!stereo_paths[0].empty();
        if(static_stereo) static_pixels=LoadStaticStereo(stereo_paths,WIDTH,HEIGHT);
        XrInstanceProperties runtime{XR_TYPE_INSTANCE_PROPERTIES};
        XCheck(xr.GetInstanceProperties(xr.instance,&runtime),"xrGetInstanceProperties");
        XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO}; system_info.formFactor=XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XrSystemId system=XR_NULL_SYSTEM_ID;
        XCheck(xr.GetSystem(xr.instance,&system_info,&system),"xrGetSystem");
        XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
        XCheck(xr.GetSystemProperties(xr.instance,system,&properties),"xrGetSystemProperties");
        if(properties.graphicsProperties.maxLayerCount<2) throw std::runtime_error("Two quad layers required");
        if(options.eye_width>properties.graphicsProperties.maxSwapchainImageWidth||options.eye_height>properties.graphicsProperties.maxSwapchainImageHeight)
            throw std::runtime_error("Eye resolution exceeds runtime swapchain limits");
        XrGraphicsRequirementsD3D12KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
        XCheck(xr.GetD3D12GraphicsRequirementsKHR(xr.instance,system,&requirements),"xrGetD3D12GraphicsRequirementsKHR");
        gpu=std::make_unique<GpuUpload>(requirements);
        XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
        binding.device=gpu->device.Get(); binding.queue=gpu->queue.Get();
        XrSessionCreateInfo create{XR_TYPE_SESSION_CREATE_INFO}; create.next=&binding; create.systemId=system;
        XCheck(xr.CreateSession(xr.instance,&create,&session),"xrCreateSession");
        for(auto entry:{std::pair{XR_REFERENCE_SPACE_TYPE_LOCAL,&local},std::pair{XR_REFERENCE_SPACE_TYPE_VIEW,&view}}) {
            XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO}; space.referenceSpaceType=entry.first;
            space.poseInReferenceSpace.orientation.w=1;
            XCheck(xr.CreateReferenceSpace(session,&space,entry.second),"xrCreateReferenceSpace");
        }
        std::uint32_t count=0;
        XCheck(xr.EnumerateSwapchainFormats(session,0,&count,nullptr),"xrEnumerateSwapchainFormats count");
        std::vector<std::int64_t> formats(count);
        XCheck(xr.EnumerateSwapchainFormats(session,count,&count,formats.data()),"xrEnumerateSwapchainFormats");
        for(auto preferred:{DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB})
            if(format==DXGI_FORMAT_UNKNOWN && std::find(formats.begin(),formats.end(),preferred)!=formats.end()) format=preferred;
        if(format==DXGI_FORMAT_UNKNOWN) throw std::runtime_error("No supported sRGB RGBA/BGRA swapchain format");
        for(unsigned eye=0;eye<2;++eye) {
            XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            info.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT|XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
            info.format=format; info.sampleCount=1; info.width=options.eye_width; info.height=options.eye_height;
            info.faceCount=info.arraySize=info.mipCount=1;
            XCheck(xr.CreateSwapchain(session,&info,&chains[eye]),"xrCreateSwapchain");
            XCheck(xr.EnumerateSwapchainImages(chains[eye],0,&count,nullptr),"xrEnumerateSwapchainImages count");
            images[eye].assign(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
            XCheck(xr.EnumerateSwapchainImages(chains[eye],count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images[eye].data())),"xrEnumerateSwapchainImages");
        }
        if(!stream_name.empty()) pair_stream=std::make_unique<LiveStereo>(gpu->device.Get(),gpu->queue.Get(),stream_name,0.5f,options.eye_width,options.eye_height);
        else if(pid && gpu_mode) {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            gpu_capture=std::make_unique<GpuWindowBridge>(pid,gpu->device.Get(),gpu->queue.Get());
        } else if(pid) capture=std::make_unique<WindowCapture>(pid,gpu->adapter.Get());
        if(tracking) publisher=std::make_unique<PosePublisher>(pid,options.rotation_limit);
        if(pair_stream) Event("{\"kind\":\"projection_calibration\",\"projection\":"+pair_stream->Calibration().Json()+"}");
        Event("{\"kind\":\"metadata\",\"runtime\":"+Quote(runtime.runtimeName)+",\"system\":"+Quote(properties.systemName)+
            ",\"graphics\":\"D3D12\",\"mode\":"+Quote(pair_stream?"live_depth_reprojected_pair_quad":static_stereo?"static_depth_reprojected_pair_quad":pid?"game_window_quad":"per_eye_pattern_quad")+
            ",\"game_pid\":"+std::to_string(pid)+",\"width\":"+std::to_string(options.eye_width)+",\"height\":"+std::to_string(options.eye_height)+",\"format\":"+std::to_string(format)+
            ",\"head_locked\":"+std::string(tracking?"true":"false")+",\"headtracking\":"+std::string(tracking?"true":"false")+
            ",\"capture_transport\":"+Quote(pair_stream?"gpu_shared_paired_buffer":static_stereo?"static_rgba_upload":gpu_mode?"gpu_shared_texture":"cpu_readback")+",\"pixel_changes_measured\":"+std::string(pair_stream||gpu_mode||static_stereo?"false":"true")+
            ",\"depth_reprojected_stereo\":"+std::string(pair_stream||static_stereo?"true":"false")+",\"source_is_live\":"+std::string(pair_stream||pid?"true":"false")+
            ",\"eye_images_distinct\":"+std::string(pair_stream?"null":static_stereo&&static_pixels[0]!=static_pixels[1]?"true":"false")+
            ",\"rotation_limit_degrees\":"+std::to_string(options.rotation_limit)+",\"pose_coordinate_conversion\":\"reflect_z\",\"game_geometry_stereo\":false}");
    }
    void Poll() {
        while(true) {
            XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
            const auto result=xr.PollEvent(xr.instance,&event);
            if(result==XR_EVENT_UNAVAILABLE) return;
            XCheck(result,"xrPollEvent");
            if(event.type==XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) { stop=true; return; }
            if(event.type!=XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) continue;
            state=reinterpret_cast<XrEventDataSessionStateChanged*>(&event)->state;
            Event("{\"kind\":\"session_state\",\"state\":"+std::to_string(state)+"}");
            if(state==XR_SESSION_STATE_READY && !running) {
                XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO}; begin.primaryViewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XCheck(xr.BeginSession(session,&begin),"xrBeginSession"); running=true;
            } else if(state==XR_SESSION_STATE_STOPPING && running) {
                XCheck(xr.EndSession(session),"xrEndSession"); running=false; stop=true;
            } else if(state==XR_SESSION_STATE_EXITING || state==XR_SESSION_STATE_LOSS_PENDING) stop=true;
        }
    }
    int Run(double seconds) {
        auto pixels=static_stereo?static_pixels:std::array{Pattern(0),Pattern(1)};
        auto preview=output; preview.replace_extension(L".bmp");
        if(!capture && !gpu_capture && !pair_stream) SavePreview(preview,pixels[0]);
        if(static_stereo) { auto right_preview=output; right_preview.replace_extension(L".right.bmp"); SavePreview(right_preview,pixels[1]); }
        const bool captures_game=bool(capture)||bool(gpu_capture)||bool(pair_stream);
        bool centered=false,preview_saved=false;
        bool capture_was_stale=false;
        ULONGLONG last_capture_tick=0;
        XrPosef quad_pose{}; quad_pose.orientation.w=1;
        const auto began=Clock::now();
        auto visible_began=Clock::time_point{};
        auto last_capture=began-std::chrono::seconds(1),last_log=began;
        auto previous_frame=Clock::time_point{};
        // Give the user 30 seconds to put the headset back on after replying.
        // Never count SYNCHRONIZED-only frames as the visual test interval.
        while(!stop && std::chrono::duration<double>(Clock::now()-began).count()<seconds+30) {
            if(GetAsyncKeyState(VK_F12)&0x8000) { Event("{\"kind\":\"cancelled\",\"key\":\"F12\"}"); break; }
            Poll(); if(stop) break;
            if(state>=XR_SESSION_STATE_VISIBLE && state<=XR_SESSION_STATE_FOCUSED && visible_began==Clock::time_point{})
                visible_began=Clock::now();
            if(visible_began!=Clock::time_point{} && std::chrono::duration<double>(Clock::now()-visible_began).count()>=seconds) break;
            if(!running) { Sleep(10); continue; }
            const auto frame_start=Clock::now();
            if(previous_frame!=Clock::time_point{}) frame_interval_timing.Add(std::chrono::duration<double,std::milli>(frame_start-previous_frame).count());
            previous_frame=frame_start;
            XrFrameState frame{XR_TYPE_FRAME_STATE}; XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
            const auto wait_start=Clock::now();
            XCheck(xr.WaitFrame(session,&wait,&frame),"xrWaitFrame");
            wait_timing.Add(Milliseconds(wait_start));
            XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO}; XCheck(xr.BeginFrame(session,&begin),"xrBeginFrame");
            ++frames;
            if(!centered) {
                XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
                XCheck(xr.LocateSpace(view,local,frame.predictedDisplayTime,&head),"xrLocateSpace");
                if((head.locationFlags&3)==3) {
                    const auto& q=head.pose.orientation;
                    auto displacement=DirectX::XMVector3Rotate(DirectX::XMVectorSet(0,0,-2,0),DirectX::XMVectorSet(q.x,q.y,q.z,q.w));
                    DirectX::XMFLOAT3 delta; DirectX::XMStoreFloat3(&delta,displacement);
                    quad_pose=head.pose;
                    quad_pose.position.x+=delta.x; quad_pose.position.y+=delta.y; quad_pose.position.z+=delta.z;
                    centered=true;
                    Event("{\"kind\":\"quad_centered\"}");
                }
            }
            if(capture && std::chrono::duration<double>(Clock::now()-last_capture).count()>=1.0/30) {
                last_capture=Clock::now(); std::vector<std::uint8_t> next;
                if(capture->Next(next,WIDTH,HEIGHT)) {
                    if(captured && next!=pixels[0]) ++changed;
                    pixels[0]=std::move(next); pixels[1]=pixels[0]; ++captured;
                    last_capture_tick=GetTickCount64();
                    if(!preview_saved) { SavePreview(preview,pixels[0]); preview_saved=true; }
                }
            }
            if(gpu_capture && gpu_capture->Update()) {
                ++captured; last_capture_tick=GetTickCount64();
            }
            if(pair_stream) {
                const auto update_start=Clock::now();
                if(pair_stream->Update()) { ++captured;last_capture_tick=pair_stream->CaptureTick(); }
                update_timing.Add(Milliseconds(update_start));
                const auto now=GetTickCount64();
                if(last_capture_tick&&now>=last_capture_tick) source_age_timing.Add(double(now-last_capture_tick));
            }
            const bool fresh=!captures_game || CaptureFresh(GetTickCount64(),last_capture_tick,captured!=0);
            if(captures_game && captured && !fresh && !capture_was_stale) {
                ++capture_losses; Event("{\"kind\":\"capture_stale\",\"image_hidden\":true,\"tracking_invalidated\":true}");
            }
            capture_was_stale=captures_game && captured && !fresh;
            if(publisher) {
                XrSpaceLocation pose{XR_TYPE_SPACE_LOCATION};
                XCheck(xr.LocateSpace(view,local,frame.predictedDisplayTime,&pose),"xrLocateSpace tracking");
                const auto& q=pose.pose.orientation;
                const bool valid=HeadTrackingReady(state==XR_SESSION_STATE_VISIBLE || state==XR_SESSION_STATE_FOCUSED,
                    frame.shouldRender,fresh,(pose.locationFlags&XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)!=0,
                    (pose.locationFlags&XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT)!=0);
                publisher->Update({q.x,q.y,q.z,q.w},valid);
                tracking_ready=valid;
                if(valid) ++valid_poses;
            }
            XrCompositionLayerQuad layers[2]={{XR_TYPE_COMPOSITION_LAYER_QUAD},{XR_TYPE_COMPOSITION_LAYER_QUAD}};
            const XrCompositionLayerBaseHeader* bases[2]={reinterpret_cast<XrCompositionLayerBaseHeader*>(&layers[0]),reinterpret_cast<XrCompositionLayerBaseHeader*>(&layers[1])};
            const bool eligible=frame.shouldRender && centered && fresh;
            const auto source_frame_id=pair_stream?pair_stream->FrameId():0;
            std::array<std::uint32_t,2> indices{};
            const auto stereo_start=Clock::now();
            const bool visible=PresentStereoFrame(eligible,
                [&](unsigned eye) {
                    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                    XCheck(xr.AcquireSwapchainImage(chains[eye],&acquire,&indices[eye]),"xrAcquireSwapchainImage");
                    XrSwapchainImageWaitInfo available{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; available.timeout=XR_INFINITE_DURATION;
                    XCheck(xr.WaitSwapchainImage(chains[eye],&available),"xrWaitSwapchainImage");
                },
                [&](unsigned eye) {
                    auto* target=images[eye].at(indices[eye]).texture;
                    if(pair_stream) {
                        const auto blit_start=Clock::now();const bool result=pair_stream->TryBlit(target,format,eye);
                        blit_timings[eye].Add(Milliseconds(blit_start));if(!result) return false;
                    }
                    else if(gpu_capture) gpu_capture->Blit(target,format);
                    else gpu->Put(target,pixels[eye],format);
                    auto& layer=layers[eye]; layer.space=publisher?view:local; layer.eyeVisibility=eye?XR_EYE_VISIBILITY_RIGHT:XR_EYE_VISIBILITY_LEFT;
                    layer.subImage.swapchain=chains[eye]; layer.subImage.imageRect.extent={int(options.eye_width),int(options.eye_height)};
                    layer.pose=quad_pose; layer.size={2.4f,1.35f};
                    if(publisher) {
                        layer.pose={}; layer.pose.orientation.w=1; layer.pose.position.z=-1.3f;
                        // Match the current camera's approximate 120-degree horizontal FOV.
                        layer.size={4.5f,2.53125f};
                    }
                    return true;
                },
                [&](unsigned eye) {
                    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                    XCheck(xr.ReleaseSwapchainImage(chains[eye],&release),"xrReleaseSwapchainImage");
                },
                [&] {
                    // The second copy/release may outlive the source's 500ms
                    // budget or observe a producer failure. Submit both or none.
                    return !pair_stream || (pair_stream->FrameId()==source_frame_id&&pair_stream->HasFreshFrame());
                },
                [&](bool ready) {
                    const auto end_start=Clock::now();
                    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO}; end.displayTime=frame.predictedDisplayTime; end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                    end.layerCount=ready?2:0; end.layers=ready?bases:nullptr;
                    XCheck(xr.EndFrame(session,&end),"xrEndFrame");
                    end_timing.Add(Milliseconds(end_start));
                });
            stereo_timing.Add(Milliseconds(stereo_start));
            if(eligible&&!visible&&pair_stream) {
                ++discarded_source_frames;tracking_ready=false;if(publisher) publisher->Invalidate();
                Event("{\"kind\":\"source_discarded_during_frame\",\"layers\":0}");
            }
            if(visible) { ++submitted; if(state==XR_SESSION_STATE_VISIBLE || state==XR_SESSION_STATE_FOCUSED) ++visible_frames; if(state==XR_SESSION_STATE_FOCUSED) ++focused; }
            if(Clock::now()-last_log>=std::chrono::seconds(1)) {
                last_log=Clock::now();
                Event("{\"kind\":\"progress\",\"submitted_frames\":"+std::to_string(submitted)+",\"focused_frames\":"+std::to_string(focused)+",\"captured_frames\":"+std::to_string(captured)+",\"source_frame_id\":"+std::to_string(pair_stream?pair_stream->FrameId():0)+",\"valid_published_poses\":"+std::to_string(valid_poses)+",\"tracking_ready\":"+std::string(tracking_ready?"true":"false")+"}");
            }
        }
        if(publisher) publisher->Invalidate();
        ShutdownSession();
        Timing("xr_wait_frame",wait_timing);Timing("pair_update_including_gpu_wait",update_timing);
        Timing("left_blit_including_gpu_wait",blit_timings[0]);Timing("right_blit_including_gpu_wait",blit_timings[1]);
        Timing("stereo_transaction",stereo_timing);Timing("xr_end_frame",end_timing);
        Timing("source_age",source_age_timing);Timing("frame_interval",frame_interval_timing);
        Event("{\"kind\":\"summary\",\"frames\":"+std::to_string(frames)+",\"submitted_frames\":"+std::to_string(submitted)+
            ",\"visible_frames\":"+std::to_string(visible_frames)+",\"focused_frames\":"+std::to_string(focused)+",\"captured_frames\":"+std::to_string(captured)+",\"changed_capture_frames\":"+std::to_string(changed)+
            ",\"valid_published_poses\":"+std::to_string(valid_poses)+",\"capture_loss_events\":"+std::to_string(capture_losses)+",\"discarded_source_frames\":"+std::to_string(discarded_source_frames)+",\"session_running\":false,\"game_geometry_stereo\":false,\"depth_reprojected_stereo\":"+std::string(pair_stream||static_stereo?"true":"false")+",\"source_is_live\":"+std::string(captures_game?"true":"false")+"}");
        return visible_frames>0 && (!publisher || valid_poses>0) && (!captures_game || captured>0) && capture_losses==0?0:2;
    }
    void ShutdownSession() {
        if(!running) return;
        XCheck(xr.RequestExitSession(session),"xrRequestExitSession");
        const auto deadline=Clock::now()+std::chrono::seconds(3);
        while(running && Clock::now()<deadline) { Poll(); Sleep(10); }
        if(running) throw std::runtime_error("Session did not stop within cleanup window");
    }
    ~Probe() {
        publisher.reset(); capture.reset(); gpu_capture.reset();pair_stream.reset();
        for(auto chain:chains) if(chain) xr.DestroySwapchain(chain);
        if(view) xr.DestroySpace(view);
        if(local) xr.DestroySpace(local);
        if(session) xr.DestroySession(session);
    }
};
int wmain(int argc,wchar_t** argv) {
    try {
        double seconds=15; DWORD pid=0; bool tracking=false,gpu_mode=false; std::filesystem::path output;
        std::array<std::filesystem::path,2> stereo_paths;
        std::wstring stream_name;
        PresentationOptions options;
        for(int i=1;i<argc;++i) {
            const std::wstring arg=argv[i];
            if(arg==L"--seconds" && i+1<argc) seconds=std::stod(argv[++i]);
            else if(arg==L"--game-pid" && i+1<argc) pid=std::stoul(argv[++i]);
            else if(arg==L"--output" && i+1<argc) output=argv[++i];
            else if(arg==L"--headtracking") tracking=true;
            else if(arg==L"--gpu-capture") gpu_mode=true;
            else if(arg==L"--stereo-left" && i+1<argc) stereo_paths[0]=argv[++i];
            else if(arg==L"--stereo-right" && i+1<argc) stereo_paths[1]=argv[++i];
            else if(arg==L"--pair-stream" && i+1<argc) stream_name=argv[++i];
            else if(arg==L"--eye-width" && i+1<argc) options.eye_width=ParseEyeDimension(argv[++i]);
            else if(arg==L"--eye-height" && i+1<argc) options.eye_height=ParseEyeDimension(argv[++i]);
            else if(arg==L"--rotation-limit-deg" && i+1<argc) options.rotation_limit=ParseRotationLimit(argv[++i]);
            else throw std::runtime_error("Usage: --seconds 1..90 --output new.jsonl [--game-pid PID] [--headtracking] [--gpu-capture] [--pair-stream NAME] [--eye-width W --eye-height H] [--rotation-limit-deg 1..180] [--stereo-left left.rgba.raw --stereo-right right.rgba.raw]");
        }
        if(output.empty() || !std::isfinite(seconds) || seconds<1 || seconds>90) throw std::runtime_error("Output and bounded duration required");
        if(tracking && !pid) throw std::runtime_error("Head tracking requires a selected game PID");
        if(gpu_mode && !pid) throw std::runtime_error("GPU capture requires a selected game PID");
        if(stereo_paths[0].empty()!=stereo_paths[1].empty()) throw std::runtime_error("Both stereo eye paths are required");
        if(!stereo_paths[0].empty()&&(pid||tracking||gpu_mode)) throw std::runtime_error("Static stereo cannot be combined with live game capture or camera tracking");
        if(!stream_name.empty()&&(!stereo_paths[0].empty()||gpu_mode)) throw std::runtime_error("Paired stream cannot be combined with window/static capture");
        options.Validate(!stream_name.empty(),tracking);
        Probe probe(output,pid,tracking,gpu_mode,stereo_paths,stream_name,options); return probe.Run(seconds);
    } catch(const winrt::hresult_error& e) { std::cerr << "WinRT: " << winrt::to_string(e.message()) << '\n'; return 1; }
    catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
