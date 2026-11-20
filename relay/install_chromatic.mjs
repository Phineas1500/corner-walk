import { appendFile } from "node:fs/promises";
import { join } from "node:path";
import { pathToFileURL } from "node:url";

const [packageRoot, workspaceRoot, romPath, targetJson, jobId, evidencePath, mode] = process.argv.slice(2);
const target = JSON.parse(targetJson);
const { openDeviceMcpSession } = await import(pathToFileURL(join(packageRoot, "scripts/device-mcp.mjs")));
const session = await openDeviceMcpSession({ packageRoot, workspaceRoot, requestTimeoutMs: 120000 });

async function call(tool, args) {
  const operation = await session.call(tool, args);
  await appendFile(evidencePath, JSON.stringify(operation) + "\n", { mode: 0o600 });
  return operation;
}

try {
  const scan = await call("device", { command: "list_devices", requestId: "walk-scan-" + jobId });
  const listing = scan.result?.structuredContent;
  const devices = listing?.result?.devices ?? [];
  const device = devices.length === 1 && devices[0];
  const matches = device && device.player === target.player &&
    device.location?.bus_id === target.location.bus_id &&
    JSON.stringify(device.location.port_chain) === JSON.stringify(target.location.port_chain);
  if (scan.completion !== "observed" || listing?.state !== "succeeded" || !matches ||
      listing.result.conflicts?.length || listing.result.unmatched?.length) {
    process.exitCode = 2;
  } else if (romPath !== "--check") {
    const inspected = await call("rom_inspect", { romPath });
    const info = inspected.result?.structuredContent;
    if (inspected.result?.isError || !info?.valid || !info?.path) {
      process.exitCode = 2;
    } else if (mode === "install") {
      const flash = await call("flash", {
        confirm: true, deviceToken: device.deviceToken, romPath: info.path,
        expectedSha256: info.sha256, expectedSizeBytes: info.sizeBytes, requestId: "walk-flash-" + jobId,
      });
      const outcome = flash.result?.structuredContent;
      if (flash.completion !== "observed" || outcome?.cartridgeWrite === "outcome-unverified") process.exitCode = 4;
      else if (outcome?.state !== "succeeded") process.exitCode = 3;
    }
  }
} finally {
  let closing = await session.close();
  while (closing.closeDeferred && !session.state().transportClosed) {
    await new Promise(resolve => setTimeout(resolve, 1500));
    const recovered = await session.reconcile();
    await appendFile(evidencePath, JSON.stringify(recovered) + "\n", { mode: 0o600 });
    closing = await session.close();
  }
}
