import Link from "next/link";
import { Bot, Gamepad2, Code2, Award } from "lucide-react";
import Logo from "@/components/Logo";

const features = [
  [Bot, "Interactive Chat Lessons", "Pass before you watch."],
  [Gamepad2, "Gamification", "XP, coins, streaks & more."],
  [Code2, "Real Code Execution", "Run code inside your browser."],
  [Award, "Certificates", "Showcase your skills."],
];

export default function HomePage() {
  return (
    <main className="min-h-screen grid-bg">
      <header className="mx-auto flex max-w-7xl items-center justify-between px-6 py-5">
        <Logo />
        <nav className="hidden gap-8 text-sm text-slate-300 md:flex">
          <Link href="/">Home</Link>
          <Link href="/courses">Courses</Link>
          <Link href="/projects">Projects</Link>
          <a href="#about">About</a>
        </nav>
        <div className="flex gap-3">
          <Link href="/login" className="rounded-xl border border-white/10 px-4 py-2 text-sm">Login</Link>
          <Link href="/courses" className="accent rounded-xl px-4 py-2 text-sm font-semibold shadow-glow">Get Started</Link>
        </div>
      </header>

      <section className="mx-auto grid max-w-7xl items-center gap-14 px-6 pb-20 pt-16 lg:grid-cols-2 lg:pt-24">
        <div>
          <p className="mb-4 text-sm font-medium uppercase tracking-[.28em] text-indigo-300">Learn by doing</p>
          <h1 className="max-w-xl text-5xl font-black leading-[1.03] sm:text-6xl">
            Learn. Build. <span className="bg-gradient-to-r from-indigo-400 to-violet-400 bg-clip-text text-transparent">Level Up.</span>
          </h1>
          <p className="mt-6 max-w-xl text-lg leading-8 muted">
            Interactive programming education for Arab youth, with real projects,
            gamified progress and chat-based lessons that keep students active.
          </p>
          <div className="mt-8 flex flex-wrap gap-3">
            <Link href="/courses" className="accent rounded-xl px-6 py-3 font-semibold">Start Learning</Link>
            <Link href="/courses" className="rounded-xl border border-indigo-400/40 px-6 py-3 font-semibold">Explore Courses</Link>
          </div>
        </div>

        <div className="relative">
          <div className="absolute inset-0 -z-10 rounded-full bg-indigo-500/20 blur-3xl" />
          <div className="card rotate-2 rounded-3xl p-5 shadow-2xl">
            <div className="mb-4 flex items-center justify-between border-b border-white/10 pb-4">
              <div className="flex gap-2"><i className="h-2.5 w-2.5 rounded-full bg-red-400"/><i className="h-2.5 w-2.5 rounded-full bg-amber-300"/><i className="h-2.5 w-2.5 rounded-full bg-emerald-400"/></div>
              <span className="text-xs muted">codex.app/lesson</span>
            </div>
            <pre className="overflow-hidden rounded-2xl bg-black/30 p-5 text-sm leading-7 text-indigo-200">
{`function learn(topic) {
  const skill = practice(topic);
  return build(skill);
}

learn("JavaScript");`}
            </pre>
            <div className="mt-4 ml-auto w-72 rounded-2xl bg-indigo-600/20 p-4">
              <div className="mb-2 flex items-center gap-2"><Bot size={18}/><b className="text-sm">CodeX Coach</b></div>
              <p className="text-sm text-slate-300">Great! Now let’s solve a quick challenge together.</p>
            </div>
          </div>
        </div>
      </section>

      <section className="mx-auto grid max-w-7xl gap-4 px-6 pb-16 sm:grid-cols-2 lg:grid-cols-4">
        {features.map(([Icon, title, copy]) => (
          <div key={title as string} className="glass rounded-2xl p-5">
            <Icon className="mb-4 text-indigo-300" />
            <h3 className="font-semibold">{title as string}</h3>
            <p className="mt-1 text-sm muted">{copy as string}</p>
          </div>
        ))}
      </section>
    </main>
  );
}
