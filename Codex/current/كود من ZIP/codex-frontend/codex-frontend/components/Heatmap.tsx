export default function Heatmap() {
  const cells = Array.from({ length: 84 }, (_, i) => (i * 7 + i * i) % 5);
  return (
    <div className="grid grid-cols-14 gap-1.5">
      {cells.map((v, i) => (
        <div
          key={i}
          className="aspect-square rounded-[4px]"
          style={{
            background:
              v === 0 ? "rgba(255,255,255,.05)" :
              v === 1 ? "rgba(16,185,129,.20)" :
              v === 2 ? "rgba(16,185,129,.38)" :
              v === 3 ? "rgba(16,185,129,.62)" :
                        "rgba(16,185,129,.92)"
          }}
        />
      ))}
    </div>
  );
}
