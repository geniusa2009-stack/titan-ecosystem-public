"use client";
import { useState } from "react";
import { Bot, Heart, Lightbulb, Send, CheckCircle2 } from "lucide-react";

export default function LessonPage() {
  const [selected, setSelected] = useState<string | null>(null);
  const [done, setDone] = useState(false);
  const answers = ["var x = 5", "x = 5", "int x = 5", "create x = 5"];

  return (
    <main className="min-h-screen p-5 lg:p-8">
      <div className="mx-auto max-w-6xl overflow-hidden rounded-3xl border border-white/10 bg-[#081423]">
        <div className="flex items-center justify-between border-b border-white/10 p-5">
          <div>
            <p className="text-xs muted">Python Playlist › Lesson 3</p>
            <h1 className="mt-1 font-semibold">Variables and Data Types</h1>
          </div>
          <div className="flex items-center gap-2 text-red-400">{[1,2,3].map(i=><Heart key={i} size={18} fill="currentColor"/>)} <span className="ml-2 text-xs muted">3/3</span></div>
        </div>

        <div className="grid lg:grid-cols-[1.4fr_.6fr]">
          <section className="p-5 lg:p-7">
            <div className="mb-5 flex gap-2 text-sm">
              {["Chat", "Reading", "Quiz", "Code Editor"].map((x,i)=><button key={x} className={`rounded-xl px-4 py-2 ${i===0?"bg-indigo-600":"bg-white/5 text-slate-400"}`}>{x}</button>)}
            </div>

            <div className="space-y-4">
              <div className="flex gap-3">
                <div className="grid h-10 w-10 shrink-0 place-items-center rounded-full bg-indigo-500/15 text-indigo-300"><Bot size={20}/></div>
                <div className="max-w-xl rounded-2xl rounded-tl-sm bg-white/8 p-4 text-sm leading-6">
                  Hi! 👋 Let’s learn about variables in Python. Variables are used to store data values.
                </div>
              </div>

              <div className="ml-12 max-w-xl rounded-2xl border border-indigo-500/20 bg-indigo-500/10 p-5">
                <p className="font-medium">What is the correct way to create a variable in Python?</p>
                <div className="mt-4 space-y-2">
                  {answers.map((a, i) => {
                    const good = done && i === 1;
                    return (
                      <button key={a} onClick={()=>!done&&setSelected(a)} className={`flex w-full items-center gap-3 rounded-xl border p-3 text-left text-sm ${
                        good ? "border-emerald-400/40 bg-emerald-500/15" :
                        selected===a ? "border-indigo-400 bg-indigo-500/20" :
                        "border-white/10 bg-black/10"
                      }`}>
                        <span className="grid h-6 w-6 place-items-center rounded-full bg-white/5">{String.fromCharCode(65+i)}</span>{a}
                        {good && <CheckCircle2 className="ml-auto text-emerald-400" size={18}/>}
                      </button>
                    )
                  })}
                </div>
                <div className="mt-4 flex items-center justify-between">
                  <button className="flex items-center gap-2 text-xs text-amber-300"><Lightbulb size={14}/> Hint</button>
                  <button disabled={!selected} onClick={()=>setDone(true)} className="rounded-xl bg-indigo-600 px-4 py-2 text-sm font-semibold disabled:opacity-40">Check Answer</button>
                </div>
              </div>
            </div>

            <div className="mt-6 flex rounded-xl border border-white/10 bg-white/5 p-2">
              <input className="w-full bg-transparent px-3 text-sm outline-none" placeholder="Type your message..." />
              <button className="grid h-10 w-10 place-items-center rounded-lg bg-indigo-600"><Send size={17}/></button>
            </div>
          </section>

          <aside className="border-t border-white/10 p-6 lg:border-l lg:border-t-0">
            <h2 className="font-semibold">Scenario Progress</h2>
            <div className="mt-5 space-y-5">
              {["Introduction","Question 1","Question 2","Key Point","Completion"].map((x,i)=>(
                <div key={x} className="flex items-center gap-3">
                  <div className={`grid h-7 w-7 place-items-center rounded-full text-xs ${i<2?"bg-emerald-500 text-white":i===2?"bg-indigo-600":"bg-white/5 text-slate-500"}`}>{i+1}</div>
                  <span className={`text-sm ${i<3?"":"muted"}`}>{x}</span>
                </div>
              ))}
            </div>
          </aside>
        </div>
      </div>
    </main>
  );
}
