import Sidebar from "@/components/Sidebar";

export default function SettingsPage() {
  return (
    <div className="flex min-h-screen bg-slate-50 text-slate-950">
      <Sidebar />
      <main className="w-full p-6 lg:p-10">
        <div className="mx-auto max-w-4xl">
          <h1 className="text-3xl font-bold">Profile Settings</h1>
          <p className="mt-2 text-slate-500">Manage your account and preferences.</p>
          <section className="mt-8 rounded-2xl border border-slate-200 bg-white p-6">
            <div className="flex items-center gap-4"><div className="h-16 w-16 rounded-full bg-gradient-to-br from-indigo-400 to-violet-600"/><div><h2 className="font-bold">Ahmed Mohamed</h2><p className="text-sm text-slate-500">Student</p></div></div>
            <div className="mt-7 grid gap-5 md:grid-cols-2">
              {[["Full Name","Ahmed Mohamed"],["Email","ahmed@example.com"],["Role","Student"],["Language","Arabic / English"]].map(([l,v])=>(
                <label key={l} className="text-sm"><span className="mb-2 block text-slate-500">{l}</span><input defaultValue={v} className="w-full rounded-xl border border-slate-200 px-4 py-3 outline-none focus:border-indigo-500"/></label>
              ))}
            </div>
            <button className="mt-6 rounded-xl bg-indigo-600 px-5 py-3 font-semibold text-white">Save Changes</button>
          </section>
        </div>
      </main>
    </div>
  );
}
