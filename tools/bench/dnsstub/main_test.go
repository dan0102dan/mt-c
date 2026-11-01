package main

import (
	"net"
	"testing"

	"github.com/miekg/dns"
)

func fixtureDefaults(t *testing.T) {
	t.Helper()
	oldC, oldN, oldTTL, oldAAAA, oldCompress := *cnameLen, *answerCount, *ttl, *alsoAAAA, *compress
	*cnameLen, *answerCount, *ttl, *alsoAAAA, *compress = 0, 1, 300, false, false
	t.Cleanup(func() { *cnameLen, *answerCount, *ttl, *alsoAAAA, *compress = oldC, oldN, oldTTL, oldAAAA, oldCompress })
}

func TestHistoricalDefault(t *testing.T) {
	fixtureDefaults(t)
	for _, typ := range []uint16{dns.TypeA, dns.TypeAAAA} {
		q := new(dns.Msg)
		q.SetQuestion("a.example.", typ)
		r := makeReply(q)
		if len(r.Answer) != 1 || r.Answer[0].Header().Rrtype != typ || r.Answer[0].Header().Ttl != 300 || r.Compress {
			t.Fatal(r)
		}
	}
}

func TestMixedCompressedChain(t *testing.T) {
	fixtureDefaults(t)
	*cnameLen, *answerCount, *ttl, *alsoAAAA, *compress = 2, 4, 60, true, true
	q := new(dns.Msg)
	q.SetQuestion("d000000.bench.example.com.", dns.TypeA)
	wire, err := makeReply(q).Pack()
	if err != nil {
		t.Fatal(err)
	}
	r := new(dns.Msg)
	if err = r.Unpack(wire); err != nil {
		t.Fatal(err)
	}
	if len(r.Answer) != 10 {
		t.Fatal(r)
	}
	owner := q.Question[0].Name
	for i, rr := range r.Answer {
		if rr.Header().Name != owner || rr.Header().Class != dns.ClassINET || rr.Header().Ttl != 60 {
			t.Fatal(rr)
		}
		if i < 2 {
			owner = rr.(*dns.CNAME).Target
		}
	}
	if r.Answer[5].(*dns.A).A.String() != "10.99.0.4" || r.Answer[9].(*dns.AAAA).AAAA.String() != "fd00::4" {
		t.Fatal(r)
	}
}

func TestConsecutiveIPCarryDoesNotMutate(t *testing.T) {
	for _, pair := range [][2]string{{"10.99.0.255", "10.99.1.0"}, {"fd00::ffff", "fd00::1:0"}} {
		base := net.ParseIP(pair[0])
		if base.To4() != nil {
			base = base.To4()
		}
		if consecutiveIP(base, 1).String() != pair[1] || base.String() != pair[0] {
			t.Fatal(pair, base)
		}
	}
}
