import type { RunnerRanking } from '../types/runner';
import { RankingList } from './RankingList';

type SidebarProps = {
  selectedRunnerId: string | null;
  error: string | null;
  pathError: string | null;
  rankings: RunnerRanking[];
  onSelectRunner: (runnerId: string) => void;
};

export function Sidebar({
  selectedRunnerId,
  error,
  pathError,
  rankings,
  onSelectRunner,
}: SidebarProps) {
  return (
    <aside className="z-40 flex h-full min-h-0 flex-col border-t border-slate-200 bg-slate-50 lg:w-[420px] lg:shrink-0 lg:border-l lg:border-t-0">
      <div className="flex min-h-0 flex-1 flex-col gap-5 p-6">
        {error && (
          <p className="rounded-lg bg-red-50 px-3 py-2 text-sm text-red-700">
            {error}
          </p>
        )}
        {pathError && (
          <p className="rounded-lg bg-red-50 px-3 py-2 text-sm text-red-700">
            {pathError}
          </p>
        )}

        <RankingList
          rankings={rankings}
          selectedRunnerId={selectedRunnerId}
          onSelectRunner={onSelectRunner}
        />
      </div>
    </aside>
  );
}
