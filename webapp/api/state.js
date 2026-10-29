// GET /api/state: Einkaufsliste und Pinnwand, für die Handys und das Display
import { allow } from "../lib/auth.js";
import { load, NAMES, LIMITS } from "../lib/store.js";

export default async function handler(req, res) {
  if (req.method !== "GET") return res.status(405).json({ error: "Nur GET" });
  if (!allow(req, res, { display: true })) return;

  const state = await load();
  res.setHeader("Cache-Control", "no-store");
  res.status(200).json({ ...state, names: NAMES, limits: LIMITS });
}
