// Zugriff: Handys schicken das WG-Passwort, das Display seinen eigenen Schlüssel
import { createHash, timingSafeEqual } from "node:crypto";

function same(given, expected) {
  if (!expected) return false; // ohne gesetztes Passwort kommt niemand rein
  const a = createHash("sha256").update(String(given ?? "")).digest();
  const b = createHash("sha256").update(expected).digest();
  return timingSafeEqual(a, b);
}

export function isPhone(req) {
  return same(req.headers["x-wg-password"], process.env.WG_PASSWORD);
}

export function isDisplay(req) {
  return same(req.headers["x-display-key"], process.env.DISPLAY_KEY);
}

// Antwortet mit 401 und gibt false zurück, wenn der Zugriff nicht erlaubt ist
export function allow(req, res, { display = false } = {}) {
  if (isPhone(req) || (display && isDisplay(req))) return true;
  res.status(401).json({ error: "Falsches Passwort" });
  return false;
}
