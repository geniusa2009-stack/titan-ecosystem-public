import "./globals.css";

export const metadata = {
  title: "CodeX",
  description: "Interactive programming education platform",
};

export default function RootLayout({ children }: { children: React.ReactNode }) {
  return (
    <html lang="en">
      <body>{children}</body>
    </html>
  );
}
