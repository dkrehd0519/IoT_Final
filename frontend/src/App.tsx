import { useEffect, useMemo, useState } from 'react';

import {
  fetchRunnerPath,
  fetchRunnerRankings,
  fetchRunners,
} from './api/runners';
import { RunnerMap } from './components/RunnerMap';
import { Sidebar } from './components/Sidebar';
import type { RunnerLocation, RunnerRanking } from './types/runner';

export function App() {
  const [runners, setRunners] = useState<RunnerLocation[]>([]);
  const [rankings, setRankings] = useState<RunnerRanking[]>([]);
  const [runnerPaths, setRunnerPaths] = useState<Record<string, RunnerLocation[]>>(
    {},
  );
  const [selectedRunnerId, setSelectedRunnerId] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [pathError, setPathError] = useState<string | null>(null);

  async function loadRunnerPath(runnerId: string) {
    try {
      const data = await fetchRunnerPath(runnerId);
      setRunnerPaths((current) => ({ ...current, [runnerId]: data }));
      setPathError(null);
    } catch (err) {
      setRunnerPaths((current) => ({ ...current, [runnerId]: [] }));
      setPathError(err instanceof Error ? err.message : 'Unknown path error');
    }
  }

  async function loadRunnerPaths(nextRunners: RunnerLocation[]) {
    try {
      const entries = await Promise.all(
        nextRunners.map(async (runner) => [
          runner.runner_id,
          await fetchRunnerPath(runner.runner_id),
        ] as const),
      );
      setRunnerPaths(Object.fromEntries(entries));
      setPathError(null);
    } catch (err) {
      setPathError(err instanceof Error ? err.message : 'Unknown path error');
    }
  }

  async function loadRunners() {
    try {
      const [data, rankingData] = await Promise.all([
        fetchRunners(),
        fetchRunnerRankings(),
      ]);
      setRunners(data);
      setRankings(rankingData);
      loadRunnerPaths(data);
      setSelectedRunnerId((current) => {
        if (!current) return current;
        return data.some((runner) => runner.runner_id === current) ? current : null;
      });
      setError(null);
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Unknown error');
    }
  }

  useEffect(() => {
    loadRunners();
    const timer = window.setInterval(loadRunners, 3000);
    return () => window.clearInterval(timer);
  }, []);

  useEffect(() => {
    if (!selectedRunnerId) {
      setPathError(null);
      return;
    }

    loadRunnerPath(selectedRunnerId);
    const timer = window.setInterval(() => loadRunnerPath(selectedRunnerId), 3000);
    return () => window.clearInterval(timer);
  }, [selectedRunnerId]);

  const selectedRunner = useMemo(() => {
    if (!selectedRunnerId) return null;
    return runners.find((runner) => runner.runner_id === selectedRunnerId) ?? null;
  }, [runners, selectedRunnerId]);

  function toggleSelectedRunner(runnerId: string) {
    setSelectedRunnerId((current) => (current === runnerId ? null : runnerId));
  }

  return (
    <main className="flex h-full w-full flex-col bg-slate-50 text-slate-900 lg:flex-row">
      <RunnerMap
        runners={runners}
        selectedRunner={selectedRunner}
        selectedRunnerId={selectedRunnerId}
        runnerPaths={runnerPaths}
      />
      <Sidebar
        selectedRunnerId={selectedRunnerId}
        error={error}
        pathError={pathError}
        rankings={rankings}
        onSelectRunner={toggleSelectedRunner}
      />
    </main>
  );
}
