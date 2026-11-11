# 第三方许可

## Qt

本仓库的 C++ 自研代码采用 [MIT](LICENSE)。主程序使用 Qt Base 的 Core、Gui、Widgets、Network 和 Concurrent 共享库。开源构建可选用 Qt 提供的 LGPL-3.0 条款，本仓库只发布源码，由构建者自行安装 Qt SDK。CMake 会校验这些 Qt 模块为动态库。

若另行发布包含 Qt DLL 或插件的安装包，需要核对随包组件的各自许可证，并履行对应的再分发义务。使用 LGPL-3.0 路径时，请保留许可和署名，按条款提供对应库源码或合法获取方式，并允许用户替换兼容的 Qt 库及为调试其修改而进行逆向工程。Windows SDK 和系统库适用 Microsoft 的相应条款。[Qt Base 6.8.3 许可文件](https://github.com/qt/qtbase/tree/v6.8.3/LICENSES) · [Qt LGPL 发布说明](https://www.qt.io/licensing/open-source-lgpl-obligations)

## 随源码分发的字体

| 字体 | 许可与版权声明 |
| --- | --- |
| Barlow Semi Condensed | SIL OFL 1.1 · Copyright 2017 The Barlow Project Authors。[许可全文](assets/themes/kda/fonts/Barlow-OFL.txt) |
| Inter | SIL OFL 1.1 · Copyright 2020 The Inter Project Authors。[许可全文](assets/themes/kda/fonts/Inter-OFL.txt) |
| Noto Sans SC | SIL OFL 1.1 · Copyright 2014–2021 Adobe，保留字体名 Source。[许可全文](assets/themes/kda/fonts/NotoSansSC-OFL.txt) |

## 鸣谢与游戏素材

- RankPeek（AGPL-3.0）和 ARAM Oracle（MIT）是设计调研的鸣谢链接。本仓库的构建不复制或链接其源码，也不打包其项目图标。[RankPeek](https://github.com/wxl11071123/rankpeek) · [ARAM Oracle](https://github.com/fabs133/league-aram-builder)
- 《英雄联盟》、K/DA、商标及截图中的游戏图标属于各自权利方。MIT 仅授予贡献者有权许可的自研代码及随附文档，不授予第三方素材、商标或字体权利。
- `demo/` 中的目录和数值是用来启动程序的合成样例，不能用于实际对局判断。公开源码不附带完整对局资料库、训练集或模型。
