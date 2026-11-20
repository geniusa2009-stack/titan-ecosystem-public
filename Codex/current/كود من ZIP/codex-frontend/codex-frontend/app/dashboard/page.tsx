import Sidebar from "@/components/Sidebar";
import StatCard from "@/components/StatCard";
import Heatmap from "@/components/Heatmap";
import { Award, Flame, BookOpen } from "lucide-react";

const courses = [
  ["Python for Beginners", 65],
  ["Web Development", 35],
  ["Data Structures & Algorithms", 12],
];

export default function DashboardPage() {
  return (
    <div className="flex min-h-screen">
      <Sidebar />
      <main className="w-full p-5 sm:p-8">
        <div className="mx-auto max-w-6xl">
          <div className="flex items-center justify-between">
            <div>
              <p className="muted">Student Dashboard</p>
              <h1 className="mt-1 text-3xl font-bold">Good afternoon, Ahmed! 👋</h1>
            </div>
            <div className="h-10 w-10 rounded-full bg-gradient-to-br from-indigo-400 to-violet-600" />
          </div>

          <div className="mt-8 grid gap-4 md:grid-cols-3">
            <StatCard label="XP" value="2,450" note="+120 today" />
            <StatCard label="Coins" value="380" note="+20 today" />
            <StatCard label="Streak" value="7 days" note="Best: 12 days" />
          </div>

          <div className="mt-6 grid gap-6 xl:grid-cols-[1.5fr_.8fr]">
            <section className="card rounded-2xl p-6">
              <div className="mb-5 flex items-center justify-between">
                <h2 className="font-semibold">Activity Heatmap</h2>
                <span className="text-xs muted">Last 12 weeks</span>
              </div>
              <Heatmap />
            </section>

            <section className="card rounded-2xl p-6">
              <h2 className="font-semibold">Recent Achievements</h2>
              <div className="mt-5 space-y-4">
                {[["First Lesson", Award], ["7 Day Streak", Flame], ["Quiz Master", BookOpen]].map(([x, Icon]: any) => (
                  <div key={x} className="flex items-center gap-3">
                    <div className="grid h-10 w-10 place-items-center rounded-xl bg-indigo-500/15 text-indigo-300"><Icon size={19}/></div>
                    <div><p className="text-sm font-medium">{x}</p><p className="text-xs muted">Achievement unlocked</p></div>
                  </div>
                ))}
              </div>
            </section>
          </div>

          <section className="card mt-6 rounded-2xl p-6">
            <div className="mb-5 flex items-center justify-between"><h2 className="font-semibold">My Courses</h2><a className="text-sm text-indigo-300" href="/courses">View all</a></div>
            <div className="space-y-5">
              {courses.map(([name, progress]) => (
                <div key={name as string}>
                  <div className="mb-2 flex justify-between text-sm"><span>{name}</span><span className="muted">{progress}%</span></div>
                  <div className="h-2 overflow-hidden rounded-full bg-white/5"><div className="h-full rounded-full bg-indigo-500" style={{width: `${progress}%`}} /></div>
                </div>
              ))}
            </div>
          </section>
        </div>
      </main>
    </div>
  );
}
