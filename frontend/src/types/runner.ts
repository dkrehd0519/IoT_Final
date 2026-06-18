export type RunnerLocation = {
  runner_id: string;
  latitude: number;
  longitude: number;
  pace: number | null;
  battery: number | null;
  status: string;
  received_at: string;
};

export type RunnerRanking = {
  rank: number;
  runner_id: string;
  distance_m: number;
  pace: number | null;
  battery: number | null;
  status: string;
  last_updated: string;
};
