import { fireEvent, render, screen } from "@testing-library/react";
import { describe, expect, it, vi } from "vitest";

import { alertRow } from "../test/fixtures";
import { AlertQueue } from "./AlertQueue";

describe("AlertQueue", () => {
  const rows = [
    alertRow({ alert_id: 1, participant: 1031, confidence: 0.81 }),
    alertRow({ alert_id: 2, participant: 1004, confidence: 0.44, degraded: true }),
    alertRow({ alert_id: 3, participant: 1002, confidence: 0.3, status: "closed" }),
  ];

  it("says so when the filters exclude everything", () => {
    render(
      <AlertQueue
        alerts={[]}
        selectedId={null}
        originTs={0}
        checked={new Set()}
        onSelect={() => {}}
        onToggleCheck={() => {}}
      />,
    );
    expect(screen.getByText(/No alerts match/i)).toBeTruthy();
  });

  it("renders one option per alert and marks the selected one", () => {
    render(
      <AlertQueue
        alerts={rows}
        selectedId={2}
        originTs={0}
        checked={new Set()}
        onSelect={() => {}}
        onToggleCheck={() => {}}
      />,
    );
    const options = screen.getAllByRole("option");
    expect(options).toHaveLength(3);
    expect(options[1]?.getAttribute("aria-selected")).toBe("true");
    expect(options[0]?.getAttribute("aria-selected")).toBe("false");
  });

  it("flags degraded alerts so a low confidence has a visible reason", () => {
    render(
      <AlertQueue
        alerts={rows}
        selectedId={null}
        originTs={0}
        checked={new Set()}
        onSelect={() => {}}
        onToggleCheck={() => {}}
      />,
    );
    expect(screen.getByTitle(/damaged feed/i)).toBeTruthy();
    expect(screen.getByText("closed")).toBeTruthy();
  });

  it("selects on click and on keyboard", () => {
    const onSelect = vi.fn();
    render(
      <AlertQueue
        alerts={rows}
        selectedId={null}
        originTs={0}
        checked={new Set()}
        onSelect={onSelect}
        onToggleCheck={() => {}}
      />,
    );
    fireEvent.click(screen.getAllByRole("option")[0]!);
    expect(onSelect).toHaveBeenCalledWith(1);
    fireEvent.keyDown(screen.getAllByRole("option")[2]!, { key: "Enter" });
    expect(onSelect).toHaveBeenCalledWith(3);
  });

  it("ticking an alert does not also select it", () => {
    // Otherwise building a case out of six alerts drags the detail pane
    // through all six on the way.
    const onSelect = vi.fn();
    const onToggleCheck = vi.fn();
    render(
      <AlertQueue
        alerts={rows}
        selectedId={null}
        originTs={0}
        checked={new Set()}
        onSelect={onSelect}
        onToggleCheck={onToggleCheck}
      />,
    );
    fireEvent.click(screen.getByLabelText("select alert 2"));
    expect(onToggleCheck).toHaveBeenCalledWith(2);
    expect(onSelect).not.toHaveBeenCalled();
  });
});
