import Sidebar from "@/components/Sidebar";

const courses = [
  ["🐍", "Python for Beginners", "Learn Python from scratch with hands-on projects.", "6 lessons"],
  ["</>", "Web Development", "Build modern websites with HTML, CSS and JavaScript.", "10 lessons"],
  ["⚙️", "Data Structures & Algorithms", "Master the fundamentals of problem solving.", "12 lessons"],
  ["🧠", "Machine Learning", "Explore AI and build intelligent systems.", "10 lessons"],
];

export default function CoursesPage() {
  return (
    <div className="flex min-h-screen bg-slate-50 text-slate-950">
      <Sidebar />
      <main className="w-full p-6 lg:p-10">
        <div className="mx-auto max-w-6xl">
          <h1 className="text-3xl font-bold">Course Catalog</h1>
          <p className="mt-2 text-slate-500">Explore our programming courses and start your journey.</p>
          <div className="mt-6 flex flex-wrap gap-2">
            {["All", "Beginner", "Intermediate", "Advanced"].map((x, i) => <button key={x} className={`rounded-full px-4 py-2 text-sm ${i===0?"bg-indigo-600 text-white":"bg-white border border-slate-200"}`}>{x}</button>)}
          </div>
          <div className="mt-8 grid gap-5 md:grid-cols-2">
            {courses.map(([icon, title, desc, lessons]) => (
              <article key={title} className="rounded-2xl border border-slate-200 bg-white p-6 shadow-sm">
                <div className="text-3xl">{icon}</div>
                <h2 className="mt-4 text-xl font-bold">{title}</h2>
                <p className="mt-2 text-sm leading-6 text-slate-500">{desc}</p>
                <p className="mt-4 text-xs text-slate-400">{lessons} • Beginner</p>
                <a href="/lesson" className="mt-6 inline-block w-full rounded-xl bg-indigo-600 px-4 py-3 text-center font-semibold text-white">View Course</a>
              </article>
            ))}
          </div>
        </div>
      </main>
    </div>
  );
}
