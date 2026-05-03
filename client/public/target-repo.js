'use strict';

// State machine: UNKNOWN → CONFIRMED | INACTIVE; CONFIRMED → NEUTRALIZED; terminals: INACTIVE, NEUTRALIZED
const TARGET_STATE_TRANSITIONS = {
  TARGET_STATE_UNKNOWN:     new Set(['TARGET_STATE_CONFIRMED', 'TARGET_STATE_INACTIVE']),
  TARGET_STATE_CONFIRMED:   new Set(['TARGET_STATE_NEUTRALIZED']),
  TARGET_STATE_NEUTRALIZED: new Set(),
  TARGET_STATE_INACTIVE:    new Set(),
};

function isValidStateTransition(from, to) {
  if (!from || from === to) return true;
  return (TARGET_STATE_TRANSITIONS[from] ?? new Set()).has(to);
}

class TargetRepo {
  #targets = new Map();

  // Always use string keys so integer IDs from real targets and UUID strings
  // from mock targets are looked up consistently (HTML dataset is always string).
  #k(id) { return String(id); }

  get count() { return this.#targets.size; }

  get(id) { return this.#targets.get(this.#k(id)) ?? null; }

  list() { return [...this.#targets.values()]; }

  create(target) {
    const k = this.#k(target.id);
    if (this.#targets.has(k)) return false;
    this.#targets.set(k, target);
    return true;
  }

  // Returns true only if the target existed, the incoming is strictly newer,
  // and the state transition is valid.
  update(target) {
    const k = this.#k(target.id);
    const existing = this.#targets.get(k);
    if (!existing) return false;
    if (!this.#isNewer(target, existing)) return false;
    if (!isValidStateTransition(existing.state, target.state)) return false;
    this.#targets.set(k, target);
    return true;
  }

  // Create-or-update: skips when incoming is not newer or state transition is invalid.
  upsert(target) {
    const k = this.#k(target.id);
    const existing = this.#targets.get(k);
    if (existing) {
      if (!this.#isNewer(target, existing)) return false;
      if (!isValidStateTransition(existing.state, target.state)) return false;
    }
    this.#targets.set(k, target);
    return true;
  }

  delete(id) { return this.#targets.delete(this.#k(id)); }

  reset(targets) {
    this.#targets.clear();
    for (const t of targets) this.#targets.set(this.#k(t.id), t);
  }

  #isNewer(incoming, existing) {
    const a = (incoming.updated_date ?? {}).seconds ?? 0;
    const b = (existing.updated_date ?? {}).seconds ?? 0;
    return a > b;
  }
}
