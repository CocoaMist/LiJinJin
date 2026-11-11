#pragma once
#include <QImage>
#include <QSize>
#include <QtMath>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace Kda {
namespace GaussianMaterialDetail {
using Channels=std::array<int,4>;
inline void add(Channels &sum,QRgb pixel,int weight=1){sum[0]+=qRed(pixel)*weight;sum[1]+=qGreen(pixel)*weight;sum[2]+=qBlue(pixel)*weight;sum[3]+=qAlpha(pixel)*weight;}
inline QRgb average(const Channels &sum,int divisor){const int half=divisor/2;return qRgba((sum[0]+half)/divisor,(sum[1]+half)/divisor,(sum[2]+half)/divisor,(sum[3]+half)/divisor);}
inline void horizontal(const QImage &source,QImage &destination,int radius){
    const int width=source.width(),height=source.height(),divisor=radius*2+1;
    for(int y=0;y<height;++y){const auto line=reinterpret_cast<const QRgb*>(source.constScanLine(y));auto out=reinterpret_cast<QRgb*>(destination.scanLine(y));Channels sum{};
        for(int x=-radius;x<=radius;++x)add(sum,line[std::clamp(x,0,width-1)]);
        for(int x=0;x<width;++x){out[x]=average(sum,divisor);add(sum,line[std::clamp(x-radius,0,width-1)],-1);add(sum,line[std::clamp(x+radius+1,0,width-1)]);}
    }
}
inline void vertical(const QImage &source,QImage &destination,int radius){
    const int width=source.width(),height=source.height(),divisor=radius*2+1;std::vector<Channels> sums(static_cast<size_t>(width),Channels{});
    for(int y=-radius;y<=radius;++y){const auto line=reinterpret_cast<const QRgb*>(source.constScanLine(std::clamp(y,0,height-1)));for(int x=0;x<width;++x)add(sums[static_cast<size_t>(x)],line[x]);}
    for(int y=0;y<height;++y){auto out=reinterpret_cast<QRgb*>(destination.scanLine(y));
        const auto leaving=reinterpret_cast<const QRgb*>(source.constScanLine(std::clamp(y-radius,0,height-1)));
        const auto entering=reinterpret_cast<const QRgb*>(source.constScanLine(std::clamp(y+radius+1,0,height-1)));
        for(int x=0;x<width;++x){auto &sum=sums[static_cast<size_t>(x)];out[x]=average(sum,divisor);add(sum,leaving[x],-1);add(sum,entering[x]);}
    }
}
inline std::array<int,3> radii(qreal sigma){
    const qreal ideal=std::sqrt(4*sigma*sigma+1);int lower=std::max(1,int(std::floor(ideal)));if(lower%2==0)--lower;
    const int upper=lower+2;const int lowerCount=std::clamp(qRound((12*sigma*sigma-3*lower*lower-12*lower-9)/(-4.*lower-4)),0,3);
    std::array<int,3> result{};for(int i=0;i<3;++i)result[i]=((i<lowerCount?lower:upper)-1)/2;return result;
}
}

// Material bitmap only: three separable boxes approximate a Gaussian. All
// channels are averaged in premultiplied alpha, including alpha itself, so
// translucent edges cannot acquire dark/colored halos. The caller caches the
// result; this function never reads widgets or modifies the source image.
// Pixel dimensions and source DPR metadata are preserved. The explicit DPR
// converts the requested logical-pixel sigma into the physical-pixel kernel.
inline QImage gaussianMaterialBlur(const QImage &source,qreal logicalSigma,qreal dpr){
    if(source.isNull()||!std::isfinite(logicalSigma)||logicalSigma<=0)return source;
    if(!std::isfinite(dpr)||dpr<=0)dpr=source.devicePixelRatioF();if(!std::isfinite(dpr)||dpr<=0)dpr=1;
    constexpr int MaximumSide=768;constexpr qreal MaximumPixels=196608;
    const qreal width=source.width(),height=source.height();
    const qreal scale=std::min({qreal(1),MaximumSide/std::max(width,height),std::sqrt(MaximumPixels/(width*height))});
    const QSize workSize(std::max(1,int(std::floor(width*scale))),std::max(1,int(std::floor(height*scale))));
    QImage work=source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if(work.size()!=workSize)work=work.scaled(workSize,Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
    if(work.isNull())return {};
    const qreal physicalSigma=logicalSigma*dpr;
    const qreal sigma=std::min(physicalSigma*scale,qreal(MaximumSide));
    QImage scratch(work.size(),QImage::Format_ARGB32_Premultiplied);if(scratch.isNull())return {};
    for(const int radius:GaussianMaterialDetail::radii(sigma))if(radius>0){GaussianMaterialDetail::horizontal(work,scratch,radius);GaussianMaterialDetail::vertical(scratch,work,radius);}
    QImage result=work.size()==source.size()?work:work.scaled(source.size(),Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
    result.setDevicePixelRatio(source.devicePixelRatioF());return result;
}
}
