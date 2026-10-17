# DNS query history in WebUI

Available in **Settings → DNS queries** on the C daemon of mt-c.

- Capture occurs after validating incoming UDP/TCP DNS requests, before forwarding or generating a local fake-PTR response.
- Each question produces a row: timestamp (Unix milliseconds), client IP (without source port), domain, DNS query type, and transport protocol. A query with multiple questions produces multiple rows.
- The in-memory ring stores the most recent **1000 questions**. It consumes approximately 1.1 MiB and is cleared when the daemon restarts. Config exports do not include it.
- The standard system API provides authenticated `GET /api/v1/system/dns/queries` and `DELETE /api/v1/system/dns/queries`. The response is `{"queries":[{"id":1,"time_ms":...,"client":"192.168.1.10","domain":"example.com","qtype":1,"protocol":"udp"}],"capacity":1000}`. The HTTP response has `Cache-Control: no-store`.
- The UI filters by client IP or domain substring, optionally groups by domain (count, clients, last request), and paginates results; aggregation takes place before pagination. It polls every three seconds while the Settings tab is visible unless paused.
- DNS traffic from clients bypassing the proxy (including many DoH/DoT clients) is **not** captured. DNS caching can also eliminate upstream queries. The client IP is only as specific as the address visible at the proxy (NAT/forwarder addresses are not magically resolved to end devices).

History is intentionally **not a browsing history**: DNS requests do not prove any page was visited, nor do they include full URLs, request paths, or response content.
