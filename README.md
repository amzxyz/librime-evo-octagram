# Evo Octagram v2

基于 [librime-evo](https://github.com/amzxyz/librime-evo) 的 Octagram 语言模型插件。使用 Darts Double-Array 存储连续文本匹配关系，支持**命名分区、公共权重压缩和逐词权重**。模型统一编译为一个可内存映射的 `.dlm`（Dictionary Language Model）文件。

## 设计

### 分区与权重压缩

模型由多个有序的命名分区组成。每个分区可包含一个或多个物理段，支持两种权重存储方式：

| 模式 | 构建参数 | 存储方式 | 适用数据 |
| --- | --- | --- | --- |
| 公共权重 | `-t FILE -w WEIGHT` | 词条终端值统一为 0，每段保存原始权重和整数 log 评分 | 大规模固定权重词组 |
| 逐词权重 | `-t FILE` | 每个词条保存自己的整数 log 评分 | 需要保留频次差异的词组 |

权重在**构建时**换算为整数 log 评分并写入模型，查询时直接使用；公共权重段不再为每条词分别存储相同的分数。Darts 索引与公共终端值有利于降低模型体积，具体压缩率取决于数据分布。

分区名称由 `--section NAME` 自定义，支持中文和英文。下面的名称只是示例，方案中的 `grammar/contextual_sections` 使用构建时写入的名称。

相邻输入在分区名与权重模式、公共权重一致时合并为同一物理段。段内重复词条去重（逐词权重保留最大频次）；**跨段相同词条以后面的段为准**。

### 双评分视图

同一份模型提供两种视图，不重复加载索引：

- **整句内部造句**：使用全部分区，参与句子内部的连续匹配评分。
- **前文调频**：只使用 Rime 方案 `grammar/contextual_sections` 指定的分区，为已选或已提交文字之后的候选评分。

分区名称在模型加载时解析为物理段编号，查询时直接按编号访问。切换前文分区只需修改方案并重新部署，无需重新构建模型。

## 构建模型

```sh
build_grammar --section weighted -m "保留各词原有频次" -t precise.txt \
  --section collocations -m "四字 2+2 搭配，用于联想" \
  -t four.txt -w 500000 -t extra.txt -w 500000 \
  --section long_phrases -m "长词固定搭配" -t long.txt -w 5000000000 \
  --name wanxiang-lts --language zh-hans
```

生成 `wanxiang-lts-zh-hans.dlm`，输出在当前工作目录。构建成功时标准输出模型的绝对路径；`--name` 和 `--language` 的默认值分别是 `wanxiang-lts`、`zh-hans`。

| 分区 | 示例文件 | 权重策略 |
| --- | --- | --- |
| `weighted` | `precise.txt` | 读取每条词的频次 |
| `collocations` | `four.txt`、`extra.txt` | 统一频次 500000 |
| `long_phrases` | `long.txt` | 统一频次 5000000000 |

**输入格式**

公共权重文件（每行一个词条，可附带原频次）：

```text
历史文化
历史文化名城	100
```

逐词权重文件（必须有正数频次）：

```text
获得了加成	12345
历史文化	678
```

公共权重模式下，若输入提供了第二列，构建器会校验其为合法正数，但不使用该数值。

**构建参数**

| 参数 | 作用 |
| --- | --- |
| `--section NAME` | 指定后续输入文件的分区名称，默认 `default` |
| `-m TEXT` | 给当前分区添加可选说明，写入该名称下每个物理段的文件头 |
| `-t FILE` | 添加一个输入文件，按命令顺序处理 |
| `-w WEIGHT` | 为紧邻的前一个 `-t` 文件指定公共频次 |
| `--name NAME` | 指定模型名前缀 |
| `--language LANG` | 指定模型语言后缀 |

每个使用公共权重的输入文件都需要各自指定 `-w`。输入文件的顺序决定跨段重复词条的覆盖优先级。

### 从模型查看分区信息

分区名、权重模式和 `-m` 说明在构建时写入 `.dlm` 文件头。Rime 加载模型时，INFO 日志按文件顺序列出各段信息。公共权重段还会显示原始权重和整数评分，逐词权重段显示 `weight=per-key`。

如果原构建命令或随附说明找不到了，拿到 `.dlm` 就可以查看文件中保存的分区名、权重和用途说明。构建时省略 `-m` 的分区会显示空说明。

```sh
build_grammar --inspect wanxiang-lts-zh-hans.dlm
```

```text
grammar section[1] name="collocations" weight=500000 score=131223 description="四字 2+2 搭配，用于联想"
```

## Rime 配置

将 `wanxiang-lts-zh-hans.dlm` 放入 Rime 用户目录。`grammar/language` 填模型文件名，省略 `.dlm`。匹配参数和翻译器开关沿用万象方案默认值，下面的分区列表对应上面的构建示例。

```yaml
grammar:
  language: wanxiang-lts-zh-hans
  contextual_sections: [collocations, long_phrases]
  collocation_max_length: 6
  collocation_min_length: 2
  collocation_penalty: -14
  non_collocation_penalty: -6
  weak_collocation_penalty: -100
  rear_penalty: -18

translator:
  contextual_suggestions: false
```

万象默认关闭前文调频。启用时，将主翻译器的 `contextual_suggestions` 改为 `true`，并在 `contextual_sections` 中列出要参与调频的分区名。此例选中 `collocations` 和 `long_phrases`，整句内部评分仍使用全部分区。

省略 `contextual_sections` 或将其设为 `[]` 时，前文调频关闭。如果列表引用了文件里不存在的名称，加载时会报错并禁用前文调频。可以用 `--inspect` 核对名称。

## 安装插件和构建工具

Evo 的 macOS、Windows CI 和 Docker 构建会安装此插件。本地可在 Evo 根目录按需安装。

```sh
git submodule update --init --recursive
bash install-plugins.sh amzxyz/librime-evo-octagram
RIME_PLUGINS=amzxyz/librime-evo-octagram cmake -S . -B build -DBUILD_TOOLS=ON
cmake --build build --target build_grammar -j2
./build/bin/build_grammar --help
```

构建器位于 Evo 的 `build/bin/build_grammar`，生成 `Rime::EvoGrammar/2` 格式的 `.dlm` 文件。Windows 多配置构建的工具在 `build/bin/Release/build_grammar.exe`。安装 Evo 时，构建器随其他工具安装到同一个 `bin` 目录。
