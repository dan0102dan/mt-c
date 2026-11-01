package main

import (
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"reflect"
	"strings"
	"testing"
	"time"

	"github.com/miekg/dns"
)

// Each fixture owns one ephemeral loopback listener and handles one query.
// Cleanup waits for its goroutine; no public upstream or persistent server.
func udpFixture(t *testing.T, reply func(*dns.Msg) ([]byte, error)) string {
	t.Helper()
	conn, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	done := make(chan error, 1)
	go func() {
		buf := make([]byte, 65535)
		n, peer, err := conn.ReadFrom(buf)
		if err == nil {
			q := new(dns.Msg)
			err = q.Unpack(buf[:n])
			if err == nil {
				var wire []byte
				wire, err = reply(q)
				if err == nil && wire != nil {
					_, err = conn.WriteTo(wire, peer)
				}
			}
		}
		done <- err
	}()
	t.Cleanup(func() {
		_ = conn.Close()
		if err := <-done; err != nil && !errors.Is(err, net.ErrClosed) {
			t.Error(err)
		}
	})
	return conn.LocalAddr().String()
}

func tcpFixture(t *testing.T, reply func(*dns.Msg, *net.TCPConn, <-chan struct{}) error) string {
	t.Helper()
	listener, err := net.ListenTCP("tcp", &net.TCPAddr{IP: net.ParseIP("127.0.0.1")})
	if err != nil {
		t.Fatal(err)
	}
	stop := make(chan struct{})
	done := make(chan error, 1)
	go func() {
		conn, err := listener.AcceptTCP()
		if err != nil {
			done <- err
			return
		}
		defer conn.Close()
		var prefix [2]byte
		_, err = io.ReadFull(conn, prefix[:])
		if err == nil {
			wire := make([]byte, binary.BigEndian.Uint16(prefix[:]))
			_, err = io.ReadFull(conn, wire)
			if err == nil {
				q := new(dns.Msg)
				err = q.Unpack(wire)
				if err == nil {
					err = reply(q, conn, stop)
				}
			}
		}
		done <- err
	}()
	t.Cleanup(func() {
		close(stop)
		_ = listener.Close()
		if err := <-done; err != nil && !errors.Is(err, net.ErrClosed) {
			t.Error(err)
		}
	})
	return listener.Addr().String()
}

func messageFixture(t *testing.T, proto string, reply func(*dns.Msg) *dns.Msg) string {
	t.Helper()
	if proto == "udp" {
		return udpFixture(t, func(q *dns.Msg) ([]byte, error) { return reply(q).Pack() })
	}
	return tcpFixture(t, func(q *dns.Msg, conn *net.TCPConn, _ <-chan struct{}) error {
		wire, err := reply(q).Pack()
		if err != nil {
			return err
		}
		frame := make([]byte, 2, len(wire)+2)
		binary.BigEndian.PutUint16(frame, uint16(len(wire)))
		frame = append(frame, wire...)
		// A TCP stream may split both its length and its DNS message anywhere.
		for _, octet := range frame {
			if _, err := conn.Write([]byte{octet}); err != nil {
				return err
			}
		}
		return nil
	})
}

func TestLoopbackWireFieldsAndJSON(t *testing.T) {
	for _, proto := range []string{"udp", "tcp"} {
		for _, typ := range []uint16{dns.TypeA, dns.TypeAAAA} {
			t.Run(proto+"/"+dns.TypeToString[typ], func(t *testing.T) {
				name := "MiXeD.bench.example."
				target := "alias." + name
				want := []record{{Name: name, Type: 5, Class: 1, TTL: 97, Value: target}}
				for i := 1; i <= 2; i++ {
					ip := fmt.Sprintf("10.99.0.%d", i)
					if typ == dns.TypeAAAA {
						ip = fmt.Sprintf("fd00::%d", i)
					}
					want = append(want, record{Name: target, Type: typ, Class: 1, TTL: uint32(60 + i), Value: ip})
				}
				server := messageFixture(t, proto, func(q *dns.Msg) *dns.Msg {
					r := new(dns.Msg)
					r.SetReply(q)
					r.Compress = true
					r.Answer = append(r.Answer, &dns.CNAME{Hdr: dns.RR_Header{Name: name, Rrtype: 5, Class: 1, Ttl: 97}, Target: target})
					for _, rr := range want[1:] {
						h := dns.RR_Header{Name: rr.Name, Rrtype: rr.Type, Class: rr.Class, Ttl: rr.TTL}
						if typ == dns.TypeA {
							r.Answer = append(r.Answer, &dns.A{Hdr: h, A: net.ParseIP(rr.Value).To4()})
						} else {
							r.Answer = append(r.Answer, &dns.AAAA{Hdr: h, AAAA: net.ParseIP(rr.Value).To16()})
						}
					}
					return r
				})
				got := exchange(server, proto, strings.TrimSuffix(name, "."), typ, 2*time.Second)
				if got.Error != "" || got.Detail != "" || !reflect.DeepEqual(got.Records, want) {
					t.Fatalf("wire result = %#v, want records %#v", got, want)
				}
				// The Python guest runner consumes these JSON keys, not Go field names.
				got.Elapsed = 0
				wire, err := json.Marshal(got)
				if err != nil {
					t.Fatal(err)
				}
				var document map[string]json.RawMessage
				if err := json.Unmarshal(wire, &document); err != nil {
					t.Fatal(err)
				}
				if len(document) != 2 || string(document["elapsed_seconds"]) != "0" || document["records"] == nil {
					t.Fatalf("unexpected result JSON: %s", wire)
				}
				var records []map[string]any
				if err := json.Unmarshal(document["records"], &records); err != nil {
					t.Fatal(err)
				}
				for i, rr := range records {
					expected := map[string]any{"name": want[i].Name, "type": float64(want[i].Type), "class": float64(want[i].Class), "ttl": float64(want[i].TTL), "value": want[i].Value}
					if !reflect.DeepEqual(rr, expected) {
						t.Fatalf("record JSON = %#v, want %#v", rr, expected)
					}
				}
			})
		}
	}
}

