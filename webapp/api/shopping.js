// POST /api/shopping {text}: Eintrag hinzufügen
// DELETE /api/shopping?id=...: Eintrag erledigt
import { allow } from "../lib/auth.js";
import { load, save, cleanText, newId, LIMITS } from "../lib/store.js";

export default async function handler(req, res) {
  if (!allow(req, res)) return;
  const state = await load();

  if (req.method === "POST") {
    const text = cleanText(req.body?.text, LIMITS.shoppingLength);
    if (!text) return res.status(400).json({ error: "Leerer Eintrag" });
    if (state.shopping.length >= LIMITS.shoppingMax) {
      return res.status(400).json({ error: `Höchstens ${LIMITS.shoppingMax} Einträge` });
    }
    state.shopping.push({ id: newId(), text });
  } else if (req.method === "DELETE") {
    state.shopping = state.shopping.filter((item) => item.id !== req.query.id);
  } else {
    return res.status(405).json({ error: "Nur POST oder DELETE" });
  }

  await save(state);
  res.status(200).json(state);
}
