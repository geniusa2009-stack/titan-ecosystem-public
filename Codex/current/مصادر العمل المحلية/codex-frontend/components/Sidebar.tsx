import Link from "next/link";
import { LayoutDashboard, BookOpen, FolderKanban, Trophy, Target, Settings } from "lucide-react";
import Logo from "./Logo";

const items = [
  ["Dashboard", "/dashboard", LayoutDashboard],
  ["My Courses", "/courses", BookOpen],
  ["Projects", "/projects", FolderKanban],
  ["Achievements", "/dashboard", Trophy],
  ["Quests", "/dashboard", Target],
  ["Settings", "/settings", Settings],
] as const;

export default function Sidebar() {
  return (
    <aside className="hidden min-h-screen w-64 shrink-0 border-r border-white/10 bg-[#07111f]/90 p-5 lg:block">
      <Logo />
      <nav className="mt-10 space-y-2">
        {items.map(([label, href, Icon], i) => (
          <Link
            key={label}
            href={href}
            className={`flex items-center gap-3 rounded-xl px-4 py-3 text-sm transition hover:bg-white/5 ${
              i === 0 ? "bg-indigo-600/20 text-indigo-300" : "text-slate-300"
            }`}
          >
            <Icon size={18} />
            {label}
          </Link>
        ))}
      </nav>
    </aside>
  );
}
