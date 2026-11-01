// dnsquery performs one validated UDP/TCP exchange and emits exact answer
// records as JSON. Cross-build it to exercise loopback inside a router guest.
package main

import (
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"os"
	"strings"
	"syscall"
	"time"

	"github.com/miekg/dns"
)

type record struct {
	Name  string `json:"name"`
	Type  uint16 `json:"type"`
	Class uint16 `json:"class"`
	TTL   uint32 `json:"ttl"`
	Value string `json:"value"`
}

type result struct {
	Records []record `json:"records,omitempty"`
	Error   string   `json:"error,omitempty"`
	Detail  string   `json:"detail,omitempty"`
	Elapsed float64  `json:"elapsed_seconds"`
}

func validResponse(q, r *dns.Msg) bool {
	return r != nil && r.Response && r.Id == q.Id && r.Opcode == dns.OpcodeQuery && r.Rcode == dns.RcodeSuccess && !r.Truncated && len(r.Question) == 1 && strings.EqualFold(r.Question[0].Name, q.Question[0].Name) && r.Question[0].Qtype == q.Question[0].Qtype && r.Question[0].Qclass == dns.ClassINET
}

func exchange(server, proto, name string, typ uint16, timeout time.Duration) result {
	q := new(dns.Msg)
	q.SetQuestion(dns.Fqdn(name), typ)
	start := time.Now()
	r, _, err := (&dns.Client{Net: proto, Timeout: timeout}).Exchange(q, server)
	out := result{Elapsed: time.Since(start).Seconds()}
	if err != nil {
		out.Error, out.Detail = "network", err.Error()
		var ne net.Error
		switch {
		case errors.As(err, &ne) && ne.Timeout():
			out.Error = "timeout"
		case errors.Is(err, io.EOF), errors.Is(err, io.ErrUnexpectedEOF):
			out.Error = "eof"
		case errors.Is(err, syscall.ECONNRESET):
			out.Error = "reset"
		}
		return out
	}
	if !validResponse(q, r) {
		out.Error = "invalid"
		return out
	}
	for _, rr := range r.Answer {
		h := rr.Header()
		x := record{Name: h.Name, Type: h.Rrtype, Class: h.Class, TTL: h.Ttl}
		switch v := rr.(type) {
		case *dns.A:
			x.Value = v.A.String()
		case *dns.AAAA:
			x.Value = v.AAAA.String()
		case *dns.CNAME:
			x.Value = v.Target
		default:
			x.Value = rr.String()
		}
		out.Records = append(out.Records, x)
	}
	return out
}

func main() {
	server := flag.String("server", "127.0.0.1:3553", "DNS address")
	proto := flag.String("proto", "udp", "udp or tcp")
	name := flag.String("name", "d000000.bench.example.com.", "question name")
	typeName := flag.String("type", "A", "A or AAAA")
	timeout := flag.Duration("timeout", 3*time.Second, "exchange deadline")
	flag.Parse()
	typ, ok := dns.StringToType[*typeName]
	if !ok || (typ != dns.TypeA && typ != dns.TypeAAAA) || (*proto != "udp" && *proto != "tcp") || *timeout <= 0 {
		fmt.Fprintln(os.Stderr, "invalid query options")
		os.Exit(2)
	}
	if err := json.NewEncoder(os.Stdout).Encode(exchange(*server, *proto, *name, typ, *timeout)); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
