#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <cmath>

namespace DesktopBlur {
struct Result { bool accepted=false,clipped=false; };
inline Result apply(HWND window,bool enabled,int radius){
    Result result;if(!window||!IsWindow(window))return result;
    struct Policy{int state=0,flags=0;DWORD tint=0;int animation=0;};
    struct Data{int attribute=19;void *data=nullptr;SIZE_T size=0;};
    using SetAttribute=BOOL(WINAPI *)(HWND,Data *);
    static const auto library=LoadLibraryW(L"user32.dll");
    static const auto setAttribute=reinterpret_cast<SetAttribute>(GetProcAddress(library,"SetWindowCompositionAttribute"));
    HIGHCONTRASTW contrast{};contrast.cbSize=sizeof(contrast);
    if(SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(contrast),&contrast,0)&&(contrast.dwFlags&HCF_HIGHCONTRASTON))enabled=false;
    if(GetSystemMetrics(SM_REMOTESESSION))enabled=false;
    Policy policy;policy.state=enabled?3:0;Data data;data.data=&policy;data.size=sizeof(policy);
    result.accepted=setAttribute&&setAttribute(window,&data)&&enabled;
    if(!result.accepted){SetWindowRgn(window,nullptr,TRUE);return result;}
    RECT bounds{};if(!GetWindowRect(window,&bounds))return result;
    const int w=bounds.right-bounds.left,h=bounds.bottom-bounds.top,r=std::clamp(radius,0,std::min(w,h)/2);
    // Exclude only pixels wholly outside the rounded coverage. Qt/Chromium
    // still owns the fractional edge alpha; the native clip never sharpens it.
    HRGN region=CreateRectRgn(0,0,w,h);if(!region)return result;
    for(int y=0;y<r;++y){const double dy=r-y-1.;const int cut=std::max(0,int(std::floor(r-std::sqrt(std::max(0.,double(r*r)-dy*dy))))-1);
        if(!cut)continue;for(const int row:{y,h-y-1}){HRGN left=CreateRectRgn(0,row,cut,row+1),right=CreateRectRgn(w-cut,row,w,row+1);if(left){CombineRgn(region,region,left,RGN_DIFF);DeleteObject(left);}if(right){CombineRgn(region,region,right,RGN_DIFF);DeleteObject(right);}}
    }
    result.clipped=SetWindowRgn(window,region,TRUE)!=0;if(!result.clipped)DeleteObject(region);
    return result;
}
}
#endif
