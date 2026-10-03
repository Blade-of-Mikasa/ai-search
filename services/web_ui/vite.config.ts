import { defineConfig, loadEnv } from "vite";
import react from "@vitejs/plugin-react";

export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, ".", "");
  return {
    plugins: [react()],
    server: {
      port: 5173,
      proxy: {
        "/api": {
          target: env.VITE_API_PROXY_TARGET ?? "http://127.0.0.1:8000",
          changeOrigin: true,
          // 仅供本地开发。生产环境必须由完成认证的可信网关注入并覆盖这些头。
          headers: {
            "X-Tenant-ID": "local-demo",
            "X-User-ID": "local-user",
            "X-ACL-IDs": "local-demo",
          },
        },
      },
    },
  };
});
