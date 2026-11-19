# librime-evo-octagram

[librime-evo](https://github.com/amzxyz/librime-evo) 的语言模型组件，基于 [Octagram](https://github.com/lotem/librime-octagram)，使用 MARISA 索引和无损评分压缩，模型后缀为 `.gramx`。

在 librime-evo 根目录安装并构建：

```sh
bash install-plugins.sh amzxyz/librime-evo-octagram
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
```

输入文件 `model.txt` 为 UTF-8，每行一个词条和正数频次，以空白分隔，无表头，例如 `你好 12345`。词条不限制字数；实际评分仍使用有限上下文窗口。

生成模型（Linux/macOS）：

```sh
build/plugins/evo-octagram/bin/build_grammar my_model < model.txt
```

输出为 `my_model.gramx`。将它放入 Rime 用户目录，并在方案中设置：

```yaml
grammar:
  language: my_model
```

Darts 格式模型需要重新生成，不能只改后缀。许可证：[BSD-3-Clause](LICENSE)。
