# Octagram · 分区压缩 Darts 语言模型

基于 [librime-evo](https://github.com/amzxyz/librime-evo) 的 Octagram 语言模型插件。使用 Darts Double-Array 存储连续文本匹配关系，支持**命名分区、公共权重压缩和逐词权重**。模型统一编译为一个可内存映射的 `.gram` 文件。

## 设计

### 分区与权重压缩

模型由多个有序的命名分区组成。每个分区可包含一个或多个物理段，支持两种权重存储方式：

| 模式 | 构建参数 | 存储方式 | 适用数据 |
| --- | --- | --- | --- |
| 公共权重 | `-t FILE -w WEIGHT` | 词条终端值统一为 0，整个物理段只保存一份 log 权重 | 大规模固定权重词组 |
| 逐词权重 | `-t FILE` | 每个词条保存自己的整数 log 评分 | 需要保留频次差异的词组 |

权重在**构建时**换算为整数 log 评分并写入模型，查询时直接使用；公共权重段不再为每条词分别存储相同的分数。Darts 索引与公共终端值有利于降低模型体积，具体压缩率取决于数据分布。

相邻输入在分区名与权重模式、公共权重一致时合并为同一物理段。段内重复词条去重（逐词权重保留最大频次）；**跨段相同词条以后面的段为准**。

### 双评分视图

同一份模型提供两种视图，不重复加载索引：

- **整句内部造句**：使用全部分区，参与句子内部的连续匹配评分。
- **前文调频**：只使用 Rime 方案 `grammar/contextual_sections` 指定的分区，为已选或已提交文字之后的候选评分。

分区名称在模型加载时解析为物理段编号，查询时直接按编号访问。切换前文分区只需修改方案并重新部署，无需重新构建模型。

## 构建模型

```sh
build_grammar --section weighted -t precise.txt --section collocations -t four.txt -w 500000 -t extra.txt -w 500000 --section long_phrases -t long.txt -w 5000000000 --name wanxiang-lts --language zh-hans
```

生成 `wanxiang-lts-zh-hans.gram`，输出在当前工作目录。构建成功时标准输出模型的绝对路径；`--name` 和 `--language` 的默认值分别是 `wanxiang-lts`、`zh-hans`。

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
| `-t FILE` | 添加一个输入文件，按命令顺序处理 |
| `-w WEIGHT` | 为紧邻的前一个 `-t` 文件指定公共频次 |
| `--name NAME` | 指定模型名前缀 |
| `--language LANG` | 指定模型语言后缀 |

每个使用公共权重的输入文件都需要各自指定 `-w`。输入文件的顺序决定跨段重复词条的覆盖优先级。

## Rime 配置

将生成的 `wanxiang-lts-zh-hans.gram` 放入 Rime 用户目录，在方案中配置：

```yaml
grammar:
  language: wanxiang-lts-zh-hans
  contextual_sections: [collocations]
  collocation_max_length: 8
  collocation_min_length: 3

translator:
  contextual_suggestions: true
```

其中 `contextual_sections: [collocations]` 表示**只有 `collocations` 参与前文调频**，但整句内部仍使用全部分区。也可指定多个名称：

```yaml
grammar:
  contextual_sections: [collocations, weighted]
```

不设置或设置为空列表 `[]` 时，不启用前文调频；指定不存在的名称会禁用该视图，不回退到全模型。其他原有 Octagram 参数（`collocation_penalty`、`non_collocation_penalty`、`weak_collocation_penalty`、`rear_penalty` 等）仍按原方式配置。

## 集成与测试

二进制格式：`Rime::DartsGrammar/3`，模型后缀 `.gram`。需使用本仓库提供的配套插件读取。主内核需要支持 `Grammar::Component::CreateContextual` 及 Poet 的对应调用，接入补丁见 [`patches/librime-grammar-scopes.patch`](patches/librime-grammar-scopes.patch)。

```sh
ctest -R 'octagram_test|octagram_cli|grammar_scope_test' --output-on-failure
```
