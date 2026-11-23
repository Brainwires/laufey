// The B0 app: a column with a title and a pressable counter. Each press event from the host
// increments the counter and commits the whole tree again.
const { ops } = Deno.core;

const BUTTON = 3;
let count = 0;

function tree() {
  return {
    id: 1,
    type: "view",
    style: {
      flex: 1,
      flexDirection: "column",
      alignItems: "flex-start",
      padding: 24,
      gap: 16,
      backgroundColor: "#101418",
    },
    children: [
      {
        id: 2,
        type: "text",
        style: { color: "#e6e6e6", fontSize: 22 },
        text: "Laufey GPUI B0 spike",
      },
      {
        id: BUTTON,
        type: "pressable",
        style: { padding: 12, backgroundColor: "#2d6cdf", borderRadius: 6 },
        children: [
          { id: 4, type: "text", style: { color: "#ffffff" }, text: `Count: ${count}` },
        ],
      },
    ],
  };
}

function commit() {
  ops.op_gpui_commit(JSON.stringify(tree()));
}

(async () => {
  commit();
  for (;;) {
    // Resolves with null once the host has closed the event channel (window closed).
    const event = JSON.parse(await ops.op_gpui_next_event());
    if (event === null) break;
    if (event.type === "press" && event.id === BUTTON) {
      count += 1;
      commit();
    }
  }
})();
