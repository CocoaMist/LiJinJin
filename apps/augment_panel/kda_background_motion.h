#pragma once
#include <QColor>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointF>
#include <QString>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace Kda {
struct BackgroundMotionProfile {
    bool valid=false;
    int sampleCount=0;
    qreal meanLuminance=0,meanSaturation=0,contrast=0,warmth=0;
    QPointF focus{.5,.5},direction{1,0};
    std::array<QColor,3> palette{QColor(190,222,233),QColor(214,233,239),QColor(237,245,247)};
    std::array<qreal,96> energy{}; // Row-major 12 x 8, normalized to its peak.
    QJsonObject report()const{
        QJsonArray colors,field;
        for(const auto &color:palette)colors.append(color.name(QColor::HexRgb));
        for(const auto value:energy)field.append(value);
        return {{"valid",valid},{"sample_count",sampleCount},{"mean_luminance",meanLuminance},
            {"mean_saturation",meanSaturation},{"contrast",contrast},{"warmth",warmth},
            {"focus",QJsonArray{focus.x(),focus.y()}},{"direction",QJsonArray{direction.x(),direction.y()}},
            {"palette",colors},{"energy_grid",QJsonArray{12,8}},{"energy",field}};
    }
    qreal energyAt(QPointF normalized)const{
        if(!valid)return 0;
        if(!std::isfinite(normalized.x())||!std::isfinite(normalized.y()))normalized={.5,.5};
        const qreal x=std::clamp(std::clamp(normalized.x(),qreal(0),qreal(1))*12-.5,qreal(0),qreal(11));
        const qreal y=std::clamp(std::clamp(normalized.y(),qreal(0),qreal(1))*8-.5,qreal(0),qreal(7));
        const int x0=int(std::floor(x)),y0=int(std::floor(y)),x1=std::min(11,x0+1),y1=std::min(7,y0+1);
        const qreal tx=x-x0,ty=y-y0;
        return std::clamp((energy[y0*12+x0]*(1-tx)+energy[y0*12+x1]*tx)*(1-ty)
            +(energy[y1*12+x0]*(1-tx)+energy[y1*12+x1]*tx)*ty,qreal(0),qreal(1));
    }
};

