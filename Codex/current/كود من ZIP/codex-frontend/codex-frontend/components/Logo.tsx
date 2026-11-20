export default function Logo({ compact = false }: { compact?: boolean }) {
  return (
    <div className="flex items-center gap-3 font-semibold tracking-tight">
      <div className="relative grid h-9 w-9 place-items-center rounded-xl border border-white/20 bg-white/5">
        <span className="text-xl font-black">X</span>
      </div>
      {!compact && <span className="text-xl">CodeX</span>}
    </div>
  );
}
