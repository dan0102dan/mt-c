// dnsstub is a minimal fixed-answer upstream DNS server used as the upstream
// for MagiTrickle benchmark runs. It answers every A query with a fixed
// address (optionally behind a CNAME chain) with near-zero processing cost,
// so measured latency/throughput is dominated by the proxy under test.
package main

import (
	"flag"
	"fmt"
	"log"
	"net"
	"os"
	"strings"

	"github.com/miekg/dns"
)

var (
	listen      = flag.String("listen", "127.0.0.1:5399", "address to listen on (UDP and TCP)")
	cnameLen    = flag.Int("cname", 0, "length of CNAME chain to prepend to every A answer")
	ttl         = flag.Uint("ttl", 300, "TTL for all answers")
	answerIP    = flag.String("ip", "10.99.0.1", "IPv4 address returned in A answers")
	answerIP6   = flag.String("ip6", "fd00::1", "IPv6 address returned in AAAA answers")
	answerCount = flag.Int("answer-count", 1, "number of consecutive addresses (1..64)")
	alsoAAAA    = flag.Bool("also-aaaa", false, "append AAAA records to A replies")
	compress    = flag.Bool("compress", false, "compress response names")
	dropSuffix  = flag.String("drop-suffix", "", "silently drop questions with this suffix")
)

// consecutiveIP copies its argument; subsequent requests start at the same IP.
func consecutiveIP(base net.IP, offset int) net.IP {
	ip := append(net.IP(nil), base...)
	for i := len(ip) - 1; i >= 0 && offset > 0; i-- {
		n := int(ip[i]) + offset
		ip[i], offset = byte(n), n>>8
	}
	return ip
}

func makeReply(req *dns.Msg) *dns.Msg {
	resp := new(dns.Msg)
	resp.SetReply(req)
	resp.Authoritative = true
	resp.Compress = *compress

	if len(req.Question) == 1 {
		q := req.Question[0]
		switch q.Qtype {
		case dns.TypeA, dns.TypeAAAA:
			owner := q.Name
			for i := 0; i < *cnameLen; i++ {
				target := fmt.Sprintf("c%d.%s", i, q.Name)
				resp.Answer = append(resp.Answer, &dns.CNAME{
					Hdr:    dns.RR_Header{Name: owner, Rrtype: dns.TypeCNAME, Class: dns.ClassINET, Ttl: uint32(*ttl)},
					Target: target,
				})
				owner = target
			}
			for i := 0; i < *answerCount && q.Qtype == dns.TypeA; i++ {
				resp.Answer = append(resp.Answer, &dns.A{
					Hdr: dns.RR_Header{Name: owner, Rrtype: dns.TypeA, Class: dns.ClassINET, Ttl: uint32(*ttl)},
					A:   consecutiveIP(net.ParseIP(*answerIP).To4(), i),
				})
			}
			for i := 0; i < *answerCount && (q.Qtype == dns.TypeAAAA || *alsoAAAA); i++ {
				resp.Answer = append(resp.Answer, &dns.AAAA{
					Hdr:  dns.RR_Header{Name: owner, Rrtype: dns.TypeAAAA, Class: dns.ClassINET, Ttl: uint32(*ttl)},
					AAAA: consecutiveIP(net.ParseIP(*answerIP6).To16(), i),
				})
			}
		default:
			resp.Rcode = dns.RcodeNameError
		}
	}
	return resp
}

func handler(w dns.ResponseWriter, req *dns.Msg) {
	if *dropSuffix != "" && len(req.Question) == 1 && strings.HasSuffix(strings.ToLower(req.Question[0].Name), strings.ToLower(dns.Fqdn(*dropSuffix))) {
		return
	}
	resp := makeReply(req)
	_ = w.WriteMsg(resp)
}

func main() {
	flag.Parse()
	if *answerCount < 1 || *answerCount > 64 || *cnameLen < 0 || *cnameLen > 16 || uint64(*ttl) > 0xffffffff || net.ParseIP(*answerIP).To4() == nil || net.ParseIP(*answerIP6).To16() == nil || net.ParseIP(*answerIP6).To4() != nil {
		log.Fatal("invalid fixture address, count, CNAME length or TTL")
	}
	dns.HandleFunc(".", handler)

	errCh := make(chan error, 2)
	for _, proto := range []string{"udp", "tcp"} {
		srv := &dns.Server{Addr: *listen, Net: proto}
		go func() { errCh <- srv.ListenAndServe() }()
	}
	fmt.Fprintf(os.Stderr, "dnsstub: listening on %s (udp+tcp), cname=%d\n", *listen, *cnameLen)
	err := <-errCh
	if err != nil && !strings.Contains(err.Error(), "use of closed") {
		log.Fatal(err)
	}
}
