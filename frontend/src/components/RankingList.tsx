import type { RunnerRanking } from '../types/runner';
import { formatTime } from '../utils/time';

type RankingListProps = {
  rankings: RunnerRanking[];
  selectedRunnerId: string | null;
  onSelectRunner: (runnerId: string) => void;
};

export function RankingList({
  rankings,
  selectedRunnerId,
  onSelectRunner,
}: RankingListProps) {
  return (
    <section>
      <div className="mb-3 flex items-center justify-between px-1">
        <h3 className="text-sm font-bold text-slate-900">Runner Board</h3>
        <span className="rounded-full bg-slate-100 px-2.5 py-1 text-xs font-bold text-slate-500">
          {rankings.length}
        </span>
      </div>
      {rankings.length === 0 ? (
        <p className="rounded-lg border border-dashed border-slate-300 bg-white p-4 text-sm text-slate-500">
          No route data yet.
        </p>
      ) : (
        <ol className="flex flex-col gap-2">
          {rankings.slice(0, 5).map((ranking) => {
            const selected = ranking.runner_id === selectedRunnerId;
            const emergency = isEmergencyStatus(ranking.status);

            return (
            <li key={ranking.runner_id}>
              <div
                className={
                  emergency
                    ? selected
                      ? 'rounded-lg border border-red-500 bg-red-50'
                      : 'rounded-lg border border-red-200 bg-white transition hover:border-red-500 hover:bg-red-50'
                    : selected
                      ? 'rounded-lg border border-emerald-500 bg-emerald-50'
                      : 'rounded-lg border border-slate-200 bg-white transition hover:border-emerald-500 hover:bg-emerald-50'
                }
              >
                <button
                  type="button"
                  className="flex w-full items-center justify-between px-4 py-3 text-left"
                  onClick={() => onSelectRunner(ranking.runner_id)}
                >
                  <div className="flex items-center gap-3">
                    <span
                      className={
                        emergency
                          ? 'flex h-8 w-8 items-center justify-center rounded-full bg-red-600 text-sm font-bold text-white'
                          : 'flex h-8 w-8 items-center justify-center rounded-full bg-slate-900 text-sm font-bold text-white'
                      }
                    >
                      {ranking.rank}
                    </span>
                    <div>
                      <div className="flex flex-wrap items-center gap-2">
                        <p className="text-sm font-bold text-slate-900">
                          Runner #{ranking.runner_id}
                        </p>
                        {emergency && (
                          <span className="rounded-full bg-red-100 px-2 py-0.5 text-[9px] font-bold uppercase tracking-wide text-red-700">
                            Emergency
                          </span>
                        )}
                      </div>
                      <p className="text-xs text-slate-500">
                        {formatDistance(ranking.distance_m)}
                      </p>
                    </div>
                  </div>
                  <span
                    className={
                      emergency
                        ? 'min-w-[86px] rounded-lg bg-red-100 px-2.5 py-1.5 text-right'
                        : 'min-w-[74px] rounded-lg bg-white/80 px-2.5 py-1.5 text-right'
                    }
                  >
                    {emergency ? (
                      <span className="block text-[10px] font-bold uppercase tracking-wide text-red-700">
                        Alert
                      </span>
                    ) : (
                      <>
                        <span className="block text-[9px] font-bold uppercase tracking-wide text-emerald-700/70">
                          Pace
                        </span>
                        <span className="block text-sm font-bold leading-tight text-emerald-700">
                          {formatPace(ranking.pace)}
                        </span>
                      </>
                    )}
                  </span>
                </button>
                {selected && (
                  <div
                    className={
                      emergency
                        ? 'grid grid-cols-2 gap-2 border-t border-red-200 px-4 pb-4 pt-3 sm:grid-cols-4'
                        : 'grid grid-cols-2 gap-2 border-t border-emerald-200 px-4 pb-4 pt-3 sm:grid-cols-4'
                    }
                  >
                    <RankingMetric label="Battery" value={`${ranking.battery ?? '-'}%`} />
                    <RankingMetric label="Pace" value={formatPace(ranking.pace)} />
                    <RankingMetric label="Status" value={formatStatus(ranking.status)} />
                    <RankingMetric
                      label="Update"
                      value={formatTime(ranking.last_updated)}
                    />
                  </div>
                )}
              </div>
            </li>
            );
          })}
        </ol>
      )}
    </section>
  );
}

function RankingMetric({ label, value }: { label: string; value: string }) {
  return (
    <div className="flex min-h-[70px] flex-col items-center justify-center rounded-md bg-white/80 p-2 text-center">
      <p className="text-[9px] font-bold uppercase tracking-wide text-slate-500">
        {label}
      </p>
      <p className="mt-1 whitespace-nowrap text-sm font-bold text-slate-900">
        {value}
      </p>
    </div>
  );
}

function formatPace(pace: number | null) {
  if (pace === null) {
    return '-';
  }

  const totalSeconds = Math.max(0, Math.round(pace));
  const minutes = Math.floor(totalSeconds / 60);
  const seconds = totalSeconds % 60;
  return `${minutes}:${seconds.toString().padStart(2, '0')}`;
}

function formatDistance(distanceM: number) {
  if (distanceM >= 1000) {
    return `${(distanceM / 1000).toFixed(2)} km`;
  }
  return `${Math.round(distanceM)} m`;
}

function isEmergencyStatus(status: string) {
  return status.toLowerCase() === 'emergency';
}

function formatStatus(status: string) {
  return status || '-';
}