// Only the bounded image samples survive this call, as aggregate numbers and
// colors. Coordinates and the principal axis use normalized image space.
inline BackgroundMotionProfile analyzeBackgroundMotion(const QImage &image,QColor fallback=QColor(QStringLiteral("#bedee9"))){
    BackgroundMotionProfile result;
    if(!fallback.isValid())fallback=QColor(190,222,233);
    fallback.setAlpha(255);
    const auto particleColor=[&](const QColor &base,int tone){
        const qreal light=std::clamp(std::max(qreal(base.lightnessF()),qreal(.68+.065*tone)),qreal(0),qreal(.95));
        if(base.hsvSaturationF()<.10){
            // A neutral material stays metallic; the fallback is only a tint.
            const qreal neutral=std::max(light,qreal(.77+.055*tone));
            return QColor::fromRgbF(std::clamp(neutral*.84+fallback.redF()*.16,qreal(0),qreal(1)),
                std::clamp(neutral*.84+fallback.greenF()*.16,qreal(0),qreal(1)),
                std::clamp(neutral*.84+fallback.blueF()*.16,qreal(0),qreal(1)));
        }
        return QColor::fromHslF(base.hslHueF(),std::clamp(base.hslSaturationF()*.72,qreal(.18),qreal(.78)),light);
    };
    for(int i=0;i<3;++i)result.palette[i]=particleColor(fallback,i);
    if(image.isNull()||image.width()<1||image.height()<1)return result;
    // Each axis is capped independently: the statistics and axis below are in
    // UV space, so very wide and very tall images retain the same convention.
    const QImage sample=image.scaled(std::min(96,image.width()),std::min(60,image.height()),
        Qt::IgnoreAspectRatio,Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32);
    if(sample.isNull())return result;
    const int width=sample.width(),height=sample.height();
    struct Pixel {qreal r=0,g=0,b=0,a=0,luma=0,saturation=0,weight=0;};
    struct ColorBin {qreal weight=0,r=0,g=0,b=0;};
    std::vector<Pixel> pixels(size_t(width)*size_t(height));
    std::array<ColorBin,512> bins{};
    const auto linear=[](qreal value){return value<=.04045?value/12.92:std::pow((value+.055)/1.055,2.4);};
    qreal coverage=0,luminance=0,luminanceSquared=0,saturation=0,warmth=0;
    int visible=0,meaningful=0;
    for(int y=0;y<height;++y)for(int x=0;x<width;++x){
        const QColor color=sample.pixelColor(x,y);auto &pixel=pixels[size_t(y)*width+x];
        pixel.a=color.alphaF();if(pixel.a<.02)continue;
        pixel.r=color.redF();pixel.g=color.greenF();pixel.b=color.blueF();
        const qreal high=std::max({pixel.r,pixel.g,pixel.b}),low=std::min({pixel.r,pixel.g,pixel.b});
        pixel.saturation=high>0?(high-low)/high:0;
        pixel.luma=.2126*linear(pixel.r)+.7152*linear(pixel.g)+.0722*linear(pixel.b);
        ++visible;coverage+=pixel.a;luminance+=pixel.luma*pixel.a;
        luminanceSquared+=pixel.luma*pixel.luma*pixel.a;saturation+=pixel.saturation*pixel.a;
        warmth+=(pixel.r-pixel.b)*pixel.a;
        if(pixel.luma>.004||(pixel.saturation>.2&&high>.08))++meaningful;
    }
    if(!meaningful||coverage<=0)return result; // Empty, transparent or near black.
    result.valid=true;result.sampleCount=visible;
    result.meanLuminance=std::clamp(luminance/coverage,qreal(0),qreal(1));
    result.meanSaturation=std::clamp(saturation/coverage,qreal(0),qreal(1));
    result.contrast=std::sqrt(std::max(qreal(0),luminanceSquared/coverage-result.meanLuminance*result.meanLuminance));
    result.warmth=std::clamp(warmth/coverage,qreal(-1),qreal(1));
    const auto lumaAt=[&](int x,int y){const auto &p=pixels[size_t(std::clamp(y,0,height-1))*width+std::clamp(x,0,width-1)];return p.luma*p.a;};
    qreal total=0,mx=0,my=0,mxx=0,myy=0,mxy=0;
    for(int y=0;y<height;++y)for(int x=0;x<width;++x){
        auto &pixel=pixels[size_t(y)*width+x];if(pixel.a<.02)continue;
        const qreal edge=std::hypot(lumaAt(x+1,y)-lumaAt(x-1,y),lumaAt(x,y+1)-lumaAt(x,y-1))*.5;
        pixel.weight=pixel.a*(.02*std::sqrt(pixel.luma)+.5*pixel.luma
            +.35*pixel.saturation*std::sqrt(std::max({pixel.r,pixel.g,pixel.b}))+.5*edge);
        const qreal u=(x+.5)/width,v=(y+.5)/height,w=pixel.weight;
        total+=w;mx+=w*u;my+=w*v;mxx+=w*u*u;myy+=w*v*v;mxy+=w*u*v;
        const int key=(std::min(7,int(pixel.r*8))<<6)|(std::min(7,int(pixel.g*8))<<3)|std::min(7,int(pixel.b*8));
        auto &bin=bins[key];bin.weight+=w;bin.r+=pixel.r*w;bin.g+=pixel.g*w;bin.b+=pixel.b*w;
    }
    if(total>0){
        result.focus={std::clamp(mx/total,qreal(0),qreal(1)),std::clamp(my/total,qreal(0),qreal(1))};
        const qreal xx=std::max(qreal(0),mxx/total-result.focus.x()*result.focus.x());
        const qreal yy=std::max(qreal(0),myy/total-result.focus.y()*result.focus.y());
        const qreal xy=mxy/total-result.focus.x()*result.focus.y();
        if(std::hypot(xx-yy,2*xy)>(xx+yy)*.025+1e-10){
            const qreal angle=.5*std::atan2(2*xy,xx-yy);
            result.direction={std::cos(angle),std::sin(angle)};
            if(std::abs(result.direction.x())<=1e-9)result.direction={0,1};
            else if(result.direction.x()<0)result.direction=-result.direction;
        }
    }
    // Integrate pixel footprints instead of assigning centers to cells. This
    // also gives a uniform 1x1 image a uniform field rather than one hot cell.
    qreal peak=0;
    for(int gy=0;gy<8;++gy)for(int gx=0;gx<12;++gx){
        const qreal x0=gx*width/12.,x1=(gx+1)*width/12.,y0=gy*height/8.,y1=(gy+1)*height/8.;
        qreal value=0;
        for(int y=int(std::floor(y0));y<std::min(height,int(std::ceil(y1)));++y)
            for(int x=int(std::floor(x0));x<std::min(width,int(std::ceil(x1)));++x)
                value+=pixels[size_t(y)*width+x].weight*(std::min(x1,qreal(x+1))-std::max(x0,qreal(x)))
                    *(std::min(y1,qreal(y+1))-std::max(y0,qreal(y)));
        result.energy[gy*12+gx]=value/((x1-x0)*(y1-y0));peak=std::max(peak,result.energy[gy*12+gx]);
    }
    if(peak>0)for(auto &value:result.energy)value=std::clamp(value/peak,qreal(0),qreal(1));
    std::array<int,512> order{};for(int i=0;i<512;++i)order[i]=i;
    std::sort(order.begin(),order.end(),[&](int a,int b){return bins[a].weight==bins[b].weight?a<b:bins[a].weight>bins[b].weight;});
    QColor dominant=fallback;
    for(int i=0;i<3;++i){
        const auto &bin=bins[order[i]];
        if(bin.weight>0)dominant=QColor::fromRgbF(std::clamp(bin.r/bin.weight,qreal(0),qreal(1)),
            std::clamp(bin.g/bin.weight,qreal(0),qreal(1)),std::clamp(bin.b/bin.weight,qreal(0),qreal(1)));
        result.palette[i]=particleColor(dominant,i);
    }
    return result;
}
}
