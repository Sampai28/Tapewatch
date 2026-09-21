import { fireEvent, render, screen } from "@testing-library/react";
import { describe, expect, it, vi } from "vitest";

import { alertDetail } from "../test/fixtures";
import { AlertDetail } from "./AlertDetail";

describe("AlertDetail", () => {
  it("shows each signal's contribution, strongest first", () => {
    render(
      <AlertDetail alert={alertDetail()} originTs={0} busy={false} onSetState={() => {}} />,
    );
    const rows = screen.getAllByRole("row");
    // Header plus five signals, in the signals table; the strongest
    // contribution (size: 0.9 x 0.28) must lead so an analyst reads why it
    // fired rather than hunting for it.
    const first = rows[1]?.textContent ?? "";
    expect(first).toContain("size");
  });

  it("explains a degraded alert instead of only showing a lower number", () => {
    render(
      <AlertDetail
        alert={alertDetail({ degraded: true, score: 0.8, confidence: 0.48 })}
        originTs={0}
        busy={false}
        onSetState={() => {}}
      />,
    );
    expect(screen.getByText(/feed was damaged/i)).toBeTruthy();
    expect(screen.getByText(/cut for feed damage/i)).toBeTruthy();
  });

  it("labels ground truth as something a real desk would not have", () => {
    render(
      <AlertDetail alert={alertDetail()} originTs={0} busy={false} onSetState={() => {}} />,
    );
    expect(screen.getByText(/Only a laboratory has this/i)).toBeTruthy();
  });

  it("passes the typed disposition and note through to the handler", () => {
    const onSetState = vi.fn();
    render(
      <AlertDetail alert={alertDetail()} originTs={0} busy={false} onSetState={onSetState} />,
    );
    fireEvent.change(screen.getByLabelText("disposition"), {
      target: { value: "market making" },
    });
    fireEvent.change(screen.getByLabelText("notes"), { target: { value: "two-sided" } });
    fireEvent.click(screen.getByRole("button", { name: /close/i }));
    expect(onSetState).toHaveBeenCalledWith("closed", "market making", "two-sided");
  });

  it("renders evidence in the right units", () => {
    render(
      <AlertDetail alert={alertDetail()} originTs={0} busy={false} onSetState={() => {}} />,
    );
    expect(screen.getByText("420 ms")).toBeTruthy();
    expect(screen.getByText("240")).toBeTruthy();
  });

  it("disables triage while a request is in flight", () => {
    render(
      <AlertDetail alert={alertDetail()} originTs={0} busy onSetState={() => {}} />,
    );
    expect(screen.getByRole("button", { name: /close/i }).hasAttribute("disabled")).toBe(true);
  });
});
