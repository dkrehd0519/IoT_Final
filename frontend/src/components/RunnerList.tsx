import type { RunnerLocation } from '../types/runner';

type RunnerListProps = {
  runners: RunnerLocation[];
  loading: boolean;
  selectedRunnerId: string | null;
  onSelectRunner: (runnerId: string) => void;
};

export function RunnerList({
  runners,
  loading,
  selectedRunnerId,
  onSelectRunner,
}: RunnerListProps) {
  return (
    <section className="flex min-h-0 flex-1 flex-col">
      <div className="mb-3 flex items-center justify-between px-1">
        <h3 className="text-sm font-bold text-slate-900">Active Runners</h3>
        <span className="rounded-full bg-slate-100 px-2.5 py-1 text-xs font-bold text-slate-500">
          {loading ? 'Syncing' : runners.length}
        </span>
      </div>

      {runners.length === 0 && !loading ? (
        <p className="rounded-lg border border-dashed border-slate-300 bg-white p-4 text-sm text-slate-500">
          No active runner data.
        </p>
      ) : (
        <ul className="custom-scrollbar flex min-h-0 flex-col gap-2 overflow-y-auto pr-1">
          {runners.map((runner) => {
            const selected = runner.runner_id === selectedRunnerId;
            return (
              <li key={runner.runner_id}>
                <button
                  type="button"
                  className={
                    selected
                      ? 'flex w-full items-center justify-between rounded-lg border border-emerald-500 bg-emerald-500 px-4 py-3 text-left text-white shadow-sm transition'
                      : 'flex w-full items-center justify-between rounded-lg border border-slate-200 bg-white px-4 py-3 text-left text-slate-900 transition hover:border-emerald-500 hover:bg-emerald-50 hover:text-emerald-700'
                  }
                  onClick={() => onSelectRunner(runner.runner_id)}
                >
                  <span>
                    <span className="block text-sm font-bold">
                      Runner #{runner.runner_id}
                    </span>
                    <span
                      className={
                        selected
                          ? 'text-xs text-white/75'
                          : 'text-xs text-slate-500'
                      }
                    >
                      {runner.status}
                    </span>
                  </span>
                  <span
                    className={
                      selected
                        ? 'rounded-full bg-white px-2 py-1 text-xs font-bold text-emerald-600'
                        : 'rounded-full bg-emerald-50 px-2 py-1 text-xs font-bold text-emerald-700'
                    }
                  >
                    {runner.battery ?? '-'}%
                  </span>
                </button>
              </li>
            );
          })}
        </ul>
      )}
    </section>
  );
}
