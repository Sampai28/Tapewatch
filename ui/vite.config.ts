import { defineConfig, loadEnv } from "vite";
import react from "@vitejs/plugin-react";

// The dev server and `vite preview` need the same proxy. `preview` does not
// inherit `server.proxy`, so the object is shared explicitly -- otherwise the
// production bundle appears broken when previewed locally and works fine in
// the container, which is a confusing half hour.
// loadEnv rather than process.env: this file is type-checked with the DOM
// lib and no @types/node, and pulling in Node's types purely to read one
// variable would put `process` and friends in scope for the whole console.
export const apiProxy = (target: string) => ({
  "/api": { target, changeOrigin: true },
});

export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, ".", "TAPEWATCH_");
  const proxy = apiProxy(env.TAPEWATCH_API ?? "http://localhost:8090");
  return {
    plugins: [react()],
    server: { port: 5180, proxy },
    preview: { port: 5180, proxy },
    build: { outDir: "dist", sourcemap: true },
  };
});
