import { Container, getContainer } from "@cloudflare/containers";

// One container instance runs relay.py (ffmpeg + numpy + yt-dlp). The Worker just forwards every request to it.
export class Relay extends Container<{ AIRWAVE_TOKEN?: string }> {
  defaultPort = 8787;
  sleepAfter = "15m"; // stops when idle, wakes on the next request (cold start takes a few seconds)

  constructor(ctx: DurableObjectState, env: { AIRWAVE_TOKEN?: string }) {
    super(ctx, env);
    this.envVars = { AIRWAVE_TOKEN: env.AIRWAVE_TOKEN ?? "" };
  }
}

export default {
  async fetch(request: Request, env: { RELAY: DurableObjectNamespace<Relay> }): Promise<Response> {
    return getContainer(env.RELAY).fetch(request);
  },
};
