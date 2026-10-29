// Lokaler Testserver: npm run dev, dann http://localhost:3000 (Passwort "test", Display-Schlüssel "display")
// Ahmt Vercel nach: Dateien aus public/ und die Funktionen aus api/. Daten nur im Arbeitsspeicher.
import { createServer } from "node:http";
import { readFile } from "node:fs/promises";

process.env.WG_PASSWORD ??= "test";
process.env.DISPLAY_KEY ??= "display";

const routes = {
  "/api/state": (await import("./api/state.js")).default,
  "/api/shopping": (await import("./api/shopping.js")).default,
  "/api/notes": (await import("./api/notes.js")).default,
};

createServer(async (req, res) => {
  const url = new URL(req.url, "http://localhost");
  const handler = routes[url.pathname];

  if (!handler) {
    const html = await readFile(new URL("./public/index.html", import.meta.url));
    res.writeHead(200, { "Content-Type": "text/html; charset=utf-8" });
    return res.end(html);
  }

  let body = "";
  for await (const chunk of req) body += chunk;
  req.body = body ? JSON.parse(body) : undefined;
  req.query = Object.fromEntries(url.searchParams);
  res.status = (code) => ((res.statusCode = code), res);
  res.json = (data) => {
    res.setHeader("Content-Type", "application/json; charset=utf-8");
    res.end(JSON.stringify(data));
  };
  await handler(req, res);
}).listen(3000, () => console.log("WG Display läuft auf http://localhost:3000"));
