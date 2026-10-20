#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>
#include <shellapi.h>
#include <commdlg.h>
#include <wrl/client.h>
#include "gui.h"
#include "history_archive.h"
#include "imgui_internal.h"
#include "implot.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <unordered_map>
#include <chrono>

using Microsoft::WRL::ComPtr;
static ComPtr<ID3D11Device> device;
static ComPtr<ID3D11DeviceContext> context;
static ComPtr<IDXGISwapChain> swapchain;
static ComPtr<ID3D11RenderTargetView> target;
static UINT resize_w=0,resize_h=0;
static bool software_renderer=false;
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
    if(FAILED(hr)){software_renderer=true;hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,levels,2,D3D11_SDK_VERSION,&sd,&swapchain,&device,nullptr,&context);}
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
static std::filesystem::path dialog(HWND hwnd,bool save,bool csv,bool checkpoint=false,bool workspace=false,bool weights=false) {
    wchar_t buf[32768]{};if(save)wcscpy_s(buf,weights?L"weights.nsweights":workspace?L"plots.nsworkspace":checkpoint?L"experiment.nscp":csv?L"experiment.csv":L"network.json");
    OPENFILENAMEW ofn{};ofn.lStructSize=sizeof(ofn);ofn.hwndOwner=hwnd;ofn.lpstrFile=buf;ofn.nMaxFile=32768;
    ofn.lpstrFilter=weights?L"Weight snapshots / checkpoints / summary\0*.nsweights;*.nscp;*_summary.json\0\0":workspace?L"Network Studio plot workspace\0*.nsworkspace\0\0":checkpoint?L"Network Studio checkpoint\0*.nscp\0\0":csv?L"CSV results\0*.csv\0\0":L"Network configuration\0*.json\0\0";
    ofn.lpstrDefExt=weights?L"nsweights":workspace?L"nsworkspace":checkpoint?L"nscp":csv?L"csv":L"json";ofn.Flags=OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|(save?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
    if(save?GetSaveFileNameW(&ofn):GetOpenFileNameW(&ofn))return buf;return {};
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int) {
    int argc=0;wchar_t** args=CommandLineToArgvW(GetCommandLineW(),&argc);
    double display_hz=10;
    std::wstring compute_threads_option=L"1",benchmark_until_option;
    std::filesystem::path smoke,benchmark,runner_close,config_path,checkpoint_path,workspace_path;int smoke_preset=0;bool animat_smoke=false,weights_smoke=false,analysis_smoke=false,archive_smoke=false;bool smoke_wizard=false,viewport_smoke=false,plot_demo=false,field_smoke=false,experiment_smoke=false,stimulator_smoke=false;
    std::vector<std::filesystem::path> weight_smoke_inputs;
    for(int i=1;i<argc;++i) {
        if(std::wstring(args[i])==L"--smoke"&&i+1<argc)smoke=args[++i];
        else if(std::wstring(args[i])==L"--benchmark-until")benchmark_until_option=i+1<argc?args[++i]:L"invalid";
        else if(std::wstring(args[i])==L"--benchmark"&&i+1<argc)benchmark=args[++i];
        else if(std::wstring(args[i])==L"--runner-close-smoke"&&i+1<argc)runner_close=args[++i];
        else if(std::wstring(args[i])==L"--display-hz"&&i+1<argc)display_hz=_wtof(args[++i]);
        else if(std::wstring(args[i])==L"--compute-threads")compute_threads_option=i+1<argc?args[++i]:L"";
        else if(std::wstring(args[i])==L"--animat-smoke"&&i+1<argc){smoke=args[++i];animat_smoke=true;}
        else if(std::wstring(args[i])==L"--weights-smoke"&&i+1<argc){smoke=args[++i];weights_smoke=true;}
        else if(std::wstring(args[i])==L"--weights-smoke-input"&&i+1<argc)weight_smoke_inputs.emplace_back(args[++i]);
        else if(std::wstring(args[i])==L"--archive-smoke"&&i+1<argc){smoke=args[++i];archive_smoke=true;}
        else if(std::wstring(args[i])==L"--analysis-smoke"&&i+1<argc){smoke=args[++i];analysis_smoke=true;}
        else if(std::wstring(args[i])==L"--stimulator-smoke"&&i+1<argc){smoke=args[++i];stimulator_smoke=true;}
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
    bool close_requested=false,active_before_close=false;
    uint64_t close_steps=0;double close_model_time=0;
    int close_compute_threads=1,close_effective_threads=1;
    size_t close_neurons=0,close_synapses=0;
    std::chrono::steady_clock::time_point close_begin;
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    WNDCLASSEXW wc{sizeof(wc),CS_CLASSDC,wndproc,0,0,instance,nullptr,LoadCursor(nullptr,IDC_ARROW),nullptr,nullptr,L"NeuroAstroStudio",nullptr};
    RegisterClassExW(&wc);
    try {
        if(!benchmark_until_option.empty()&&benchmark.empty())throw std::runtime_error("--benchmark-until requires --benchmark.");
        const int compute_threads=studio::compute::parse(std::wstring_view(compute_threads_option),"--compute-threads");
        if(!runner_close.empty()) {
            std::filesystem::create_directories(runner_close);
            if(!smoke.empty()||!benchmark.empty()||!workspace_path.empty()||!config_path.empty()||!checkpoint_path.empty()||plot_demo||smoke_wizard)
                throw std::runtime_error("--runner-close-smoke requires its own fixed fixture and no other run mode.");
        }
        if(!benchmark.empty()) {
            std::filesystem::create_directories(benchmark);
            if(!smoke.empty()||!workspace_path.empty()||plot_demo||smoke_wizard)
                throw std::runtime_error("--benchmark cannot be combined with smoke, workspace or plot-demo options.");
            if(config_path.empty()&&checkpoint_path.empty())
                throw std::runtime_error("--benchmark requires an explicit configuration or checkpoint.");
        }
        ImGui_ImplWin32_EnableDpiAwareness();
        hwnd=CreateWindowW(wc.lpszClassName,L"Neuro / Astro — Network Studio",WS_OVERLAPPEDWINDOW,70,40,(field_smoke||experiment_smoke||stimulator_smoke||analysis_smoke)?1800:1480,(field_smoke||experiment_smoke||stimulator_smoke||analysis_smoke)?1300:1000,nullptr,nullptr,instance,nullptr);
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
        studio::App app;app.set_display_hz(display_hz);app.set_compute_threads(compute_threads,true);app.file_dialog=[&](bool save,bool csv){return dialog(hwnd,save,csv);};
        app.checkpoint_dialog=[&](bool save){return dialog(hwnd,save,false,true);};
        app.weight_dialog=[&](bool save){return dialog(hwnd,save,false,false,false,true);};
        app.workspace_dialog=[&](bool save){return dialog(hwnd,save,false,false,true);};
        if(!runner_close.empty()) {
            auto c=studio::preset(6);c.name="Native close while computing";c.duration=1000;c.sample=.01;
            if(compute_threads>1) {
                // The small default fixture deliberately exercises serial
                // fallback. Explicit parallel close tests need real helpers.
                studio::NetworkGeneration g;g.name="Native close with parallel helpers";
                g.seed=706;g.excitatory.count=102;g.inhibitory.count=26;
                g.excitatory.in_degree=g.inhibitory.in_degree={32,0};
                g.excitatory.weight=g.inhibitory.weight={.1,0};
                g.synapse.stdp=true;g.synapse.stdp_rule=studio::StdpRule::Multiplicative;
                g.duration=1000;g.sample=.01;
                c=studio::generate_network(studio::Config{},g,false);
                c.noise_sigma=2;c.noise_interval=.001;
                for(auto& n:c.neurons)n.current=8;
            }
            app.replace(std::move(c));
        }
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
                auto c=studio::preset((viewport_smoke||weights_smoke)?6:smoke_preset);if(viewport_smoke||analysis_smoke||archive_smoke||weights_smoke)c.duration=.3;if(archive_smoke){c.history_capacity=32;c.bounded_journals=true;}app.replace(c);
            }
            if(experiment_smoke&&!app.config.stages.empty()){studio::RunnerCommand c;c.kind=studio::RunnerCommand::Kind::EnterStage;c.stages=app.config.stages;c.index=0;c.flag=true;app.command(std::move(c),true);}
            if(archive_smoke){studio::RunnerCommand c;c.kind=studio::RunnerCommand::Kind::StartArchive;c.path=smoke/L"history";c.index=32;c.flag=true;auto r=app.wait_command(app.command(c));if(!r.ok)throw std::runtime_error(r.error);}
            if(weights_smoke&&weight_smoke_inputs.empty())app.add_weight_snapshot(studio::initial_weights(app.config));
            if(!weights_smoke||weight_smoke_inputs.empty())app.advance_for(app.config.duration);
            if(weights_smoke){
                if(weight_smoke_inputs.empty()){app.capture_weight_snapshot();app.synchronize();}
                else for(const auto& p:weight_smoke_inputs)app.import_weight_snapshot(p);
                app.show_weights=true;app.weights_include_live=false;app.weight_reference=0;app.weight_target=app.weight_snapshots.size()>1?1:0;
                app.weight_message="Native Win32/DX11: full weights, shared bins, delta";
                std::vector<const studio::WeightSnapshot*> views;for(const auto& s:app.weight_snapshots)views.push_back(s.get());
                studio::export_weights(views,app.weight_settings,app.weight_reference,app.weight_target,smoke/L"weights");
            }
            if(archive_smoke){app.show_archive=true;auto path=(smoke/L"history").u8string();std::snprintf(app.archive_path.data(),app.archive_path.size(),"%s",path.c_str());app.archive_interval={0,.3,false};app.archive_profile={"Native archive",app.config.analysis};app.archive_result=studio::analyze_archive(smoke/L"history",app.config.analysis,app.archive_interval);app.archive_message="Native Win32/DX11 archive analysis";studio::export_analysis(app.archive_result,app.config.analysis,smoke/L"disk_analysis","Native archive");}
            if(!app.simulation->error.empty())throw std::runtime_error(app.simulation->error);
            app.status="Проверка прототипа: расчёт завершён, графики построены.";
        }else if(benchmark.empty()&&runner_close.empty()) {ShowWindow(hwnd,SW_SHOWDEFAULT);UpdateWindow(hwnd);}
        if(!benchmark.empty()) {
            // Native GUI consumes snapshots while the controller runs independently.
            // The hidden HWND and Present(1) match the previous-stage benchmark.
        }
        if(workspace_path.empty()&&smoke.empty()&&benchmark.empty()&&runner_close.empty()) {
            wchar_t local[32768]{};if(GetEnvironmentVariableW(L"LOCALAPPDATA",local,32768))workspace_path=std::filesystem::path(local)/L"NetworkStudio"/L"plots.nsworkspace";
        }
        if(!workspace_path.empty()) {
            if(std::filesystem::exists(workspace_path)&&!app.plot_windows.load(workspace_path))app.plot_windows.automatic_save_allowed=false;
            app.plot_windows.workspace_path=workspace_path;
            if(!app.plot_windows.automatic_save_allowed)app.status=app.plot_windows.message;
        }
        if(plot_demo)app.plot_windows.demo();
        if(animat_smoke)app.show_animat=true;
        if(analysis_smoke){auto& w=app.plot_windows.embedded[7];w.analysis_interval={.05,.25,false};app.plot_windows.create_analysis_profile(w,"Native analysis profile");app.plot_windows.create(studio::PlotKind::Network);app.plot_windows.apply_analysis_to_all(w);}
        if(field_smoke){auto& v=app.plot_windows.topology;v.field=true;v.columns=8;v.rows=6;v.scale=app.config.neurons.size()<32?.08:.002;v.edges=app.config.neurons.size()<32;}
        if(stimulator_smoke){auto& v=app.plot_windows.topology;v.field=v.activity=true;v.columns=8;v.rows=6;v.scale=v.activity_scale=.08;v.activity_window=.1;app.selected_stimulator=app.config.stimulators.empty()?0:app.config.stimulators[0].id;}
        if(viewport_smoke&&app.plot_windows.windows.empty())for(auto kind:{studio::PlotKind::Voltage,studio::PlotKind::Neuron,studio::PlotKind::Raster,studio::PlotKind::Synapse}) {
            auto id=app.plot_windows.create(kind);auto* w=app.plot_windows.find(id);if(kind==studio::PlotKind::Synapse)w->view=1;
            w->size={680,440};w->position={80.f+float(id%2)*700,60.f+float((id-1)/2)*460};
        }
        bool done=false;int frames=0;int minimum_vertices=100000000;
        double field_frame_ms=0;
        int native_checks=0;double minimized_time=0;std::vector<HWND> native_handles;
        auto verify=[&](bool value,const char* reason){if(!value)throw std::runtime_error(reason);++native_checks;};
        double benchmark_draw=0,benchmark_render=0,benchmark_present=0,benchmark_idle=0;
        uint64_t benchmark_occluded=0,benchmark_presented=0,benchmark_other_status=0;
        const double benchmark_start_time=app.simulation->time;
        double benchmark_target=app.config.duration;
        if(!benchmark_until_option.empty()) {
            size_t used=0;benchmark_target=std::stod(benchmark_until_option,&used);
            if(used!=benchmark_until_option.size()||!std::isfinite(benchmark_target)||benchmark_target<=benchmark_start_time||benchmark_target>app.config.duration)
                throw std::runtime_error("--benchmark-until must be an absolute time after the current time and within the configured duration.");
        }

        const auto benchmark_begin=std::chrono::steady_clock::now();
        if(!benchmark.empty()){studio::RunnerCommand c;c.kind=studio::RunnerCommand::Kind::RunTo;c.value=benchmark_target;app.command(std::move(c));}
        else if(!runner_close.empty())app.run();
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
                int group=frames/4;app.request_plot_tab=analysis_smoke?7:group;
                if(stimulator_smoke&&frames==16)app.show_stimulators=true;
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
            if(animat_smoke){app.request_animat_tab=std::min(3,frames/7);if(frames==20){studio::RunnerCommand c;c.kind=studio::RunnerCommand::Kind::AnalyzeAnimat;c.value=0;c.second=app.simulation->time;auto result=app.wait_command(app.command(c));if(!result.ok)throw std::runtime_error(result.error);app.animat_result=*result.animat_analysis;app.animat_to=app.simulation->time;}}
            if(weights_smoke&&frames==25)if(auto* w=ImGui::FindWindowByName("Веса всей сети###weight_analysis"))ImGui::SetScrollY(w,w->ScrollMax.y);
            app.draw();
            const auto benchmark_render_begin=benchmark.empty()?frame_begin:std::chrono::steady_clock::now();
            if(!benchmark.empty())benchmark_draw+=std::chrono::duration<double>(benchmark_render_begin-frame_begin).count();
            ImGui::Render();auto* data=ImGui::GetDrawData();minimum_vertices=std::min(minimum_vertices,data->TotalVtxCount);
            if(field_smoke&&frames>=4)field_frame_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-frame_begin).count();
            if(!IsIconic(hwnd)) {
                const float clear[]={.055f,.073f,.1f,1};ID3D11RenderTargetView* rtv=target.Get();context->OMSetRenderTargets(1,&rtv,nullptr);context->ClearRenderTargetView(rtv,clear);
                ImGui_ImplDX11_RenderDrawData(data);
                if(!smoke.empty()&&!viewport_smoke&&frames%4==3)capture_png(smoke/(L"tab_"+std::to_wstring(frames/4)+L".png"));
            }
            ImGui::UpdatePlatformWindows();ImGui::RenderPlatformWindowsDefault();
            if(!benchmark.empty())benchmark_render+=std::chrono::duration<double>(std::chrono::steady_clock::now()-benchmark_render_begin).count();
            if((animat_smoke&&(frames==3||frames==11||frames==19||frames==27))||(experiment_smoke&&(frames==35||frames==39))||(stimulator_smoke&&frames==23)||(analysis_smoke&&frames==23)||(archive_smoke&&frames==23)||(weights_smoke&&(frames==23||frames==27))) {
                auto& platform=ImGui::GetPlatformIO();
                if(animat_smoke){auto* w=ImGui::FindWindowByName("Нейроанимат###neuroanimat");verify(w&&w->Viewport&&w->Viewport->PlatformHandleRaw&&IsWindow(static_cast<HWND>(w->Viewport->PlatformHandleRaw)),"Animat native window missing");verify(w->Viewport->DrawData&&w->Viewport->DrawData->TotalVtxCount>100,"Animat native draw data missing");}
                for(int i=1;i<platform.Viewports.Size;++i){auto* v=platform.Viewports[i];if(!v->DrawData||v->DrawData->TotalVtxCount<100)continue;
                    platform.Renderer_RenderWindow(v,nullptr);ComPtr<ID3D11RenderTargetView> view;context->OMGetRenderTargets(1,&view,nullptr);
                    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);ComPtr<ID3D11Texture2D> texture;check(resource.As(&texture),"Experiment viewport capture failed");
                    capture_png(smoke/((animat_smoke?(L"animat_"+std::to_wstring(frames)+L"_"):weights_smoke?(frames==27?L"weights_details_":L"weights_"):archive_smoke?L"archive_":analysis_smoke?L"analysis_":stimulator_smoke?L"stimulators_":frames==35?L"stages_":L"generator_")+std::to_wstring(i)+L".png"),texture.Get());}
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
                    studio::RunnerCommand c;c.kind=studio::RunnerCommand::Kind::Extend;c.value=.5;app.command(std::move(c),true);app.synchronize();minimized_time=app.simulation->time;app.run();
                    ShowWindow(hwnd,SW_MINIMIZE);
                }
                if(frames==7) {
                    verify(IsIconic(hwnd)!=FALSE,"Main window did not minimize.");
                    for(auto handle:native_handles)verify(IsWindowVisible(handle)&&!IsIconic(handle),"Detached plot stopped being visible with minimized main window.");
                    // Check acknowledged progress instead of assuming publication at a frame count.
                    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
                    while(app.simulation->time<=minimized_time&&std::chrono::steady_clock::now()<deadline){app.advance();Sleep(1);}
                    app.pause();app.synchronize();
                    verify(app.simulation->time>minimized_time,"Simulation stopped while main window was minimized.");
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
            if(!IsIconic(hwnd)) {
                const auto present_begin=benchmark.empty()?benchmark_begin:std::chrono::steady_clock::now();
                HRESULT hr=swapchain->Present(smoke.empty()?1:0,0);
                if(!benchmark.empty()) {
                    benchmark_present+=std::chrono::duration<double>(std::chrono::steady_clock::now()-present_begin).count();
                    if(hr==DXGI_STATUS_OCCLUDED)++benchmark_occluded;
                    else if(hr==S_OK)++benchmark_presented;
                    else ++benchmark_other_status;
                }
                if(FAILED(hr))throw std::runtime_error("Direct3D presentation failed.");
                if(hr==DXGI_STATUS_OCCLUDED) {
                    const auto idle_begin=benchmark.empty()?benchmark_begin:std::chrono::steady_clock::now();Sleep(10);
                    if(!benchmark.empty())benchmark_idle+=std::chrono::duration<double>(std::chrono::steady_clock::now()-idle_begin).count();
                }
            }
            else Sleep(10);
            ++frames;if(!smoke.empty()&&frames>=(experiment_smoke?40:28))done=true;
            if(!runner_close.empty()&&!close_requested) {
                if(!app.simulation->error.empty())throw std::runtime_error(app.simulation->error);
                if(app.simulation->metrics.running&&app.simulation->steps>0&&!app.simulation->done()) {
                    close_compute_threads=app.simulation->metrics.compute_threads;
                    close_effective_threads=app.simulation->metrics.effective_compute_threads;
                    close_neurons=app.simulation->config.neurons.size();close_synapses=app.simulation->config.synapses.size();
                    if(close_compute_threads!=compute_threads||(compute_threads>1&&close_effective_threads!=compute_threads))
                        throw std::runtime_error("Native parallel close fixture did not activate the requested helper threads.");
                    active_before_close=true;close_steps=app.simulation->steps;close_model_time=app.simulation->time;
                    close_begin=std::chrono::steady_clock::now();
                    if(!PostMessageW(hwnd,WM_CLOSE,0,0))throw std::runtime_error("Could not post native close request.");
                    close_requested=true;
                } else if(std::chrono::steady_clock::now()-benchmark_begin>std::chrono::seconds(10))
                    throw std::runtime_error("Native close fixture did not publish active computation within 10 seconds.");
            }
            if(!benchmark.empty()) {
                if(!app.simulation->error.empty())throw std::runtime_error(app.simulation->error);
                if(app.simulation->time>=benchmark_target&&!app.simulation->metrics.running)done=true;
                else if(!app.running)throw std::runtime_error("GUI benchmark stopped before completion.");
            }
        }
        const double benchmark_wall=std::chrono::duration<double>(std::chrono::steady_clock::now()-benchmark_begin).count();
        const auto benchmark_metrics=app.simulation->metrics;
        if(!benchmark.empty()){studio::RunnerCommand c;c.kind=studio::RunnerCommand::Kind::WriteProfile;c.path=benchmark/L"profile.json";app.command(std::move(c),true);app.synchronize();}
        if(!benchmark.empty()) {
            if(app.simulation->time<benchmark_target||app.simulation->metrics.running)throw std::runtime_error("GUI benchmark window closed before the requested safe boundary.");
            app.export_data(benchmark/L"result");
            app.save_state(benchmark/L"final.nscp");
            std::ofstream log(benchmark/L"benchmark.json");
            log<<std::setprecision(17)<<"{\n  \"mode\": \"hidden_win32_dx11_runner\",\n  \"hidden\": true,\n"
                <<"  \"renderer\": \""<<(software_renderer?"WARP":"hardware")<<"\",\n"
                <<"  \"wall_seconds\": "<<benchmark_wall<<",\n  \"app_draw_snapshot_seconds\": "<<benchmark_draw
                <<",\n  \"render_cpu_seconds\": "<<benchmark_render<<",\n  \"present_seconds\": "<<benchmark_present
                <<",\n  \"occlusion_sleep_seconds\": "<<benchmark_idle<<",\n  \"frames\": "<<frames
                <<",\n  \"present_ok\": "<<benchmark_presented<<",\n  \"present_occluded\": "<<benchmark_occluded
                <<",\n  \"present_other_status\": "<<benchmark_other_status<<",\n  \"minimum_vertices\": "<<minimum_vertices
                <<",\n  \"model_start_seconds\": "<<benchmark_start_time<<",\n  \"model_final_seconds\": "<<app.simulation->time
                <<",\n  \"steps\": "<<app.simulation->steps<<",\n  \"samples\": "<<app.simulation->history.time.size()
                <<",\n  \"worker_elapsed_seconds\": "<<benchmark_metrics.elapsed_seconds
                <<",\n  \"worker_compute_seconds\": "<<benchmark_metrics.compute_seconds
                <<",\n  \"worker_publish_seconds\": "<<benchmark_metrics.publish_seconds
                <<",\n  \"worker_publications\": "<<benchmark_metrics.publish_count
                <<",\n  \"display_hz\": "<<display_hz
                <<",\n  \"compute_threads\": "<<benchmark_metrics.compute_threads
                <<",\n  \"effective_compute_threads\": "<<benchmark_metrics.effective_compute_threads
                <<",\n  \"limits\": \"Hidden native window; independent solver controller with configured helper threads, Present(1). Snapshot construction and GUI run concurrently. Not visible GUI or GPU execution timing. Initialization, export and checkpoint are outside wall_seconds. App draw excludes solver work.\"\n}\n";
            if(!log)throw std::runtime_error("Cannot write GUI benchmark metrics.");
            std::error_code ec;std::filesystem::remove(benchmark/L"error.txt",ec);
        }
        if(app.simulation->metrics.archive_active){studio::RunnerCommand c;c.kind=studio::RunnerCommand::Kind::Pause;auto r=app.wait_command(app.command(c));if(!r.ok)throw std::runtime_error("Archive flush on close failed: "+r.error);}
        if(smoke.empty()&&benchmark.empty()&&runner_close.empty()&&app.plot_windows.automatic_save_allowed&&!app.plot_windows.workspace_path.empty()) {
            const auto parent=app.plot_windows.workspace_path.parent_path();
            if(!parent.empty())std::filesystem::create_directories(parent);
            if(!app.plot_windows.save(app.plot_windows.workspace_path))MessageBoxA(hwnd,app.plot_windows.message.c_str(),"Workspace",MB_OK|MB_ICONWARNING);
        }
        if(!smoke.empty()) {
            app.pause();app.synchronize();app.export_data(smoke/L"demo");
            std::ofstream log(smoke/L"renderer.txt");log<<"frames="<<frames<<"\nminimum_vertices="<<minimum_vertices<<"\nsamples="<<app.simulation->history.time.size()<<"\nerror="<<app.simulation->error<<"\n";
            log<<"native_viewport_checks="<<native_checks<<"\nmonitors="<<ImGui::GetPlatformIO().Monitors.Size<<"\n";
            if(animat_smoke)log<<"animat_win32_dx11=1\nanimat_rows="<<app.simulation->animat.rows.size()<<"\n";
            if(weights_smoke)log<<"weights_win32_dx11=1\nweight_snapshots="<<app.weight_snapshots.size()<<"\n";
            if(archive_smoke)log<<"archive_win32_dx11=1\ncommitted_until="<<app.simulation->metrics.disk_until<<"\nram_from="<<app.simulation->metrics.memory_samples_from<<"\n";
            if(stimulator_smoke)log<<"shared_sources="<<app.config.stimulators.size()<<"\nshared_pulses="<<app.simulation->history.shared_pulses.size()<<"\nactivity_maximum="<<app.activity_field.maximum<<"\nactivity_events_processed="<<app.activity_field.processed_events<<"\n";
            if(field_smoke) {
                log<<"weight_field=1\nincluded_edges="<<app.weight_field.included_edges<<"\ngeometry_builds="<<app.weight_field.geometry_builds
                    <<"\nfield_geometry_ms="<<app.weight_field.geometry_ms<<"\nfield_update_ms="<<app.weight_field.update_ms
                    <<"\nimgui_frame_ms="<<field_frame_ms/std::max(1,frames-4)<<"\nfont_scales=1,1.5\n";
                if(!app.weight_field.available||app.weight_field.geometry_builds!=1)throw std::runtime_error("Field render did not reuse valid geometry.");
            }
            if(!viewport_smoke&&minimum_vertices<100)throw std::runtime_error("Renderer emitted too few vertices.");
            std::error_code ec;std::filesystem::remove(smoke/L"error.txt",ec);
        }
        if(!runner_close.empty()&&(!close_requested||!active_before_close||IsWindow(hwnd)))
            throw std::runtime_error("Native close did not destroy the active main window.");
        // On this path App is destroyed while the controller is still active.
        // Do not pause, synchronize, or export before its shutdown/join.
    }catch(const std::exception& e) {
        result=1;
        if(!runner_close.empty()){std::filesystem::create_directories(runner_close);std::ofstream log(runner_close/L"error.txt");log<<e.what();}
        else if(!benchmark.empty()){std::filesystem::create_directories(benchmark);std::ofstream log(benchmark/L"error.txt");log<<e.what();}
        else if(smoke.empty())MessageBoxA(hwnd,e.what(),"Network Studio",MB_OK|MB_ICONERROR);
        else {std::filesystem::create_directories(smoke);std::ofstream log(smoke/L"error.txt");log<<e.what();}
    }
    if(backends){ImGui::DestroyPlatformWindows();if(!platform_procedures.empty())result=1;ImGui_ImplDX11_Shutdown();ImGui_ImplWin32_Shutdown();}
    if(imgui){ImPlot::DestroyContext();ImGui::DestroyContext();}
    target.Reset();swapchain.Reset();context.Reset();device.Reset();
    if(!runner_close.empty()&&result==0) {
        // Reached only after App::~App() returned from SimulationRunner::shutdown
        // and GUI/Direct3D resources were released; a stuck join produces no file.
        std::ofstream log(runner_close/L"close_result.json");
        log<<std::setprecision(17)<<"{\n  \"active_before_close\": true,\n  \"wm_close_posted\": true,\n  \"joined\": true,\n"
            <<"  \"resources_released\": true,\n  \"steps_before_close\": "<<close_steps<<",\n  \"model_time_before_close\": "<<close_model_time
            <<",\n  \"compute_threads\": "<<close_compute_threads<<",\n  \"effective_compute_threads\": "<<close_effective_threads
            <<",\n  \"neurons\": "<<close_neurons<<",\n  \"synapses\": "<<close_synapses
            <<",\n  \"close_seconds\": "<<std::chrono::duration<double>(std::chrono::steady_clock::now()-close_begin).count()<<"\n}\n";
        if(!log)result=1;
    }
    if(hwnd)DestroyWindow(hwnd);UnregisterClassW(wc.lpszClassName,instance);CoUninitialize();return result;
}
