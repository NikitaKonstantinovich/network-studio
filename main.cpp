#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>
#include <shellapi.h>
#include <commdlg.h>
#include <wrl/client.h>
#include "gui.h"
#include "imgui_internal.h"
#include "implot.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include <fstream>
#include <stdexcept>
#include <unordered_map>
#include <chrono>

using Microsoft::WRL::ComPtr;
static ComPtr<ID3D11Device> device;
static ComPtr<ID3D11DeviceContext> context;
static ComPtr<IDXGISwapChain> swapchain;
static ComPtr<ID3D11RenderTargetView> target;
static UINT resize_w=0,resize_h=0;
static std::unordered_map<HWND,WNDPROC> platform_procedures;
static void (*backend_create_window)(ImGuiViewport*)=nullptr;
// The bundled 1.91.5 Win32 backend reports monitor DPI but doesn't apply the
// WM_DPICHANGED suggested rectangle to secondary HWNDs. Keep this adapter local.
static LRESULT CALLBACK platform_wndproc(HWND hwnd,UINT msg,WPARAM w,LPARAM l) {
    auto it=platform_procedures.find(hwnd);auto previous=it==platform_procedures.end()?DefWindowProcW:it->second;
    if(msg==WM_DPICHANGED) {
        const auto* r=reinterpret_cast<RECT*>(l);
        SetWindowPos(hwnd,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);return 0;
    }
    auto result=CallWindowProcW(previous,hwnd,msg,w,l);
    if(msg==WM_NCDESTROY)platform_procedures.erase(hwnd);
    return result;
}
static void create_platform_window(ImGuiViewport* viewport) {
    backend_create_window(viewport);auto hwnd=static_cast<HWND>(viewport->PlatformHandle);
    platform_procedures[hwnd]=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(platform_wndproc)));
}
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);
static void check(HRESULT hr,const char* text) {if(FAILED(hr))throw std::runtime_error(text);}
static void render_target() {
    ComPtr<ID3D11Texture2D> back;
    check(swapchain->GetBuffer(0,IID_PPV_ARGS(&back)),"Cannot get swapchain buffer.");
    check(device->CreateRenderTargetView(back.Get(),nullptr,&target),"Cannot create render target.");
}
static void create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};sd.BufferCount=2;sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.OutputWindow=hwnd;sd.SampleDesc.Count=1;
    sd.Windowed=TRUE;sd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_0};
    auto hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,levels,2,D3D11_SDK_VERSION,&sd,&swapchain,&device,nullptr,&context);
    if(FAILED(hr))hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,levels,2,D3D11_SDK_VERSION,&sd,&swapchain,&device,nullptr,&context);
    check(hr,"Direct3D 11 initialization failed.");render_target();
}
static LRESULT WINAPI wndproc(HWND hwnd,UINT msg,WPARAM w,LPARAM l) {
    if(ImGui_ImplWin32_WndProcHandler(hwnd,msg,w,l))return true;
    switch(msg) {
        case WM_SIZE:if(w!=SIZE_MINIMIZED){resize_w=LOWORD(l);resize_h=HIWORD(l);}return 0;
        case WM_DPICHANGED: {
            auto* rect=reinterpret_cast<RECT*>(l);
            SetWindowPos(hwnd,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE);return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* info=reinterpret_cast<MINMAXINFO*>(l);info->ptMinTrackSize={1120,720};return 0;
        }
        case WM_SYSCOMMAND:if((w&0xfff0)==SC_KEYMENU)return 0;break;
        case WM_DESTROY:PostQuitMessage(0);return 0;
    }return DefWindowProcW(hwnd,msg,w,l);
}
static void capture_png(const std::filesystem::path& path,ID3D11Texture2D* texture=nullptr) {
    ComPtr<ID3D11Texture2D> back=texture,staging;if(!back)check(swapchain->GetBuffer(0,IID_PPV_ARGS(&back)),"Screenshot buffer failed.");
    D3D11_TEXTURE2D_DESC desc{};back->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
    check(device->CreateTexture2D(&desc,nullptr,&staging),"Screenshot texture failed.");context->CopyResource(staging.Get(),back.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Screenshot map failed.");
    try {
        ComPtr<IWICImagingFactory> factory;check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"WIC factory failed.");
        ComPtr<IWICStream> stream;check(factory->CreateStream(&stream),"WIC stream failed.");check(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE),"Screenshot output failed.");
        ComPtr<IWICBitmapEncoder> encoder;check(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"PNG encoder failed.");
        check(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"PNG initialization failed.");
        ComPtr<IWICBitmapFrameEncode> frame;check(encoder->CreateNewFrame(&frame,nullptr),"PNG frame failed.");check(frame->Initialize(nullptr),"PNG frame initialization failed.");
        check(frame->SetSize(desc.Width,desc.Height),"PNG size failed.");WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
        check(frame->SetPixelFormat(&format),"PNG pixel format failed.");
        ComPtr<IWICBitmap> bitmap;check(factory->CreateBitmapFromMemory(desc.Width,desc.Height,GUID_WICPixelFormat32bppRGBA,
            mapped.RowPitch,mapped.RowPitch*desc.Height,static_cast<BYTE*>(mapped.pData),&bitmap),"WIC bitmap failed.");
        ComPtr<IWICFormatConverter> converter;check(factory->CreateFormatConverter(&converter),"WIC converter failed.");
        check(converter->Initialize(bitmap.Get(),format,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom),"PNG conversion failed.");
        check(frame->WriteSource(converter.Get(),nullptr),"PNG pixel write failed.");
        check(frame->Commit(),"PNG frame commit failed.");check(encoder->Commit(),"PNG commit failed.");
    }catch(...){context->Unmap(staging.Get(),0);throw;}
    context->Unmap(staging.Get(),0);
}
static std::filesystem::path dialog(HWND hwnd,bool save,bool csv,bool checkpoint=false,bool workspace=false) {
    wchar_t buf[32768]{};if(save)wcscpy_s(buf,workspace?L"plots.nsworkspace":checkpoint?L"experiment.nscp":csv?L"experiment.csv":L"network.json");
    OPENFILENAMEW ofn{};ofn.lStructSize=sizeof(ofn);ofn.hwndOwner=hwnd;ofn.lpstrFile=buf;ofn.nMaxFile=32768;
    ofn.lpstrFilter=workspace?L"Network Studio plot workspace\0*.nsworkspace\0\0":checkpoint?L"Network Studio checkpoint\0*.nscp\0\0":csv?L"CSV results\0*.csv\0\0":L"Network configuration\0*.json\0\0";
    ofn.lpstrDefExt=workspace?L"nsworkspace":checkpoint?L"nscp":csv?L"csv":L"json";ofn.Flags=OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|(save?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
    if(save?GetSaveFileNameW(&ofn):GetOpenFileNameW(&ofn))return buf;return {};
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int) {
    int argc=0;wchar_t** args=CommandLineToArgvW(GetCommandLineW(),&argc);
    std::filesystem::path smoke,config_path,checkpoint_path,workspace_path;int smoke_preset=0;bool smoke_wizard=false,viewport_smoke=false,plot_demo=false,field_smoke=false,experiment_smoke=false;
    for(int i=1;i<argc;++i) {
        if(std::wstring(args[i])==L"--smoke"&&i+1<argc)smoke=args[++i];
        else if(std::wstring(args[i])==L"--experiment-smoke"&&i+1<argc){smoke=args[++i];experiment_smoke=true;}
        else if(std::wstring(args[i])==L"--field-smoke"&&i+1<argc){smoke=args[++i];field_smoke=true;}
        else if(std::wstring(args[i])==L"--viewport-smoke"&&i+1<argc){smoke=args[++i];viewport_smoke=true;}
        else if(std::wstring(args[i])==L"--workspace"&&i+1<argc)workspace_path=args[++i];
        else if(std::wstring(args[i])==L"--plot-demo")plot_demo=true;
        else if(std::wstring(args[i])==L"--preset"&&i+1<argc)smoke_preset=_wtoi(args[++i]);
        else if(std::wstring(args[i])==L"--smoke-wizard")smoke_wizard=true;
        else if(std::wstring(args[i])==L"--resume"&&i+1<argc)checkpoint_path=args[++i];
        else config_path=args[i];
    }LocalFree(args);
    HWND hwnd=nullptr;bool imgui=false,backends=false;int result=0;
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    WNDCLASSEXW wc{sizeof(wc),CS_CLASSDC,wndproc,0,0,instance,nullptr,LoadCursor(nullptr,IDC_ARROW),nullptr,nullptr,L"NeuroAstroStudio",nullptr};
    RegisterClassExW(&wc);
    try {
        ImGui_ImplWin32_EnableDpiAwareness();
        hwnd=CreateWindowW(wc.lpszClassName,L"Neuro / Astro — Network Studio",WS_OVERLAPPEDWINDOW,70,40,(field_smoke||experiment_smoke)?1800:1480,(field_smoke||experiment_smoke)?1300:1000,nullptr,nullptr,instance,nullptr);
        if(!hwnd)throw std::runtime_error("Cannot create the application window.");create_device(hwnd);resize_w=resize_h=0;
        IMGUI_CHECKVERSION();ImGui::CreateContext();ImPlot::CreateContext();imgui=true;
        auto& io=ImGui::GetIO();io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard|ImGuiConfigFlags_DockingEnable|ImGuiConfigFlags_ViewportsEnable|
            ImGuiConfigFlags_DpiEnableScaleViewports|ImGuiConfigFlags_DpiEnableScaleFonts;
        io.ConfigViewportsNoAutoMerge=true;io.ConfigViewportsNoDefaultParent=true;
        io.ConfigViewportsNoTaskBarIcon=false;io.IniFilename=nullptr; // ini is embedded in the separate, atomic workspace document
        wchar_t windir[MAX_PATH]{};GetWindowsDirectoryW(windir,MAX_PATH);
        auto font=std::filesystem::path(windir)/L"Fonts"/L"segoeui.ttf";
        // Segoe UI supplies Cyrillic; the vendored monospace font is the fallback.
        std::string font_utf8;auto u8=font.u8string();font_utf8.assign(u8.begin(),u8.end());
        ImFontGlyphRangesBuilder glyph_builder;ImVector<ImWchar> glyph_ranges;
        const ImWchar symbols[]={0x0370,0x03ff,0x2000,0x209f,0x2190,0x22ff,0x25a0,0x25ff,0};
        glyph_builder.AddRanges(io.Fonts->GetGlyphRangesCyrillic());glyph_builder.AddRanges(symbols);glyph_builder.BuildRanges(&glyph_ranges);
        if(!io.Fonts->AddFontFromFileTTF(font_utf8.c_str(),17,nullptr,glyph_ranges.Data))
            io.Fonts->AddFontDefault();
        studio::apply_style();
        if(!ImGui_ImplWin32_Init(hwnd)||!ImGui_ImplDX11_Init(device.Get(),context.Get()))throw std::runtime_error("ImGui backend initialization failed.");backends=true;
        backend_create_window=ImGui::GetPlatformIO().Platform_CreateWindow;ImGui::GetPlatformIO().Platform_CreateWindow=create_platform_window;
        studio::App app;app.file_dialog=[&](bool save,bool csv){return dialog(hwnd,save,csv);};
        app.checkpoint_dialog=[&](bool save){return dialog(hwnd,save,false,true);};
        app.workspace_dialog=[&](bool save){return dialog(hwnd,save,false,false,true);};
        // A command-line configuration must fail explicitly rather than silently
        // running the default experiment when reading the requested file fails.
        if(!config_path.empty())app.replace(studio::load_config(config_path));
        if(!checkpoint_path.empty()) {
            // Validate before changing the GUI; startup failures are explicit.
            if(!app.restore_state(checkpoint_path))throw std::runtime_error(app.status);
        }
        if(!smoke.empty()) {
            std::filesystem::create_directories(smoke);
            if(config_path.empty()&&checkpoint_path.empty()) {
                auto c=studio::preset(viewport_smoke?6:smoke_preset);if(viewport_smoke)c.duration=.3;app.replace(c);
            }
            if(experiment_smoke&&!app.config.stages.empty())app.simulation->enter_stage(0,true);
            while(!app.simulation->done())if(!app.simulation->step())break;
            if(!app.simulation->error.empty())throw std::runtime_error(app.simulation->error);
            app.status="Проверка прототипа: расчёт завершён, графики построены.";
        }else {ShowWindow(hwnd,SW_SHOWDEFAULT);UpdateWindow(hwnd);}
        if(workspace_path.empty()&&smoke.empty()) {
            wchar_t local[32768]{};if(GetEnvironmentVariableW(L"LOCALAPPDATA",local,32768))workspace_path=std::filesystem::path(local)/L"NetworkStudio"/L"plots.nsworkspace";
        }
        if(!workspace_path.empty()) {
            if(std::filesystem::exists(workspace_path)&&!app.plot_windows.load(workspace_path))app.plot_windows.automatic_save_allowed=false;
            app.plot_windows.workspace_path=workspace_path;
            if(!app.plot_windows.automatic_save_allowed)app.status=app.plot_windows.message;
        }
        if(plot_demo)app.plot_windows.demo();
        if(field_smoke){auto& v=app.plot_windows.topology;v.field=true;v.columns=8;v.rows=6;v.scale=app.config.neurons.size()<32?.08:.002;v.edges=app.config.neurons.size()<32;}
        if(viewport_smoke&&app.plot_windows.windows.empty())for(auto kind:{studio::PlotKind::Voltage,studio::PlotKind::Neuron,studio::PlotKind::Raster,studio::PlotKind::Synapse}) {
            auto id=app.plot_windows.create(kind);auto* w=app.plot_windows.find(id);if(kind==studio::PlotKind::Synapse)w->view=1;
            w->size={680,440};w->position={80.f+float(id%2)*700,60.f+float((id-1)/2)*460};
        }
        bool done=false;int frames=0;int minimum_vertices=100000000;
        double field_frame_ms=0;
        int native_checks=0;double minimized_time=0;std::vector<HWND> native_handles;
        auto verify=[&](bool value,const char* reason){if(!value)throw std::runtime_error(reason);++native_checks;};
        while(!done) {
            MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);if(msg.message==WM_QUIT)done=true;}
            if(done)break;
            if(resize_w&&resize_h){target.Reset();check(swapchain->ResizeBuffers(0,resize_w,resize_h,DXGI_FORMAT_UNKNOWN,0),"Window resize failed.");resize_w=resize_h=0;render_target();}
            // A minimized/occluded main swapchain must not suspend other HWNDs or simulation.
            if(field_smoke&&frames==16)io.FontGlobalScale=1.5f;
            if(field_smoke&&frames==24&&app.config.neurons.size()>32)app.plot_windows.topology.scale=.08; // Exercise visible saturation markers.
            ImGui_ImplDX11_NewFrame();ImGui_ImplWin32_NewFrame();ImGui::NewFrame();
            if(!smoke.empty()&&!viewport_smoke) {
                if(smoke_wizard && frames==0)app.request_network_wizard=true;
                int group=frames/4;app.request_plot_tab=group;
                if(experiment_smoke&&frames==32)app.show_stages=true;
                if(experiment_smoke&&frames==36) {
                    app.show_stages=false;studio::NetworkGeneration g;g.topology=studio::NetworkTopology::WithinRadius;g.space={10,10,"model_unit"};g.radius=2;
                    g.excitatory.in_degree=g.inhibitory.in_degree={30,3};g.duration=100;g.sample=.002;g.synapse.stdp=true;g.synapse.stdp_rule=studio::StdpRule::Multiplicative;
                    app.preview_network(g);app.request_network_wizard=true;app.request_wizard_tab=1;
                }
                app.selected_kind=(group==0||group==5||app.config.astrocytes.empty())?0:(group==3?2:1);
                if(group==3&&!app.config.synapses.empty())app.selected_kind=2;
                app.selected_index=0;
            }
            const auto frame_begin=std::chrono::steady_clock::now();
            app.draw();ImGui::Render();auto* data=ImGui::GetDrawData();minimum_vertices=std::min(minimum_vertices,data->TotalVtxCount);
            if(field_smoke&&frames>=4)field_frame_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-frame_begin).count();
            if(!IsIconic(hwnd)) {
                const float clear[]={.055f,.073f,.1f,1};ID3D11RenderTargetView* rtv=target.Get();context->OMSetRenderTargets(1,&rtv,nullptr);context->ClearRenderTargetView(rtv,clear);
                ImGui_ImplDX11_RenderDrawData(data);
                if(!smoke.empty()&&!viewport_smoke&&frames%4==3)capture_png(smoke/(L"tab_"+std::to_wstring(frames/4)+L".png"));
            }
            ImGui::UpdatePlatformWindows();ImGui::RenderPlatformWindowsDefault();
            if(experiment_smoke&&(frames==35||frames==39)) {
                auto& platform=ImGui::GetPlatformIO();
                for(int i=1;i<platform.Viewports.Size;++i){auto* v=platform.Viewports[i];if(!v->DrawData||v->DrawData->TotalVtxCount<100)continue;
                    platform.Renderer_RenderWindow(v,nullptr);ComPtr<ID3D11RenderTargetView> view;context->OMGetRenderTargets(1,&view,nullptr);
                    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);ComPtr<ID3D11Texture2D> texture;check(resource.As(&texture),"Experiment viewport capture failed");
                    capture_png(smoke/((frames==35?L"stages_":L"generator_")+std::to_wstring(i)+L".png"),texture.Get());}
            }
            if(viewport_smoke) {
                auto& platform=ImGui::GetPlatformIO();
                if(frames==4) {
                    verify(platform.Viewports.Size>=5,"Multi-viewport did not create four independent windows.");
                    for(int i=1;i<platform.Viewports.Size;++i) {
                        auto* v=platform.Viewports[i];auto handle=static_cast<HWND>(v->PlatformHandle);native_handles.push_back(handle);
                        verify(IsWindow(handle)&&IsWindowVisible(handle),"Secondary HWND is not visible.");
                        verify(GetWindow(handle,GW_OWNER)==nullptr,"Secondary HWND is owned by main window and would minimize with it.");
                        verify(v->RendererUserData&&v->DrawData&&v->DrawData->TotalVtxCount>100,"Secondary DX11 draw data/resources missing.");
                        platform.Renderer_RenderWindow(v,nullptr);
                        ComPtr<ID3D11RenderTargetView> view;context->OMGetRenderTargets(1,&view,nullptr);ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
                        ComPtr<ID3D11Texture2D> texture;check(resource.As(&texture),"Viewport capture texture failed.");capture_png(smoke/(L"window_"+std::to_wstring(i)+L".png"),texture.Get());
                    }
                    app.simulation->extend_duration(.5);app.config.duration=.5;minimized_time=app.simulation->time;app.running=true;
                    ShowWindow(hwnd,SW_MINIMIZE);
                }
                if(frames==7) {
                    verify(IsIconic(hwnd)!=FALSE,"Main window did not minimize.");
                    for(auto handle:native_handles)verify(IsWindowVisible(handle)&&!IsIconic(handle),"Detached plot stopped being visible with minimized main window.");
                    verify(app.simulation->time>minimized_time,"Simulation stopped while main window was minimized.");app.running=false;
                    SetWindowPos(native_handles[0],nullptr,100,100,620,410,SWP_NOZORDER);
                    platform.Platform_SetWindowFocus(ImGui::FindViewportByPlatformHandle(native_handles[0]));
                }
                if(frames==10) {
                    RECT rect{};GetWindowRect(native_handles[0],&rect);verify(rect.right-rect.left==620,"Native plot resize was not applied.");
                    auto* first=ImGui::FindWindowByName(app.plot_windows.windows[0].window_name().c_str());
                    verify(first&&first->Viewport->Size.x>500,"Resized platform viewport was not synchronized.");
                    verify(GetFocus()==native_handles[0],"Detached window did not acquire native keyboard focus.");
                    // Exercise the real backend's DPI-change message path (physical second DPI monitor is optional).
                    rect.right=rect.left+640;SendMessageW(native_handles[0],WM_DPICHANGED,MAKELONG(144,144),reinterpret_cast<LPARAM>(&rect));
                    RECT changed{};GetWindowRect(native_handles[0],&changed);verify(changed.right-changed.left==640,"DPI suggested rectangle was ignored.");
                    if(platform.Monitors.Size>1) {
                        const auto& monitor=platform.Monitors[1];
                        SetWindowPos(native_handles[1],nullptr,int(monitor.WorkPos.x+50),int(monitor.WorkPos.y+50),0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
                        verify(MonitorFromWindow(native_handles[1],MONITOR_DEFAULTTONEAREST)!=MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),"Plot did not move to the second monitor.");
                    }
                    ShowWindow(hwnd,SW_RESTORE);
                    verify(app.plot_windows.save(smoke/L"plots.nsworkspace"),"Native workspace save failed.");
                    app.plot_windows.demo();
                }
                if(frames==15) {
                    int docked=0;for(const auto& w:app.plot_windows.windows){auto* iw=ImGui::FindWindowByName(w.window_name().c_str());if(iw&&iw->DockId)++docked;}
                    verify(docked>=4,"Four-panel docking did not produce docked windows.");
                    auto* host=ImGui::FindWindowByName("Рабочая область графиков###studio_plot_host");
                    verify(host&&host->Viewport->RendererUserData,"Dock host has no native DX11 surface.");
                    platform.Renderer_RenderWindow(host->Viewport,nullptr);
                    ComPtr<ID3D11RenderTargetView> view;context->OMGetRenderTargets(1,&view,nullptr);ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
                    ComPtr<ID3D11Texture2D> texture;check(resource.As(&texture),"Dock capture texture failed.");capture_png(smoke/L"four_plots.png",texture.Get());
                    const auto& first=app.plot_windows.windows[app.plot_windows.windows.size()-4];
                    auto* first_window=ImGui::FindWindowByName(first.window_name().c_str());
                    ImGui::DockBuilderDockWindow(app.plot_windows.windows.back().window_name().c_str(),first_window->DockId);
                    ImGui::DockBuilderFinish(ImHashStr("studio_plot_dock"));
                }
                if(frames==17) {
                    const auto& first=app.plot_windows.windows[app.plot_windows.windows.size()-4];
                    auto* a=ImGui::FindWindowByName(first.window_name().c_str());auto* b=ImGui::FindWindowByName(app.plot_windows.windows.back().window_name().c_str());
                    verify(a->DockId!=0&&a->DockId==b->DockId&&a->DockNode->Windows.Size>=2,"Docking as tabs failed.");
                    verify(app.plot_windows.load(smoke/L"plots.nsworkspace"),"Native workspace restore failed.");
                }
                if(frames==19) {
                    verify(app.plot_windows.windows.size()==4,"Workspace restore lost window collection.");
                    auto* iw=ImGui::FindWindowByName(app.plot_windows.windows[0].window_name().c_str());
                    PostMessageW(static_cast<HWND>(iw->Viewport->PlatformHandle),WM_CLOSE,0,0);
                }
                if(frames==23)verify(app.plot_windows.windows.size()==3,"Native WM_CLOSE did not remove the graph.");
            }
            if(!IsIconic(hwnd)) {HRESULT hr=swapchain->Present(smoke.empty()?1:0,0);if(FAILED(hr))throw std::runtime_error("Direct3D presentation failed.");if(hr==DXGI_STATUS_OCCLUDED)Sleep(10);}
            else Sleep(10);
            ++frames;if(!smoke.empty()&&frames>=(experiment_smoke?40:28))done=true;
        }
        if(smoke.empty()&&app.plot_windows.automatic_save_allowed&&!app.plot_windows.workspace_path.empty()) {
            const auto parent=app.plot_windows.workspace_path.parent_path();
            if(!parent.empty())std::filesystem::create_directories(parent);
            if(!app.plot_windows.save(app.plot_windows.workspace_path))MessageBoxA(hwnd,app.plot_windows.message.c_str(),"Workspace",MB_OK|MB_ICONWARNING);
        }
        if(!smoke.empty()) {
            studio::export_results(*app.simulation,smoke/L"demo");
            std::ofstream log(smoke/L"renderer.txt");log<<"frames="<<frames<<"\nminimum_vertices="<<minimum_vertices<<"\nsamples="<<app.simulation->history.time.size()<<"\nerror="<<app.simulation->error<<"\n";
            log<<"native_viewport_checks="<<native_checks<<"\nmonitors="<<ImGui::GetPlatformIO().Monitors.Size<<"\n";
            if(field_smoke) {
                log<<"weight_field=1\nincluded_edges="<<app.weight_field.included_edges<<"\ngeometry_builds="<<app.weight_field.geometry_builds
                    <<"\nfield_geometry_ms="<<app.weight_field.geometry_ms<<"\nfield_update_ms="<<app.weight_field.update_ms
                    <<"\nimgui_frame_ms="<<field_frame_ms/std::max(1,frames-4)<<"\nfont_scales=1,1.5\n";
                if(!app.weight_field.available||app.weight_field.geometry_builds!=1)throw std::runtime_error("Field render did not reuse valid geometry.");
            }
            if(!viewport_smoke&&minimum_vertices<100)throw std::runtime_error("Renderer emitted too few vertices.");
            std::error_code ec;std::filesystem::remove(smoke/L"error.txt",ec);
        }
    }catch(const std::exception& e) {
        result=1;
        if(smoke.empty())MessageBoxA(hwnd,e.what(),"Network Studio",MB_OK|MB_ICONERROR);
        else {std::filesystem::create_directories(smoke);std::ofstream log(smoke/L"error.txt");log<<e.what();}
    }
    if(backends){ImGui::DestroyPlatformWindows();if(!platform_procedures.empty())result=1;ImGui_ImplDX11_Shutdown();ImGui_ImplWin32_Shutdown();}
    if(imgui){ImPlot::DestroyContext();ImGui::DestroyContext();}
    target.Reset();swapchain.Reset();context.Reset();device.Reset();
    if(hwnd)DestroyWindow(hwnd);UnregisterClassW(wc.lpszClassName,instance);CoUninitialize();return result;
}
