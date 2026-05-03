'use strict';

class TargetRepo {
  #targets = new Map();

  get count() { return this.#targets.size; }

  get(id) { return this.#targets.get(id) ?? null; }

  list() { return [...this.#targets.values()]; }

  create(target) {
    if (this.#targets.has(target.id)) return false;
    this.#targets.set(target.id, target);
    return true;
  }

  // Returns true only if the target existed and the incoming is strictly newer.
  update(target) {
    const existing = this.#targets.get(target.id);
    if (!existing) return false;
    if (!this.#isNewer(target, existing)) return false;
    this.#targets.set(target.id, target);
    return true;
  }

  // Create-or-update; skips (returns false) when incoming is not newer than stored.
  upsert(target) {
    const existing = this.#targets.get(target.id);
    if (existing && !this.#isNewer(target, existing)) return false;
    this.#targets.set(target.id, target);
    return true;
  }

  delete(id) { return this.#targets.delete(id); }

  reset(targets) {
    this.#targets.clear();
    for (const t of targets) this.#targets.set(t.id, t);
  }

  #isNewer(incoming, existing) {
    const a = (incoming.updated_date ?? {}).seconds ?? 0;
    const b = (existing.updated_date ?? {}).seconds ?? 0;
    return a > b;
  }
}
