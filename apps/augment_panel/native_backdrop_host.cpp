#include "native_desktop_blur.h"
#include <cstdint>
#include <cstdlib>
#include <iostream>

int main(int argc,char **argv){
#ifdef _WIN32
    if(argc!=5)return 2;
    // Region coordinates must use the same physical pixels as Qt/Electron.
    // A console helper otherwise gets DPI-virtualized bounds at 150% scaling.
    using SetDpiContext=BOOL(WINAPI *)(HANDLE);
    bool dpiSet=false;if(const auto setDpi=reinterpret_cast<SetDpiContext>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"SetProcessDpiAwarenessContext")))dpiSet=setDpi(reinterpret_cast<HANDLE>(-4))!=FALSE;
    const auto window=reinterpret_cast<HWND>(std::strtoull(argv[1],nullptr,16));
    DWORD process=0;GetWindowThreadProcessId(window,&process);
    if(!process||process!=std::strtoul(argv[2],nullptr,10))return 3;
    const auto result=DesktopBlur::apply(window,std::string(argv[3])=="1",std::clamp(std::atoi(argv[4]),0,64));
    // Recheck ownership before reading diagnostics if the window disappeared
    // or its handle was reused while applying the backdrop.
    DWORD currentProcess=0;GetWindowThreadProcessId(window,&currentProcess);
    if(currentProcess!=process)return 3;
    RECT bounds{};GetWindowRect(window,&bounds);
    using GetDwmAttribute=HRESULT(WINAPI *)(HWND,DWORD,PVOID,DWORD);
    DWORD cloaked=0;bool cloakQueried=false;
    if(const auto dwm=LoadLibraryW(L"dwmapi.dll")){
        const auto getAttribute=reinterpret_cast<GetDwmAttribute>(GetProcAddress(dwm,"DwmGetWindowAttribute"));
        constexpr DWORD cloakedAttribute=14; // DWMWA_CLOAKED
        cloakQueried=getAttribute&&SUCCEEDED(getAttribute(window,cloakedAttribute,&cloaked,sizeof(cloaked)));
        FreeLibrary(dwm);
    }
    RECT regionBounds{};int regionType=ERROR;
    if(const auto region=CreateRectRgn(0,0,0,0)){
        regionType=GetWindowRgn(window,region);
        if(regionType!=ERROR)GetRgnBox(region,&regionBounds);
        DeleteObject(region);
    }
    const auto extendedStyle=GetWindowLongPtrW(window,GWL_EXSTYLE);
    const auto boolean=[](bool value){return value?"true":"false";};
    std::cout<<"{\"accepted\":"<<boolean(result.accepted)<<",\"clipped\":"<<boolean(result.clipped)<<",\"dpi_configured\":"<<boolean(dpiSet)<<",\"native_size\":["<<bounds.right-bounds.left<<','<<bounds.bottom-bounds.top<<']'
             <<",\"handle_hex\":\""<<std::hex<<reinterpret_cast<std::uintptr_t>(window)<<std::dec<<"\",\"owner_pid\":"<<process
             <<",\"native_visible\":"<<boolean(IsWindowVisible(window)!=FALSE)<<",\"native_minimized\":"<<boolean(IsIconic(window)!=FALSE)<<",\"native_foreground\":"<<boolean(GetForegroundWindow()==window)
             <<",\"dwm_cloaked_queried\":"<<boolean(cloakQueried)<<",\"dwm_cloaked\":";
    if(cloakQueried)std::cout<<cloaked;else std::cout<<"null";
    // ERROR can also mean the window has no explicit region; distinguish it
    // from a successfully queried NULLREGION (an explicitly empty clip).
    std::cout<<",\"native_region_type\":"<<regionType<<",\"native_region_queried\":"<<boolean(regionType!=ERROR)<<",\"native_region_bounds\":";
    if(regionType!=ERROR)std::cout<<'['<<regionBounds.left<<','<<regionBounds.top<<','<<regionBounds.right<<','<<regionBounds.bottom<<']';else std::cout<<"null";
    std::cout<<",\"extended_style\":{\"layered\":"<<boolean((extendedStyle&WS_EX_LAYERED)!=0)<<",\"transparent\":"<<boolean((extendedStyle&WS_EX_TRANSPARENT)!=0)<<",\"noactivate\":"<<boolean((extendedStyle&WS_EX_NOACTIVATE)!=0)<<",\"topmost\":"<<boolean((extendedStyle&WS_EX_TOPMOST)!=0)<<"}}";
    return 0;
#else
    return 2;
#endif
}
