import { defineConfig } from "vite";
import solid from "@solidjs/vite-plugin";
import tailwindcss from "@tailwindcss/vite";
export default defineConfig({
  plugins: [solid(), tailwindcss()],
  build: {
    lib: {
      entry: "app.jsx",
      formats: ["es"],
      fileName: () => "ui.js",
      cssFileName: "ui",
    },
    target: "es2020",
  },
});
