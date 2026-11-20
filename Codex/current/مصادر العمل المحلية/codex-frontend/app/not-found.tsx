import Link from "next/link";
import Logo from "@/components/Logo";

export default function NotFound() {
  return (
    <main className="grid min-h-screen place-items-center px-6 text-center">
      <div>
        <div className="mb-12 flex justify-center"><Logo /></div>
        <div className="text-8xl font-black tracking-tight text-indigo-400">4<span className="mx-2 text-white">X</span>4</div>
        <h1 className="mt-6 text-2xl font-bold">Page not found</h1>
        <p className="mt-3 muted">The page you’re looking for doesn’t exist or has been moved.</p>
        <Link href="/" className="accent mt-7 inline-block rounded-xl px-5 py-3 font-semibold">Go Home</Link>
      </div>
    </main>
  );
}
