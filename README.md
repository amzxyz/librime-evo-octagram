# Octagram：命名分区 Darts 模型

一个模型可包含任意数量的命名区域。整句内部评分使用全部区域；已提交或已选文字对后续候选的调频，仅使用配置允许的区域。两种评分共享同一份内存映射。

构建配置 `model.yaml`：

```yaml
name: wanxiang-lts
language: zh-hans
sections:
  - name: collocations
    weight: 500000
    files: [four.txt, extra.txt]
  - name: long_phrases
    weight: 5000000000
    files: [long.txt]
  - name: weighted
    files: [precise.txt]
```

```sh
build_grammar --config model.yaml
```

输入路径相对于配置文件；输出位于当前目录，以上生成 `wanxiang-lts-zh-hans.gram`。构建成功时标准输出为生成文件的绝对路径。`--name`、`--language` 可覆盖配置，默认分别为 `wanxiang-lts`、`zh-hans`。

- 有 `weight`：每行仅需词条，也可保留原频次；每条使用公共频次。
- 无 `weight`：每行写 `词条<TAB>正数频次`。
- 相邻且名称、权重模式均相同的输入合并成一个物理段。
- 同段重复词条取最大频次；跨段重复词条由后段覆盖。
- 同一名称可对应多个不同权重的物理段，选择名称会同时选择这些段。

也可直接传入多个文件：

```sh
build_grammar --section collocations -t four.txt -w 500000 \
  --section weighted -t precise.txt --name wanxiang-lts --language zh-hans
```

Rime 方案配置：

```yaml
grammar:
  language: wanxiang-lts-zh-hans
  contextual_sections: [collocations]

translator:
  contextual_suggestions: true
```

`contextual_sections` 只按区域名称筛选，不检查前文字数或语言学边界。未设置或空列表关闭上下文调频；未知名称会报错并禁用该视图，不回退到全模型。被排除区域的普通匹配、句尾 `$` 匹配都不参与上下文评分。修改此数组只需重新部署方案，无须重新训练模型。普通整句造句继续使用全部区域。

公共频次仅保存一份，Darts 终端使用零值以共享后缀；逐词频次保留原有整数 log 评分。构建器逐段释放输入缓冲，查询合流使用固定栈缓冲。名称在加载时解析成段编号，查询不进行名称查找。

模型后缀为 `.gram`，格式为 `Rime::DartsGrammar/3`，需要本插件配套读取，不能使用原版或旧实验模型。主内核需要 `Grammar::Component::CreateContextual` 和 Poet 的对应调用；仓库中的 `patches/librime-grammar-scopes.patch` 提供该接入改动。补丁基于 librime-evo，前端 C API 无须改动，C++ 插件须重新编译。

测试使用 `ctest -R 'octagram_test|octagram_cli|grammar_scope_test' --output-on-failure`，覆盖分区优先级、读写、长词、输入校验、上下文过滤和整句边界。
