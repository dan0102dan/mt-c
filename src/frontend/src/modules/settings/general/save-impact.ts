import type { GeneralSettings } from "./config";

/** Immediate UI warning from normalized values already loaded with the form.
 * PUT remains authoritative: allow_dns_restart=false forbids an unannounced
 * restart if the server's active runtime no longer matches this snapshot. */
export function requiresDnsRestart(current: GeneralSettings, next: GeneralSettings): boolean {
  const restartValues = (settings: GeneralSettings) => [
    settings.httpWeb.host.address,
    settings.httpWeb.host.port,
    settings.dnsProxy.host.address,
    settings.dnsProxy.host.port,
    settings.dnsProxy.upstream.address,
    settings.dnsProxy.upstream.port,
    settings.dnsProxy.disableRemap53,
    settings.dnsProxy.maxIdleConns,
    settings.dnsProxy.maxConcurrent,
    settings.dnsProxy.timeoutMs,
    settings.netfilter.disableIPv4,
    settings.netfilter.disableIPv6,
    settings.link,
  ];
  return JSON.stringify(restartValues(current)) !== JSON.stringify(restartValues(next));
}
