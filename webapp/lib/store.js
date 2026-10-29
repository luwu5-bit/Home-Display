// Datenspeicher: Upstash Redis auf Vercel, lokal (ohne Zugangsdaten) nur im Arbeitsspeicher
import { Redis } from "@upstash/redis";

export const NAMES = ["Luca", "Arthur", "Daniel"];
export const LIMITS = {
  shoppingMax: 20, // Einträge
  shoppingLength: 30, // Zeichen pro Eintrag
  notesMax: 10, // Nachrichten, sonst fliegt die älteste raus
  noteLength: 100, // Zeichen pro Nachricht
  noteDays: 7, // danach verschwindet eine Nachricht
};

const KEY = "wg:state";

// Die Vercel Marketplace Integration setzt KV_*, Upstash direkt setzt UPSTASH_*
const url = process.env.KV_REST_API_URL || process.env.UPSTASH_REDIS_REST_URL;
const token = process.env.KV_REST_API_TOKEN || process.env.UPSTASH_REDIS_REST_TOKEN;
const redis = url && token ? new Redis({ url, token }) : null;
let memory = null;

function empty() {
  return { shopping: [], notes: [] };
}

// Alte Nachrichten entfernen
function tidy(state) {
  const s = state && Array.isArray(state.shopping) && Array.isArray(state.notes) ? state : empty();
  const oldest = Date.now() / 1000 - LIMITS.noteDays * 86400;
  s.notes = s.notes.filter((n) => n.time >= oldest);
  return s;
}

export async function load() {
  return tidy(redis ? await redis.get(KEY) : memory);
}

export async function save(state) {
  if (redis) await redis.set(KEY, state);
  else memory = state;
}

// Text aufräumen: keine Zeilenumbrüche, auf maximale Länge gekürzt
export function cleanText(text, maxLength) {
  return Array.from(String(text ?? "").replace(/\s+/g, " ").trim())
    .slice(0, maxLength)
    .join("");
}

export function newId() {
  return crypto.randomUUID().slice(0, 8);
}
