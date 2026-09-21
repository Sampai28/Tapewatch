import { describe, expect, it, vi } from "vitest";

import { api, ApiError } from "./api";

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json" },
  });
}

describe("api", () => {
  it("turns an RFC 7807 problem into one readable error", () => {
    const fetchMock = vi.fn().mockResolvedValue(
      new Response(
        JSON.stringify({
          type: "about:blank",
          title: "No such alert",
          status: 404,
          detail: "alert 99 is not in the store",
        }),
        { status: 404, headers: { "Content-Type": "application/problem+json" } },
      ),
    );
    vi.stubGlobal("fetch", fetchMock);

    return api.alert(99).then(
      () => expect.unreachable("should have rejected"),
      (e: unknown) => {
        expect(e).toBeInstanceOf(ApiError);
        expect((e as ApiError).status).toBe(404);
        expect((e as ApiError).message).toBe("No such alert: alert 99 is not in the store");
      },
    );
  });

  it("reports an unreachable server rather than throwing a raw TypeError", async () => {
    vi.stubGlobal("fetch", vi.fn().mockRejectedValue(new TypeError("failed to fetch")));
    await expect(api.summary()).rejects.toThrowError(/cannot reach the API/);
  });

  it("omits empty and false query parameters", async () => {
    const fetchMock = vi.fn().mockResolvedValue(jsonResponse({ alerts: [] }));
    vi.stubGlobal("fetch", fetchMock);

    await api.alerts({ detector: "spoofing", symbol: "", tuned: false, limit: 50 });
    const url = fetchMock.mock.calls[0]?.[0] as string;
    expect(url).toContain("detector=spoofing");
    expect(url).toContain("limit=50");
    expect(url).not.toContain("symbol=");
    expect(url).not.toContain("tuned=");
  });

  it("encodes highlighted order ids as a comma list", async () => {
    const fetchMock = vi.fn().mockResolvedValue(jsonResponse({ frames: [] }));
    vi.stubGlobal("fetch", fetchMock);

    await api.replay({ symbol: "TWX", from_ts: 1, to_ts: 2, order_ids: [7, 8, 9] });
    const url = decodeURIComponent(fetchMock.mock.calls[0]?.[0] as string);
    expect(url).toContain("order_ids=7,8,9");
  });

  it("sends triage state as JSON with defaults filled in", async () => {
    const fetchMock = vi.fn().mockResolvedValue(jsonResponse({ alert_id: 1 }));
    vi.stubGlobal("fetch", fetchMock);

    await api.setAlertState(1, { status: "closed" });
    const init = fetchMock.mock.calls[0]?.[1] as RequestInit;
    expect(init.method).toBe("POST");
    expect(JSON.parse(String(init.body))).toEqual({
      status: "closed",
      disposition: "",
      note: "",
    });
  });
});
