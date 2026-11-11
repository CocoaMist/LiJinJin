# 从源码构建

在 Windows x64 上安装 Visual Studio 2022 的「使用 C++ 的桌面开发」工作负载、CMake 3.24+、Ninja 和 Qt 6.8+ 的 MSVC x64 动态库版本。在 VS 的 x64 Developer PowerShell 中运行，按实际安装位置修改 `$qt`。

```powershell
$qt = 'C:/Qt/6.8.3/msvc2022_64'
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$qt"
cmake --build build --parallel
$env:Path = "$qt/bin;$env:Path"
$env:QT_PLUGIN_PATH = "$qt/plugins"
ctest --test-dir build --output-on-failure
```

## 启动演示

```powershell
& .\build\apps\augment_panel\lixin_augment_panel.exe --root .\demo --demo --inspection-window --dashboard --quit-after-ms 60000
```

首次看到本地启动说明时可以阅读并关闭。演示使用合成对局，约一分钟后退出。公开版使用李金金标记和自制主题资源，部分本地版角色插画和教程图片在公开版中会走界面回退。

真实对局需要另备同版本、来源获准的目录和数值资料，并通过 `--root` 指向该工作目录。目录指针、数值和文件清单的格式可参考 [演示目录](demo) 与 [Catalog 加载器](apps/native/catalog.cpp)。访问客户端的 TLS 信任文件应从 Riot 官方来源取得，放在该工作目录的 `data/riotgames.pem`。DeepSeek 为可选功能，密钥由用户在本地配置。[Riot 官方证书](https://static.developer.riotgames.com/docs/lol/riotgames.pem)

本仓库提供源码和演示数据。打包 Qt 运行库前请查看 [第三方许可](THIRD_PARTY_NOTICES.md)。
