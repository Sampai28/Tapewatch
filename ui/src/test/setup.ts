// Vitest setup. jsdom has no fetch and no ResizeObserver; the components
// under test use the former through src/lib/api.ts, which the tests stub per
// case, so all that is needed here is a definition to spy on.

import { afterEach, vi } from "vitest";

if (!globalThis.fetch) {
  globalThis.fetch = (() =>
    Promise.reject(new Error("fetch not stubbed in this test"))) as typeof fetch;
}

afterEach(() => {
  vi.restoreAllMocks();
});
