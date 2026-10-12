#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>
#include <shellapi.h>
#include <commdlg.h>
#include <wrl/client.h>
#include "gui.h"
#include "implot.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include <fstream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
static ComPtr<ID3D11Device> device;
static ComPtr<ID3D11DeviceContext> context;
static ComPtr<IDXGISwapChain> swapchain;
static ComPtr<ID3D11RenderTargetView> target;
static UINT resize_w=0,resize_h=0;
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
        case WM_GETMINMAXINFO: {
            auto* info=reinterpret_cast<MINMAXINFO*>(l);info->ptMinTrackSize={1120,720};return 0;
        }
        case WM_SYSCOMMAND:if((w&0xfff0)==SC_KEYMENU)return 0;break;
        case WM_DESTROY:PostQuitMessage(0);return 0;
    }return DefWindowProcW(hwnd,msg,w,l);
}
static void capture_png(const std::filesystem::path& path) {
    ComPtr<ID3D11Texture2D> back,staging;check(swapchain->GetBuffer(0,IID_PPV_ARGS(&back)),"Screenshot buffer failed.");
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
static std::filesystem::path dialog(HWND hwnd,bool save,bool csv,bool checkpoint=false) {
    wchar_t buf[32768]{};if(save)wcscpy_s(buf,checkpoint?L"experiment.nscp":csv?L"experiment.csv":L"network.json");
    OPENFILENAMEW ofn{};ofn.lStructSize=sizeof(ofn);ofn.hwndOwner=hwnd;ofn.lpstrFile=buf;ofn.nMaxFile=32768;
    ofn.lpstrFilter=checkpoint?L"Network Studio checkpoint\0*.nscp\0\0":csv?L"CSV results\0*.csv\0\0":L"Network configuration\0*.json\0\0";
    ofn.lpstrDefExt=checkpoint?L"nscp":csv?L"csv":L"json";ofn.Flags=OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|(save?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
    if(save?GetSaveFileNameW(&ofn):GetOpenFileNameW(&ofn))return buf;return {};
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int) {
    int argc=0;wchar_t** args=CommandLineToArgvW(GetCommandLineW(),&argc);
    std::filesystem::path smoke,config_path,checkpoint_path;int smoke_preset=0;bool smoke_wizard=false;
    for(int i=1;i<argc;++i) {
        if(std::wstring(args[i])==L"--smoke"&&i+1<argc)smoke=args[++i];
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
        hwnd=CreateWindowW(wc.lpszClassName,L"Neuro / Astro — Network Studio",WS_OVERLAPPEDWINDOW,70,40,1480,1000,nullptr,nullptr,instance,nullptr);
        if(!hwnd)throw std::runtime_error("Cannot create the application window.");create_device(hwnd);resize_w=resize_h=0;
        IMGUI_CHECKVERSION();ImGui::CreateContext();ImPlot::CreateContext();imgui=true;
        auto& io=ImGui::GetIO();io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;io.IniFilename=nullptr;
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
        studio::App app;app.file_dialog=[&](bool save,bool csv){return dialog(hwnd,save,csv);};
        app.checkpoint_dialog=[&](bool save){return dialog(hwnd,save,false,true);};
        // A command-line configuration must fail explicitly rather than silently
        // running the default experiment when reading the requested file fails.
        if(!config_path.empty())app.replace(studio::load_config(config_path));
        if(!checkpoint_path.empty()) {
            // Validate before changing the GUI; startup failures are explicit.
            if(!app.restore_state(checkpoint_path))throw std::runtime_error(app.status);
        }
        if(!smoke.empty()) {
            std::filesystem::create_directories(smoke);
            if(config_path.empty()&&checkpoint_path.empty()) app.replace(studio::preset(smoke_preset));
            while(!app.simulation->done())if(!app.simulation->step())break;
            if(!app.simulation->error.empty())throw std::runtime_error(app.simulation->error);
            app.status="Проверка прототипа: расчёт завершён, графики построены.";
        }else {ShowWindow(hwnd,SW_SHOWDEFAULT);UpdateWindow(hwnd);}
        bool done=false;int frames=0;int minimum_vertices=100000000;
        while(!done) {
            MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);if(msg.message==WM_QUIT)done=true;}
            if(done)break;
            if(resize_w&&resize_h){target.Reset();check(swapchain->ResizeBuffers(0,resize_w,resize_h,DXGI_FORMAT_UNKNOWN,0),"Window resize failed.");resize_w=resize_h=0;render_target();}
            if(smoke.empty()&&IsIconic(hwnd)){Sleep(20);continue;}
            ImGui_ImplDX11_NewFrame();ImGui_ImplWin32_NewFrame();ImGui::NewFrame();
            if(!smoke.empty()) {
                if(smoke_wizard && frames==0)app.request_network_wizard=true;
                int group=frames/4;app.request_plot_tab=group;
                app.selected_kind=(group==0||group==5||app.config.astrocytes.empty())?0:(group==3?2:1);
                if(group==3&&!app.config.synapses.empty())app.selected_kind=2;
                app.selected_index=0;
            }
            app.draw();ImGui::Render();auto* data=ImGui::GetDrawData();minimum_vertices=std::min(minimum_vertices,data->TotalVtxCount);
            const float clear[]={.055f,.073f,.1f,1};ID3D11RenderTargetView* rtv=target.Get();context->OMSetRenderTargets(1,&rtv,nullptr);context->ClearRenderTargetView(rtv,clear);
            ImGui_ImplDX11_RenderDrawData(data);
            if(!smoke.empty()&&frames%4==3)capture_png(smoke/(L"tab_"+std::to_wstring(frames/4)+L".png"));
            HRESULT hr=swapchain->Present(smoke.empty()?1:0,0);if(FAILED(hr))throw std::runtime_error("Direct3D presentation failed.");
            ++frames;if(!smoke.empty()&&frames>=24)done=true;
        }
        if(!smoke.empty()) {
            studio::export_results(*app.simulation,smoke/L"demo");
            std::ofstream log(smoke/L"renderer.txt");log<<"frames="<<frames<<"\nminimum_vertices="<<minimum_vertices<<"\nsamples="<<app.simulation->history.time.size()<<"\nerror="<<app.simulation->error<<"\n";
            if(minimum_vertices<100)throw std::runtime_error("Renderer emitted too few vertices.");
            std::error_code ec;std::filesystem::remove(smoke/L"error.txt",ec);
        }
    }catch(const std::exception& e) {
        result=1;
        if(smoke.empty())MessageBoxA(hwnd,e.what(),"Network Studio",MB_OK|MB_ICONERROR);
        else {std::filesystem::create_directories(smoke);std::ofstream log(smoke/L"error.txt");log<<e.what();}
    }
    if(backends){ImGui_ImplDX11_Shutdown();ImGui_ImplWin32_Shutdown();}
    if(imgui){ImPlot::DestroyContext();ImGui::DestroyContext();}
    target.Reset();swapchain.Reset();context.Reset();device.Reset();
    if(hwnd)DestroyWindow(hwnd);UnregisterClassW(wc.lpszClassName,instance);CoUninitialize();return result;
}
