import type { RunnerLocation, RunnerRanking } from '../types/runner';

const apiBaseUrl = import.meta.env.VITE_API_BASE_URL.replace(/\/+$/, '');

export async function fetchRunners() {
  const response = await fetch(`${apiBaseUrl}/runners`);
  if (!response.ok) {
    throw new Error(`GET /runners failed: ${response.status}`);
  }
  return (await response.json()) as RunnerLocation[];
}

export async function fetchRunnerPath(runnerId: string) {
  const response = await fetch(`${apiBaseUrl}/runners/${runnerId}/path`);
  if (!response.ok) {
    throw new Error(`GET /runners/${runnerId}/path failed: ${response.status}`);
  }
  return (await response.json()) as RunnerLocation[];
}

export async function fetchRunnerRankings() {
  const response = await fetch(`${apiBaseUrl}/runners/rankings`);
  if (!response.ok) {
    throw new Error(`GET /runners/rankings failed: ${response.status}`);
  }
  return (await response.json()) as RunnerRanking[];
}
