import { useEffect, useRef } from 'react';
import L, { type LatLngExpression } from 'leaflet';
import {
  MapContainer,
  Marker,
  Polyline,
  TileLayer,
  useMap,
} from 'react-leaflet';

import type { RunnerLocation } from '../types/runner';

const defaultCenter: [number, number] = [36.10321, 129.38712];

type RunnerMapProps = {
  runners: RunnerLocation[];
  selectedRunner: RunnerLocation | null;
  selectedRunnerId: string | null;
  runnerPaths: Record<string, RunnerLocation[]>;
};

export function RunnerMap({
  runners,
  selectedRunner,
  selectedRunnerId,
  runnerPaths,
}: RunnerMapProps) {
  const center: [number, number] =
    runners.length === 0
      ? defaultCenter
      : [runners[0].latitude, runners[0].longitude];

  return (
    <section
      className="relative min-h-[68vh] min-w-0 flex-1 overflow-hidden bg-slate-200 lg:min-h-screen"
      aria-label="Runner location map"
    >
      <MapContainer center={center} zoom={15} className="runner-map">
        <TileLayer
          attribution='&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a>'
          url="https://tile.openstreetmap.org/{z}/{x}/{y}.png"
        />
        <MapViewport
          runners={runners}
          selectedRunner={selectedRunner}
          selectedRunnerId={selectedRunnerId}
        />
        {getVisiblePaths(runners, runnerPaths, selectedRunnerId).map(
          ({ runnerId, path }) =>
            path.length >= 2 && (
              <Polyline
                key={runnerId}
                positions={path.map((point) => [
                  point.latitude,
                  point.longitude,
                ])}
                pathOptions={{
                  color: getPathColor(runnerId, path),
                  weight: selectedRunnerId ? 6 : 4,
                  opacity: selectedRunnerId ? 0.9 : 0.62,
                }}
              />
            ),
        )}
        {runners.map((runner) => (
          <Marker
            key={runner.runner_id}
            position={[runner.latitude, runner.longitude]}
            icon={createRunnerIcon(
              runner.runner_id,
              runner.runner_id === selectedRunnerId,
              isEmergencyStatus(runner.status),
            )}
          />
        ))}
      </MapContainer>
    </section>
  );
}

function MapViewport({
  runners,
  selectedRunner,
  selectedRunnerId,
}: {
  runners: RunnerLocation[];
  selectedRunner: RunnerLocation | null;
  selectedRunnerId: string | null;
}) {
  const map = useMap();
  const didInitialFit = useRef(false);
  const lastFocusedRunnerId = useRef<string | null>(null);

  useEffect(() => {
    if (
      selectedRunner &&
      selectedRunnerId &&
      lastFocusedRunnerId.current !== selectedRunnerId
    ) {
      map.flyTo([selectedRunner.latitude, selectedRunner.longitude], 17, {
        duration: 0.6,
      });
      lastFocusedRunnerId.current = selectedRunnerId;
      return;
    }

    if (didInitialFit.current || runners.length === 0) return;

    const bounds = L.latLngBounds(
      runners.map(
        (runner) => [runner.latitude, runner.longitude] as LatLngExpression,
      ),
    );
    map.fitBounds(bounds, { padding: [36, 36], maxZoom: 16 });
    didInitialFit.current = true;
  }, [map, runners, selectedRunner, selectedRunnerId]);

  return null;
}

const runnerPalette = [
  '#10b981',
  '#3b82f6',
  '#f97316',
  '#8b5cf6',
  '#ec4899',
  '#14b8a6',
  '#eab308',
  '#ef4444',
];

function createRunnerIcon(
  runnerId: string,
  selected: boolean,
  emergency: boolean,
) {
  const size = selected ? 48 : 40;
  const iconSize: [number, number] = [size, size];
  const iconAnchor: [number, number] = [size / 2, size / 2];
  const className = selected
    ? 'runner-div-icon runner-div-icon-selected'
    : 'runner-div-icon';
  const color = emergency ? '#ef4444' : getRunnerColor(runnerId);

  return L.divIcon({
    className,
    iconSize,
    iconAnchor,
    popupAnchor: [0, -size / 2],
    html: `
      <div class="runner-marker-shell ${selected ? 'selected' : ''} ${emergency ? 'emergency' : ''}" style="--runner-color: ${color};">
        <div class="runner-marker-core">
          <svg aria-hidden="true" viewBox="0 0 24 24" class="runner-marker-symbol">
            <path fill="currentColor" d="M13.5 5.5a2 2 0 1 1-4 0 2 2 0 0 1 4 0Zm-2.3 3.1c.7 0 1.4.3 1.9.8l1.1 1.1c.3.3.7.5 1.1.5h1.2a1 1 0 1 1 0 2h-1.2c-1 0-1.9-.4-2.6-1.1l-.3-.3-.7 2.2 1.9 1.6c.4.3.6.8.6 1.3V20a1 1 0 1 1-2 0v-2.8l-2.1-1.8-1 2.6c-.2.5-.7.8-1.2.8H5.5a1 1 0 1 1 0-2h1.7l1.4-3.9.5-1.8-1.1.6c-.5.3-.8.7-1 1.2l-.3.9a1 1 0 0 1-1.9-.6l.3-.9c.3-1 1-1.8 1.9-2.3l2.2-1.2c.6-.3 1.3-.5 2-.5Z"/>
          </svg>
        </div>
      </div>
    `,
  });
}

function getPathColor(runnerId: string, path: RunnerLocation[]) {
  const lastPoint = path[path.length - 1];
  return lastPoint && isEmergencyStatus(lastPoint.status)
    ? '#ef4444'
    : getRunnerColor(runnerId);
}

function isEmergencyStatus(status: string) {
  return status.toLowerCase() === 'emergency';
}

function getRunnerColor(runnerId: string) {
  let hash = 0;
  for (let index = 0; index < runnerId.length; index += 1) {
    hash = runnerId.charCodeAt(index) + ((hash << 5) - hash);
  }
  return runnerPalette[Math.abs(hash) % runnerPalette.length];
}

function getVisiblePaths(
  runners: RunnerLocation[],
  runnerPaths: Record<string, RunnerLocation[]>,
  selectedRunnerId: string | null,
) {
  if (selectedRunnerId) {
    return [
      {
        runnerId: selectedRunnerId,
        path: runnerPaths[selectedRunnerId] ?? [],
      },
    ];
  }

  return runners.map((runner) => ({
    runnerId: runner.runner_id,
    path: runnerPaths[runner.runner_id] ?? [],
  }));
}
