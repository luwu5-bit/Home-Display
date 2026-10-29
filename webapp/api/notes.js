// POST /api/notes {author, text}: Nachricht anpinnen
// DELETE /api/notes?id=...: Nachricht löschen
import { allow } from "../lib/auth.js";
import { load, save, cleanText, newId, NAMES, LIMITS } from "../lib/store.js";

export default async function handler(req, res) {
  if (!allow(req, res)) return;
  const state = await load();

  if (req.method === "POST") {
    const text = cleanText(req.body?.text, LIMITS.noteLength);
    const author = NAMES.includes(req.body?.author) ? req.body.author : null;
    if (!text || !author) return res.status(400).json({ error: "Name und Text fehlen" });
    state.notes.push({ id: newId(), time: Math.floor(Date.now() / 1000), author, text });
    state.notes = state.notes.slice(-LIMITS.notesMax); // älteste fliegt raus
  } else if (req.method === "DELETE") {
    state.notes = state.notes.filter((note) => note.id !== req.query.id);
  } else {
    return res.status(405).json({ error: "Nur POST oder DELETE" });
  }

  await save(state);
  res.status(200).json(state);
}
