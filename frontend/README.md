# Marathon Runner Map

React + Leaflet client for displaying runner locations from the FastAPI server with OpenStreetMap tiles.

## Run

```bash
cd frontend
npm install
npm run dev
```

Open `http://localhost:3000`.

The API base URL is stored in `.env`:

```text
VITE_API_BASE_URL=https://iot-final.leehan416.dev/api/
```

The app polls:

```text
GET https://iot-final.leehan416.dev/api/runners
```
