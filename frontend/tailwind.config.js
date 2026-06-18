/** @type {import('tailwindcss').Config} */
export default {
  content: ['./index.html', './src/**/*.{ts,tsx}'],
  theme: {
    extend: {
      colors: {
        race: {
          ink: '#17201d',
          muted: '#61716b',
          line: '#d8e2dc',
          tint: '#eef8f6',
          green: '#0f766e',
          deep: '#115e59',
        },
      },
      boxShadow: {
        panel: '0 18px 45px rgba(23, 32, 29, 0.14)',
        soft: '0 4px 20px -2px rgba(0, 0, 0, 0.05)',
      },
    },
  },
  plugins: [],
};
