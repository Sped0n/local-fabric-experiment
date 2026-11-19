import { createSignal, Show } from "solid-js";
import { render } from "@solidjs/web";
import {
  token,
  setToken,
  link,
  notice,
  setNotice,
  rules,
  attempt,
  connect,
} from "./connection.js";
import { RuleEditor } from "./rules/RuleEditor.jsx";
import { DevicePanel } from "./DevicePanel.jsx";
import "./styles.css";

function App() {
  const [replacement, setReplacement] = createSignal("");
  const [rotating, setRotating] = createSignal(false);
  async function rotate() {
    if (
      rotating() ||
      !confirm(
        "Replace this board’s access token? Every client will be disconnected.",
      )
    )
      return;
    setRotating(true);
    try {
      const r = await fetch("/settings/token/rotate", {
        method: "POST",
        cache: "no-store",
        headers: { Authorization: "Bearer " + token() },
      });
      if (!r.ok) throw Error(r.status + " " + (await r.text()));
      setReplacement(await r.text());
      setToken("");
      setNotice("Token replaced. Copy the new token, then reconnect.");
    } finally {
      setRotating(false);
    }
  }
  connect();
  return (
    <>
      <header class="sticky top-0 z-10 flex min-h-13 items-center gap-2 sm:gap-3">
        <h1>Local Fabric</h1>
        <span class="board">ESP32-S31</span>
        <span class="connection">{token() ? link() : "Locked"}</span>
        <Show when={token()}>
          <nav class="ml-auto flex gap-3" aria-label="Control panels">
            <a href="#devices">Devices</a>
            <Show when={rules()}>
              <a href="#rules">Rules</a>
            </Show>
          </nav>
        </Show>
      </header>
      <output aria-live="polite">{notice()}</output>
      <Show when={replacement()}>
        <section>
          <h2>New access token</h2>
          <p>
            Copy this before dismissing. If you lose it, run{" "}
            <code>fabric token</code> on the board’s UART.
          </p>
          <input
            aria-label="New access token"
            readonly
            value={replacement()}
            onFocus={(e) => e.currentTarget.select()}
          />
          <button onClick={() => setReplacement("")}>Dismiss</button>
        </section>
      </Show>
      <Show
        when={token()}
        fallback={
          <section>
            <form
              onSubmit={(e) => {
                e.preventDefault();
                setNotice("");
                setReplacement("");
                setToken(
                  e.currentTarget.secret.value.trim().replace(/^Bearer /, ""),
                );
              }}
            >
              <label>
                Board access token (UART: fabric token)
                <input
                  type="password"
                  name="secret"
                  required
                  autocomplete="off"
                />
              </label>
              <button>Connect</button>
            </form>
          </section>
        }
      >
        <Show when={link() === "Live"}>
          <details>
            <summary>Settings</summary>
            <p>
              Rotate the shared access token for this board. You will need to
              update every client.
            </p>
            <button
              type="button"
              disabled={rotating()}
              onClick={() => attempt(rotate)}
            >
              Rotate access token
            </button>
          </details>
        </Show>
        <DevicePanel />
        <Show when={rules()}>
          <RuleEditor />
        </Show>
      </Show>
    </>
  );
}
render(App, document.getElementById("app"));
