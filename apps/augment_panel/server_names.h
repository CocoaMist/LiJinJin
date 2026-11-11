#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

namespace GameRegion {
struct Region {QString code,name;QStringList members;};
// Display names and merged-region membership, checked against LeagueAkari
// 56cdfde00e56148f4641d1a8cf86acb66e903124 / platform-names.ts and CombinedTencentServers.vue.
inline const QVector<Region> &regions(){
    static const QVector<Region> entries{
        {QStringLiteral("HN1"),QStringLiteral("艾欧尼亚"),{}},
        {QStringLiteral("HN10"),QStringLiteral("黑色玫瑰"),{}},
        {QStringLiteral("NJ100"),QStringLiteral("联盟一区"),{QStringLiteral("祖安"),QStringLiteral("皮尔特沃夫"),QStringLiteral("巨神峰"),QStringLiteral("教育网"),QStringLiteral("男爵领域"),QStringLiteral("均衡教派"),QStringLiteral("影流"),QStringLiteral("守望之海")}},
        {QStringLiteral("GZ100"),QStringLiteral("联盟二区"),{QStringLiteral("暗影岛"),QStringLiteral("卡拉曼达"),QStringLiteral("雷瑟守备"),QStringLiteral("诺克萨斯"),QStringLiteral("征服之海"),QStringLiteral("战争学院")}},
        {QStringLiteral("CQ100"),QStringLiteral("联盟三区"),{QStringLiteral("班德尔城"),QStringLiteral("裁决之地"),QStringLiteral("钢铁烈阳"),QStringLiteral("皮城警备"),QStringLiteral("水晶之痕")}},
        {QStringLiteral("TJ100"),QStringLiteral("联盟四区"),{QStringLiteral("比尔吉沃特"),QStringLiteral("弗雷尔卓德"),QStringLiteral("扭曲丛林")}},
        {QStringLiteral("TJ101"),QStringLiteral("联盟五区"),{QStringLiteral("德玛西亚"),QStringLiteral("巨龙之巢"),QStringLiteral("恕瑞玛"),QStringLiteral("无畏先锋")}},
        {QStringLiteral("BGP2"),QStringLiteral("峡谷之巅"),{}}
    };return entries;
}
inline QString normalizedCode(QString code){code=code.trimmed().toUpper();if(code.startsWith(QStringLiteral("TENCENT_")))code.remove(0,8);return code;}
inline QString name(const QString &code){const auto normalized=normalizedCode(code);for(const auto &entry:regions())if(entry.code==normalized)return entry.name;return QStringLiteral("大区未识别");}
inline QString description(const Region &entry){return entry.members.isEmpty()?entry.name:entry.name+QStringLiteral(" · ")+entry.members.join(QStringLiteral("、"));}
}
