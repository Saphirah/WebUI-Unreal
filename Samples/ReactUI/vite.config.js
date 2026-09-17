import {defineConfig} from 'vite';
import react from '@vitejs/plugin-react';
export default defineConfig({
  plugins: [react()],
  base: './',
  build: {outDir: '../WebUIDemo/Content/WebUIReact', emptyOutDir: true},
  server: {port: 5173, strictPort: true}
});
