import Link from "next/link";
import Logo from "@/components/Logo";

export default function LoginPage() {
  return (
    <main className="grid min-h-screen place-items-center bg-slate-50 px-6 text-slate-900">
      <section className="w-full max-w-md rounded-3xl border border-slate-200 bg-white p-8 shadow-xl">
        <div className="mb-8 flex justify-center text-slate-900"><Logo compact /></div>
        <h1 className="text-center text-3xl font-bold">Welcome back!</h1>
        <p className="mt-2 text-center text-sm text-slate-500">Sign in to continue your learning journey.</p>
        <div className="mt-8 space-y-3">
          <button className="w-full rounded-xl border border-slate-200 px-4 py-3 font-semibold hover:bg-slate-50">Continue with Google</button>
          <button className="w-full rounded-xl border border-slate-200 px-4 py-3 font-semibold hover:bg-slate-50">Continue with GitHub</button>
        </div>
        <p className="mt-6 text-center text-xs text-slate-400">OAuth only — no password needed</p>
        <Link href="/" className="mt-6 block text-center text-sm text-indigo-600">Back to home</Link>
      </section>
    </main>
  );
}
