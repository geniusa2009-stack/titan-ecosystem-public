import Sidebar from "@/components/Sidebar";

const projects = [
  ["Weather App", "OpenWeather API"],
  ["Task Manager", "React local state"],
  ["AI Chatbot", "OpenAI integration"],
  ["3D Portfolio", "Three.js"],
  ["Game — Snake", "Canvas"],
  ["Expense Tracker", "Data visualization"],
];

export default function ProjectsPage() {
  return (
    <div className="flex min-h-screen bg-slate-50 text-slate-950">
      <Sidebar />
      <main className="w-full p-6 lg:p-10">
        <div className="mx-auto max-w-6xl">
          <div className="flex items-center justify-between">
            <div><h1 className="text-3xl font-bold">Project Showcase</h1><p className="mt-2 text-slate-500">Discover amazing projects from our community.</p></div>
            <button className="rounded-xl bg-indigo-600 px-4 py-3 text-sm font-semibold text-white">Submit Project</button>
          </div>
          <div className="mt-8 grid gap-5 md:grid-cols-2 xl:grid-cols-3">
            {projects.map(([title, tech], i)=>(
              <article key={title} className="overflow-hidden rounded-2xl border border-slate-200 bg-white">
                <div className="grid h-36 place-items-center bg-gradient-to-br from-slate-950 to-indigo-950 text-5xl">{["☁️","✅","🤖","🧊","🐍","📊"][i]}</div>
                <div className="p-5"><h2 className="font-bold">{title}</h2><p className="mt-1 text-sm text-slate-500">{tech}</p><div className="mt-4 text-xs text-slate-400">♡ {12+i*17} &nbsp; ◇ {3+i}</div></div>
              </article>
            ))}
          </div>
        </div>
      </main>
    </div>
  );
}
