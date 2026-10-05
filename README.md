# Nano AI Search

一个可以在本地直接启动的轻量 AI Search：搜索网页、抽取证据、治理引用，并流式生成
带来源的回答。

```text
React (5173)
  → FastAPI / model & web adapters (8000)
  → HTTP + JSON
  → Python evidence core / in-memory index (8081)
```

服务端已全部使用 Python。独立 Core 进程让 API 和异步入库 worker 共享同一份内存索引，
共享数据类和 Pydantic 负责 HTTP/JSON 契约，不再需要 C++ 工具链。Nano 默认走网页搜索，
因此最小启动不需要 MySQL、Kafka、S3 或向量数据库。

## 快速启动

需要 Linux、Python 3.11+ 和 Node.js/npm。

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

Python 服务可单独做导入和语法检查：

```bash
PYTHONPATH=services/python_api/src .venv/bin/python -m compileall -q services/python_api/src
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