func TestMalformedHeadersAndQuestionsAreRejected(t *testing.T) {
	cases := []struct {
		name   string
		mutate func(*dns.Msg)
	}{
		{"query-not-reply", func(r *dns.Msg) { r.Response = false }},
		{"wrong-id", func(r *dns.Msg) { r.Id++ }},
		{"wrong-opcode", func(r *dns.Msg) { r.Opcode = dns.OpcodeStatus }},
		{"error-rcode", func(r *dns.Msg) { r.Rcode = dns.RcodeServerFailure }},
		{"truncated", func(r *dns.Msg) { r.Truncated = true }},
		{"missing-question", func(r *dns.Msg) { r.Question = nil }},
		{"extra-question", func(r *dns.Msg) { r.Question = append(r.Question, r.Question[0]) }},
		{"wrong-name", func(r *dns.Msg) { r.Question[0].Name = "other.example." }},
		{"wrong-type", func(r *dns.Msg) { r.Question[0].Qtype = dns.TypeAAAA }},
		{"wrong-class", func(r *dns.Msg) { r.Question[0].Qclass = dns.ClassCHAOS }},
	}
	for _, proto := range []string{"udp", "tcp"} {
		for _, tc := range cases {
			t.Run(proto+"/"+tc.name, func(t *testing.T) {
				server := messageFixture(t, proto, func(q *dns.Msg) *dns.Msg {
					r := new(dns.Msg)
					r.SetReply(q)
					tc.mutate(r)
					return r
				})
				got := exchange(server, proto, "a.example.", dns.TypeA, 2*time.Second)
				// The DNS library itself rejects some malformed packets (e.g. ID).
				// Either layer must report failure, never successful empty records.
				if got.Error == "" || len(got.Records) != 0 {
					t.Fatalf("malformed reply was accepted: %#v", got)
				}
			})
		}
	}
	q := new(dns.Msg)
	q.SetQuestion("a.example.", dns.TypeA)
	if validResponse(q, nil) {
		t.Fatal("nil response was accepted")
	}
	r := new(dns.Msg)
	r.SetReply(q)
	r.Question[0].Name = "A.EXAMPLE."
	if !validResponse(q, r) {
		t.Fatal("DNS case-insensitive question echo was rejected")
	}
}

func TestDroppedQueriesReportTimeout(t *testing.T) {
	for _, proto := range []string{"udp", "tcp"} {
		t.Run(proto, func(t *testing.T) {
			var server string
			if proto == "udp" {
				server = udpFixture(t, func(_ *dns.Msg) ([]byte, error) { return nil, nil })
			} else {
				server = tcpFixture(t, func(_ *dns.Msg, _ *net.TCPConn, stop <-chan struct{}) error {
					<-stop
					return nil
				})
			}
			got := exchange(server, proto, "drop.example.", dns.TypeA, 40*time.Millisecond)
			if got.Error != "timeout" || got.Detail == "" || len(got.Records) != 0 {
				t.Fatalf("drop result = %#v", got)
			}
			// No elapsed-time performance threshold: only error classification.
		})
	}
}

func TestTCPClosureIsDistinctFromClientTimeout(t *testing.T) {
	cases := []struct {
		name, want string
		reply      func(*dns.Msg, *net.TCPConn, <-chan struct{}) error
	}{
		{"eof", "eof", func(_ *dns.Msg, _ *net.TCPConn, _ <-chan struct{}) error { return nil }},
		{"partial-length", "eof", func(_ *dns.Msg, c *net.TCPConn, _ <-chan struct{}) error { _, err := c.Write([]byte{0}); return err }},
		{"partial-message", "eof", func(_ *dns.Msg, c *net.TCPConn, _ <-chan struct{}) error {
			_, err := c.Write([]byte{0, 12, 0})
			return err
		}},
		{"reset", "reset", func(_ *dns.Msg, c *net.TCPConn, _ <-chan struct{}) error { return c.SetLinger(0) }},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			server := tcpFixture(t, tc.reply)
			got := exchange(server, "tcp", "close.example.", dns.TypeA, 2*time.Second)
			if got.Error != tc.want || got.Detail == "" || len(got.Records) != 0 {
				t.Fatalf("close result = %#v, want %s", got, tc.want)
			}
		})
	}
}
