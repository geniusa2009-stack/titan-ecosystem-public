export default function StatCard({ label, value, note }: { label: string; value: string; note: string }) {
  return (
    <div className="card rounded-2xl p-5">
      <p className="text-sm muted">{label}</p>
      <div className="mt-2 text-3xl font-bold">{value}</div>
      <p className="mt-2 text-xs text-emerald-400">{note}</p>
    </div>
  );
}
