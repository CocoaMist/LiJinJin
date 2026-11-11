#pragma once
#include <QImage>
#include <QPoint>
#include <QString>
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

namespace Kda::MetallicDetail {
inline constexpr int flakeOrientations=8;
// Decode one immutable embedded photograph. Connected bright cuts retain their
// photographed silhouettes and share one orientation, including across edges.
// Keep the two most recent tile sizes when they fit the memory budget.
class FlakeTextures {
public:
    struct Pixel {quint8 body,glint,orientation,r,g,b;};
    void prepare(qreal dpr){
        if(!attempted){
            attempted=true;++decodes;QImage original(QStringLiteral(":/lixin/kda/textures/crushed-diamond-lacquer-v1.png"));
            if(original.isNull())return;
            colorSource=original.scaled(1024,1024,Qt::IgnoreAspectRatio,Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32);
            source=colorSource.convertToFormat(QImage::Format_Grayscale8);
            glintSource=QImage(source.size(),QImage::Format_Grayscale8);
            for(int y=0;y<source.height();++y)for(int x=0;x<source.width();++x)glintSource.scanLine(y)[x]=uchar(std::clamp(qRound((source.constScanLine(y)[x]-65)*2.2),0,210));
            groups=QImage(source.size(),QImage::Format_Grayscale8);groups.fill(255);
            std::vector<QPoint> pending;pending.reserve(256);
            for(int y=0;y<source.height();++y)for(int x=0;x<source.width();++x){
                if(source.constScanLine(y)[x]<65||groups.constScanLine(y)[x]!=255)continue;
                const auto group=uchar((quint32(x)*73856093u^quint32(y)*19349663u)%flakeOrientations);
                pending.clear();pending.push_back({x,y});groups.scanLine(y)[x]=group;
                for(size_t i=0;i<pending.size();++i){
                    const auto point=pending[i];
                    for(const auto delta:{QPoint(1,0),QPoint(-1,0),QPoint(0,1),QPoint(0,-1)}){
                        const int px=(point.x()+delta.x()+source.width())%source.width(),py=(point.y()+delta.y()+source.height())%source.height();
                        if(groups.constScanLine(py)[px]!=255||source.constScanLine(py)[px]<65)continue;
                        groups.scanLine(py)[px]=group;pending.push_back({px,py});
                    }
                }
                ++cuts;
            }
        }
        if(source.isNull())return;
        const int edge=std::clamp(qRound(512*dpr),128,2048);
        if(edge==active.edge)return;
        if(edge==previous.edge){std::swap(active,previous);return;}

        const size_t pixelCount=size_t(edge)*edge;
        constexpr size_t tileBudget=16*1024*1024;
        // Release any evicted tile before allocating the next one. Large
        // supported tiles may use the cache alone, even beyond the budget.
        previous={};
        if((active.pixels.capacity()+pixelCount)*sizeof(Pixel)<=tileBudget)std::swap(active,previous);
        else active={};
        Tile prepared{edge,std::vector<Pixel>(pixelCount)};
        const auto resolved=source.scaled(edge,edge,Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
        const auto glare=glintSource.scaled(edge,edge,Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
        for(int y=0;y<edge;++y){const auto luminance=resolved.constScanLine(y),reflection=glare.constScanLine(y);
            for(int x=0;x<edge;++x){
                int bestX=x*source.width()/edge,bestY=y*source.height()/edge;
                const int lastX=std::min(source.width()-1,int(std::ceil((x+1.)*source.width()/edge))-1),lastY=std::min(source.height()-1,int(std::ceil((y+1.)*source.height()/edge))-1);
                for(int sy=bestY;sy<=lastY;++sy)for(int sx=x*source.width()/edge;sx<=lastX;++sx)if(source.constScanLine(sy)[sx]>source.constScanLine(bestY)[bestX]){bestX=sx;bestY=sy;}
                const auto group=groups.constScanLine(bestY)[bestX];const auto rgb=colorSource.pixel(bestX,bestY);const int peak=std::max({1,qRed(rgb),qGreen(rgb),qBlue(rgb)});
                prepared.pixels[size_t(y)*edge+x]={quint8(std::clamp(qRound((luminance[x]-18)*.22),0,45)),quint8(group<flakeOrientations?reflection[x]:0),quint8(group<flakeOrientations?group:0),quint8(210+45*qRed(rgb)/peak),quint8(210+45*qGreen(rgb)/peak),quint8(210+45*qBlue(rgb)/peak)};
            }
        }
        active=std::move(prepared);++preparations;
    }
    const Pixel *row(int y)const{return active.pixels.data()+size_t(y)*active.edge;}
    int tileEdge()const{return active.edge;}
    bool loaded()const{return !active.pixels.empty();}
    int decodeCount()const{return decodes;}
    int prepareCount()const{return preparations;}
    int cutCount()const{return cuts;}
private:
    struct Tile {int edge=0;std::vector<Pixel> pixels;};
    QImage source,colorSource,glintSource,groups;
    Tile active,previous;
    bool attempted=false;int decodes=0,preparations=0,cuts=0;
};
inline FlakeTextures &flakeTextures(){static FlakeTextures value;return value;}
}
