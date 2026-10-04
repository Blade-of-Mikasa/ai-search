# Nano AI Search

一个可以在本地直接启动的轻量 AI Search：搜索网页、抽取证据、治理引用，并流式生成
带来源的回答。

```text
React (5173)
  → FastAPI / model & web adapters (8000)
  → HTTP + JSON
  → C++20 evidence core / in-memory index (8081)
```

项目不再使用 gRPC、Protobuf、CMake、Conan 或 Milvus。C++ 使用
[Blade](https://github.com/blade-build/blade-build) 构建，Python 与 C++ 之间使用普通
HTTP/JSON。Nano 默认走网页搜索，因此最小启动不需要 MySQL、Kafka、S3 或向量数据库。

## 快速启动

需要 Linux、Python 3.11+、Node.js/npm、Git 和支持 C++20 的 GCC/Clang。

```bash
./scripts/bootstrap.sh
cp .env.example .env
# 编辑 .env，填入 RAG_CHAT_API_KEY 和 RAG_TAVILY_API_KEY
./scripts/run.sh
```

打开 <http://127.0.0.1:5173>。启动状态可查看
<http://127.0.0.1:8000/health/ready>。

端口冲突时可以直接覆盖默认端口；脚本会同步更新服务间地址和前端代理：

```bash
NANO_CORE_PORT=18081 NANO_API_PORT=18000 NANO_WEB_PORT=15173 ./scripts/run.sh
```

`bootstrap.sh` 会在仓库的 `.tools/` 下安装固定版本的 Blade，不污染系统目录；如果系统
已有 `blade` 命令则直接使用。构建命令也可以单独执行：

```bash
./scripts/blade.sh build //core:nano_core
```

## 最小必需项

`.env.example` 中填写生成模型和 Tavily 搜索配置：

```dotenv
RAG_CHAT_ENDPOINT_URL=你的 OpenAI-compatible /v1/responses 地址
RAG_CHAT_API_KEY=你的模型Key
RAG_CHAT_MODEL_ID=你的模型ID
RAG_WEB_SEARCH_PROVIDER=tavily
RAG_TAVILY_API_KEY=你的TavilyKey
```

更多可选能力及其依赖见 [配置说明](docs/configuration.md)。

## 当前边界

- 网页 AI Search：生成模型与搜索 provider 解耦；默认示例使用 Tavily，另支持
  Responses `web_search` 和 Microsoft Foundry Bing Grounding。
- 本地多模态索引：仍保留 Python 文档/图片/视频处理器，但 Nano Core 默认仅内存存储。
- 上传和异步入库：保留为可选扩展；只有使用它们时才需要 MySQL、S3 和 Kafka。
- 自动化测试：已按本次收缩要求移除；交付验证采用真实构建、进程健康检查和 HTTP
  冒烟请求。
